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
#ifdef _WIN32
int gfx_warm_shaders_main(const char *dir, const char *luid);   /* (gpu_d3d11.c: ssbu --warm-shaders) */
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

static _Noreturn void crash_report(EXCEPTION_POINTERS *e, DWORD tid) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    char name[96], report[MAX_PATH];
    fflush(stdout);
    report_open("crash", report, sizeof report);
    out("\n*** host exception %#lx%s during %s", code, code == EXCEPTION_STACK_OVERFLOW ? " (stack overflow)" : "", phase);
    if (code == EXCEPTION_ACCESS_VIOLATION)
        out(": %s address %s", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            rt_describe(e->ExceptionRecord->ExceptionInformation[1], name, sizeof name));
    out("\n");
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

/* Map module m's image at its guest address and make its code reachable. */
static void map_module(u32 m, const char *dir) {
    const RtModule *mod = modules[m];
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.reloc.bin", dir, mod->name);
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s", path);
    u64 size = (mod->image_size + 0xFFFF) & ~0xFFFFull, text = (mod->text_size + 0xFFF) & ~0xFFFull;
    u8 *image = VirtualAlloc(G2H(mod->base), size, MEM_COMMIT, PAGE_READWRITE);   /* inside its reservation (NROs) */
    if (image != G2H(mod->base)) image = VirtualAlloc(G2H(mod->base), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
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
        if (VirtualAlloc(G2H(nro->base), size, MEM_RESERVE, PAGE_NOACCESS) != G2H(nro->base))
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
        void (*bind)(GuestFn *(*)(u64), GuestFn *(*)(u64, RtCache *), void (*)(Cpu *, u64, const char *)) =
            (void *)GetProcAddress(lib, "ssbu_plugin_bind");
        const RtModule *(*modfn)(void) = (void *)GetProcAddress(lib, "ssbu_plugin_module");
        if (!bind || !modfn) {
            fprintf(stderr, "[plugins] %s has no plugin entry\n", full);
            FreeLibrary(lib);
            continue;
        }
        bind(rt_lookup, rt_lookup_cached, rt_trap);
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
    return mod->base;
}

/* Skyline plugins (hle_skyline.c): a recompiled NRO loaded by its module name, not by an image the
 * game hands over; 0 if no such module was recompiled in. */
u64 rt_load_nro_named(Cpu *c, const char *name) { return load_nro(c, NULL, name); }

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

/* SSBU_CRASH_TEST=<seconds>: a deliberate fault then (to try the crash report and minidump). */
static DWORD WINAPI crash_test(void *seconds) {
    Sleep((DWORD)(uintptr_t)seconds * 1000);
    fprintf(stderr, "[test] SSBU_CRASH_TEST: writing to address 0\n");
    *(volatile int *)0 = 0;
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = argc >= 2 ? argv[1] : NULL;
    int options = 2;   /* (where the options after it start) */
#ifdef _WIN32
    if (argc >= 6 && !strcmp(argv[1], "--write-dump")) return write_dump_main(argv);
    if (argc >= 4 && !strcmp(argv[1], "--warm-shaders")) return gfx_warm_shaders_main(argv[2], argv[3]);
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
    dump_helper_start();
#else
    install_signals();
    setvbuf(stdout, NULL, _IOLBF, 0);
    fprintf(stderr, "[mem] write watch: %s\n", rt_ww_mode());
#endif
    if (getenv("SSBU_CRASH_TEST")) CloseHandle(CreateThread(NULL, 0, crash_test, (void *)(uintptr_t)atoi(getenv("SSBU_CRASH_TEST")), 0, NULL));
    rt_data_dir = dir;
    kernel_init();
    load(dir);
    movie_test_start();
    test_fight_start();
#ifndef _WIN32
    rt_window_init();   /* (SDL wants its window and events on the main thread, which then runs them) */
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
