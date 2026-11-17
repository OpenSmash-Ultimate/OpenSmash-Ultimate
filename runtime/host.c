/* Loader and dispatcher for the recompiled game: the console's dynamic linker, natively.
 *
 * Maps each recompiled module's image at its guest address, resolves symbols across the
 * modules in load order (plus native replacements), and then starts the process the way the
 * console does - by calling the SDK's nn::init::Start, which initialises the C library and
 * OS layer, runs the game's nninitStartup, asks the linker to run every module's static
 * constructors, and finally calls the game's nnMain.
 *
 * Usage: ssbu <directory with *.reloc.bin images> [--trace]
 * Built with the game's modules in a library of their own (tools/game_dll.py, runtime/game_dll.h),
 * also: ssbu --game-library, which loads and checks that library and stops (0: it can be used). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
char rt_romfs_warning[320];   /* (hle_fs.c fills it: unconfirmed game data, for crash reports) */   /* (every platform) */
#include "clock.h"
#include "display.h"
#include "hookable.h"
#include "input_seq.h"
#include "settings.h"
/* rt_cpu_needs: what this file was compiled for, which cpu_check.c checks the processor for first */
#define RT_CPU_DEFINE_NEEDS
#include "cpu_check.h"
#ifdef _WIN32
#include <dbghelp.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef SSBU_GAME_DLL
/* The game's modules are in a library of their own (runtime/game_dll.h), which game_open loads at
 * start: the boot modules' tables are copied here, where the rest of the runtime finds them
 * (net_relay.c reads main's build id), and the NROs' list is the library's. */
#include "game_dll.h"
RtModule rt_module_main, rt_module_subsdk0, rt_module_sdk;
static const RtModule *const *game_nros;
static u32 game_nro_count;
#define rt_nro_modules game_nros
#define rt_nro_count game_nro_count
#endif
#define BOOT_MODULES 3
#define MAX_MODULES 1024
/* Loaded modules in load (and symbol search) order: the three the process starts with, then
 * the NROs the game loads through nn::ro. */
static const RtModule *modules[MAX_MODULES] = {&rt_module_main, &rt_module_subsdk0, &rt_module_sdk};
static u32 module_count = BOOT_MODULES;
static GuestFn **code[MAX_MODULES];               /* per module, indexed by (address - base) / 4 */

/* Dispatch: which module covers each 16 MB of the module area (module index + 1). */
#define SLOT_BASE 0x7000000000ull
#define SLOT_SHIFT 24
#define SLOT_COUNT 1024
static u16 slot_module[SLOT_COUNT];
/* Script modules without an NRO (hle_fighter_registry.c: a new fighter's lua2cpp_<name>): an address
 * range of their own whose functions are a donor module's, or a fighter library's, by offset. Their
 * dispatch slots hold CLONE_FLAG | (index + 1); they are not loaded modules (nn::ro's lists, symbols). */
#define CLONE_FLAG 0x8000u
#define MAX_CLONES 32
static struct Clone { char name[64]; u64 base, image_size, text_size; GuestFn **code, **odd; } clones[MAX_CLONES];
static u32 clone_count;
static struct Native { GuestFn *fn; const char *name; } natives[8192];
static u32 native_count;
static __declspec(thread) u32 last_native;        /* which native slot the current thread just looked up */
static const char *phase = "start-up";
int rt_trace;
static int status_every;   /* seconds between thread dumps, 0 for none */
extern u64 gpu_stat_batches, gpu_stat_lists, gpu_stat_draws, gpu_stat_dispatches, gfx_stat_dispatched, gfx_stat_residents, gfx_stat_residents_retired, gfx_stat_clears, gfx_stat_drawn, gfx_stat_skipped,
    gfx_stat_shaders, gfx_stat_shader_failures, gfx_stat_textures, gfx_stat_unpacked, gfx_stat_watch_us, gfx_skip_shader, gfx_skip_topology,
    gfx_skip_target, gfx_stat_unmap_dropped;
u64 present_frames(void);
void movie_test_start(void);   /* (hle_movie_skip.c) SSBU_SKIP_MOVIES, SSBU_SEQ's mark movie */
void test_fight_start(void);   /* (hle_fight_test.c) SSBU_FIGHT_HASH, SSBU_DETERMINISTIC, SSBU_DEBUG_MATCH... */
void gfx_report_compiles(void (*line)(const char *text));
void gfx_status_lines(void);
#if defined(_WIN32) || defined(__linux__)
int gfx_warm_shaders_main(const char *dir, const char *arg);   /* (gpu_d3d11.c: ssbu --warm-shaders) */
#endif
const char *rt_data_dir;
/* This thread is reading another thread's stack (debug_profile.c): a fault there is expected and
 * handled where it happens, not a crash. */
__declspec(thread) int rt_probing;

/* ---- diagnostics ----------------------------------------------------------------- */
static struct HostFn { u64 host, guest; } *by_host;   /* guest == 0 marks runtime code */
static u32 by_host_count;

static int by_host_cmp(const void *a, const void *b) {
    u64 x = ((const struct HostFn *)a)->host, y = ((const struct HostFn *)b)->host;
    return x < y ? -1 : x > y;
}

const char *rt_describe(u64 addr, char *buf, size_t n) {
    for (u32 m = 0; m < module_count; m++)
        if (addr - modules[m]->base < modules[m]->image_size) {
            snprintf(buf, n, "%s+%#llx", modules[m]->name, addr - modules[m]->base);
            return buf;
        }
    for (u32 i = 0; i < clone_count; i++)
        if (addr - clones[i].base < clones[i].image_size) {
            snprintf(buf, n, "%s+%#llx", clones[i].name, addr - clones[i].base);
            return buf;
        }
    if (addr - NATIVE_BASE < (u64)__atomic_load_n(&native_count, __ATOMIC_ACQUIRE) * 16) snprintf(buf, n, "native:%s", natives[(addr - NATIVE_BASE) >> 4].name);
    else snprintf(buf, n, "%#llx", addr);
    return buf;
}

/* The module whose code (text) holds addr, and the offset there: NULL if none. */
const char *rt_code_offset(u64 addr, u64 *off) {
    for (u32 m = 0; m < module_count; m++)
        if (addr - modules[m]->base < modules[m]->text_size) { *off = addr - modules[m]->base; return modules[m]->name; }
    return NULL;
}

/* Guest function containing a host code address, or 0 for runtime code. */
static u64 guest_of(u64 pc) {
    u32 lo = 0, hi = by_host_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (by_host[mid].host <= pc) lo = mid + 1; else hi = mid;
    }
    return lo && pc - by_host[lo - 1].host <= (4u << 20) ? by_host[lo - 1].guest : 0;
}

/* A backtrace frame's guest function. A frame is a return address, looked up as its call (ra - 1),
 * unless it is a faulting or stopped context's own pc (is_pc). Looked up as it is, a return address
 * is the next function's first instruction when the call was the caller's last (to abort, rt_trap,
 * rt_fatal, a C++ terminate: noreturn), and names that function instead. */
static u64 guest_of_frame(u64 frame, int is_pc) { return guest_of(is_pc ? frame : frame - 1); }

/* A host code address in recompiled code, as the guest function's place ("main+0x1234"); NULL
 * for any other code (debug_profile.c's thread dumps). */
const char *rt_describe_host(u64 pc, char *buf, size_t n) {
    u64 guest = by_host ? guest_of(pc) : 0;
    return guest ? rt_describe(guest, buf, n) : NULL;
}

/* ---- reports: to the log, and for a crash or a stop also to reports\<kind>_<time>.txt ------ */
#ifdef _WIN32
static HANDLE report_file = INVALID_HANDLE_VALUE;
#else
static int report_fd = -1;   /* (Linux: a plain descriptor, so that writing a crash report allocates nothing) */
#endif

/* (no heap: a crash may have happened inside the allocator) */
static void out(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    fputs(buf, stderr);
#ifdef _WIN32
    DWORD written;
    if (report_file != INVALID_HANDLE_VALUE) WriteFile(report_file, buf, (DWORD)n, &written, NULL);
#else
    if (report_fd >= 0 && write(report_fd, buf, (size_t)n) < 0) report_fd = -1;
#endif
}
static void out_line(const char *text) { out("%s", text); }

/* reports\<kind>_<date>_<time>.<ext>, in the current directory (the port root, where every
 * launcher starts the game) */
void rt_report_path(char *path, size_t n, const char *kind, const char *ext) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    CreateDirectoryA("reports", NULL);
#ifdef _WIN32
    snprintf(path, n, "reports\\%s_%04u%02u%02u_%02u%02u%02u.%s", kind, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
#else
    snprintf(path, n, "reports/%s_%04u%02u%02u_%02u%02u%02u.%s", kind, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
#endif
}

static void report_open(const char *kind, char *path, size_t n) {
    rt_report_path(path, n, kind, "txt");
#ifdef _WIN32
    report_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
#else
    report_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#endif
}

/* The report's last line (where it is), and closed. */
static void report_close(const char *path) {
#ifdef _WIN32
    if (report_file == INVALID_HANDLE_VALUE) return;
    out("report: %s\n", path);
    CloseHandle(report_file);
    report_file = INVALID_HANDLE_VALUE;
#else
    if (report_fd < 0) return;
    out("report: %s\n", path);
    close(report_fd);
    report_fd = -1;
#endif
}

#ifdef _WIN32
/* ---- minidumps, written by a second process (this exe with --write-dump): a process dumping
 * itself can deadlock on a lock one of its suspended threads holds. The request goes through a
 * helper thread started with the process, so a faulting thread with no stack left needs none.
 * SSBU_MINIDUMP=0: none; =full: all memory (the guest's too: several GB). */
static struct { HANDLE request, done; char path[MAX_PATH]; DWORD tid; EXCEPTION_POINTERS *e; volatile LONG ok, busy; } dumper;

static DWORD WINAPI dump_helper(void *unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"minidump helper");
    for (;;) {
        WaitForSingleObject(dumper.request, INFINITE);
        wchar_t exe[MAX_PATH], cmd[2 * MAX_PATH + 160];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        _snwprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" --write-dump %lu %lu %llx \"%hs\"", exe, GetCurrentProcessId(), dumper.tid,
                   (unsigned long long)(uintptr_t)dumper.e, dumper.path);
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        InterlockedExchange(&dumper.ok, code == 0);
        SetEvent(dumper.done);
    }
}

/* A minidump of this process at path (with thread tid's exception, if e); 1 when written. */
int rt_write_dump(const char *path, DWORD tid, EXCEPTION_POINTERS *e, DWORD wait_ms) {
    if (!dumper.request || InterlockedExchange(&dumper.busy, 1)) return 0;
    snprintf(dumper.path, sizeof dumper.path, "%s", path);
    dumper.tid = tid;
    dumper.e = e;
    ResetEvent(dumper.done);
    SetEvent(dumper.request);
    int ok = WaitForSingleObject(dumper.done, wait_ms) == WAIT_OBJECT_0 && dumper.ok;
    InterlockedExchange(&dumper.busy, 0);
    return ok;
}

static void dump_helper_start(void) {
    const char *m = getenv("SSBU_MINIDUMP");
    if (m && !strcmp(m, "0")) return;
    dumper.request = CreateEventW(NULL, FALSE, FALSE, NULL);
    dumper.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, dump_helper, NULL, 0, NULL));
}

/* The second process: ssbu --write-dump <pid> <thread id> <exception pointers, hex> <file> */
static int write_dump_main(char **argv) {
    DWORD pid = strtoul(argv[2], NULL, 10), tid = strtoul(argv[3], NULL, 10);
    unsigned long long ptrs = strtoull(argv[4], NULL, 16);
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    BOOL (WINAPI *write)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                         PMINIDUMP_CALLBACK_INFORMATION) =
        dbghelp ? (BOOL (WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                   PMINIDUMP_CALLBACK_INFORMATION))(void *)GetProcAddress(dbghelp, "MiniDumpWriteDump") : NULL;
    if (!proc || !write) return 2;
    HANDLE f = CreateFileA(argv[5], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 3;
    const char *m = getenv("SSBU_MINIDUMP");
    MINIDUMP_TYPE type = m && !_stricmp(m, "full")
                             ? (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules)
                             : (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
    MINIDUMP_EXCEPTION_INFORMATION mei = {tid, (PEXCEPTION_POINTERS)(uintptr_t)ptrs, TRUE};
    BOOL ok = write(proc, pid, f, type, ptrs ? &mei : NULL, NULL, NULL);
    CloseHandle(f);
    if (!ok) DeleteFileA(argv[5]);
    return ok ? 0 : 4;
}
#else
/* Linux: no minidumps. A crash or freeze report is text only; the system's core dump, where it
 * keeps them (ulimit -c, systemd-coredump), has the rest. */
int rt_write_dump(const char *path, DWORD tid, EXCEPTION_POINTERS *e, DWORD wait_ms) {
    (void)path; (void)tid; (void)e; (void)wait_ms;
    return 0;
}
#endif

/* (from_pc: frames[0] is the faulting context's pc; every other frame is a return address) */
static void print_frames(void **frames, int n, int from_pc) {
    char name[96];
    out("guest backtrace (innermost first):\n");
    for (int i = 0, shown = 0; i < n && shown < 28; i++) {
        u64 guest = guest_of_frame((u64)frames[i], i == 0 && from_pc);
        if (!guest) {   /* runtime code: its offset in the executable (see the linker map) */
            static u64 image;
            if (!image) image = (u64)GetModuleHandleW(NULL);
            if (rt_trace || getenv("SSBU_HOST_FRAMES")) out("  [host +%#llx]\n", (u64)frames[i] - image);
            continue;
        }
        out("  %s\n", rt_describe(guest, name, sizeof name));
        shown++;
    }
}

void rt_backtrace(void) {
    void *frames[62];
    print_frames(frames, CaptureStackBackTrace(1, 62, frames, NULL), 0);
}

#ifdef _WIN32
/* Unwind a host context, collecting return addresses. */
static int unwind(CONTEXT ctx, void **frames, int max) {
    int n = 0;
    while (n < max && ctx.Rip) {
        frames[n++] = (void *)ctx.Rip;
        DWORD64 image;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
        if (!fn) {   /* a leaf function: no unwind data, the return address is on top of the stack */
            if (!ctx.Rsp || IsBadReadPtr((void *)ctx.Rsp, 8)) break;
            ctx.Rip = *(DWORD64 *)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        void *handler_data;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &establisher, NULL);
    }
    return n;
}

/* SSBU_DEBUG_CAMERA_HUNT (ultrawide research): at frame ends, every 10 s, the heap and the
 * alias area are searched for perspective matrices of the game's 16:9 shape (the y scale 16/9 of
 * the x scale, no rotation terms, -1 or 1 in w), logged, and their pages made read-only for 2 s:
 * each game function that writes one is logged once with its callers, then the write goes on
 * (the page opened for one instruction, single-stepped, and closed again). Which code builds the
 * camera's projection, so its shape can be widened where the game makes it.
 * ponytail: page-protection tracer, one thread's write at a time; research only */
#define HUNT_MAX 16
static struct { u64 at; u8 *page; DWORD old; int armed; } hunt[HUNT_MAX];
static int nhunt;
static u64 hunt_until;
static volatile LONG hunt_logged;
static __declspec(thread) u8 *hunt_rearm;
static u64 hunt_seen[64];
static int nseen;

static volatile LONG64 hunt_wanted[HUNT_MAX];   /* (from the renderer's thread: gpu_d3d11.c cam_hunt_cb) */
static volatile LONG hunt_nwanted;
void camera_hunt_watch(u64 guest) {
    LONG k = InterlockedIncrement(&hunt_nwanted) - 1;
    if (k < HUNT_MAX) InterlockedExchange64(&hunt_wanted[k], (LONG64)guest);
}

/* (the game's thread, at frame ends: what to watch for 2 s - the renderer's camera matrices) */
static void hunt_scan(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)args; (void)ctx;
    static u64 last;
    if (rt_now_ns() - last < 3000000000ull) return;
    last = rt_now_ns();
    for (int i = 0; i < nhunt; i++)
        if (hunt[i].armed) { DWORD o; hunt[i].armed = 0; VirtualProtect(hunt[i].page, 4096, hunt[i].old, &o); }
    nhunt = 0;
    LONG n = InterlockedExchange(&hunt_nwanted, 0);
    if (n > HUNT_MAX) n = HUNT_MAX;
    for (LONG k = 0; k < n; k++) {
        u64 g = (u64)hunt_wanted[k];
        u8 *page = (u8 *)((uintptr_t)G2H(g) & ~(uintptr_t)4095);
        int dup = 0;
        for (int i = 0; i < nhunt; i++) dup |= hunt[i].page == page;
        if (!dup) { hunt[nhunt].at = g; hunt[nhunt].page = page; nhunt++; }
    }
    if (!nhunt) { fflush(stdout); return; }
    printf("[hunt] watching %d pages for 2 s\n", nhunt);
    fflush(stdout);
    hunt_until = rt_now_ns() + 2000000000ull;
    for (int i = 0; i < nhunt; i++) hunt[i].armed = VirtualProtect(hunt[i].page, 4096, PAGE_READONLY, &hunt[i].old) != 0;
}

#else
void camera_hunt_watch(u64 guest) { (void)guest; }   /* (SSBU_DEBUG_CAMERA_HUNT: Windows only, a research tool) */
#endif

/* ---- ultrawide (docs/ULTRAWIDE.md): the views and the HUD for a picture wider than 16:9, on Windows and
 * Linux (macOS is held at 16:9: display_shape_locked) ---------------------------------------------- */
#ifndef __APPLE__
#include <math.h>
#include "frame_hooks.h"

