/* The session's performance summary (v0.5.0): ONE small file a session, reports/perf_session.json, written
 * again every 30 s and when the game closes, for the launcher to send after the game (OpenSmash.Core PerfReport.cs,
 * only for a signed-in player who has not turned it off: config/telemetry.txt "off"). Aggregates only: counts,
 * percentiles and a fixed histogram of the times between frames shown, what held frames (shader waits), the rollback's
 * totals and memory use. Nothing per frame, and nothing that names the player or the PC: no path, no name, no address.
 * SSBU_PERF_SESSION=0: nothing is counted or written.
 *
 * Counted here from three places: ipc_vi.c's show (a frame shown), the renderers' first-use waits (gpu_d3d11.c
 * wait_logged, gpu_vk.c make_note_pipe) and hle_rb_core.c (a rollback's catch-up done, a fight over). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#ifdef _WIN32
#include <psapi.h>
#endif

#define FT_STEP 0.25           /* ms a bucket of the fine histogram (percentiles) */
#define FT_BUCKETS 800         /* to 200 ms; the last also takes what is longer */
static const double hist_edge[] = {8, 12, 15, 17.5, 20, 25, 33.4, 50, 100};   /* the histogram sent: "<8", "<12", ..., ">=100" */
#define HIST_N ((int)(sizeof hist_edge / sizeof *hist_edge) + 1)

static int on = -1;
static double freq, t_start, t_last, t_written;
static unsigned ft[FT_BUCKETS], rbt[FT_BUCKETS];
static unsigned long long frames, hist[HIST_N], stutters, late, shader_waits, rb_rollbacks, rb_frames_again;
static double ft_max, shader_wait_ms, refresh_ms = 1000.0 / 60.0;
static unsigned rb_fights, rb_desyncs, rb_depth_max;
static unsigned long long rb_over;   /* (catch-ups longer than a refresh) */
static unsigned long long commit_peak_mb, avail_min_mb = ~0ull;
static volatile long busy;

static double now_ms(void) {
    LARGE_INTEGER c;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = (double)f.QuadPart / 1000.0; }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / freq;
}
static int is_on(void) {
    if (on < 0) {
        const char *e = getenv("SSBU_PERF_SESSION");
        on = !(e && !strcmp(e, "0"));
        t_start = t_written = now_ms();
    }
    return on;
}
static double pct(const unsigned *h, unsigned long long n, double p) {
    if (!n) return 0;
    unsigned long long want = (unsigned long long)((double)n * p), sum = 0;
    for (int i = 0; i < FT_BUCKETS; i++) { sum += h[i]; if (sum > want) return (i + 0.5) * FT_STEP; }
    return FT_BUCKETS * FT_STEP;
}
static void memory_look(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pm;
    MEMORYSTATUSEX ms;
    pm.cb = sizeof pm; ms.dwLength = sizeof ms;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm) && pm.PeakPagefileUsage >> 20 > commit_peak_mb) commit_peak_mb = pm.PeakPagefileUsage >> 20;
    if (GlobalMemoryStatusEx(&ms) && ms.ullAvailPhys >> 20 < avail_min_mb) avail_min_mb = ms.ullAvailPhys >> 20;
#endif
}

