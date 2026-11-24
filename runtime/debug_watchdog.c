/* The freeze watchdog: when no frame has been presented for 5 seconds, a report of what every
 * thread is doing goes to reports\freeze_<time>.txt (and one line to the log), with a minidump;
 * again at 30 s and 120 s of the same freeze. It starts with the first presented frame, and
 * waits while the software keyboard's dialog is open or a debugger is attached.
 *
 * A report: how long, the owners of the locks presenting depends on (the renderer's, the
 * present queue's, the kernel's, the GPU channel's), the shader compiles in flight, the guest
 * threads' states, and every thread's stack (guest functions named by their place in the game's
 * modules; the runtime's as ssbu.exe+offset, which tools/dev/hostsym.py names).
 * SSBU_WATCHDOG=0: no watchdog. SSBU_FREEZE_TEST=<seconds>: then the vsync thread stops for
 * 7 s once (to try it). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "debug_profile.h"

typedef unsigned long long U64;

DWORD gfx_lock_owner(void), gfx_last_enter_thread(void), kernel_lock_owner(void), nv_lock_owner(void);
DWORD present_queue_holder(U64 *since_ms);
void gfx_report_compiles(void (*line)(const char *text));
void kernel_thread_states(void (*line)(const char *text));
void rt_report_path(char *path, size_t n, const char *kind, const char *ext);
int rt_write_dump(const char *path, DWORD tid, EXCEPTION_POINTERS *e, DWORD wait_ms);

static volatile U64 last_present;
static volatile U64 presented;
static volatile LONG paused, started = -1;
static FILE *report;

static void to_report(const char *text) { if (report) fputs(text, report); }

static void write_report(U64 since, int first) {
    char path[MAX_PATH], text[300];
    rt_report_path(path, sizeof path, "freeze", "txt");
    report = fopen(path, "w");
    if (!report) return;
    fprintf(report, "No frame presented for %.1f s (frames so far: %llu).\n\n", since / 1000.0, presented);
    U64 held_at;
    DWORD q = present_queue_holder(&held_at);
    fprintf(report, "Locks:\n  renderer: %s%lu (last entered by thread %lu)\n", gfx_lock_owner() ? "held by thread " : "free ", (unsigned long)gfx_lock_owner(),
            (unsigned long)gfx_last_enter_thread());
    if (q) fprintf(report, "  present queue: held by thread %lu for %.1f s\n", (unsigned long)q, (GetTickCount64() - held_at) / 1000.0);
    else fprintf(report, "  present queue: free\n");
    fprintf(report, "  kernel: %s%lu\n  GPU channel: %s%lu\n\n", kernel_lock_owner() ? "held by thread " : "free ", (unsigned long)kernel_lock_owner(),
            nv_lock_owner() ? "held by thread " : "free ", (unsigned long)nv_lock_owner());
    gfx_report_compiles(to_report);
    fprintf(report, "\nGuest threads:\n");
    kernel_thread_states(to_report);
    fprintf(report, "\nEvery thread's stack (innermost first):\n");
    profile_dump_threads(to_report);
    fclose(report);
    report = NULL;
    char dump[MAX_PATH];
    snprintf(dump, sizeof dump, "%.*s.dmp", (int)strlen(path) - 4, path);
    int dumped = first && rt_write_dump(dump, 0, NULL, 100000);
    snprintf(text, sizeof text, "[watchdog] no frame presented for %.0f s: report in %s%s%s\n", since / 1000.0, path, dumped ? ", minidump " : "",
             dumped ? dump : "");
    fputs(text, stderr);
}

static DWORD WINAPI watchdog(void *unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"freeze watchdog");
    static const U64 AT[] = {5000, 30000, 120000};
    int reports = 0;
    for (;;) {
        Sleep(500);
        U64 now = GetTickCount64(), since = now - last_present;
        if (paused || IsDebuggerPresent()) continue;
        if (since < 5000) {
            if (reports) { fprintf(stderr, "[watchdog] frames are presented again\n"); reports = 0; }
            continue;
        }
        if (reports < 3 && since >= AT[reports]) write_report(since, reports++ == 0);
    }
}

void watchdog_present(void) {
    last_present = GetTickCount64();
    presented++;
    if (started < 0) {   /* the first frame: start (once) */
        const char *v = getenv("SSBU_WATCHDOG");
        started = !v || atoi(v) != 0;
        if (started) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    }
    static double freeze_at = -1;
    if (freeze_at == -1) freeze_at = getenv("SSBU_FREEZE_TEST") ? atof(getenv("SSBU_FREEZE_TEST")) : -2;
    static U64 t0;
    if (!t0) t0 = GetTickCount64();
    if (freeze_at >= 0 && GetTickCount64() - t0 >= (U64)(freeze_at * 1000)) {
        freeze_at = -2;
        fprintf(stderr, "[test] SSBU_FREEZE_TEST: the vsync thread stops for 7 s\n");
        Sleep(7000);
    }
}

/* (Vulkan's present, #498 review) a frame shown, without starting the watchdog: Vulkan never started it before, and still
 * does not offline; once something else started it (the rollback code's waits, online), its frames keep it quiet. */
void watchdog_frame(void) {
    if (started <= 0) return;
    last_present = GetTickCount64();
    presented++;
}

/* A dialog the game waits on (the software keyboard): not a freeze. */
void watchdog_pause(int on) {
    InterlockedAdd(&paused, on ? 1 : -1);
    if (!on) last_present = GetTickCount64();
}