/* SSBU_DEBUG_CAM_TRACE=<n> (research): every n fight frames, both cameras on one line, to see how the 16:9
 * camera behaves (docs/ULTRAWIDE.md). The fight frame is SSBU_FIGHT_HASH's (hle_fight_test.c), so its
 * lines - the fighters' positions - join on it; it counts only with SSBU_FIGHT_HASH (or
 * SSBU_DETERMINISTIC) set. Render camera: position (its inverse view, +0x280), field of view (+0x15c),
 * width (+0x164). Game camera manager M (main+0x52b8f00): the stage's camera range (+0xc20: left,
 * right, top, bottom), the blast zones (+0xc30), the visible range's centre (+0xeb0) and half height
 * (+0xf38 x +0xf68), its width / height (+0xf58). */
long long fight_test_frame0(void);   /* (hle_fight_test.c) */
static int wide_style_stage(void);   /* (below) */
static void cam_trace(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)args; (void)ctx;
    static long long last = -1;
    static int every = -1;
    if (every < 0) { every = atoi(getenv("SSBU_DEBUG_CAM_TRACE")); if (every < 1) every = 10; }
    long long f = fight_test_frame0();
    if (f <= 0 || f == last || f % every) return;
    last = f;
    u64 a = *P(u64, 0x7105337ce8ull), m = *P(u64, 0x71052b8f00ull);
    if (!a || !(a = *P(u64, a + 0x490)) || !m || !(m = *P(u64, m))) return;
    u64 cam = *P(u64, a + 0xd8) ? *P(u64, a + 0xe0) : 0x7105334060ull;
    const float *pos = P(float, cam + 0x280), *rg = P(float, m + 0xc20), *dz = P(float, m + 0xc30), *vc = P(float, m + 0xeb0);
    printf("[camtrace] F %lld | render pos %.2f %.2f %.2f fov %.2f width %.0f | range %.1f %.1f %.1f %.1f | dead %.1f %.1f %.1f %.1f"
           " | visible centre %.2f %.2f half-height %.2f aspect %.4f\n",
           f, pos[0], pos[1], pos[2], *P(float, cam + 0x15c), *P(float, cam + 0x164), rg[0], rg[1], rg[2], rg[3], dz[0], dz[1], dz[2], dz[3],
           vc[0], vc[1], *P(float, m + 0xf38) * *P(float, m + 0xf68), *P(float, m + 0xf58));
    {   /* (the views: the offset the picture's camera has from the game's, and its zoom) */
        extern float wide_now_x, wide_now_y, wide_now_z;
        extern volatile float shadow_k_out, stage_out_k, stage_out_l, stage_out_r;
        printf("[camwide] F %lld | offset %.2f %.2f %.2f k %.4f view %.1f %.1f\n", f, wide_now_x, wide_now_y, wide_now_z, wide_style_stage() ? stage_out_k : shadow_k_out,
               stage_out_l, stage_out_r);
    }
    {
        extern volatile long hud_calls[12];
        char line[200];
        int n = snprintf(line, sizeof line, "[hudcalls] F %lld |", f);
        for (int k = 0; k < 12; k++) n += snprintf(line + n, sizeof line - n, " %ld", InterlockedExchange(&hud_calls[k], 0));
        printf("%s\n", line);
    }
    if (getenv("SSBU_DEBUG_CAM_TRACE_OBJ"))   /* (the render camera's start: its inputs) */
        for (u32 o = 0; o < 0x140; o += 0x20) {
            const float *v = P(float, cam + o);
            printf("[camobj] F %lld +%03x: %g %g %g %g  %g %g %g %g\n", f, o, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        }
    fflush(stdout);
}

/* SSBU_DEBUG_FRAMING_DUMP=<n> (research, reads only): every n fight frames, the game camera that frames the
 * fight as f_71004fe4f0 gets it (x0), +0x40..+0x240, and the object at its +0x8 (where it takes its
 * aspect from, +0x128), +0x60..+0x140: where it keeps the fighters' box it frames. */