/* The file, whole each time (written beside and renamed over: never half a file). clean: the game is closing by itself. */
void perf_session_write(int clean) {
    if (!is_on() || !frames) return;
#ifdef _WIN32
    if (InterlockedExchange(&busy, 1)) return;
#endif
    memory_look();
    unsigned long long rb_n = 0;
    for (int i = 0; i < FT_BUCKETS; i++) rb_n += rbt[i];
#ifdef _WIN32
    CreateDirectoryA("reports", NULL);
    const char *tmp = "reports\\perf_session.tmp", *path = "reports\\perf_session.json";
#else
    const char *tmp = "reports/perf_session.tmp", *path = "reports/perf_session.json";
#endif
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "{\"schema\":1,\"clean_exit\":%s,\"session_s\":%.0f,\"frames\":%llu,", clean ? "true" : "false", (t_last - t_start) / 1000.0, frames);
        fprintf(f, "\"ft_p50\":%.2f,\"ft_p95\":%.2f,\"ft_p99\":%.2f,\"ft_max\":%.1f,", pct(ft, frames, 0.50), pct(ft, frames, 0.95), pct(ft, frames, 0.99), ft_max);
        fprintf(f, "\"late_pct\":%.3f,\"stutters\":%llu,\"hist\":{", 100.0 * (double)late / (double)frames, stutters);
        for (int i = 0; i < HIST_N; i++) {
            if (i < HIST_N - 1) fprintf(f, "\"<%g\":%llu,", hist_edge[i], hist[i]);
            else fprintf(f, "\">=%g\":%llu},", hist_edge[HIST_N - 2], hist[i]);
        }
        fprintf(f, "\"shader_compiles\":%llu,\"shader_hitch_ms\":%.0f,", shader_waits, shader_wait_ms);
        fprintf(f, "\"rb_fights\":%u,\"rb_rollbacks\":%llu,\"rb_frames_again\":%llu,\"rb_depth_max\":%u,\"rb_tick_p50\":%.2f,\"rb_tick_p99\":%.2f,\"rb_over_pct\":%.3f,\"rb_desyncs\":%u,",
                rb_fights, rb_rollbacks, rb_frames_again, rb_depth_max, pct(rbt, rb_n, 0.50), pct(rbt, rb_n, 0.99),
                rb_n ? 100.0 * (double)rb_over / (double)rb_n : 0.0, rb_desyncs);
        fprintf(f, "\"commit_peak_mb\":%llu,\"ram_avail_min_mb\":%llu}\n", commit_peak_mb, avail_min_mb == ~0ull ? 0 : avail_min_mb);
        fclose(f);
#ifdef _WIN32
        MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
        rename(tmp, path);
#endif
    }
    t_written = now_ms();
#ifdef _WIN32
    InterlockedExchange(&busy, 0);
#endif
}

/* SSBU_PERF_FRAMES=<file> (tests: tools/dev/stutter_run.py): every frame's time since the one before, in ms, a line
 * each, whatever the telemetry setting - the hitches a player sees, frame by frame. */
static void perf_frame_log(void) {
    static FILE *f;
    static int asked;
    static double last;
    static unsigned n;
    if (!asked) { asked = 1; const char *p = getenv("SSBU_PERF_FRAMES"); if (p && *p) f = fopen(p, "w"); }
    if (!f) return;
    double now = now_ms();
    if (n) fprintf(f, "%.2f\n", now - last);
    last = now;
    if (++n % 30 == 0) fflush(f);
}

/* ipc_vi.c: a frame was shown */
void perf_frame_shown(void) {
    perf_frame_log();
    if (!is_on()) return;
    double now = now_ms();
    if (frames) {
        double dt = now - t_last;
        if (dt < 2000) {   /* (a pause longer than that is the game held by a window drag, a debugger or a sleep: not a frame time) */
            int b = (int)(dt / FT_STEP);
            ft[b < 0 ? 0 : b >= FT_BUCKETS ? FT_BUCKETS - 1 : b]++;
            int h = 0;
            while (h < HIST_N - 1 && dt >= hist_edge[h]) h++;
            hist[h]++;
            if (dt > ft_max) ft_max = dt;
            if (dt > 50) stutters++;
            if (dt > refresh_ms * 1.5) late++;
        }
    }
    frames++;
    t_last = now;
    if (now - t_written >= 30000) perf_session_write(0);
}
/* the renderers: a draw waited ms for a shader or a pipeline first used now */
void perf_shader_wait(double ms) {
    if (!is_on() || ms < 1) return;
    shader_waits++; shader_wait_ms += ms;
}
/* hle_rb_core.c: a rollback's catch-up is done: depth frames run again, ms from its load to their end */
void perf_rb_catch(int depth, double ms) {
    if (!is_on()) return;
    rb_rollbacks++; rb_frames_again += (unsigned long long)(depth > 0 ? depth : 0);
    if ((unsigned)depth > rb_depth_max) rb_depth_max = (unsigned)depth;
    int b = (int)(ms / FT_STEP);
    rbt[b < 0 ? 0 : b >= FT_BUCKETS ? FT_BUCKETS - 1 : b]++;
    if (ms > 1000.0 / 60.0) rb_over++;
}
/* hle_rb_core.c: a rollback fight is over, with so many frames found different from the other game's */
void perf_rb_fight(int desyncs) {
    if (!is_on()) return;
    rb_fights++; rb_desyncs += (unsigned)(desyncs > 0 ? desyncs : 0);
}
