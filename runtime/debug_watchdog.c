/* The freeze watchdog: when no frame has been presented for 15 seconds, a report of what every
 * thread is doing goes to reports\freeze_<time>.txt (and one line to the log), with a minidump;
 * again at 30 s and 120 s of the same freeze. A shorter stall (5 s or more) is one log line with
 * its length (freeze_schedule.h; SSBU_FREEZE_CONFIRM=0: the first report after 5 s, as before). It starts with the first presented frame, and
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
#include "freeze_schedule.h"

typedef unsigned long long U64;

DWORD gfx_lock_owner(void), gfx_last_enter_thread(void), kernel_lock_owner(void), nv_lock_owner(void);
DWORD present_queue_holder(U64 *since_ms);
void gfx_report_compiles(void (*line)(const char *text));
void kernel_thread_states(void (*line)(const char *text));
void rt_report_path(char *path, size_t n, const char *kind, const char *ext);
int rt_write_dump(const char *path, DWORD tid, EXCEPTION_POINTERS *e, DWORD wait_ms);

static volatile U64 last_present;
static volatile U64 presented;
static volatile LONG paused, started = -1, disarmed;
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
    FreezeSchedule s;   /* (freeze_schedule.h: SSBU_FREEZE_CONFIRM, the first report after 15 s; 0: after 5 s, as before) */
    freeze_schedule_init(&s, getenv("SSBU_FREEZE_CONFIRM"));
    if (!s.old) fprintf(stderr, "[watchdog] a freeze report after %.0f s with no frame (SSBU_FREEZE_CONFIRM; 0: after 5 s, as before)\n", s.at[0] / 1000.0);
    U64 stalls = 0;
    for (;;) {
        Sleep(500);
        U64 now = GetTickCount64(), len = 0;
        if (disarmed) return 0;   /* (the game is closing: watchdog_disarm) */
        if (paused || IsDebuggerPresent()) continue;
        switch (freeze_schedule_tick(&s, now, last_present, &len)) {
        case FREEZE_REPORT: write_report(len, s.reports == 1); break;
        case FREEZE_RESUMED:
            if (s.old) fprintf(stderr, "[watchdog] frames are presented again\n");
            else fprintf(stderr, "[watchdog] frames are presented again (after %.1f s without)\n", len / 1000.0);
            break;
        case FREEZE_STALL_ENDED:
            fprintf(stderr, "[watchdog] a stall: no frame presented for %.1f s, then frames again - no report (%llu so far; SSBU_FREEZE_CONFIRM: "
                            "a report after %.0f s)\n", len / 1000.0, ++stalls, s.at[0] / 1000.0);
            break;
        }
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

/* (v0.5.5) The game is closing (the settings menu's Quit, the window's close: gpu_backend.c gfx_closing): no more reports.
 * The close saves the shader and pipeline caches, which takes seconds with no frame presented: the watchdog took that
 * for a freeze (0.5.4: after "[menu] quit", 400-1200 MB of Vulkan pipeline cache saved in 5-27 s, every save over 5 s
 * with a freeze report written meanwhile), and its report and minidump - written while the cache was - made the save
 * slower still. SSBU_WATCHDOG_QUIT=0: still armed while the game closes, as before. */
void watchdog_disarm(void) {
    const char *v = getenv("SSBU_WATCHDOG_QUIT");
    if (v && *v == '0') return;
    U64 since = GetTickCount64() - last_present;
    if (started > 0 && since > 2000) {   /* (a close of a game already frozen - the player gave up on it: its report is wanted) */
        fprintf(stderr, "[watchdog] stays on: no frame for %.1f s before the close\n", since / 1000.0);
        return;
    }
    if (!InterlockedExchange(&disarmed, 1) && started > 0) fprintf(stderr, "[watchdog] off: the game is closing\n");
}

/* A dialog the game waits on (the software keyboard): not a freeze. */
void watchdog_pause(int on) {
    InterlockedAdd(&paused, on ? 1 : -1);
    if (!on) last_present = GetTickCount64();
}