static int view_on(void);   /* (below) */
static GuestFn *fdump_orig;
static volatile float wide_goal_offset[3];   /* (the view's goal: its look-at minus the game's - zoomed: the shadow camera's; hybrid: the slide along the stage) */
static void framing_dump(Cpu *c) {
    static long long last = -1;
    static int every = -1;
    if (every < 0) { const char *e = getenv("SSBU_DEBUG_FRAMING_DUMP"); every = e ? atoi(e) : 0; if (e && every < 1) every = 30; }
    long long f = fight_test_frame0();
    if (every > 0 && f > 0 && f != last && f % every == 0) {
        last = f;
        u64 cam = c->x[0], src = *P(u64, cam + 0x8);
        printf("[framing] F %lld camera %#llx (source %#llx), s0 %g\n", f, (unsigned long long)cam, (unsigned long long)src, c->v[0].f[0]);
        for (u32 o = 0x40; o < 0x240; o += 0x20) {
            const float *v = P(float, cam + o);
            printf("[framing] cam+%03x: %g %g %g %g  %g %g %g %g\n", o, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        }
        if (src)
            for (u32 o = 0x60; o < 0x140; o += 0x20) {
                const float *v = P(float, src + o);
                printf("[framing] src+%03x: %g %g %g %g  %g %g %g %g\n", o, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
            }
        fflush(stdout);
    }
    static int game_wide = -1;   /* (SSBU_DEBUG_REFERENCE_VIEW: the game's own framing widened - changes the fight) */
    if (game_wide < 0) game_wide = getenv("SSBU_DEBUG_REFERENCE_VIEW") != NULL;
    if (game_wide) {
        u64 cam = c->x[0], src = *P(u64, cam + 0x8);
        float a = display_aspect(), *own = P(float, cam + 0x134), *from = src ? P(float, src + 0x128) : NULL;
        if (from && fabsf(*from - 16.0f / 9) < 1e-3f) *from = a;
        if (fabsf(*own - 16.0f / 9) < 1e-3f) *own = a;
        if (fabsf(c->v[0].f[0] - 16.0f / 9) < 1e-3f) c->v[0].f[0] = a;
    }
    fdump_orig(c);
}
/* The views (wide, zoomed, hybrid): the render camera's matrices are copied for drawing, culling and the HUD
 * by f_7103555140 (dst x0 = the camera + 0x1d0, src x1 = its own); after that copy, that block is moved by the
 * view's offset and zoom: position (inverse view, +0xb0 of the block), look-at (+0x100) and the view's
 * translation (+0x30, minus the offset turned by the view). The wide view moves nothing (the game's own camera);
 * the zoomed view follows its shadow camera, the hybrid view slides and zooms within the stage. The game's
 * camera is not touched: the fight is the same. */
static int wide_style_stage(void), wide_style_shadow(void);   /* (below) */
static GuestFn *copy_orig;
static float wide_now[3];
float wide_now_x, wide_now_y, wide_now_z;   /* (a copy for SSBU_DEBUG_CAM_TRACE) */
static u64 render_camera(void);   /* (below) */
/* In a fight: the fighters' frame line ran in the last quarter second (a frame hook of its own). */
static volatile u64 fight_line_at;
static void fight_line(Cpu *c, const u64 args[8], void *ctx) { (void)c; (void)args; (void)ctx; fight_line_at = rt_now_ns(); }
static int line_on(void) { return fight_line_at && rt_now_ns() - fight_line_at < 250000000ull; }
/* A pause is still the fight: the line stops, the ready-go flag stays on (dropping the views there made
 * the picture jump to the game's own camera, most near the stage's edge, and the next frames start the
 * zoomed view's shadow camera afresh). */
int game_fight_on(void);   /* (hle_fight_test.c) */
static int in_fight(void) { return line_on() || (fight_line_at && game_fight_on()); }
/* The offset is on the block only while the frame is prepared for drawing (from this copy until the
 * scene set-up, f_7103747820, returns): game logic may ask where things are on screen (scripts have
 * sv_camera_manager::world_to_screen), and a moved camera changed a fighter's end-of-game pose. */
static struct { u64 block; float t[3], eye[3], at[3]; int on; } wide_applied;
static volatile float shadow_k = 1;
volatile float shadow_k_out = 1;   /* (for SSBU_DEBUG_CAM_TRACE) */   /* (stage style: the shadow's distance over the game's; d is then the look-at's shift) */
static void wide_apply(u64 block, const float d[3]) {
    float *v = P(float, block), *eye = P(float, block + 0xb0), *at = P(float, block + 0x100);
    wide_applied.block = block;
    memcpy(wide_applied.t, v + 12, 12); memcpy(wide_applied.eye, eye, 12); memcpy(wide_applied.at, at, 12);
    if (wide_style_stage() || wide_style_shadow()) {
        float k = shadow_k;
        for (int j = 0; j < 3; j++) {
            eye[j] = at[j] + (eye[j] - at[j]) * k + d[j];
            at[j] += d[j];
        }
        for (int j = 0; j < 3; j++) v[12 + j] = -(eye[0] * v[j] + eye[1] * v[4 + j] + eye[2] * v[8 + j]);   /* (row-vector view: t = -eye R) */
    } else
        for (int j = 0; j < 3; j++) {
            v[12 + j] -= d[0] * v[j] + d[1] * v[4 + j] + d[2] * v[8 + j];   /* (row-vector view: t' = t - d R) */
            eye[j] += d[j];
            at[j] += d[j];
        }
    wide_applied.on = 1;
}
static void wide_restore(void) {
    if (!wide_applied.on) return;
    u64 b = wide_applied.block;
    memcpy(P(float, b) + 12, wide_applied.t, 12); memcpy(P(float, b + 0xb0), wide_applied.eye, 12); memcpy(P(float, b + 0x100), wide_applied.at, 12);
    wide_applied.on = 0;
}
/* The hybrid view: the game's own camera, its view only widened, as long as that view stays inside
 * the stage's camera range (the camera manager's +0xc20: left, right); past it, the view first slides
 * along the stage and then zooms in, only as far as it must. The middle 16:9 - what the game framed -
 * always stays in view (zoom at most to the 16:9 width, slide at most the extra width). On the
 * fighters' plane (z = 0): half height = the eye's distance x tan(fov / 2). */
volatile float stage_out_k = 1, stage_out_dx, stage_out_l, stage_out_r;   /* (for SSBU_DEBUG_CAM_TRACE) */
static void stage_fit(u64 block) {
    u64 m = *P(u64, 0x71052b8f00ull), cam = render_camera();
    m = m ? *P(u64, m) : 0;
    float k = 1, dx = 0;
    if (m && cam) {
        const float *eye = P(float, block + 0xb0), *at = P(float, block + 0x100), *range = P(float, m + 0xc20);
        float fov = *P(float, cam + 0x15c) * 3.14159265f / 180, L = range[0], R = range[1];
        float a = display_aspect(), dir[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
        float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        float t = dir[2] < -1e-3f ? -eye[2] / dir[2] : 0;   /* (the ray through the look-at, down to the fighters' plane z = 0) */
        float dist = t * dl;
        if (t > 0 && dist > 1 && fov > 0.01f && R - L > 1) {
            float hh = dist * tanf(fov / 2), hw = hh * a, hw169 = hh * (16.0f / 9), cx = eye[0] + dir[0] * t;
            if (hw > (R - L) / 2) k = (R - L) / 2 / hw;           /* (wider than the stage: zoom in) */
            if (k < hw169 / hw) k = hw169 / hw;                  /* (never less than what 16:9 shows) */
            float w = hw * k, room = w - hw169;   /* (how far the view may slide and keep the 16:9 middle) */
            if (room < 0) room = 0;
            if (cx - w < L) dx = L - (cx - w);
            else if (cx + w > R) dx = R - (cx + w);
            if (dx > room) dx = room;
            if (dx < -room) dx = -room;
            stage_out_l = cx + dx - w; stage_out_r = cx + dx + w;
        }
    }
    wide_goal_offset[0] = dx; wide_goal_offset[1] = 0; wide_goal_offset[2] = 0;
    shadow_k = k;
    stage_out_k = k; stage_out_dx = dx;
}

static void wide_copy(Cpu *c) {
    u64 dst = c->x[0];
    copy_orig(c);
    u64 cam = render_camera();
    /* Training's camera is orthographic (+0x150 = 1; its box at +0x140: left, right, top, bottom), and the
     * game builds that projection (+0x290, inverse +0x2d0) from the box, 16:9 whatever +0x164 says: the
     * 3D was drawn stretched across a wider picture. Widened here for drawing, after the copy; only the
     * game's own 16:9 matrix is changed (the check), so a second copy in one frame leaves it alone. */
    if (cam && dst == cam + 0x1d0 && view_on() && *P(u32, cam + 0x150) == 1) {
        const float *box = P(float, cam + 0x140);
        float *pr = P(float, cam + 0x290), *inv = P(float, cam + 0x2d0), w = box[1] - box[0];
        if (w > 1e-3f && fabsf(pr[0] * w - 2) < 1e-3f) {
            pr[0] = 2 / (w * display_aspect() / (16.0f / 9));
            inv[0] = 1 / pr[0];
        }
    }
    if (!cam || dst != cam + 0x1d0) return;   /* (another camera's copy: the views' state is left alone, or the zoom
                                                * was cleared each frame and a pause, which stops the shadow camera, kept 1) */
    if (!in_fight() || !view_on()) { wide_now[0] = wide_now[1] = wide_now[2] = 0; shadow_k = 1; return; }
    static int cycle = -1;   /* SSBU_TEST_VIEW_CYCLE=<n> (test): the next view every n fight frames, as the menu would */
    if (cycle < 0) { const char *e = getenv("SSBU_TEST_VIEW_CYCLE"); cycle = e ? atoi(e) : 0; }
    if (cycle > 0) { long long f = fight_test_frame0(); if (f > 0) display_set_view((int)(f / cycle % 3)); }
    if (wide_style_stage()) stage_fit(dst);   /* (sets wide_goal_offset and shadow_k) */
    else if (!wide_style_shadow()) {   /* the wide view: the game's own camera, the side areas added */
        shadow_k = 1;
        wide_goal_offset[0] = wide_goal_offset[1] = wide_goal_offset[2] = 0;
    }
    for (int i = 0; i < 3; i++) wide_now[i] = wide_goal_offset[i];
    {   /* Paused: the pause shot starts from the picture's view, whose offset fades out as the player
         * moves the pause camera from where it started (gone a quarter of the camera's distance away):
         * kept whole, it would push a zoomed-in pause shot off what it shows. */
        static int paused; static float eye0[3], dist0, now0[3], k0;
        const float *eye = P(float, dst + 0xb0), *at = P(float, dst + 0x100);
        if (!line_on()) {
            if (!paused) {
                paused = 1;
                memcpy(eye0, eye, 12); memcpy(now0, wide_now, 12); k0 = shadow_k;
                dist0 = sqrtf((eye[0] - at[0]) * (eye[0] - at[0]) + (eye[1] - at[1]) * (eye[1] - at[1]) + (eye[2] - at[2]) * (eye[2] - at[2]));
            }
            float moved = sqrtf((eye[0] - eye0[0]) * (eye[0] - eye0[0]) + (eye[1] - eye0[1]) * (eye[1] - eye0[1]) + (eye[2] - eye0[2]) * (eye[2] - eye0[2]));
            float fade = dist0 > 1 ? 1 - moved / (dist0 / 4) : 1;
            if (fade < 0) fade = 0;
            for (int i = 0; i < 3; i++) wide_now[i] = now0[i] * fade;
            shadow_k = 1 + (k0 - 1) * fade;
        } else paused = 0;
    }
    wide_now_x = wide_now[0]; wide_now_y = wide_now[1]; wide_now_z = wide_now[2];
    wide_applied.on = 0;   /* (the copy wrote fresh values: nothing of the last frame to put back) */
    wide_apply(dst, wide_now);
}
/* The zoomed view: a shadow camera. The game camera's update
 * (f_71004fdc10, x0 the camera) frames with f_71004fe4f0 (goal into the block at +0x70: look now +0x0,
 * goal +0x10, position now +0x20, goal +0x30) and moves it with f_71004fec80 (x3 = +0x110). Right after
 * it, both run again with the picture's aspect on a block of this file's own (kept from frame to frame,
 * so the game's easing, zoom limits and stage clamping act on it as on the real one); only the picture
 * follows it (wide_copy). The one write outside the block (camera manager +0xc80) is put back. */
static GuestFn *upd_orig;
static u8 shadow_block[0xa0], shadow_x3[8];
/* The two calls as the game made them this frame (every register, and x1's buffer, which the caller
 * fills), for the block at camera + 0x70: replayed on the shadow with only the block and the aspect
 * changed. */
typedef struct { Cpu c; u8 x1buf[0x80]; int valid; } CallRec;
static CallRec rec_frame, rec_move;
static GuestFn *frame_fn_orig, *move_fn_orig;
static u64 rec_cam;
static void rec_call(CallRec *r, Cpu *c, GuestFn *orig) {
    if (rec_cam && c->x[0] == rec_cam && c->x[2] == rec_cam + 0x70) {
        r->c = *c;
        memcpy(r->x1buf, G2H(c->x[1]), sizeof r->x1buf);
        r->valid = 1;
    }
    orig(c);
}
static void rec_frame_call(Cpu *c) { rec_call(&rec_frame, c, frame_fn_orig); }
static void rec_move_call(Cpu *c) { rec_call(&rec_move, c, move_fn_orig); }
static void replay_call(const CallRec *r, Cpu *base_c, u64 base, u64 blk, u64 x3, GuestFn *fn) {
    Cpu s = r->c;
    u64 out = base + 0x100;
    memcpy(G2H(out), r->x1buf, sizeof r->x1buf);
    s.x[1] = out;
    s.x[2] = blk;
    if (fn == move_fn_orig) s.x[3] = x3;
    s.v[0].f[0] = display_aspect();
    s.sp = base - 0x10;
    s.x[30] = 0;
    (void)base_c;
    fn(&s);
}
static u64 shadow_cam;
static int shadow_ready;
static void shadow_update(Cpu *c) {
    if (!view_on() || !wide_style_shadow()) { shadow_ready = 0; upd_orig(c); return; }   /* (another view now: the next zoomed one starts afresh) */
    u64 cam = c->x[0];
    rec_cam = cam;
    rec_frame.valid = rec_move.valid = 0;
    upd_orig(c);
    rec_cam = 0;
    if (!in_fight() || !rec_frame.valid || !rec_move.valid) { if (!in_fight()) shadow_ready = 0; return; }
    if (!shadow_ready || cam != shadow_cam) {   /* (a new fight: start from the game's camera) */
        memcpy(shadow_block, G2H(cam + 0x70), sizeof shadow_block);
        memcpy(shadow_x3, G2H(cam + 0x110), sizeof shadow_x3);
        shadow_cam = cam;
        shadow_ready = 1;
    }
    u64 base = (c->sp - 0x800) & ~0xFull, blk = base, x3 = base + 0xc0, out = base + 0x100;
    u64 m = *P(u64, 0x71052b8f00ull);
    m = m ? *P(u64, m) : 0;
    float keep_c80 = m ? *P(float, m + 0xc80) : 0;
    memcpy(G2H(blk), shadow_block, sizeof shadow_block);
    memcpy(G2H(x3), shadow_x3, sizeof shadow_x3);
    (void)out;
    replay_call(&rec_frame, c, base, blk, x3, frame_fn_orig);
    replay_call(&rec_move, c, base, blk, x3, move_fn_orig);
    memcpy(shadow_block, G2H(blk), sizeof shadow_block);
    memcpy(shadow_x3, G2H(x3), sizeof shadow_x3);
    if (m) *P(float, m + 0xc80) = keep_c80;
    /* the picture's change: the shadow's look-at minus the game's, and its distance over the game's */
    const float *sl = (const float *)shadow_block, *gl = P(float, cam + 0x70);
    const float *sp = (const float *)(shadow_block + 0x20), *gp = P(float, cam + 0x90);
    float d[3] = {sl[0] - gl[0], sl[1] - gl[1], sl[2] - gl[2]};
    float ds = sqrtf((sp[0] - sl[0]) * (sp[0] - sl[0]) + (sp[1] - sl[1]) * (sp[1] - sl[1]) + (sp[2] - sl[2]) * (sp[2] - sl[2]));
    float dg = sqrtf((gp[0] - gl[0]) * (gp[0] - gl[0]) + (gp[1] - gl[1]) * (gp[1] - gl[1]) + (gp[2] - gl[2]) * (gp[2] - gl[2]));
    if (dg > 1e-3f && ds == ds && ds / dg > 0.05f && ds / dg < 20) shadow_k = ds / dg;
    shadow_k_out = wide_style_shadow() ? shadow_k : 1;
    if (d[0] == d[0] && d[1] == d[1] && d[2] == d[2] && fabsf(d[0]) < 2000 && fabsf(d[1]) < 2000 && fabsf(d[2]) < 2000)
        memcpy((void *)wide_goal_offset, d, sizeof d);
}
/* (the view as set now: the settings menu changes it during a fight) */
static int wide_style_shadow(void) { return display_view() == VIEW_ZOOMED; }
static int wide_style_stage(void) { return display_view() == VIEW_HYBRID; }
/* The game's framing (f_71004fe4f0), one hook for every view: recorded for the zoomed view's shadow
 * camera, left alone for the wide and hybrid views. (The wide view once ran it again for the picture's
 * shape and eased toward that; the renderer's copies for other cameras cleared it every frame, so it
 * sat at 12% of it: owner, 2026-11-12, the game's own framing instead.) */
static void framing_hook(Cpu *c) {
    static int dump = -1;
    if (dump < 0) dump = getenv("SSBU_DEBUG_FRAMING_DUMP") != NULL;
    if (rec_cam) rec_call(&rec_frame, c, frame_fn_orig);
    else if (dump) framing_dump(c);
    else frame_fn_orig(c);
}

static GuestFn *prep_orig;
static void wide_prep(Cpu *c) { prep_orig(c); wide_restore(); }

static void hook_framing_dump(void) {
    u64 orig = rt_hook_function(0x71004fe4f0ull, rt_native(framing_dump, "framing dump"));
    if (orig) fdump_orig = rt_lookup(orig);
    printf("[framing] dump %s (main+0x4fe4f0)\n", orig ? "on" : "NOT hooked");
}

/* A wider picture: the fight camera told the picture's shape, so the game builds a
 * wide projection itself and culls, frames and checks the screen with it. The camera object (as
 * cam_dump finds it): +0x164 width, +0x168 height, +0x16c width / height, +0x178 1 / width - the
 * game's 1920, 1080, 16:9. Written at every frame end, before the next frame's camera update. */
int aspect_native(void);
/* A view (wide, zoomed, hybrid) frames the picture: whenever it is wider than 16:9, except with
 * SSBU_DEBUG_REFERENCE_VIEW, the reference view, which tells the game itself the picture's shape. */
static int view_on(void) {
    static int ref = -1;
    if (ref < 0) ref = getenv("SSBU_DEBUG_REFERENCE_VIEW") != NULL;
    return aspect_native() && !ref;
}
/* (the shape as drawn now: it changes between fights, display_aspect_take) */
int aspect_native(void) { return display_aspect() != 16.0f / 9; }   /* (the game draws the 3D wide whenever the picture is wider) */
/* Online play stays 16:9 (owner, 2026-11-11, until the views are checked online): whether a networked match is set
 * up or under way, asked on the game's thread at each frame end (net_match_on reads the lobby's room) and kept for
 * the renderer, which takes 16:9 for it between fights (display_aspect_take). */
static volatile int net_match_seen;
int net_match_on(void);   /* (hle_rb_core.c) */
static volatile int hud_wide_seen;
static int hud_wide_now(void);   /* (below) */
static int hud_fought, hud_over;   /* (hud_wide_now's latch: this scene had its fight, and the fight is over) */
static void net_match_watch(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)args; (void)ctx;
    net_match_seen = net_match_on();
    hud_wide_seen = hud_wide_now();   /* (first: a new scene clears the latch) */
    if (game_fight_on()) hud_fought = 1;
    else if (hud_fought && !line_on()) hud_over = 1;   /* (the KO's finish after GAME! is still the fight: the fighters' line runs) */
}
/* (for the renderer: the HUD's panels reach the picture's edges, so its 2D may too - gpu_d3d11.c's 2D clip) */
int hud_wide_on(void) { return hud_wide_seen; }
int net_match_now(void) { return net_match_seen; }
static void wide_camera(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)args; (void)ctx;
    u64 a = *P(u64, 0x7105337ce8ull);
    if (!a || !(a = *P(u64, a + 0x490))) return;
    u64 cam = *P(u64, a + 0xd8) ? *P(u64, a + 0xe0) : 0x7105334060ull;
    float *f = P(float, cam + 0x164);
    if (f[1] != 1080.0f) return;   /* (not the camera this was measured on) */
    static float written;   /* (the shape last written: at 16:9 nothing, once 16:9's own values are back) */
    float shape = display_aspect(), w = 1080.0f * shape;
    if (shape == 16.0f / 9 && written == 0) return;
    f[0] = w;                                        /* +0x164 */
    f[2] = shape;                                    /* +0x16c */
    written = shape == 16.0f / 9 ? 0 : shape;
}

/* A wider picture: the HUD's world-to-screen code - P1/CPU tags, magnifying glasses - given the
 * render camera's projection as 16:9 has it (+0x290, its inverse at +0x2d0: the x scale times the
 * widening, the inverse's divided), while it runs. The renderer squeezes the HUD into the middle
 * 16:9, so what it places lands on its fighter again; the 3D and its culling keep the wide one.
 * The game's own camera framing is left alone: widening it changed the fight (off-screen damage),
 * where this changes nothing a fight's hash sees.
 * ponytail: the 12 functions that read the projection in main+0x134..-0x143 (the HUD's, by place);
 * which of them place the tags is what this run shows */
static const u64 hud_proj_fns[] = {0x7101341670ull, 0x710134c690ull, 0x710135ccb0ull, 0x71013955d0ull, 0x71013a3f50ull, 0x71013a53d0ull,
                                   0x71013a67c0ull, 0x71013adb10ull, 0x71013c8460ull, 0x71013ca580ull, 0x71013e29d0ull, 0x710142aed0ull};
#define NHUD (sizeof hud_proj_fns / sizeof *hud_proj_fns)
static GuestFn *hud_orig[NHUD];
static volatile LONG hud_depth;
static u64 render_camera(void) {
    u64 a = *P(u64, 0x7105337ce8ull);
    if (!a || !(a = *P(u64, a + 0x490))) return 0;
    return *P(u64, a + 0xd8) ? *P(u64, a + 0xe0) : 0x7105334060ull;
}
volatile long hud_calls[12];   /* (SSBU_DEBUG_CAM_TRACE: calls of each HUD world-to-screen function) */
static void hud_call(int k, Cpu *c) {
    InterlockedIncrement(&hud_calls[k]);
    if (!aspect_native()) { hud_orig[k](c); return; }   /* (16:9: the game's own, untouched) */
    u64 cam = render_camera();
    int wide_here = 0;   /* (the views: the HUD places things where the picture shows them) */
    if (cam && copy_orig && !wide_applied.on && in_fight() && (wide_now[0] != 0 || wide_now[1] != 0 || wide_now[2] != 0 || shadow_k != 1)) {
        float d[3] = {wide_now[0], wide_now[1], wide_now[2]};
        wide_apply(cam + 0x1d0, d);
        wide_here = 1;
    }
    /* The HUD's world-to-screen functions take the view from the camera's own matrix (+0x0), not
     * from the drawing block (+0x1d0) the views move: while one runs, the moved translation is put
     * there too (the rotation is the same; checked, so a different layout is left alone). */
    float view_t[3];
    int view_here = 0;
    if (cam && wide_applied.on && wide_applied.block == cam + 0x1d0 && !memcmp(P(float, cam), P(float, cam + 0x1d0), 48)) {
        memcpy(view_t, P(float, cam) + 12, 12);
        memcpy(P(float, cam) + 12, P(float, cam + 0x1d0) + 12, 12);
        view_here = 1;
    } else if (cam && wide_applied.on && wide_applied.block == cam + 0x1d0) {
        static int told;
        if (!told++) {
            const float *a = P(float, cam), *b = P(float, cam + 0x1d0);
            printf("[aspect] HUD view: the camera's +0x0 is not the block's view, left alone: %g %g %g / %g %g %g\n", a[0], a[1], a[2], b[0], b[1], b[2]);
        }
    }
    float *p = cam ? P(float, cam + 0x290) : NULL, *inv = cam ? P(float, cam + 0x2d0) : NULL;
    /* The widening the projection was built with (its y scale over its x scale is its shape), not the shape asked
     * for now: the renderer takes a new shape between two frames, and in the frame or two after it the camera's
     * projection and display_aspect disagreed - a tag sat off its fighter by their ratio (owner, 2026-11-14:
     * 21:9 -> 32:9 put it at 0.66 of the fighter's distance from the middle, for one capture). Training's
     * orthographic camera (+0x150 = 1) is 16:9 whatever the shape (wide_copy widens only its drawing copy). */
    float wide = display_aspect() / (16.0f / 9);
    if (p && hud_depth == 0 && *P(u32, cam + 0x150) != 1 && p[0] > 1e-6f) {   /* (an inner call sees the outer one's 16:9) */
        float built = p[5] / p[0];
        if (built > 1.7f && built < 3.7f) wide = built / (16.0f / 9);
    }
    int swap = p && *P(float, cam + 0x168) == 1080.0f && InterlockedIncrement(&hud_depth) == 1;   /* (outermost only) */
    float p0 = 0, i0 = 0;
    if (swap) { p0 = p[0]; i0 = inv[0]; p[0] = p0 * wide; inv[0] = i0 / wide; }
    hud_orig[k](c);
    if (view_here) memcpy(P(float, cam) + 12, view_t, 12);
    if (wide_here) wide_restore();
    if (swap) { p[0] = p0; inv[0] = i0; }
    if (p && *P(float, cam + 0x168) == 1080.0f) InterlockedDecrement(&hud_depth);
}
#define HUDW(k) static void hud_w##k(Cpu *c) { hud_call(k, c); }
HUDW(0) HUDW(1) HUDW(2) HUDW(3) HUDW(4) HUDW(5) HUDW(6) HUDW(7) HUDW(8) HUDW(9) HUDW(10) HUDW(11)
static GuestFn *const hud_wrappers[NHUD] = {hud_w0, hud_w1, hud_w2, hud_w3, hud_w4, hud_w5, hud_w6, hud_w7, hud_w8, hud_w9, hud_w10, hud_w11};
/* The HUD's panels where their layouts put them against the screen's edges (owner, 2026-11-12). The
 * game lays its 2D out with nn::ui2d; f_71000589a0 (Pane::CalculateGlobalMatrixSelf, x0 the pane) works
 * out a pane's place from its position (+0x30), its size (+0x50) and scale (+0x48) about its own
 * origin (+0x5b, bits 0-1: 1 left edge, 2 right edge, else the centre), under its parent (+0x18). The
 * fight's layouts mark no anchors (the parent-anchor bits, +0x5b bits 4-5, are 0), so the anchor is read
 * from where the pane is: a pane whose parents up to the layout's root are all the screen (1920 x 1080)
 * and whose right edge is within 120 px of the screen's right edge (the timer, the pause menu,
 * Training's damage panel) is moved right by the picture's extra width on that side, whole and at its
 * own size; the left edge likewise (P1 PAUSE, the camera controls list); a pane reaching both, or
 * neither, stays. The renderer's 2D squeeze maps the wider range back onto the picture. The tags (set_cursor_*,
 * in the 720p layer) are left alone by name: the game places them on what they mark (hud_call). In fights only:
 * menus keep 16:9 (black sides, owner). */
static int screen_like(u64 p) {   /* (the screen's size, 1920 x 1080, or a little more: a frame just past its edges) */
    float w = *P(float, p + 0x50), h = *P(float, p + 0x54);
    return w >= 1919.5f && w <= 2100 && h >= 1079.5f && h <= 1200;
}
static int pane_extent(u64 p, float *l, float *r, int depth);   /* (below) */
/* A plain grouping pane at the screen's origin reaching both of its edges stands for the screen too: what is
 * under it is placed one by one (Training's menu_L holds its settings menu at the left and more at the right). */
static int screen_group(u64 p) {
    float l, r;
    return *P(u64, p) == 0x7104f3f810ull && fabsf(*P(float, p + 0x30)) < 0.5f && fabsf(*P(float, p + 0x48) - 1) < 1e-3f &&
           pane_extent(p, &l, &r, 0) && l <= -960 + 120 && r >= 960 - 120;
}
static int screen_chain(u64 p) {   /* (the pane and every parent up to the layout's root: the screen) */
    for (int depth = 0; p && depth < 16; depth++, p = *P(u64, p + 0x18))
        if (!screen_like(p) && !(*P(u64, p + 0x18) && screen_group(p))) return 0;
    return p == 0;
}
/* Where a pane reaches across, in its parent's units: its own box about its origin (+0x5b bits 0-1: 1 left
 * edge, 2 right edge, else the centre), or, for a plain grouping pane (nn::ui2d::Pane's own vtable, which
 * draws nothing) with panes under it, what its visible ones (+0x58 bit 0) reach. Returns 0 for nothing. */
static int pane_extent(u64 p, float *l, float *r, int depth) {
    float x = *P(float, p + 0x30), s = fabsf(*P(float, p + 0x48));
    if (*P(u64, p) == 0x7104f3f810ull && *P(u64, p + 0x28) != p + 0x20 && depth < 8) {
        float lo = 1e9f, hi = -1e9f;
        int any = 0, n = 0;
        for (u64 node = *P(u64, p + 0x28); node != p + 0x20 && n < 256; node = *P(u64, node + 8), n++) {
            float cl, cr;
            if ((*P(u8, node - 8 + 0x58) & 1) && pane_extent(node - 8, &cl, &cr, depth + 1)) { any = 1; lo = fminf(lo, cl); hi = fmaxf(hi, cr); }
        }
        if (!any) return 0;
        *l = x + s * lo; *r = x + s * hi;
        return 1;
    }
    float w = *P(float, p + 0x50) * s;
    int origin = *P(u8, p + 0x5b) & 3;
    *l = origin == 1 ? x : origin == 2 ? x - w : x - w / 2;
    *r = *l + w;
    return 1;
}
/* A fight's scene, from its loading on (the game's camera manager, main+0x52b8f00, exists only there): the
 * HUD's layouts are placed once when they load, before GO!, and again only when they change. Until the fight
 * is over: the results screens (the winner's pose, the cards) come in the same scene, the camera manager still
 * there, and are a menu - they stay in the middle 16:9 (owner, 2026-11-14: the cards were pushed apart to the
 * picture's sides). Over: the ready-go flag went off after it was on (a pause keeps it on), and the fighters' frame
 * line stopped (the finish after GAME! runs it: cut there, its effect lost the side areas). */
static u64 hud_scene;
static int hud_wide_now(void) {
    u64 m = *P(u64, 0x71052b8f00ull);
    if (m != hud_scene) { hud_scene = m; hud_fought = hud_over = 0; }
    return aspect_native() && m && !hud_over;
}
static GuestFn *pane_place_orig, *pane_calc_orig;
/* When the picture's shape changes inside a fight's scene, every layout is placed again for half a second
 * (Pane::Calculate, f_7100058700, with x3 = 1 at a root: its whole tree recomputed), so a panel that never
 * changes takes the new shape at once. Only then: a fight's loading places everything fresh anyway. */
static volatile u64 hud_again_until;
static void pane_calc(Cpu *c) {
    if (!*P(u64, c->x[0] + 0x18)) {
        static float placed_for;   /* (0: no fight's scene) */
        float now = *P(u64, 0x71052b8f00ull) ? display_aspect() : 0;
        if (now != placed_for) {
            if (now && placed_for) hud_again_until = rt_now_ns() + 500000000ull;
            placed_for = now;
        }
        if (rt_now_ns() < hud_again_until) c->x[3] |= 1;
    }
    pane_calc_orig(c);
}
static void pane_place(Cpu *c) {
    u64 pane = c->x[0], parent = *P(u64, pane + 0x18);
    float *x = NULL, x0 = 0, *sx = NULL, sx0 = 0, g = display_aspect() / (16.0f / 9);
    int on = hud_wide_now();
    const char *name = P(char, pane + 0xb0);
    if (parent && !*P(u64, parent + 0x18) && !strcmp(name, "time") && *P(u64, 0x71052b8f00ull)) {
        /* The fight's timer (display.ini timer, the Display page's "Timer position"; any shape): placed by its own box
         * (256 px, its right edge its origin), which holds a two-digit minute too, so no time is ever cut off. Right is
         * the game's place, at the picture's corner; left keeps the same margin from the picture's left edge; center
         * centres the box; hidden folds it away. */
        int where = display_timer();
        float gw = on ? g : 1, half = 960 * gw, w = *P(float, pane + 0x50) * fabsf(*P(float, pane + 0x48)), l0, r0;
        pane_extent(pane, &l0, &r0, 99);   /* (depth 99: its own box, not its digits) */
        x = P(float, pane + 0x30);
        x0 = *x;
        if (where == TIMER_HIDDEN) { sx = P(float, pane + 0x48); sx0 = *sx; *sx = 0; }
        else if (where == TIMER_CENTER) *x = x0 - (l0 + r0) / 2;
        else if (where == TIMER_LEFT) *x = x0 + (-half + (960 - r0)) - l0;
        else *x = x0 + 960 * (gw - 1);
        (void)w;
        pane_place_orig(c);
        *x = x0;
        if (sx) *sx = sx0;
        return;
    }
    if (*P(u64, 0x71052b8f00ull) && !hud_over && !strcmp(name, "edge_y")) {
        /* The yellow frame of set_parts_edge (1990 x 1150), its border just past the screen's edges, where the
         * console never shows it: hidden in a fight's scene (its drawing keeps its 16:9 shape whatever its size or
         * scale says, so on a wider picture its border would stand at the 16:9 edges). At every shape, 16:9 too,
         * where it is off the screen anyway: hidden only while wider, the frames the game had placed before a
         * switch from 16:9 showed it for a moment (owner, 2026-11-14) */
        sx = P(float, pane + 0x48);
        sx0 = *sx;
        *sx = 0;
    }
    /* (the damage meters: static, owner. info_melee's p1..p4 for up to 4 fighters, p1_2 p2_2 p3_2 p4_2 p5..p8 for 5 to 8 - by the side
     * rules only the row's ends moved, P1 to the picture's edge (owner, 2026-11-15, 8 fighters); and the online waiting room's
     * row of player slots, info_waiting's trans_players: player_00 and _03 went to the picture's edges, the middle two stayed) */
    int meter = (name[0] == 'p' && name[1] >= '1' && name[1] <= '8' && (!name[2] || !strcmp(name + 2, "_2"))) ||
                (parent && !strcmp(P(char, parent + 0xb0), "trans_players"));
    /* (the tags - P1/CPU set_cursor_p1.., items' set_cursor_00.. - and their layer, 720p: placed by the game's world-to-screen
     * (hud_call), they follow what they mark. Moved by the side rules they jumped 330 px near the 16:9 edges, every tag of the
     * layer at once when one of them made the layer reach an edge: owner, 2026-11-14, all three views) */
    int tag = !strncmp(name, "set_cursor_", 11) || !strcmp(name, "720p");
    /* (the triple arrows, set_parts_arrow_l_00.. (and the right side's, whatever their name: set_parts_arrow_*) under the HUD's
     * root: three arrows the game puts a fixed way inside the 16:9 edge, on the off-screen fighter's side, x -752 or so. Dynamic
     * (owner, 2026-11-14): as far from the picture's edge as they were from the 16:9 one, in every view - they only say "a
     * fighter is off this side", where the bubble marks a place (and is custom: by view, gpu_aspect.c)) */
    int arrows = !strncmp(name, "set_parts_arrow_", 16) && parent && !*P(u64, parent + 0x18);   /* (not the menus' set_parts_arrow_l/_r, under anim_POP_00) */
    if (arrows && on && g > 1.001f) {
        x = P(float, pane + 0x30);
        x0 = *x;
        if (fabsf(x0) > 960 * 0.5f) *x = x0 + (x0 < 0 ? -1 : 1) * 960 * (g - 1);
    }
    /* (TIME!, the timeup layout: its two copies, timeup_anim_base_00 from the right and timeup_anim_add_00 from the left,
     * slide in from x +-1500 and collide at 0 into one word. The side rules took each, its blur lines trailing on its
     * side, for a panel against that edge and stopped it 960 x (widening - 1) short: two words (owner, 2026-11-14,
     * every view). Their x is widened instead: they start past the picture's edges and still meet in the middle) */
    int timeup = parent && !strcmp(P(char, parent + 0xb0), "timeup");
    /* (the vs screen before a fight, system/matchup: each fighter's background, set_panel_bg_00.. (960 x 1080 at x +-480
     * with two fighters), stays in the 16:9 view, the side areas open. Moved by the side rules they left a gap in the
     * middle; widened, they stretched (owner, 2026-11-15: open ends until there is a better idea) */
    int vs_bg = parent && !strncmp(name, "set_panel_bg_", 13);
    if (timeup && on && g > 1.001f) {
        x = P(float, pane + 0x30);
        x0 = *x;
        *x = x0 * g;
    }
    if (parent && on && g > 1.001f && !meter && !tag && !arrows && !timeup && !vs_bg && screen_chain(parent) && strncmp(name, "set_parts_message_", 18) && !screen_group(pane)) {
        x = P(float, pane + 0x30);
        x0 = *x;
        if (screen_like(pane)) {
            *x = x0 * g;                              /* (a screen: parked one screen away stays one picture away) */
            /* (the popup's dimming, system/popup's set_bg_00 (2000 x 1160) under anim_BG_00, is stretched across the picture:
             * it dimmed only the 16:9 middle behind "End the game?" (owner, 2026-11-15). By name: every screen-sized fill
             * stretched the results wipe's still of the fight's last frame too, which already fills the picture (it reads the
             * wide picture, so the renderer does not squeeze it) - the damage meters spread out for a few frames after GAME!) */
            if (!sx && !strcmp(name, "set_bg_00") && !strcmp(P(char, parent + 0xb0), "anim_BG_00")) {
                sx = P(float, pane + 0x48);
                sx0 = *sx;
                *sx = sx0 * g;
            }
        } else {
            float l = 0, r = 0;
            if (!pane_extent(pane, &l, &r, 0)) l = r = 0;   /* (nothing visible under it: left where it is) */
            int right = r >= 960 - 120, left = l <= -960 + 120;
            if (l >= 960 || r <= -960) *x = x0 * g;                        /* (parked off the screen: off the picture) */
            else if (right != left) *x = x0 + (right ? 1 : -1) * 960 * (g - 1);   /* (against one edge: against the picture's) */
        }
    }
    static int moved_log = -1;   /* SSBU_DEBUG_PANES_MOVED=1 (HUD research): each pane name the side, arrow or TIME! rules move, once, with its parents */
    if (moved_log < 0) moved_log = getenv("SSBU_DEBUG_PANES_MOVED") != NULL;
    if (moved_log && x && *x != x0) {
        static char seen[4096];
        char key[40];
        snprintf(key, sizeof key, "|%.30s|", name);
        if (!strstr(seen, key) && strlen(seen) + strlen(key) < sizeof seen) {
            strcat(seen, key);
            char chain[200];
            int n = 0;
            for (u64 q = parent; q && n < (int)sizeof chain - 40; q = *P(u64, q + 0x18))
                n += snprintf(chain + n, sizeof chain - n, " < %.24s", P(char, q + 0xb0));
            printf("[panes] moved %-24.24s x %g -> %g (vt %#llx)%s\n", name, x0, *x, (unsigned long long)*P(u64, pane), chain);
            fflush(stdout);   /* (a test run is stopped, not ended: an unflushed line is lost) */
        }
    }
    if (getenv("SSBU_DEBUG_PANES")) {   /* SSBU_DEBUG_PANES=<text> (HUD research): the panes whose name, or a parent's, holds it */
        const char *want = getenv("SSBU_DEBUG_PANES");
        u64 q = pane; int under = 0;
        for (int d = 0; q && d < 10; d++, q = *P(u64, q + 0x18)) if (strstr(P(char, q + 0xb0), want)) under = 1;
        static u64 t, burst;
        if (under && rt_now_ns() - t > 3000000000ull) { t = rt_now_ns(); burst = t; }
        if (under && rt_now_ns() - burst < 50000000ull)
            printf("[panes] %-22.22s < %-18.18s vt %#llx vis %d a %u origin %u pos %g %g size %g %g scale %g\n", name, P(char, parent + 0xb0), (unsigned long long)*P(u64, pane),
                   *P(u8, pane + 0x58) & 1, *P(u8, pane + 0x59), *P(u8, pane + 0x5b) & 3, *P(float, pane + 0x30), *P(float, pane + 0x34), *P(float, pane + 0x50), *P(float, pane + 0x54), *P(float, pane + 0x48));
    }
    pane_place_orig(c);
    if (x) *x = x0;
    if (sx) *sx = sx0;
}
static void hook_hud_anchors(void) {
    u64 orig = rt_hook_function(0x71000589a0ull, rt_native(pane_place, "ultrawide HUD anchors"));
    if (orig) pane_place_orig = rt_lookup(orig);
    u64 calc = rt_hook_function(0x7100058700ull, rt_native(pane_calc, "ultrawide HUD placed again"));
    if (calc) pane_calc_orig = rt_lookup(calc);
    if (!calc) orig = 0;
    printf("[aspect] the HUD's panels at the picture's edges: %s (main+0x589a0)\n", orig ? "on" : "NOT hooked");
}
static void hook_hud_projection(void) {
    int n = 0;
    for (u32 k = 0; k < NHUD; k++) {
        u64 orig = rt_hook_function(hud_proj_fns[k], rt_native(hud_wrappers[k], "ultrawide HUD projection"));
        if (orig) { hud_orig[k] = rt_lookup(orig); n++; }
    }
    printf("[aspect] the HUD's world-to-screen given 16:9: %d of %d functions\n", n, (int)NHUD);
}

#else
int net_match_now(void) { return 0; }   /* (macOS: held at 16:9) */
#endif   /* (ultrawide) */

#ifdef _WIN32
static LONG CALLBACK hunt_fault(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && hunt_rearm) {
        DWORD o;
        for (int i = 0; i < nhunt; i++)
            if (hunt[i].page == hunt_rearm && hunt[i].armed && !VirtualProtect(hunt_rearm, 4096, PAGE_READONLY, &o)) hunt[i].armed = 0;
        hunt_rearm = NULL;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    u8 *a = (u8 *)e->ExceptionRecord->ExceptionInformation[1];
    for (int i = 0; i < nhunt; i++) {
        if (!hunt[i].armed || a < hunt[i].page || a >= hunt[i].page + 4096) continue;
        DWORD o;
        VirtualProtect(hunt[i].page, 4096, hunt[i].old, &o);
        u8 *m = (u8 *)G2H(hunt[i].at);
        if (a >= m && a < m + 96 && hunt_logged < 400) {
            void *frames[16];
            int n = unwind(*e->ContextRecord, frames, 16);
            u64 key = 0;
            char line[1024];
            int l = snprintf(line, sizeof line, "[hunt] write to %#llx +%#x:", (unsigned long long)hunt[i].at, (unsigned)(a - m));
            for (int k = 0, shown = 0; k < n && shown < 8 && l < (int)sizeof line - 64; k++) {
                char d[64];
                u64 gfn = guest_of_frame((u64)frames[k], k == 0);
                if (!gfn) continue;
                key = key * 1099511628211ull ^ gfn;
                l += snprintf(line + l, sizeof line - l, " %s", rt_describe(gfn, d, sizeof d));
                shown++;
            }
            int seen = 0;
            for (int s = 0; s < nseen; s++) seen |= hunt_seen[s] == key;
            if (!seen && nseen < 64) {
                hunt_seen[nseen++] = key;
                InterlockedIncrement(&hunt_logged);
                printf("%s\n", line);
                fflush(stdout);
            }
        }
        if (rt_now_ns() > hunt_until) { hunt[i].armed = 0; return EXCEPTION_CONTINUE_EXECUTION; }
        hunt_rearm = hunt[i].page;
        e->ContextRecord->EFlags |= 0x100;   /* (one instruction, then the page closes again) */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void rt_backtrace_thread(HANDLE thread, int max_frames) {
    void *frames[96];
    char name[96];
    CONTEXT ctx = {0};
    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(thread) == (DWORD)-1) return;
    int n = GetThreadContext(thread, &ctx) ? unwind(ctx, frames, 96) : 0;
    ResumeThread(thread);
    static int host_frames = -1;
    if (host_frames < 0) host_frames = getenv("SSBU_HOST_FRAMES") != NULL;
    u64 image = (u64)GetModuleHandleW(NULL);
    for (int i = 0, shown = 0; i < n && shown < max_frames; i++) {
        u64 guest = guest_of_frame((u64)frames[i], i == 0);   /* (frames[0]: its context's pc) */
        if (!guest) {
            if (host_frames && (u64)frames[i] - image < 0x40000000ull) fprintf(stderr, "      [host +%#llx]\n", (u64)frames[i] - image);
            else if (host_frames) {
                HMODULE mod = NULL;
                char path[MAX_PATH] = "?";
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)frames[i], &mod);
                if (mod) GetModuleFileNameA(mod, path, sizeof path);
                const char *base = strrchr(path, '\\');
                fprintf(stderr, "      [%s +%#llx]\n", base ? base + 1 : path, (u64)frames[i] - (u64)mod);
            }
            continue;
        }
        fprintf(stderr, "      %s\n", rt_describe(guest, name, sizeof name));
        shown++;
    }
}

/* A host fault inside recompiled code is a guest fault: report where, and what it touched.
 * The report is written by a thread of its own, the faulting one only waits: its stack may be
 * what ran out - and after a stack overflow, too little of it is left for writing a report (on
 * the graphics driver's threads, whose stacks are 64 KB, there is no room to keep for one). */
static struct { HANDLE go; EXCEPTION_POINTERS *e; DWORD tid; } fault;

int kernel_fault_report(void (*line)(const char *), u64 g);    /* (kernel.c) */
void kernel_thread_states(void (*line)(const char *text));     /* (kernel.c) */
int profile_dump_threads(void (*line)(const char *text));      /* (debug_profile.c) */
static _Noreturn void crash_report(EXCEPTION_POINTERS *e, DWORD tid) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    char name[96], report[MAX_PATH];
    fflush(stdout);
    report_open("crash", report, sizeof report);
    out("\n*** host exception %#lx%s during %s", code, code == EXCEPTION_STACK_OVERFLOW ? " (stack overflow)" : "", phase);
    if (code == EXCEPTION_ACCESS_VIOLATION)
        out(": %s address %s", e->ExceptionRecord->ExceptionInformation[0] == 8 ? "executing" : e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            rt_describe(e->ExceptionRecord->ExceptionInformation[1], name, sizeof name));
    out("\n");
    if (rt_romfs_warning[0]) out("%s\n", rt_romfs_warning);   /* (hle_fs.c: wrong game data can be the cause) */
    /* (an address in guest memory that is not mapped: what the kernel's map says, and its recent changes there - the
     * r9n_1 crash, an alias address read on the character select screen while not committed) */
    u64 fault_addr = code == EXCEPTION_ACCESS_VIOLATION ? (u64)e->ExceptionRecord->ExceptionInformation[1] : 0;
    if (fault_addr) {
        out("memory at the address:\n");
        if (!kernel_fault_report(out_line, H2G((void *)(uintptr_t)fault_addr))) out("  not a guest address\n");
    }
    /* where on the host: the faulting instruction's module and offset (symbolize with tools/dev/hostsym.py) */
    HMODULE mod = NULL;
    char modname[MAX_PATH] = "?";
    u64 pc = (u64)e->ExceptionRecord->ExceptionAddress;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &mod))
        GetModuleFileNameA(mod, modname, sizeof modname);
    WCHAR *desc = NULL;   /* (which thread: unnamed ones are the system's or the driver's) */
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (thread && FAILED(GetThreadDescription(thread, &desc))) desc = NULL;
    out("host pc %#llx: %s +%#llx (thread %lu%s%ls)\n", pc, strrchr(modname, '\\') ? strrchr(modname, '\\') + 1 : modname,
        pc - (u64)mod, tid, desc && desc[0] ? ", " : "", desc && desc[0] ? desc : L"");
    if (desc) LocalFree(desc);
    if (thread) CloseHandle(thread);
    {   /* (r22_1: the registers, and the stack's words either side of the stack pointer: a return through a wrong stack pointer shows there) */
        const CONTEXT *x = e->ContextRecord;
        out("  rax %#llx rcx %#llx rdx %#llx rbx %#llx rsp %#llx rbp %#llx rsi %#llx rdi %#llx\n  r8 %#llx r9 %#llx r10 %#llx r11 %#llx r12 %#llx r13 %#llx r14 %#llx r15 %#llx\n",
            x->Rax, x->Rcx, x->Rdx, x->Rbx, x->Rsp, x->Rbp, x->Rsi, x->Rdi, x->R8, x->R9, x->R10, x->R11, x->R12, x->R13, x->R14, x->R15);
        MEMORY_BASIC_INFORMATION mi;
        u64 lo = x->Rsp - 0xC0, hi = x->Rsp + 0x80;
        if (VirtualQuery((void *)(uintptr_t)lo, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            && (u64)(uintptr_t)mi.BaseAddress + mi.RegionSize >= hi)
            for (u64 a = lo; a < hi; a += 32)
                out("  stack %+5lld: %#llx %#llx %#llx %#llx\n", (long long)(a - x->Rsp), *(u64 *)(uintptr_t)a, *(u64 *)(uintptr_t)(a + 8), *(u64 *)(uintptr_t)(a + 16), *(u64 *)(uintptr_t)(a + 24));
    }
    {   /* and the host frames above it */
        CONTEXT ctx = *e->ContextRecord;
        for (int i = 0; i < 24; i++) {
            DWORD64 image;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
            if (!f) break;
            void *handler;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &handler, &establisher, NULL);
            if (!ctx.Rip) break;
            mod = NULL;
            strcpy(modname, "?");
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ctx.Rip, &mod))
                GetModuleFileNameA(mod, modname, sizeof modname);
            out("  host frame %s +%#llx\n", strrchr(modname, '\\') ? strrchr(modname, '\\') + 1 : modname, ctx.Rip - (u64)mod);
        }
    }
    gfx_report_compiles(out_line);   /* (a fault in the driver's shader compiler names the program) */
    void *frames[62];
    print_frames(frames, unwind(*e->ContextRecord, frames, 62), 1);   /* from the faulting frame */
    if (fault_addr) {   /* (the other threads too: who else was at work on that memory) */
        out("\nGuest threads:\n");
        kernel_thread_states(out_line);
        out("\nEvery other thread's stack (innermost first):\n");
        profile_dump_threads(out_line);
    }
    char dump[MAX_PATH];
    snprintf(dump, sizeof dump, "%.*s.dmp", (int)strlen(report) - 4, report);
    int dumped = report_file != INVALID_HANDLE_VALUE && rt_write_dump(dump, tid, e, 100000);
    if (report_file != INVALID_HANDLE_VALUE) out("report: %s%s%s\n", report, dumped ? "; minidump: " : "", dumped ? dump : "");
    if (report_file != INVALID_HANDLE_VALUE) CloseHandle(report_file);
    fflush(stderr);
    ExitProcess(5);
}

static DWORD WINAPI crash_reporter(void *unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"crash reporter");
    WaitForSingleObject(fault.go, INFINITE);
    crash_report(fault.e, fault.tid);
}

static void crash_reporter_start(void) {
    fault.go = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE t = fault.go ? CreateThread(NULL, 0, crash_reporter, NULL, 0, NULL) : NULL;
    if (t) CloseHandle(t);
    else fault.go = NULL;
}

static LONG WINAPI on_fault(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (rt_probing) return EXCEPTION_CONTINUE_SEARCH;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_IN_PAGE_ERROR)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG faulting;
    if (InterlockedExchange(&faulting, 1)) {   /* another thread is reporting its fault: let it finish */
        Sleep(90000);
        TerminateProcess(GetCurrentProcess(), 6);
    }
    fault.e = e;
    fault.tid = GetCurrentThreadId();
    if (fault.go && SetEvent(fault.go)) for (;;) Sleep(INFINITE);   /* (the reporter ends the process) */
    crash_report(e, fault.tid);
}
#else
/* ---- Linux and macOS: faults come through signals; other threads' stacks through a signal
 * (Linux) or by stopping the thread (macOS) ------------------------------------------------ */
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#ifndef __APPLE__
#include <semaphore.h>
#include <sys/syscall.h>
#include <ucontext.h>
#endif

int rt_ww_fault(void *address);        /* (linux/win32.c; macos/memory.c) a write the protecting write watch takes */
void rt_thread_signal_stack(void);

static void host_frame(const char *indent, void *pc) {   /* "[module +offset]" */
    Dl_info di;
    if (dladdr(pc, &di) && di.dli_fname) {
        const char *base = strrchr(di.dli_fname, '/');
        out("%s[%s +%#llx]%s%s\n", indent, base ? base + 1 : di.dli_fname, (u64)pc - (u64)di.dli_fbase,
            di.dli_sname ? " " : "", di.dli_sname ? di.dli_sname : "");
    } else out("%s[%#llx]\n", indent, (u64)pc);
}

#ifdef __APPLE__
/* A frame walk's second frame is the link register when the innermost function had not saved it:
 * not a frame of its own when it points back into that same (recompiled) function. (Allocates
 * and locks nothing: also for a stopped thread. lr is a return address, looked up as its call: as
 * it is, a caller that ended in its call to the innermost function, placed right after it, would
 * be taken for that function and dropped.) */
static int walk_frames(u64 pc, u64 lr, u64 fp, void **frames, int max) {
    int n = rt_frame_walk(pc, lr, fp, frames, max);
    if (n > 1 && (u64)frames[1] == lr && guest_of(pc) && guest_of_frame(lr, 0) == guest_of(pc)) {
        memmove(frames + 1, frames + 2, sizeof *frames * (size_t)(n - 2));
        n--;
    }
    return n;
}

/* Another thread's frames: it is stopped, its registers read and its frame pointers followed,
 * then it goes on (rt_frame_walk: no signal, nothing taken that it might hold). */
void rt_backtrace_thread(HANDLE thread, int max_frames) {
    RtTid tid = rt_thread_tid(thread);
    if (!tid) return;
    void *frames[96];
    int n, self = tid == pthread_mach_thread_np(pthread_self());   /* (another thread's walk starts at its pc; this one's are all return addresses) */
    u64 pc = 0, lr = 0;
    if (self) n = CaptureStackBackTrace(0, 96, frames, NULL);
    else if ((n = rt_thread_frames(tid, frames, 96, &pc, &lr)) > 1 && (u64)frames[1] == lr && guest_of(pc) && guest_of_frame(lr, 0) == guest_of(pc)) {
        memmove(frames + 1, frames + 2, sizeof *frames * (size_t)(n - 2));   /* (as walk_frames) */
        n--;
    }
    static int host_frames = -1;
    if (host_frames < 0) host_frames = getenv("SSBU_HOST_FRAMES") != NULL;
    char name[96];
    for (int i = 0, shown = 0; i < n && shown < max_frames; i++) {
        u64 guest = guest_of_frame((u64)frames[i], i == 0 && !self);
        if (!guest) { if (host_frames) host_frame("      ", frames[i]); continue; }
        fprintf(stderr, "      %s\n", rt_describe(guest, name, sizeof name));
        shown++;
    }
}
#else
/* Another thread's frames: it is interrupted and records its own stack. */
#define SIG_BACKTRACE (SIGRTMIN + 2)
static void *bt_frames[96];
static volatile int bt_count;
static sem_t bt_done;

static void on_backtrace_signal(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    int saved = errno;
    bt_count = backtrace(bt_frames, 96);
    sem_post(&bt_done);
    errno = saved;
}

void rt_backtrace_thread(HANDLE thread, int max_frames) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pid_t tid = rt_thread_tid(thread);
    if (!tid) return;
    pthread_mutex_lock(&lock);
    while (sem_trywait(&bt_done) == 0) {}   /* (an answer that came after its wait gave up) */
    bt_count = 0;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 250000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    if (syscall(SYS_tgkill, getpid(), tid, SIG_BACKTRACE) == 0 && sem_timedwait(&bt_done, &until) == 0) {
        static int host_frames = -1;
        if (host_frames < 0) host_frames = getenv("SSBU_HOST_FRAMES") != NULL;
        char name[96];
        for (int i = 2, shown = 0; i < bt_count && shown < max_frames; i++) {   /* (past the handler and the signal frame) */
            u64 guest = guest_of_frame((u64)bt_frames[i], i == 2);   /* (bt_frames[2]: where the signal stopped it) */
            if (!guest) { if (host_frames) host_frame("      ", bt_frames[i]); continue; }
            fprintf(stderr, "      %s\n", rt_describe(guest, name, sizeof name));
            shown++;
        }
    }
    pthread_mutex_unlock(&lock);
}
#endif

/* A host fault inside recompiled code is a guest fault: report where, and what it touched.
 * (Writes to pages the protecting write watch guards are not faults: they are noted and go on.)
 * macOS: a protection fault is SIGBUS, and whether it was a write is the exception syndrome's
 * (ESR: a data abort, with WnR); the address it touched is the host's, the guest's RT_MEMBASE
 * below it. Nothing here touches a thread-local: Mach-O's first use of one on a thread
 * allocates. */
static void on_fault(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
#ifdef __APPLE__
    u64 esr = uc->uc_mcontext->__es.__esr, addr = uc->uc_mcontext->__es.__far, pc = uc->uc_mcontext->__ss.__pc;
    int writing = ((esr >> 26) == 0x24 || (esr >> 26) == 0x25) && (esr >> 6 & 1);
    if ((sig == SIGSEGV || sig == SIGBUS) && writing && rt_ww_fault((void *)(uintptr_t)addr)) return;
    static volatile RtTid reporter;
    RtTid me = pthread_mach_thread_np(pthread_self()), none = 0;
    (void)si;
#else
    int writing = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    if (sig == SIGSEGV && writing && rt_ww_fault(si->si_addr)) return;
    u64 addr = (u64)si->si_addr, pc = (u64)uc->uc_mcontext.gregs[REG_RIP];
    static volatile pid_t reporter;
    pid_t me = gettid(), none = 0;
#endif
    if (reporter == me) _exit(6);   /* (a fault while reporting one) */
    if (!__atomic_compare_exchange_n(&reporter, &none, me, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) for (;;) pause();   /* (one report at a time) */
    char name[96], report[MAX_PATH];
    fflush(stdout);
    report_open("crash", report, sizeof report);
    /* a fault right at the stack pointer: the thread's stack ran out (this runs on its signal stack;
     * on macOS a stack's guard page faults as a protection fault, SIGBUS) */
#ifdef __APPLE__
    u64 sp = uc->uc_mcontext->__ss.__sp;
    int overflow = (sig == SIGSEGV || sig == SIGBUS) && addr + 65536 > sp && addr < sp + 65536;
#else
    u64 sp = (u64)uc->uc_mcontext.gregs[REG_RSP];
    int overflow = sig == SIGSEGV && addr + 65536 > sp && addr < sp + 65536;
#endif
    out("\n*** host exception (signal %d: %s%s) during %s", sig, sig == SIGSEGV ? "access violation" : sig == SIGBUS ? "bus error" :
        sig == SIGILL ? "illegal instruction" : sig == SIGFPE ? "arithmetic" : sig == SIGTRAP ? "trap" : sig == SIGABRT ? "abort" : "?",
        overflow ? ", stack overflow" : "", phase);
#ifdef __APPLE__
    if (sig == SIGSEGV || sig == SIGBUS) {
        if (addr - RT_MEMBASE < ASPACE_BASE + ASPACE_SIZE) out(": %s address %s", writing ? "writing" : "reading", rt_describe(H2G(addr), name, sizeof name));
        else out(": %s host address %#llx (outside the guest's)", writing ? "writing" : "reading", addr);
    }
#else
    if (sig == SIGSEGV || sig == SIGBUS) out(": %s address %s", writing ? "writing" : "reading", rt_describe(addr, name, sizeof name));
#endif
    out("\n");
    if (rt_romfs_warning[0]) out("%s\n", rt_romfs_warning);   /* (hle_fs.c: wrong game data can be the cause) */
    out("host pc %#llx (thread %d %s):\n", pc, (int)me, rt_thread_description(me));
    host_frame("  ", (void *)pc);
    void *frames[64];
#ifdef __APPLE__   /* (from the faulting context: a leaf function's caller is in the link register) */
    int n = walk_frames(pc, uc->uc_mcontext->__ss.__lr, uc->uc_mcontext->__ss.__fp, frames, 64), from = 0, from_pc = 1;
#else
    int n = backtrace(frames, 64), from = 0, from_pc = 0;
    for (int i = 0; i < n; i++) if ((u64)frames[i] == pc) { from = i; from_pc = 1; break; }   /* (the faulting frame onwards) */
#endif
    for (int i = from + 1; i < n && i < from + 24; i++) host_frame("  host frame ", frames[i]);
    gfx_report_compiles(out_line);   /* (a fault in the shader compiler names the program) */
    print_frames(frames + from, n - from, from_pc);
    report_close(report);
    fflush(stderr);
    _exit(5);
}

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
#ifdef __APPLE__   /* (arm64's traps - brk: __builtin_trap, the C++ library's checks - and abort: Metal's validation, a C++ terminate) */
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
#else
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_backtrace_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIG_BACKTRACE, &sa, NULL);
#endif
    signal(SIGPIPE, SIG_IGN);
#ifndef __APPLE__
    sem_init(&bt_done, 0, 0);
#endif
    rt_thread_signal_stack();
    void *warm[4];
    backtrace(warm, 4);   /* (its first call loads the unwinder: not inside a signal handler) */
}
#endif

_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list ap;
    char text[768], report[MAX_PATH];
    fflush(stdout);
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    report_open("stopped", report, sizeof report);
    out("\n*** stopped during %s: %s\n", phase, text);
    if (rt_romfs_warning[0]) out("%s\n", rt_romfs_warning);
    rt_backtrace();
    report_close(report);
    fflush(stderr);
    ExitProcess(3);
}

_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) {
    char name[96];
    (void)c;
    rt_fatal("guest trap at %s: %s", rt_describe(pc, name, sizeof name), why);
}

/* The original of a wrapped guest function (HLE_WRAP) no module of this build has (a made-up
 * game's, tools/mac/boottest; the game's own always has them, tools/recomp.py resolve_wraps):
 * tools/recomp.py links each f_<address>_orig no module defines to this one. Nothing calls it,
 * since nothing reaches that wrapper either. */
void rt_wrap_missing(Cpu *c) { rt_trap(c, 0, "wrapped guest function missing from this build's modules"); }

/* ---- dispatch -------------------------------------------------------------------- */
/* The tables are 2 bytes per byte of code (hundreds of MB), so a lookup in them mostly misses
 * the processor's caches; a small cache of recent ones goes first. Modules are never unloaded,
 * so an entry never goes stale. Its key is the address mixed with the function, so an entry
 * another thread is halfway through writing never matches. */
#define LOOKUP_CACHE 8192
static struct { volatile u64 key; GuestFn *volatile fn; } lookup_cache[LOOKUP_CACHE];
#define LOOKUP_MIX(fn) ((u64)(fn) * 0x9E3779B97F4A7C15ull)

u64 rt_moved_in_13_0_5(u64 at);   /* (hle_skyline.c) a 13.0.4 main address that moved in 13.0.5 */
GuestFn *rt_lookup(u64 addr) {
    char name[96];
    u32 h = (u32)((addr >> 2) * 0xD6E8FEB86659FD93ull >> 51) & (LOOKUP_CACHE - 1);
    GuestFn *hit = lookup_cache[h].fn;
    if ((lookup_cache[h].key ^ LOOKUP_MIX(hit)) == addr && hit) return hit;
    u64 slot = (addr - SLOT_BASE) >> SLOT_SHIFT;
    u16 slot_m = slot < SLOT_COUNT ? __atomic_load_n(&slot_module[slot], __ATOMIC_ACQUIRE) : 0;   /* (map_module publishes it last) */
    if (slot_m & CLONE_FLAG) {
        const struct Clone *k = &clones[(slot_m & ~CLONE_FLAG) - 1u];
        u64 off = addr - k->base;
        /* (an odd offset is a function that exists only in a fighter library's source: build_fighter.py) */
        GuestFn *fn = off >= k->text_size ? NULL : off & 3 ? ((off & 3) == 1 ? k->odd[off >> 2] : NULL) : k->code[off >> 2];
        if (fn) {
            lookup_cache[h].fn = fn;
            lookup_cache[h].key = addr ^ LOOKUP_MIX(fn);
            return fn;
        }
        rt_fatal("indirect call to %s+%#llx, which is not a function entry", k->name, off);
    }
    if (slot_m) {
        u32 m = slot_m - 1u;
        u64 off = addr - modules[m]->base;
        if (off < modules[m]->text_size) {
            GuestFn *fn = off & 3 ? NULL : code[m][off >> 2];
            /* A moveset built for 13.0.4 calls a main address that moved in 13.0.5. The cache key
             * stays the address it asked for. */
            if (!fn && !strcmp(modules[m]->name, "main")) {
                u64 slid = rt_moved_in_13_0_5(addr);
                if (slid != addr) {
                    off = slid - modules[m]->base;
                    fn = (off & 3) || off >= modules[m]->text_size ? NULL : code[m][off >> 2];
                }
            }
            if (fn) {
                lookup_cache[h].fn = fn;
                lookup_cache[h].key = addr ^ LOOKUP_MIX(fn);
                return fn;
            }
            rt_fatal("indirect call to %s, which is not a function entry", rt_describe(addr, name, sizeof name));
        }
    }
    u64 off = addr - NATIVE_BASE;
    if (off < (u64)__atomic_load_n(&native_count, __ATOMIC_ACQUIRE) * 16 && !(off & 15)) {
        last_native = (u32)(off >> 4);
        return natives[off >> 4].fn;
    }
    rt_fatal("indirect call to %#llx, which is outside every module", addr);
}

static void unresolved(Cpu *c);

GuestFn *rt_lookup_cached(u64 addr, RtCache *cache) {
    GuestFn *fn = rt_lookup(addr);
    if (fn != unresolved && addr) {   /* (that one finds its symbol from the lookup itself) */
        cache->fn = fn;
        cache->key = addr ^ RT_CACHE_MIX(fn);
    }
    return fn;
}

void rt_call(Cpu *c, u64 guest_addr) {
    c->x[30] = 0;
    rt_lookup(guest_addr)(c);
}

u64 rt_symbol(const char *name);

u64 rt_callsym(Cpu *c, const char *symbol, int n, ...) {
    u64 fn = rt_symbol(symbol);
    if (!fn) rt_fatal("native code called %s, which no module exports", symbol);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) c->x[i] = va_arg(ap, u64);
    va_end(ap);
    rt_call(c, fn);
    return c->x[0];
}

/* ---- the clock: cheap to ask, also where Windows' timer is slow (clock.h) ------------ */
u64 rt_clock_ns(void) { return clock_now_ns(); }

u64 rt_now_ns(void) {
    u64 t = clock_now_ns();   /* (sets up the clock, and clock_start_ns, the first time) */
    return t > clock_start_ns ? t - clock_start_ns : 0;
}

/* The second half of the native region holds small constant data handed to the guest. */
u64 rt_guest_copy(const void *data, size_t size) {
    static volatile LONG64 used;
    u64 at = NATIVE_BASE + 0x80000 + (u64)InterlockedExchangeAdd64(&used, (LONG64)((size + 15) & ~15ull));
    if (at + size > NATIVE_BASE + 0x100000) rt_fatal("native data region exhausted");
    memcpy(P(void, at), data, size);
    return at;
}

/* (one at a time; the count goes up only once the entry is written, for rt_lookup on other threads) */
u64 rt_native(GuestFn *fn, const char *name) {
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    u32 i = native_count;
    if (i == sizeof natives / sizeof *natives) { ReleaseSRWLockExclusive(&lock); rt_fatal("too many native functions"); }
    natives[i] = (struct Native){fn, name};
    __atomic_store_n(&native_count, i + 1, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&lock);
    return NATIVE_BASE + 16ull * i;
}

/* ---- symbols --------------------------------------------------------------------- */
/* missing: nothing defines the name yet (a module loaded later may). */
static struct Sym { const char *name; u64 addr; u8 missing; } *symtab;
static u32 symtab_size, unresolved_count;
static struct Sym *sym_slot(const char *name);

/* Relocations that refer to a missing symbol, patched when a loaded module defines it. */
static struct Site { struct Sym *sym; u64 at; i64 addend; } *sites;
static u32 site_count, site_cap;

/* Bound to symbols nothing defines: binds lazily, as the console's linker does for the
 * game's references into modules it loads later, or reports the missing symbol. */
static void unresolved(Cpu *c) {
    const char *name = natives[last_native].name;
    struct Sym *s = sym_slot(name);
    if (s->name && !s->missing) { rt_lookup(s->addr)(c); return; }
    rt_fatal("call to %s, which no module defines and has no native implementation", name);
}

static u32 hash(const char *s) {
    u32 h = 2166136261u;
    while (*s) h = (h ^ (u8)*s++) * 16777619u;
    return h;
}

static struct Sym *sym_slot(const char *name) {
    u32 i = hash(name) & (symtab_size - 1);
    while (symtab[i].name && strcmp(symtab[i].name, name)) i = (i + 1) & (symtab_size - 1);
    return &symtab[i];
}

u64 rt_symbol(const char *name) { return sym_slot(name)->addr; }

const char *rt_symbol_at(u64 addr, const char *prefix) {   /* (every export looked at: for start-up checks) */
    size_t n = strlen(prefix);
    for (u32 m = 0; m < module_count; m++)
        for (u32 i = 0; i < modules[m]->export_count; i++)
            if (modules[m]->exports[i].addr == addr && !strncmp(modules[m]->exports[i].name, prefix, n)) return modules[m]->exports[i].name;
    return NULL;
}

/* Functions the loader wraps so that progress through start-up is visible. */
static u64 real_startup, real_main;
static void wrap_startup(Cpu *c) { phase = "nninitStartup"; printf("[boot] nninitStartup\n"); fflush(stdout); rt_call(c, real_startup); phase = "start-up after nninitStartup"; }
static void wrap_main(Cpu *c) { phase = "nnMain"; printf("[boot] nnMain entered\n"); fflush(stdout); rt_call(c, real_main); printf("[boot] nnMain returned\n"); }

static u64 resolve(const char *name, int weak) {
    struct Sym *s = sym_slot(name);
    if (s->name) return s->addr;
    u64 addr = 0;
    for (u32 k = 0; k < hle_count && !addr; k++)
        if (!strcmp(hle_table[k].name, name)) addr = rt_native(hle_table[k].fn, hle_table[k].name);
    int missing = !addr;
    if (!addr && !weak) {
        addr = rt_native(unresolved, name);
        unresolved_count++;
    }
    *s = (struct Sym){name, addr, (u8)missing};
    return addr;
}

/* Bind an exported name to a native function, for references linked from now on: the address it
 * had (0 if nothing defines it - then it stays as it was). The wrapper calls that one itself. */
u64 rt_wrap_symbol(const char *name, GuestFn *fn) {
    struct Sym *s = sym_slot(name);
    u64 real = s->name && !s->missing ? s->addr : 0;
    if (real) s->addr = rt_native(fn, name);
    return real;
}

#ifdef __APPLE__
void discord_init(void);   /* (macos/discord.c) */
#endif

/* ---- loading --------------------------------------------------------------------- */
int main(int argc, char **argv);
static DWORD WINAPI guest_main(LPVOID unused);
static u32 by_host_cap;

int kernel_rb_switch(void);                 /* (kernel.c: SSBU_RB, rollback netplay, on) */
void rb_mem_note(u64 at, u64 size);         /* (kernel.c: memory a rollback savestate copies whole) */

/* Map module m's image at its guest address and make its code reachable. */
static void map_module(u32 m, const char *dir) {
    const RtModule *mod = modules[m];
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.reloc.bin", dir, mod->name);
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s", path);
    u64 size = (mod->image_size + 0xFFFF) & ~0xFFFFull, text = (mod->text_size + 0xFFF) & ~0xFFFull;
    u8 *image = VirtualAlloc(G2H(mod->base), size, MEM_COMMIT, PAGE_READWRITE);   /* inside its reservation (NROs) */
    /* (rollback netplay, SSBU_RB: modules' writes are watched, so a savestate copies only the pages written) */
    if (image != G2H(mod->base))
        image = VirtualAlloc(G2H(mod->base), size, MEM_RESERVE | MEM_COMMIT | (kernel_rb_switch() ? MEM_WRITE_WATCH : 0), PAGE_READWRITE);
    if (image != G2H(mod->base)) rt_fatal("cannot map %s at %#llx", mod->name, mod->base);
    if (fread(image, 1, mod->image_size, f) < mod->text_size) rt_fatal("%s is truncated", path);
    fclose(f);
    /* code, read-only data, data: as the console maps them (plugins find a module's sections with
     * QueryMemory, hle_rtld.c). An NRO's header says where; the boot modules' are 13.0.5's. */
    u64 ro = text, data = text;
    static const struct { const char *name; u64 ro, data; } BOOT[] = {
        {"main", 0x39c9000, 0x4f17000}, {"subsdk0", 0x36d000, 0x5c7000}, {"sdk", 0x541000, 0xbda000}};
    if (*(u32 *)(image + 0x10) == 0x304F524E) { ro = *(u32 *)(image + 0x28); data = *(u32 *)(image + 0x30); }
    else for (u32 b = 0; b < 3; b++)
        if (!strcmp(mod->name, BOOT[b].name) && BOOT[b].ro == text) { ro = BOOT[b].ro; data = BOOT[b].data; }
    if (ro != text || data < ro || data > size) ro = data = text;
    /* (SSBU_RB: the savestate sees the new pages; nothing otherwise. The data only: code and read-only data are not in
     * the memory a savestate holds (kernel_rb_regions), and noted here they entered its layout all the same - 713 pages
     * of lua2cpp_samus and lua2cpp_fox, loaded after the copy at the set-up, that the next whole walk dropped.) */
    rb_mem_note(mod->base + data, size - data);
    kernel_add_region(mod->base, text, 3, 5);                        /* code */
    kernel_add_region(mod->base + ro, data - ro, 3, 1);              /* read-only data */
    kernel_add_region(mod->base + data, size - data, 4, 3);          /* data */

    for (u64 a = mod->base; a < mod->base + size; a += 1ull << SLOT_SHIFT) {
        u64 slot = (a - SLOT_BASE) >> SLOT_SHIFT;
        if (slot >= SLOT_COUNT || (slot_module[slot] && slot_module[slot] != m + 1))
            rt_fatal("%s at %#llx overlaps another module's dispatch slot", mod->name, mod->base);
    }
    code[m] = VirtualAlloc(NULL, mod->text_size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (by_host_count + mod->func_count > by_host_cap) {
        by_host_cap = (by_host_count + mod->func_count) * 2;
        by_host = realloc(by_host, sizeof *by_host * by_host_cap);
    }
    for (u32 i = 0; i < mod->func_count; i++) {
        code[m][(mod->funcs[i].addr - mod->base) >> 2] = mod->funcs[i].fn;
        by_host[by_host_count++] = (struct HostFn){(u64)mod->funcs[i].fn, mod->funcs[i].addr};
    }
    /* reachable last, its table filled: rt_lookup finds a module through its slots without the
     * loader's lock, on any thread (release, for a weakly ordered processor) */
    for (u64 a = mod->base; a < mod->base + size; a += 1ull << SLOT_SHIFT)
        __atomic_store_n(&slot_module[(a - SLOT_BASE) >> SLOT_SHIFT], (u16)(m + 1), __ATOMIC_RELEASE);
}

/* Publish module m's exports. The first module to define a name wins, except over a name
 * nothing defined yet: references to it already made are patched to the definition. */
static void publish_exports(u32 m) {
    const RtModule *mod = modules[m];
    int defined_missing = 0;
    for (u32 i = 0; i < mod->export_count; i++) {
        struct Sym *s = sym_slot(mod->exports[i].name);
        if (!s->name) *s = (struct Sym){mod->exports[i].name, mod->exports[i].addr, 0};
        else if (s->missing) { s->addr = mod->exports[i].addr; s->missing = 0; defined_missing = 1; }
    }
    if (!defined_missing) return;
    for (u32 i = 0; i < site_count;)
        if (!sites[i].sym->missing) {
            st64(sites[i].at, sites[i].sym->addr + (u64)sites[i].addend);
            sites[i] = sites[--site_count];
        } else i++;
}

/* Resolve module m's symbol references and apply its symbol relocations. */
static u32 bind_module(u32 m) {
    const RtModule *mod = modules[m];
    u64 *target = malloc(sizeof *target * (mod->import_count + 1));
    struct Sym **sym = malloc(sizeof *sym * (mod->import_count + 1));
    for (u32 i = 0; i < mod->import_count; i++) {
        target[i] = resolve(mod->imports[i], mod->import_weak[i]);
        sym[i] = sym_slot(mod->imports[i]);
    }
    for (u32 i = 0; i < mod->reloc_count; i++) {
        const RtReloc *r = &mod->relocs[i];
        st64(mod->base + r->offset, target[r->import] + (u64)r->addend);
        if (sym[r->import]->missing) {
            if (site_count == site_cap) sites = realloc(sites, sizeof *sites * (site_cap = site_cap * 2 + 1024));
            sites[site_count++] = (struct Site){sym[r->import], mod->base + r->offset, r->addend};
        }
    }
    free(target);
    free(sym);
    return mod->reloc_count;
}

static u32 nro_count_all(void);
static const RtModule *nro_by_index(u32 k);
static void load_plugin_dlls(void);

static void load(const char *dir) {
    u32 funcs = 0, exports = 0, relocs = 0, hooked = 0;
    load_plugin_dlls();
    for (u32 m = 0; m < module_count; m++) { funcs += modules[m]->func_count; exports += modules[m]->export_count; }
    for (u32 m = 0; m < nro_count_all(); m++) exports += nro_by_index(m)->export_count;
    for (RtHook *h = rt_hook_list(); h; h = h->next) hooked++;
    for (symtab_size = 1024; symtab_size < exports * 3 + 65536; symtab_size *= 2) {}
    symtab = calloc(symtab_size, sizeof *symtab);
    by_host_cap = funcs + hooked + 16;
    by_host = malloc(sizeof *by_host * by_host_cap);

    for (u32 m = 0; m < module_count; m++) {
        map_module(m, dir);
        publish_exports(m);
    }
    /* a hookable function's own code (hookable.h): the module's table holds only what calls it
     * through its slot */
    for (RtHook *h = rt_hook_list(); h; h = h->next) by_host[by_host_count++] = (struct HostFn){(u64)h->body, h->addr};
    /* The NROs the game loads later each have a fixed address: keep their ranges free now,
     * before the graphics driver and the heap start taking address space (they did, and a
     * fighter's module then could not be mapped). */
    for (u32 k = 0; k < nro_count_all(); k++) {
        const RtModule *nro = nro_by_index(k);
        u64 size = (nro->image_size + 0xFFFF) & ~0xFFFFull;
        if (VirtualAlloc(G2H(nro->base), size, MEM_RESERVE | (kernel_rb_switch() ? MEM_WRITE_WATCH : 0), PAGE_NOACCESS) != G2H(nro->base))   /* (SSBU_RB: watched, as above) */
            fprintf(stderr, "[ro] warning: cannot reserve %s's range at %#llx (error %lu)\n", nro->name, nro->base, (unsigned long)GetLastError());
    }
    GuestFn *own[] = {(GuestFn *)rt_call, (GuestFn *)rt_lookup, (GuestFn *)rt_backtrace, (GuestFn *)rt_fatal,
                      (GuestFn *)rt_trap, (GuestFn *)rt_svc, (GuestFn *)load, (GuestFn *)main, (GuestFn *)guest_main,
                      wrap_startup, wrap_main, unresolved};
    for (u32 i = 0; i < sizeof own / sizeof *own; i++) by_host[by_host_count++] = (struct HostFn){(u64)own[i], 0};
    qsort(by_host, by_host_count, sizeof *by_host, by_host_cmp);

    /* Wrap the two entry points the SDK calls back into, to report progress. */
    real_startup = rt_symbol("nninitStartup");
    real_main = rt_symbol("nnMain");
    sym_slot("nninitStartup")->addr = rt_native(wrap_startup, "nninitStartup");
    sym_slot("nnMain")->addr = rt_native(wrap_main, "nnMain");

    if (VirtualAlloc(G2H(NATIVE_BASE), 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != G2H(NATIVE_BASE)) {
        fprintf(stderr, "cannot map the native-function region\n");
        exit(2);
    }
#ifdef __APPLE__
    discord_init();   /* (before the modules are linked: it wraps some of main's exports) */
#endif
    for (u32 m = 0; m < module_count; m++) relocs += bind_module(m);
    printf("loaded %u modules: %u functions, %u exported symbols, %u symbol relocations; "
           "%u symbols bound to native code, %u left undefined (%u NROs recompiled for loading later)\n",
           module_count, funcs, exports, relocs, hle_count, unresolved_count, nro_count_all());
}

/* Mod plugins translated on their own (tools/skyline.py): build/plugins/<name>.dll, listed in
 * dump/skyline/plugin_dlls.txt. Each one hands over its RtModule. A module the program already
 * has linked in is left to that copy. */
#define MAX_EXTRA_NROS 64
static const RtModule *extra_nros[MAX_EXTRA_NROS];
static u32 extra_nro_count;

static u32 nro_count_all(void) { return rt_nro_count + extra_nro_count; }
static const RtModule *nro_by_index(u32 k) {
    return k < rt_nro_count ? rt_nro_modules[k] : extra_nros[k - rt_nro_count];
}

static int nro_linked(const RtModule *mod) {
    for (u32 k = 0; k < rt_nro_count; k++)
        if (!strcmp(rt_nro_modules[k]->name, mod->name) || !strcmp(rt_nro_modules[k]->build_id, mod->build_id))
            return 1;
    return 0;
}

static void load_plugin_dlls(void) {
#ifdef _WIN32
    char list[1200], line[1200], root[1024];
    snprintf(list, sizeof list, "%s/../skyline/plugin_dlls.txt", rt_data_dir);
    FILE *f = fopen(list, "r");
    if (!f) return;
    snprintf(root, sizeof root, "%s/../..", rt_data_dir);
    while (fgets(line, sizeof line, f) && extra_nro_count < MAX_EXTRA_NROS) {
        char *s = line, full[1200];
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s);
        while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
        if (!*s || *s == '#') continue;
        if (s[1] == ':') snprintf(full, sizeof full, "%s", s);
        else snprintf(full, sizeof full, "%s/%s", root, s);
        for (char *p = full; *p; p++) if (*p == '/') *p = '\\';
        HMODULE lib = LoadLibraryA(full);
        if (!lib) {
            fprintf(stderr, "[plugins] %s did not load (error %lu)\n", full, (unsigned long)GetLastError());
            continue;
        }
        void (*bind)(GuestFn *(*)(u64), GuestFn *(*)(u64, RtCache *), void (*)(Cpu *, u64, const char *),
                     void (*)(Cpu *, u32)) = (void *)GetProcAddress(lib, "ssbu_plugin_bind");
        const RtModule *(*modfn)(void) = (void *)GetProcAddress(lib, "ssbu_plugin_module");
        if (!bind || !modfn) {
            fprintf(stderr, "[plugins] %s has no plugin entry\n", full);
            FreeLibrary(lib);
            continue;
        }
        bind(rt_lookup, rt_lookup_cached, rt_trap, rt_svc);   /* (a DLL built before rt_svc ignores the 4th) */
        const RtModule *mod = modfn();
        if (!mod || nro_linked(mod)) {
            FreeLibrary(lib);
            continue;
        }
        extra_nros[extra_nro_count++] = mod;
        fprintf(stderr, "[plugins] %s will load from %s\n", mod->name, full);
    }
    fclose(f);
#else
    (void)0;
#endif
}

/* nn::ro: load the recompiled module built from the NRO with this image - map it at its
 * fixed address, link it against everything loaded so far and run its initialiser. Returns
 * its load address, or 0 if no recompiled module matches the image. */
static u64 load_nro(Cpu *c, const char *id, const char *name);
void skyline_module_loaded(const char *name, u64 base);   /* (hle_skyline.c: the plugins' hooks into it) */
void rtld_module_loaded(u64 base, u64 size);               /* (hle_rtld.c: nn::ro's lists) */
void native_mods_module_loaded(void);                      /* (hle_native_mods.c) */
void fighter_src_module_loaded(const char *module, u64 base); /* (hle_fighter_src.c) */

u64 rt_load_nro(Cpu *c, u64 image) {
    char id[65];
    for (int i = 0; i < 32; i++) snprintf(id + 2 * i, 3, "%02X", *P(u8, image + 0x40 + i));
    for (int n = 64; n > 1 && id[n - 1] == '0'; n--) id[n - 1] = 0;
    return load_nro(c, id, NULL);
}

/* Load the recompiled NRO module with this build id (or, with id NULL, this name), as
 * rt_load_nro does. */
static u64 load_nro(Cpu *c, const char *id, const char *name) {
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    for (u32 m = BOOT_MODULES; m < module_count; m++)
        if (id ? !strcmp(modules[m]->build_id, id) : !strcmp(modules[m]->name, name)) { ReleaseSRWLockExclusive(&lock); return modules[m]->base; }
    const RtModule *mod = NULL;
    for (u32 k = 0; k < nro_count_all() && !mod; k++)
        if (id ? !strcmp(nro_by_index(k)->build_id, id) : !strcmp(nro_by_index(k)->name, name)) mod = nro_by_index(k);
    if (!mod || module_count == MAX_MODULES) { ReleaseSRWLockExclusive(&lock); return 0; }
    u32 m = module_count;
    modules[m] = mod;
    map_module(m, rt_data_dir);
    qsort(by_host, by_host_count, sizeof *by_host, by_host_cmp);
    publish_exports(m);
    u32 relocs = bind_module(m);
    module_count++;
    ReleaseSRWLockExclusive(&lock);
    printf("[ro] loaded %s at %#llx: %u functions, %u exports, %u symbol relocations\n",
           mod->name, mod->base, mod->func_count, mod->export_count, relocs);
    fflush(stdout);
    if (input_seq_on()) {   /* (SSBU_SEQ's @module:<name>) */
        char mark[SEQ_TEXT];
        snprintf(mark, sizeof mark, "module:%s", mod->name);
        input_seq_mark(mark);
    }
    rtld_module_loaded(mod->base, mod->image_size);
    if (mod->init != mod->base) rt_call(c, mod->init);
    skyline_module_loaded(mod->name, mod->base);
    native_mods_module_loaded();   /* (hle_native_mods.c: hooks by name waiting for this module) */
    fighter_src_module_loaded(mod->name, mod->base);   /* (hle_fighter_src.c: its fighter library's C++ scripts) */
    return mod->base;
}

/* Skyline plugins (hle_skyline.c): a recompiled NRO loaded by its module name, not by an image the
 * game hands over; 0 if no such module was recompiled in. */
u64 rt_load_nro_named(Cpu *c, const char *name) { return load_nro(c, NULL, name); }

/* A loaded module by name: its base, image and code sizes; 0 if it is not loaded. */
u64 rt_module_named(const char *name, u64 *image_size, u64 *text_size) {
    for (u32 m = BOOT_MODULES; m < module_count; m++)
        if (!strcmp(modules[m]->name, name)) {
            if (image_size) *image_size = modules[m]->image_size;
            if (text_size) *text_size = modules[m]->text_size;
            return modules[m]->base;
        }
    return 0;
}

/* A loaded module's exports (the module at that base), NULL if none is loaded there. */
const RtSym *rt_module_exports(u64 base, u32 *count) {
    for (u32 m = 0; m < module_count; m++)
        if (modules[m]->base == base) { *count = modules[m]->export_count; return modules[m]->exports; }
    *count = 0;
    return NULL;
}

/* (hle_fighter_registry.c) A script module without an NRO: `name` at its own range behind every
 * module the build has, whose functions are, by offset, the loaded module `donor`'s as they are now
 * (its dispatch table: hooks included). The range is reserved and never committed: its addresses are
 * names of functions, not memory. The tables come back for the caller to put a fighter library's
 * functions into (code: by offset / 4; odd: offset 4k + 1, a function only in the source). Returns
 * the base, 0 if the donor is not loaded or there is no room. */
u64 rt_module_clone(const char *donor, const char *name, GuestFn ***code_out, GuestFn ***odd_out) {
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    for (u32 i = 0; i < clone_count; i++)
        if (!strcmp(clones[i].name, name)) {
            *code_out = clones[i].code; *odd_out = clones[i].odd;
            ReleaseSRWLockExclusive(&lock);
            return clones[i].base;
        }
    u32 dm = 0;
    for (u32 m = BOOT_MODULES; m < module_count && !dm; m++) if (!strcmp(modules[m]->name, donor)) dm = m;
    if (!dm || clone_count == MAX_CLONES) { ReleaseSRWLockExclusive(&lock); return 0; }
    const RtModule *d = modules[dm];
    u64 end = SLOT_BASE;
    for (u32 k = 0; k < nro_count_all(); k++) {
        const RtModule *nro = nro_by_index(k);
        u64 e = nro->base + ((nro->image_size + 0xFFFF) & ~0xFFFFull);
        if (e > end) end = e;
    }
    for (u32 i = 0; i < clone_count; i++) if (clones[i].base + clones[i].image_size > end) end = clones[i].base + clones[i].image_size;
    u64 slot_size = 1ull << SLOT_SHIFT, base = ((end + slot_size - 1) & ~(slot_size - 1)) + slot_size;
    u64 size = (d->image_size + slot_size - 1) & ~(slot_size - 1);
    if ((base + size - SLOT_BASE) >> SLOT_SHIFT > SLOT_COUNT) { ReleaseSRWLockExclusive(&lock); return 0; }
    for (u64 a = base; a < base + size; a += slot_size)
        if (slot_module[(a - SLOT_BASE) >> SLOT_SHIFT]) { ReleaseSRWLockExclusive(&lock); return 0; }
    VirtualAlloc(G2H(base), size, MEM_RESERVE, PAGE_NOACCESS);
    struct Clone *k = &clones[clone_count];
    snprintf(k->name, sizeof k->name, "%s", name);
    k->base = base; k->image_size = d->image_size; k->text_size = d->text_size;
    u64 words = (d->text_size + 3) / 4;
    k->code = calloc(words, sizeof *k->code);
    k->odd = calloc(words, sizeof *k->odd);
    memcpy(k->code, code[dm], words * sizeof *k->code);
    clone_count++;
    for (u64 a = base; a < base + size; a += slot_size)
        __atomic_store_n(&slot_module[(a - SLOT_BASE) >> SLOT_SHIFT], (u16)(CLONE_FLAG | clone_count), __ATOMIC_RELEASE);
    memset((void *)lookup_cache, 0, sizeof lookup_cache);
    ReleaseSRWLockExclusive(&lock);
    *code_out = k->code; *odd_out = k->odd;
    return base;
}

/* The loaded modules, for nn::ro's lists (hle_rtld.c): boot modules first. */
u32 rt_module_count(void) { return module_count; }
u64 rt_module_base(u32 i) { return i < module_count ? modules[i]->base : 0; }
u64 rt_module_size(u32 i) { return i < module_count ? modules[i]->image_size : 0; }

/* The address a loaded module exports a name at, looked up in that module alone (every plugin
 * exports its own `main`); 0 if it does not. */
u64 rt_module_export(u64 base, const char *symbol) {
    for (u32 m = 0; m < module_count; m++)
        if (modules[m]->base == base)
            for (u32 i = 0; i < modules[m]->export_count; i++)
                if (!strcmp(modules[m]->exports[i].name, symbol)) return modules[m]->exports[i].addr;
    return 0;
}

/* What a weak reference to the name binds to: an export, else a native implementation (HLE),
 * else 0 - as nn::ro::LookupSymbol finds a name. */
u64 rt_resolve_weak(const char *name) { return resolve(name, 1); }

/* Whether a loaded module has a function entry at addr (hle_skyline.c: plugins built for another game version). */
int rt_is_function(u64 addr) {
    u64 slot = (addr - SLOT_BASE) >> SLOT_SHIFT;
    u16 slot_m = slot < SLOT_COUNT ? slot_module[slot] : 0;
    if (slot_m & CLONE_FLAG) return 0;
    u64 off = slot_m ? addr - modules[slot_m - 1]->base : ~0ull;
    return slot_m && off < modules[slot_m - 1]->text_size && !(off & 3) && code[slot_m - 1][off >> 2] != NULL;
}

/* Skyline's A64HookFunction. Indirect calls go through the dispatch table. Direct calls the
 * recompiler baked in land on the function's first bytes, so those bytes are patched to jump
 * to the hook (runtime/hle_detour.c) and the replaced bytes stay callable as the original.
 * A function the build already gave a hook slot (tools/hooks.txt `hookable`) is swapped in
 * that slot instead. Returns the address that still reaches the function as it was, 0 if
 * `target` is not a function entry. */
GuestFn **rt_hook_slot(u64 guest_addr);
int rt_hook_install(GuestFn **slot, GuestFn *hook, GuestFn **next);
u64 rt_hook_function(u64 target, u64 replace) {
    static SRWLOCK lock = SRWLOCK_INIT;
    GuestFn *with = rt_lookup(replace);
    AcquireSRWLockExclusive(&lock);
    u64 slot = (target - SLOT_BASE) >> SLOT_SHIFT;
    u16 slot_m = slot < SLOT_COUNT ? slot_module[slot] : 0;
    if (slot_m & CLONE_FLAG) slot_m = 0;   /* (a script module without an NRO: not hooked this way) */
    u64 off = slot_m ? target - modules[slot_m - 1]->base : ~0ull;
    GuestFn **at = slot_m && off < modules[slot_m - 1]->text_size && !(off & 3) ? &code[slot_m - 1][off >> 2] : NULL;
    if (!at || !*at) { ReleaseSRWLockExclusive(&lock); return 0; }
    GuestFn *original_fn = *at;
    GuestFn **hook_slot = rt_hook_slot(target);
    if (hook_slot) {
        GuestFn *next = NULL;
        rt_hook_install(hook_slot, with, &next);
        if (next) original_fn = next;
    } else {
        /* Identical functions share one copy (the linker's folding). Patching that copy would
         * send every one of them into this hook. A shared copy is left alone: calls that already
         * go through the dispatch table still reach the hook, and a folded function's other
         * callers keep the original. */
        struct HostFn key = {(u64)*at, 0};
        struct HostFn *hit = by_host && by_host_count ? bsearch(&key, by_host, by_host_count, sizeof key, by_host_cmp) : NULL;
        int shared = hit && ((hit > by_host && hit[-1].host == key.host) ||
                             (hit + 1 < by_host + by_host_count && hit[1].host == key.host));
        if (!shared) {
            u32 room = 256;
            if (hit && hit + 1 < by_host + by_host_count && hit[1].host > (u64)*at) {
                u64 gap = hit[1].host - (u64)*at;
                room = gap > 256 ? 256 : (u32)gap;
            }
            GuestFn *tramp = rt_detour_entry(*at, with, hit != NULL, room);
            if (tramp) original_fn = tramp;
        }
    }
    u64 original = rt_native(original_fn, "original of a hooked function");
    __atomic_store_n(at, with, __ATOMIC_RELEASE);
    memset((void *)lookup_cache, 0, sizeof lookup_cache);
    ReleaseSRWLockExclusive(&lock);
    return original;
}

/* ---- start-up -------------------------------------------------------------------- */
static void exception_handler_ready(Cpu *c) { (void)c; }

/* The linker's part of start-up: run each module's init routine, dependencies first. Each
 * routine registers its module with the C library and runs its static constructors. */
static void call_initializers(Cpu *c) {
    for (u32 m = BOOT_MODULES; m-- > 0;) {
        static char label[64];
        snprintf(label, sizeof label, "static constructors of %s", modules[m]->name);
        phase = label;
        printf("[boot] %s\n", label);
        fflush(stdout);
        rt_call(c, modules[m]->init);
    }
    phase = "start-up after static constructors";
}

static DWORD WINAPI guest_main(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"guest main thread");
    u32 handle;
    Cpu *c = kernel_main_thread(&handle);
    u64 start = rt_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) rt_fatal("the SDK does not export nn::init::Start");
    c->x[0] = handle;
    c->x[1] = 0;
    c->x[2] = rt_native(exception_handler_ready, "exception_handler_ready");
    c->x[3] = rt_native(call_initializers, "call_initializers");
#ifndef __APPLE__   /* (ultrawide: Windows and Linux) */
    if (!display_shape_locked()) hook_hud_projection();
    hook_hud_anchors();   /* (the timer's place is a setting at any shape; the widening parts wait for a wider picture) */   /* (the shape can change between fights: in from the start, passing through at 16:9) */
    int views = !display_shape_locked() && !getenv("SSBU_DEBUG_REFERENCE_VIEW");   /* (every view's hooks: view and shape can change) */
    if (!views && (getenv("SSBU_DEBUG_FRAMING_DUMP") || getenv("SSBU_DEBUG_REFERENCE_VIEW"))) hook_framing_dump();
    if (views) {
        u64 orig = rt_hook_function(0x71004fdc10ull, rt_native(shadow_update, "ultrawide shadow camera"));
        if (orig) upd_orig = rt_lookup(orig);
        u64 f1 = rt_hook_function(0x71004fe4f0ull, rt_native(framing_hook, "ultrawide framing"));
        u64 f2 = rt_hook_function(0x71004fec80ull, rt_native(rec_move_call, "ultrawide move recorder"));
        if (f1) frame_fn_orig = fdump_orig = rt_lookup(f1);
        if (f2) move_fn_orig = rt_lookup(f2);
        if (!f1 || !f2) orig = 0;
        printf("[aspect] the views' game camera hooks: %s (main+0x4fdc10)\n", orig ? "on" : "NOT hooked");
    }
    if (views) {
        u64 orig = rt_hook_function(0x7103555140ull, rt_native(wide_copy, "ultrawide framing for the picture"));
        if (orig) copy_orig = rt_lookup(orig);
        frame_hook_add(FRAME_HOOK_LINE, 300, fight_line, NULL, NULL);
        u64 prep = rt_hook_function(0x7103747820ull, rt_native(wide_prep, "ultrawide framing put back"));
        if (prep) prep_orig = rt_lookup(prep);
        printf("[aspect] wide framing for the picture: %s (main+0x3555140)\n", orig ? "on" : "NOT hooked");
    }
#endif
    phase = "SDK initialisation (nn::init::Start)";
    printf("[boot] nn::init::Start\n");
    fflush(stdout);
    rt_call(c, start);
    return 0;
}

static void print_status(void) {
    fprintf(stderr, "\n[status] %.0f s into %s; GPU: %llu submissions, %llu command lists, %llu draws, %llu clears, %llu compute launches (%llu run, %llu GPU-resident buffers made, %llu given up); %llu frames presented\n",
            (double)rt_now_ns() / 1e9, phase, gpu_stat_batches, gpu_stat_lists, gpu_stat_draws, gfx_stat_clears, gpu_stat_dispatches,
            gfx_stat_dispatched, gfx_stat_residents, gfx_stat_residents_retired,
            present_frames());
    fprintf(stderr, "[status] renderer: %llu draws drawn, %llu skipped (%llu shader, %llu topology, %llu target); %llu shaders (%llu failed), %llu textures (%llu unpacked); %llu ms watching writes; %llu buffers dropped after unmaps\n",
            gfx_stat_drawn, gfx_stat_skipped, gfx_skip_shader, gfx_skip_topology, gfx_skip_target, gfx_stat_shaders, gfx_stat_shader_failures, gfx_stat_textures, gfx_stat_unpacked,
            gfx_stat_watch_us / 1000, gfx_stat_unmap_dropped);
    gfx_status_lines();
    u32 fiber_slots, fibers_finalized;
    u64 fibers_made;
    fiber_counts(&fiber_slots, &fibers_finalized, &fibers_made);
    fprintf(stderr, "[status] fibers: %llu started, %u host fibers (%u finalized, kept for the next ones)\n",
            (unsigned long long)fibers_made, fiber_slots, fibers_finalized);
    kernel_map_status();
    kernel_dump_threads();
    fflush(stderr);
}

#ifndef _WIN32
int rt_window_init(void);                                  /* (linux/window.c; macos/window.m) */
_Noreturn void rt_window_loop(int every, void (*status)(void));

/* (the main thread runs the window's events: the status dumps, which stop every thread a
 * moment, go on a thread of their own so that input never waits for them) */
static DWORD WINAPI status_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"status");
    for (;;) {
        Sleep((DWORD)status_every * 1000);
        print_status();
    }
}
#endif

#ifdef __APPLE__
#include <limits.h>
#include <sys/resource.h>
#include <sys/sysctl.h>

/* macOS, before anything else: the guest's address space (rt_guest_window: first, before the
 * system's allocations can take any of it - after only the log, which maps nothing), more open
 * files than macOS's 256 by default (the game's own, RomFS sources, sockets), and what a start
 * from Finder or `open` needs - no terminal and / as the working folder: SSBU_LOG=<file> takes
 * stdout and stderr, SSBU_CWD=<dir> the working folder. Run as the app
 * (<tree>/build-mac/WindowSmash.app/Contents/MacOS/ssbu) with no directory given, the tree's
 * dump/modules is the one, and (without SSBU_CWD) the port's root - the nearest folder up from
 * the tree with tools/recomp.py - the working folder, where reports/ goes as everywhere. *dir
 * and *options: the directory, and where options start. */
static void mac_start(int argc, char **argv, const char **dir, int *options) {
    /* (the log first, so that a window that cannot be had is told there: started from Finder
     * or `open`, stderr goes nowhere) */
    const char *log = getenv("SSBU_LOG"), *cwd = getenv("SSBU_CWD");
    int log_failed = 0;
    if (log && *log) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        else log_failed = errno;
    }
    if (!rt_guest_window()) {
        fprintf(stderr, "cannot reserve the guest's memory (512 GiB of address space at %#llx): see above\n", RT_MEMBASE);
        ExitProcess(2);
    }
    if (log_failed) fprintf(stderr, "[boot] cannot write the log %s (errno %d)\n", log, log_failed);
    int max_files = 0;
    size_t len = sizeof max_files;
    struct rlimit rl;
    if (sysctlbyname("kern.maxfilesperproc", &max_files, &len, NULL, 0) || max_files <= 0) max_files = 10240;
    if (!getrlimit(RLIMIT_NOFILE, &rl)) {
        rlim_t want = max_files < 10240 ? (rlim_t)max_files : 10240;
        if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max) want = rl.rlim_max;
        if (rl.rlim_cur < want) { rl.rlim_cur = want; setrlimit(RLIMIT_NOFILE, &rl); }
    }
    if (cwd && *cwd && chdir(cwd)) fprintf(stderr, "[boot] cannot work in %s (errno %d)\n", cwd, errno);
    if (argc >= 2 && argv[1][0] != '-') return;
    static char tree[PATH_MAX], modules[PATH_MAX + 16];
    snprintf(tree, sizeof tree, "%s", rt_exe_path());
    char *slash;
    for (int up = 0; up < 3; up++) {   /* ssbu, MacOS, Contents: the app */
        if (!(slash = strrchr(tree, '/'))) return;
        *slash = 0;
    }
    size_t n = strlen(tree);
    if (n < 4 || strcmp(tree + n - 4, ".app")) return;   /* (not an app: the directory must be given) */
    for (int up = 0; up < 2; up++) {   /* the app, build-mac: the tree */
        if (!(slash = strrchr(tree, '/')) || slash == tree) return;
        *slash = 0;
    }
    snprintf(modules, sizeof modules, "%s/dump/modules", tree);
    *dir = modules;
    *options = 1;
    if (cwd && *cwd) return;
    char root[PATH_MAX], probe[PATH_MAX + 32];
    snprintf(root, sizeof root, "%s", tree);
    for (;;) {
        struct stat st;
        snprintf(probe, sizeof probe, "%s/tools/recomp.py", root);
        if (!stat(probe, &st)) { if (chdir(root)) fprintf(stderr, "[boot] cannot work in %s (errno %d)\n", root, errno); return; }
        if (!(slash = strrchr(root, '/')) || slash == root) return;
        *slash = 0;
    }
}
#endif

#ifdef SSBU_GAME_DLL
/* The game library cannot be used: why (detail, for the log), and what a player does about it. */
static int game_refused(int quiet, const char *path, const char *detail) {
    static const char text[] =
        "This WindowSmash's game library (" RT_GAME_LIBRARY ") is missing, or was built for another version of "
        "WindowSmash. Run OpenSmash.exe again: it builds the game for this version.";
    fprintf(stderr, "[boot] cannot use the game library %s: %s\n%s\n", path, detail, text);
#ifdef _WIN32
    if (!quiet) MessageBoxA(NULL, text, "WindowSmash", MB_OK | MB_ICONERROR);
#else
    (void)quiet;
#endif
    return 0;
}

/* The game library (runtime/game_dll.h), from beside the program (on macOS the app's
 * Frameworks): loaded, its interface checked against this runtime's, then bound - each runtime
 * function it calls, by name; each HLE_WRAP's original; its hook slots, which its constructors
 * listed as it loaded (added here in their order, as the program's own would have been); its
 * modules. 0 if it cannot be used, said why. quiet: no message box (ssbu --game-library). */
static int game_open(int quiet) {
    char path[4096], detail[512];
#ifdef _WIN32
    wchar_t wide[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, wide, MAX_PATH);
    wchar_t *slash = n && n < MAX_PATH ? wcsrchr(wide, L'\\') : NULL;
    if (!slash) return game_refused(quiet, RT_GAME_LIBRARY, "the program's own folder is not known");
    wcscpy(slash + 1, L"" RT_GAME_LIBRARY);
    snprintf(path, sizeof path, "%ls", wide);
    HMODULE lib = LoadLibraryExW(wide, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib) {
        snprintf(detail, sizeof detail, "it could not be loaded (error %lu)", GetLastError());
        return game_refused(quiet, path, detail);
    }
    RtGameEntry *entry = (RtGameEntry *)(void *)GetProcAddress(lib, RT_GAME_ENTRY);
#else
#ifdef __APPLE__
    snprintf(path, sizeof path - 64, "%s", rt_exe_path());   /* <app>/Contents/MacOS/ssbu */
    char *slash = strrchr(path, '/');
    if (slash) { *slash = 0; slash = strrchr(path, '/'); }
    if (slash) snprintf(slash, sizeof path - (size_t)(slash - path), "/Frameworks/%s", RT_GAME_LIBRARY);
#else
    ssize_t len = readlink("/proc/self/exe", path, sizeof path - 64);
    path[len > 0 ? len : 0] = 0;
    char *slash = strrchr(path, '/');
    if (slash) snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "%s", RT_GAME_LIBRARY);
#endif
    if (!slash) return game_refused(quiet, RT_GAME_LIBRARY, "the program's own folder is not known");
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) return game_refused(quiet, path, dlerror());
    RtGameEntry *entry = (RtGameEntry *)dlsym(lib, RT_GAME_ENTRY);
#endif
    if (!entry) return game_refused(quiet, path, "it has no " RT_GAME_ENTRY ": not a game library");
    const RtGameTables *t = entry();
    if (!t || t->abi != RT_GAME_ABI) {
        snprintf(detail, sizeof detail, "it is built for game interface %u, and this runtime's is %u", t ? t->abi : 0, RT_GAME_ABI);
        return game_refused(quiet, path, detail);
    }
    static const u32 sizes[4] = RT_GAME_SIZES;
    if (memcmp(t->sizes, sizes, sizeof sizes)) {
        snprintf(detail, sizeof detail, "its structures are not this runtime's (their sizes %u %u %u %u, here %u %u %u %u): "
                 "built from other runtime headers", t->sizes[0], t->sizes[1], t->sizes[2], t->sizes[3], sizes[0], sizes[1],
                 sizes[2], sizes[3]);
        return game_refused(quiet, path, detail);
    }
    if (!t->isa || strcmp(t->isa, RT_GAME_ISA)) {
        snprintf(detail, sizeof detail, "it is compiled for %s, and this runtime for %s", t->isa ? t->isa : "?", RT_GAME_ISA);
        return game_refused(quiet, path, detail);
    }
    /* the runtime functions it calls: each one this runtime must have, and every HLE and HLE_WRAP
     * it has must be among them (the game's C is written for one set of them: tools/recomp.py) */
    u32 hle = 0, wraps = 0, wraps_here = 0;
    for (u32 k = 0; k < rt_game_func_count; k++) wraps_here += !strncmp(rt_game_funcs[k].name, "hle_", 4);
    for (u32 i = 0; i < t->import_count; i++) {
        const char *name = t->imports[i].name;
        RtGameFn *fn = NULL;
        for (u32 k = 0; k < rt_game_func_count && !fn; k++)
            if (!strcmp(rt_game_funcs[k].name, name)) { fn = rt_game_funcs[k].fn; wraps += !strncmp(name, "hle_", 4); }
        for (u32 k = 0; k < hle_count && !fn && !strncmp(name, "hle_", 4); k++)
            if (!strcmp(hle_table[k].name, name + 4)) { fn = (RtGameFn *)hle_table[k].fn; hle++; }
        if (!fn) {
            snprintf(detail, sizeof detail, "it calls %s, which this runtime does not have: built from other runtime sources", name);
            return game_refused(quiet, path, detail);
        }
        *t->imports[i].slot = fn;
    }
    if (hle != hle_count || wraps != wraps_here) {
        snprintf(detail, sizeof detail, "it is built for other native functions (%u of this runtime's %u HLE, %u of its %u "
                 "HLE_WRAP): built from other runtime sources", hle, hle_count, wraps, wraps_here);
        return game_refused(quiet, path, detail);
    }
    for (u32 k = 0; k < rt_game_func_count; k++) {
        if (!rt_game_funcs[k].orig) continue;
        u32 i = 0;
        while (i < t->orig_count && t->origs[i].addr != rt_game_funcs[k].addr) i++;
        if (i == t->orig_count) {
            snprintf(detail, sizeof detail, "it has no original of HLE_WRAP(%llx): built from other runtime sources",
                     (unsigned long long)rt_game_funcs[k].addr);
            return game_refused(quiet, path, detail);
        }
        *rt_game_funcs[k].orig = t->origs[i].fn;
    }
    u32 hooks = 0;
    for (RtHook *h = *t->hooks; h; h = h->next) hooks++;
    RtHook **list = hooks ? malloc(hooks * sizeof *list) : NULL;
    if (hooks && !list) return game_refused(quiet, path, "no memory for its hook slots' list");
    u32 at = 0;
    for (RtHook *h = *t->hooks; h; h = h->next) list[at++] = h;
    while (at) rt_hook_add(list[--at]);   /* (rt_hook_add puts each in front: the oldest goes in first) */
    free(list);
    rt_module_main = *t->boot[0];
    rt_module_subsdk0 = *t->boot[1];
    rt_module_sdk = *t->boot[2];
    game_nros = t->nros;
    game_nro_count = t->nro_count;
    fprintf(stderr, "[boot] game library %s: interface %u, %u runtime functions bound, %u hook slots, %u NROs\n", path,
            t->abi, t->import_count, hooks, t->nro_count);
    return 1;
}
#endif

/* The game is version 13.0.5: its main module's build id (tools/game_version.py MAIN_BUILD_ID, which
 * tests/test_game_version.py checks this against). The build refuses other versions; a game built
 * from one before that (or by hand) says so plainly and stops - not a crash report, not a bug. */
#define REQUIRED_VERSION "13.0.5"
#define REQUIRED_MAIN_BUILD "21450C647B8C5940ED5B68218EA128FD3C5D597D"

static int game_version_ok(void) {
    const char *id = rt_module_main.build_id;
    if (id && !_stricmp(id, REQUIRED_MAIN_BUILD)) return 1;
    static const char text[] =
        "This WindowSmash was built from another version of the game, and WindowSmash needs version "
        REQUIRED_VERSION ". Put the game's " REQUIRED_VERSION " update (an .nsp file) in your game folder, next to the "
        "game, and run OpenSmash.exe again: it sets this folder up again, keeping your save and settings.";
    fprintf(stderr, "[boot] the game's main module is %s, not %s's (%s)\n%s\n", id ? id : "unknown",
            REQUIRED_VERSION, REQUIRED_MAIN_BUILD, text);
#ifdef _WIN32
    MessageBoxA(NULL, text, "WindowSmash", MB_OK | MB_ICONERROR);
#endif
    return 0;
}

/* SSBU_CRASH_TEST=<seconds>[,<guest address, hex>]: a deliberate fault then (to try the crash report and minidump):
 * a write to address 0, or a read of that guest address (an unmapped one: the report's memory section). */
static DWORD WINAPI crash_test(void *seconds) {
    Sleep((DWORD)(uintptr_t)seconds * 1000);
    const char *e = getenv("SSBU_CRASH_TEST"), *comma = e ? strchr(e, ',') : NULL;
    if (comma) {
        u64 g = strtoull(comma + 1, NULL, 16);
        fprintf(stderr, "[test] SSBU_CRASH_TEST: reading guest address %#llx\n", g);
        (void)*(volatile u32 *)G2H(g);
    }
    fprintf(stderr, "[test] SSBU_CRASH_TEST: writing to address 0\n");
    *(volatile int *)0 = 0;
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = argc >= 2 ? argv[1] : NULL;
    int options = 2;   /* (where the options after it start) */
#ifdef _WIN32
    if (argc == 2 && !strcmp(argv[1], "--yuzu-client-check")) { puts("yuzu-room-client protocol=1"); return 0; }
    if (argc >= 6 && !strcmp(argv[1], "--write-dump")) return write_dump_main(argv);
    if (argc >= 3 && !strcmp(argv[1], "--warm-shaders")) return gfx_warm_shaders_main(argv[2], argc >= 4 ? argv[3] : "");
#elif defined(__linux__)
    /* (macOS has no --warm-shaders pass: Metal's prewarm is gpu_metal.m's, at the game's start) */
    if (argc >= 3 && !strcmp(argv[1], "--warm-shaders")) return gfx_warm_shaders_main(argv[2], argc >= 4 ? argv[3] : "");
#endif
#ifdef __APPLE__
    mac_start(argc, argv, &dir, &options);
#endif
#ifdef SSBU_GAME_DLL
    if (argc == 2 && !strcmp(argv[1], "--game-library")) return game_open(1) ? 0 : 4;
#endif
    if (!dir) { fprintf(stderr, "usage: ssbu <directory with *.reloc.bin images> [--trace]\n"); return 2; }
    display_dpi_aware();
#ifdef _WIN32
    {   /* which build this is, for reports */
        wchar_t exe[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        SYSTEMTIME st = {0};
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (GetFileAttributesExW(exe, GetFileExInfoStandard, &fa)) {
            FILETIME local;
            FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local);
            FileTimeToSystemTime(&local, &st);
        }
        fprintf(stderr, "[boot] %ls, built %04u-%02u-%02u %02u:%02u\n", exe, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    }
    {   /* started again from the settings (hle_settings.c settings_restart): the game before is
         * still letting go of its memory (with the shader prewarm, gigabytes) - wait for it, so the
         * two never hold theirs at once (a PC with little memory ran out on a restart) */
        const char *from = getenv("SSBU_RESTART_FROM");
        if (from && *from) {
            HANDLE before = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(from, NULL, 10));
            if (before) {
                DWORD w = WaitForSingleObject(before, 15000);
                CloseHandle(before);
                fprintf(stderr, "[boot] started again: the game before %s\n", w == WAIT_OBJECT_0 ? "has ended" : "is still ending (15 s waited)");
            }
            SetEnvironmentVariableA("SSBU_RESTART_FROM", NULL);
        }
    }
#else
    {   /* which build this is, for reports (and the time zone read now, not first in a crash report) */
        char exe[4096];
#ifdef __APPLE__
        snprintf(exe, sizeof exe, "%s", rt_exe_path());
        ssize_t len = (ssize_t)strlen(exe);
#else
        ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
#endif
        exe[len > 0 ? len : 0] = 0;
        struct stat st;
        struct tm tm = {0};
        tzset();
        if (len > 0 && !stat(exe, &st)) localtime_r(&st.st_mtime, &tm);
        fprintf(stderr, "[boot] %s, built %04d-%02d-%02d %02d:%02d\n", exe, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    }
#endif
#ifdef SSBU_GAME_DLL
    if (!game_open(0)) return 4;
#endif
    if (!game_version_ok()) return 4;
    for (int i = options; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) rt_trace = 1;
        if (!strcmp(argv[i], "--status") && i + 1 < argc) status_every = atoi(argv[++i]);
    }
    settings_args(argc, argv);   /* --online: the netplay settings from online_*.txt (hle_settings.c) */
#ifdef _WIN32
    crash_reporter_start();
    AddVectoredExceptionHandler(1, on_fault);
    if (getenv("SSBU_DEBUG_CAMERA_HUNT")) { AddVectoredExceptionHandler(1, hunt_fault); frame_hook_add(FRAME_HOOK_END, 300, NULL, hunt_scan, NULL); }
    dump_helper_start();
#else
    install_signals();
    setvbuf(stdout, NULL, _IOLBF, 0);
    fprintf(stderr, "[mem] write watch: %s\n", rt_ww_mode());
#endif
    if (getenv("SSBU_CRASH_TEST")) CloseHandle(CreateThread(NULL, 0, crash_test, (void *)(uintptr_t)atoi(getenv("SSBU_CRASH_TEST")), 0, NULL));
    rt_data_dir = dir;
    void mods_start(const char *data_dir);   /* (hle_mod_switch.c: a pending apply of the mods, the code patches' switch) */
    mods_start(dir);
    void discord_start(void);   /* (net_discord.c: Discord's sign-in and Rich Presence, when its SDK is beside the program) */
    discord_start();
#ifdef _WIN32
    void net_eos_prestart(void);   /* (net_relay.c: Epic Online Services log in while the game loads, when online play wants them) */
    net_eos_prestart();
#endif
    kernel_init();
    load(dir);
    void mod_refresh_init(void);
    mod_refresh_init();   /* material lifetime hooks, before guest threads start */
    movie_test_start();
    test_fight_start();
#ifndef _WIN32
    rt_window_init();   /* (SDL wants its window and events on the main thread, which then runs them) */
#endif
#ifndef __APPLE__   /* (ultrawide: Windows and Linux; on Linux after the window, so "auto" knows the monitor) */
    if (!display_shape_locked()) {
        frame_hook_add(FRAME_HOOK_END, 300, NULL, wide_camera, NULL);
        frame_hook_add(FRAME_HOOK_END, 300, NULL, net_match_watch, NULL);
        printf("[aspect] native: the fight camera and its framing told %.4g\n", display_aspect());
    }
    if (getenv("SSBU_DEBUG_CAM_TRACE")) frame_hook_add(FRAME_HOOK_END, 300, NULL, cam_trace, NULL);
#endif
    /* Guest call depth becomes host call depth, so the guest runs on a thread with a large stack. */
    HANDLE t = CreateThread(NULL, 512ull << 20, guest_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
#ifdef _WIN32
    while (WaitForSingleObject(t, status_every ? status_every * 1000 : INFINITE) == WAIT_TIMEOUT) print_status();
#else
    (void)t;
    if (status_every > 0) CloseHandle(CreateThread(NULL, 0, status_thread, NULL, 0, NULL));
    rt_window_loop(0, NULL);
#endif
    return 0;
}
