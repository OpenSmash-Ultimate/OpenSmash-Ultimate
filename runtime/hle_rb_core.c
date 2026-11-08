/* Rollback netplay (SSBU_RB): the switch, the log and the frame safe point. docs/TECHNICAL.md,
 * "Rollback netplay (SSBU_RB)", has the design. Ported from Piaka11's WindowSmash PR #103 (GPL-3.0),
 * whose design follows ssbu-recomp PR #116 by Rsoto95.
 *
 * SSBU_RB unset: nothing in the rollback files does anything. Its wraps (hle_rb_wraps.c) only
 * call the game's function, and no frame hook is registered (frame_hooks.h).
 * SSBU_RB=1: on. SSBU_RB_LOG=1: more lines in the log. SSBU_RB_QUIET_MS=<ms> (default 4): how
 * long the safe point waits, in a fight, for every other guest thread to be at a wait (M1 only
 * watches; nothing is held yet).
 *
 * THE SAFE POINT (M1). The game's main loop (13.0.5 FUN 0x7103747820) submits the command
 * buffers recorded in the previous iteration, then kicks the task workers: the call at
 * 0x710374a0d0 to FUN 0x7103549720. Right after that call returns (return address
 * 0x710374a0d4) is where ssbu-recomp PR #116 saves and loads its states (its 13.0.4
 * 0x7103749b24, mapped to 13.0.5 by that project's verified table, hostrt.c g_code_13_0_5).
 * FUN 0x7103549720 is called from other places too; only that return address counts.
 *
 * M1 measures, per fight frame, at that point: the host stack pointer (a state that includes
 * the host stack above it needs it to be the same every frame), how long until every other
 * guest thread is asleep in kwait (kernel_rb_census), which threads were not, and the fibers. */
#ifndef _WIN32
/* Rollback netplay is written for Windows (its savestates copy Windows host stacks and fiber
 * blocks): elsewhere the switch is never on, and these stand-ins keep the other files and the
 * wraps (hle_rb_wraps.c) as they are with it off. */
#include "host.h"
int rb_on, rb_log;
int rb_lite_skip(Cpu *c, int group) { (void)c; (void)group; return 0; }
int rb_vi_catchup(void) { return 0; }
int rb_dequeue_pick(u64 freemask, int dflt) { (void)freemask; return dflt; }
void rb_job_kick(Cpu *c, GuestFn *orig) { orig(c); }
void rb_tree_node(Cpu *c, GuestFn *orig) { orig(c); }
void rb_rand_draw(Cpu *c, GuestFn *orig, int which) { (void)which; orig(c); }
int rb_slot_remote(u32 id) { (void)id; return 0; }
int rb_slot_remote_gc(u32 id) { (void)id; return 0; }
u32 rb_slot_phys(u32 id) { return id; }
void rb_pad(u32 id, u64 *buttons, i32 *lx, i32 *ly, i32 *rx, i32 *ry, i32 *lt, i32 *rt) {
    (void)id; (void)buttons; (void)lx; (void)ly; (void)rx; (void)ry; (void)lt; (void)rt;
}
int rb_pad_sampling(u64 *sampling) { (void)sampling; return 0; }
int rb_ra_predicting(void) { return 0; }
void rb_guest_writing(void *p, u64 n) { (void)p; (void)n; }   /* (hle_rb_state.c's state map) */
int rb_page_cold(const void *p) { (void)p; return 0; }
#else
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>
#include <time.h>
#include <stdarg.h>

#include "host.h"
#include "frame_hooks.h"
#include "kernel_watch.h"   /* (kernel_rb_switch) */
#include "runahead.h"

void kernel_rb_census(const void *self, u32 *alive, u32 *waiting, u32 *sleeping, u64 *busy, u32 max, u32 *nbusy);
void fiber_rb_census(u32 *live, u32 *running);

/* ---- the log's [rb] lines, kept (2026-11-04: a "Lost player 1" box on the owner's PC came with a play.log of a later
 * start, the session's own lines overwritten). Every line this file writes to the log is also kept, the last
 * RB_KEEP of them, and a lost player or a desync writes them to reports\rollback_<date>_<time>.txt (rb_keep_dump),
 * with the link's figures: the report stays whatever happens to play.log. */
#define RB_KEEP 3000
#define RB_KEEP_LINE 240
static char (*rb_kept)[RB_KEEP_LINE];
static unsigned rb_kept_n;
static SRWLOCK rb_kept_lock = SRWLOCK_INIT;
static int rb_fprintf(FILE *f, const char *fmt, ...) {
    char t[2048];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(t, sizeof t, fmt, a);
    va_end(a);
    fputs(t, f);
    if (f != stderr) return n;
    AcquireSRWLockExclusive(&rb_kept_lock);
    if (!rb_kept) rb_kept = calloc(RB_KEEP, RB_KEEP_LINE);
    if (rb_kept) {
        SYSTEMTIME st; GetLocalTime(&st);
        for (const char *p = t; *p;) {
            const char *e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            snprintf(rb_kept[rb_kept_n++ % RB_KEEP], RB_KEEP_LINE, "%02d:%02d:%02d.%03d %.*s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     (int)(len < RB_KEEP_LINE ? len : RB_KEEP_LINE), p);
            p += len + (e ? 1 : 0);
        }
    }
    ReleaseSRWLockExclusive(&rb_kept_lock);
    return n;
}
#define fprintf rb_fprintf
static char rb_kept_path[MAX_PATH];
static void rb_keep_dump(const char *why) {
    static double last;
    double now = (double)GetTickCount64();
    if (last && now - last < 20000) return;   /* (one report per event, not one per frame) */
    last = now;
    SYSTEMTIME st; GetLocalTime(&st);
    CreateDirectoryA("reports", NULL);
    snprintf(rb_kept_path, sizeof rb_kept_path, "reports\\rollback_%04d%02d%02d_%02d%02d%02d.txt", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    FILE *f = fopen(rb_kept_path, "w");
    if (!f) { rb_kept_path[0] = 0; return; }
    fputs("Rollback netplay report (hle_rb_core.c rb_keep_dump): the log's last [rb] lines, oldest first.\n", f);
    fprintf(f, "Why: %s\n\n", why);
    AcquireSRWLockShared(&rb_kept_lock);
    unsigned n = rb_kept_n < RB_KEEP ? rb_kept_n : RB_KEEP;
    for (unsigned i = rb_kept_n - n; rb_kept && i < rb_kept_n; i++) { fputs(rb_kept[i % RB_KEEP], f); fputc('\n', f); }
    ReleaseSRWLockShared(&rb_kept_lock);
    fclose(f);
    fprintf(stderr, "[rb] report: %s written (%s)\n", rb_kept_path, why);
}

static void savestates(Cpu *c, u64 frame);

#define SAFE_POINT 0x710374A0D4ull
#define WINDOW 600   /* fight frames per summary line (10 s) */

int rb_on, rb_log;
/* A rollback's load and catch-up under way (set before the load, so vi_rb_load sees it; off when caught up,
 * refused or the fight over). Run-ahead's hidden frames set rb_fast too but never this. */
static volatile int rb_catchup;
static int clock_on = 1;   /* SSBU_RB_CLOCK=0: the real clock in fights too (kernel.c rt_ticks) */
void rb_clock_frame(int fight, u64 gpu_now);   /* (kernel.c) */
static double quiet_limit_ms = 4;

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void frame_end_before(Cpu *c, const u64 args[8], void *ctx);
static void frame_end_after(Cpu *c, const u64 args[8], void *ctx);
static void record_before(Cpu *c, const u64 args[8], void *ctx);
static void record_after(Cpu *c, const u64 args[8], void *ctx);
static int rb_by_default;   /* (rollback netplay on because the game was started online: net.h's net_rollback_default) */
int net_rollback_default(void);   /* (net.h) */

__attribute__((constructor)) static void rb_init(void) {
    const char *log = getenv("SSBU_RB_LOG"), *q = getenv("SSBU_RB_QUIET_MS");
    /* (run-ahead, display.ini's runahead = 1-3, needs the savestates, with the write watch on from the first module;
     * SSBU_RUNAHEAD, which kernel_rb_switch reads itself, wins, 0 included) */
    {   extern int kernel_rb_wanted; int display_runahead(void); const char *e = getenv("SSBU_RUNAHEAD");
        if (!(e && *e) && display_runahead() > 0) kernel_rb_wanted = 1; }
    /* (rollback netplay on by default online: started with --online and a lobby or a room to play in, SSBU_RB not set,
     * online_rollback.txt not "off"; its net mode then the game's own matches, Quickplay and Local Wireless rooms:
     * SSBU_RB_NET=online's, below) */
    {   extern int kernel_rb_online; const char *e = getenv("SSBU_RB");
        if (!(e && *e) && net_rollback_default()) kernel_rb_online = rb_by_default = 1; }
    rb_on = kernel_rb_switch();
    if (!rb_on) return;   /* (off: no hook, no line; the wraps only call the game's functions) */
    rb_log = log && *log && *log != '0';
    if (q && *q) quiet_limit_ms = atof(q);
    const char *ck = getenv("SSBU_RB_CLOCK");
    if (ck && *ck == '0') clock_on = 0;
    /* the frame end and the record builder through their frame hooks (hle_each_frame.c holds the wraps) */
    frame_hook_add(FRAME_HOOK_END, FRAME_ORDER_NETPLAY, frame_end_before, frame_end_after, NULL);
    frame_hook_add(FRAME_HOOK_RECORD, FRAME_ORDER_NETPLAY, record_before, record_after, NULL);
    if (getenv("SSBU_RB") && *getenv("SSBU_RB") && *getenv("SSBU_RB") != '0')
        fprintf(stderr, "[rb] rollback netplay on (SSBU_RB; savestates: F7 save, F8 load)\n");
    else if (rb_by_default)
        fprintf(stderr, "[rb] rollback netplay on (started online, the default; SSBU_RB=0 or Online > Rollback netcode off: the game's own netcode)\n");
    else fprintf(stderr, "[rb] savestates on for run-ahead (SSBU_RUNAHEAD or display.ini's runahead)\n");
}

/* lib::Singleton<app::FighterManager>::instance_ -> FighterManager -> its impl; +0xd2 is set while a fight
 * runs (ready-go to game set), as ssbu-recomp's run-ahead reads it (ra_fight_on). */
static int net_mode;   /* (the net mode: defined with its state below) */
/* (net mode) a match is set up for the players: armed by player 1's record built from the match set-up (0x710178f3b8),
 * not by the other screens that build records too - the controls screen (0x7103302aa0) with its test fight, which
 * waited for players and stayed black (10-31). A fight that starts unarmed is played here alone, as offline. */
static int net_armed;
#define MATCH_SETUP_CALLER 0x710178f3b8ull
/* (the controls screen's test fight sets the fighter manager's + 0xd2 and leaving it doesn't clear it: character select
 * then read as a fight, the pads keyed by a logic frame standing still - the controls dead, both games stuck. In net
 * mode an unarmed screen is never a fight, and a flag left over when a match is armed counts only once the game has
 * cleared it (as it loads the match) and set it again at GO!) */
static int net_leftover;
static int net_online, online_match;              /* (SSBU_RB_NET=online: defined with its section below) */
static void online_forget(const char *why);
static int fight_flag(void) {
    static u64 inst = 1;
    if (inst == 1) {
        inst = rt_symbol("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
        fprintf(stderr, "[rb] FighterManager instance: %#llx%s\n", inst, inst ? "" : " (not exported: fights can't be told apart)");
    }
    if (!inst) return 0;
    u64 mgr = ld64(inst), impl = mgr ? ld64(mgr) : 0;
    return impl && ld8(impl + 0xd2);
}
static int fight_on(void) {
    int on = fight_flag();
    if (net_mode == 2) {
        if (!net_armed) return 0;
        /* (Online mode: a fight with no set-up of its own - Quickplay's next game with the same player reuses the match
         * block, the game's own records back in it - is the game's own netcode's, on both games alike) */
        if (net_online && !online_match && on) { online_forget("a fight with no set-up of its own (a rematch): the game's own netcode plays it"); net_armed = 0; return 0; }
        if (net_leftover) { if (!on) net_leftover = 0; return 0; }
    }
    return on;
}

static struct {
    long long points, fights, frames;   /* safe points seen, fights started, fight frames */
    int in_fight;
    DWORD thread;                       /* the host thread the safe point runs on */
    int thread_changed;
    uintptr_t sp_ref;                   /* the fight's first frame's SP */
    long long sp_same, sp_other;
    uintptr_t sp_lo, sp_hi;
    long long quiet, not_quiet;         /* frames every other thread reached a wait within the limit, or not */
    double quiet_ms[WINDOW]; int nq;    /* time to quiet, for the percentiles */
    u32 alive_max, sleeping_max, fib_live_lo, fib_live_hi, fib_run_max;
    struct { u64 id; long long n; } busy[16]; int nb;
    double last_t, gap_lo, gap_hi, gap_sum; long long ngap;
    long long win;
} S;

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static void summary(const char *why) {
    char busy[400] = ""; size_t n = 0;
    for (int i = 0; i < S.nb && n < sizeof busy - 40; i++)
        n += snprintf(busy + n, sizeof busy - n, "%s#%llu x%lld", i ? ", " : "", S.busy[i].id, S.busy[i].n);
    double p50 = 0, p95 = 0, pmax = 0;
    if (S.nq) {
        qsort(S.quiet_ms, S.nq, sizeof *S.quiet_ms, cmp_d);
        p50 = S.quiet_ms[S.nq / 2]; p95 = S.quiet_ms[S.nq * 95 / 100]; pmax = S.quiet_ms[S.nq - 1];
    }
    fprintf(stderr, "[rb] %s: fight %lld, %lld frames | SP %s (%lld same as frame 1 %#llx, %lld other, range %#llx..%#llx)%s | "
            "quiet within %.1f ms: %lld of %lld (p50 %.2f p95 %.2f max %.2f ms), threads %u, sleeping up to %u, busy: %s | "
            "fibers live %u..%u, running up to %u | frame gap %.1f..%.1f ms (mean %.2f)\n",
            why, S.fights, S.frames, S.sp_other ? "VARIES" : "steady", S.sp_same, (unsigned long long)S.sp_ref, S.sp_other,
            (unsigned long long)S.sp_lo, (unsigned long long)S.sp_hi, S.thread_changed ? " | SAFE POINT CHANGED THREAD" : "",
            quiet_limit_ms, S.quiet, S.quiet + S.not_quiet, p50, p95, pmax, S.alive_max, S.sleeping_max, S.nb ? busy : "none",
            S.fib_live_lo, S.fib_live_hi, S.fib_run_max, S.ngap ? S.gap_lo : 0, S.gap_hi, S.ngap ? S.gap_sum / S.ngap : 0);
    /* the window's figures start again; the SP reference and the fight stay */
    S.sp_same = S.sp_other = 0; S.sp_lo = S.sp_hi = S.sp_ref;
    S.quiet = S.not_quiet = 0; S.nq = 0; S.alive_max = S.sleeping_max = S.fib_run_max = 0;
    S.fib_live_lo = ~0u; S.fib_live_hi = 0; S.nb = 0; S.gap_lo = 1e9; S.gap_hi = 0; S.gap_sum = 0; S.ngap = 0; S.win = 0;
}

static void note_busy(u64 id) {
    for (int i = 0; i < S.nb; i++) if (S.busy[i].id == id) { S.busy[i].n++; return; }
    if (S.nb < 16) { S.busy[S.nb].id = id; S.busy[S.nb++].n = 1; }
}

/* The profile of re-run frames (SSBU_RB_RING): per frame run again, k = 0 for those whose
 * presentation work is left out (rb_fast > 1), 1 for the last one (all of it): the game's part
 * (from the safe point's return to the next one), the safe point's census wait, and the save's
 * parts (hold and GPU drain, memory, the rest). SSBU_RB_PROFILE=<s>: also the sampling profiler
 * (debug_profile.c) for <s> seconds from the first load, sampling only while frames run again. */
extern volatile LONG rb_fast;
static double fp_census_last, fp_hash_last, fp_in, fp_out, fp_sum[2][7];   /* (fp_hash_last: the test's own fight hash, this frame) */
static long long fp_n[2];
static int fp_fast, fp_loaded;
extern double rb_save_ms[3];
extern double rb_save_parts[4]; extern long long rb_save_count;   /* (hle_rb_state.c: parking, GPU drain, memory, in all) */
void profile_start_gated(double seconds);
static void tape_clear(void);
static void tag_restore(const char *why);
static void net_left_send(void);
static void net_gone_reset(void);
static int ra_engaged;              /* (run-ahead, below: on in this fight) */
static void ra_fight_over(void);
static void online_restore(void);   /* (SSBU_RB_NET=online, below) */
static void online_fight_start(void);
static void online_forget(const char *why);
static void safe_point(Cpu *c, uintptr_t sp) {
    S.points++;
    int on = fight_on();
    if (!on) {
        if (S.in_fight) {
            summary("fight over"); S.in_fight = 0;
            online_restore();   /* (an Online match: the game's own set-up back for its results) */
            { void rb_state_map_fight(int on); rb_state_map_fight(0); }   /* (hle_rb_state.c: the alias area writable again) */
            ra_fight_over();   /* (before the catch-up's end below: run-ahead's hidden frame is not one) */
            if (net_mode == 2) net_left_send();   /* (the others stop waiting for this game's pads: v0.9) */
            /* (the other players' name tags out of this PC's list now: the game saves its list as the results come,
             * and a slot put back only after them had been saved with the friend's tag in it - gc2: 3 copies) */
            tag_restore("the fight is over");
            /* A catch-up still running when the fight ended (a rollback at the last KO): its mode - presentation
             * work left out, the vsync thread unpaced - was only switched off at fight safe points, so it stayed on
             * through the results screen, which crashed in the job system (stress3, twopc13: main+0x35c4580, both
             * on the game that rolled back, both as the fight ended). Ended here, and any held threads let go. */
            if (rb_fast > 0) {
                fprintf(stderr, "[rb] the fight ended in the middle of a catch-up (%ld frames left): catch-up mode off\n", (long)rb_fast);
                InterlockedExchange(&rb_fast, 0);
            }
            rb_catchup = 0;
            { void rb_release_held(void); rb_release_held(); }
            if (net_mode == 2) { void rb_ring_drop(void); rb_ring_drop(); }   /* (the GO! freeze: no undo of the next loading at the next fight's first save) */
        }
        return;
    }
    double t = now_ms();
    if (!S.in_fight) {
        tape_clear();
        S.in_fight = 1; S.fights++; S.frames = 0; S.thread = GetCurrentThreadId(); S.thread_changed = 0;
        S.sp_ref = sp; S.last_t = 0;
        { void rb_state_map_fight(int on); rb_state_map_fight(1); }   /* (hle_rb_state.c: the fight's first save turns the state map on) */
        summary("fight start");   /* (clears the window; its numbers are all zero) */
        if (net_mode == 2) net_gone_reset();
        online_fight_start();
        S.sp_same = 0;
    } else if (S.last_t) {
        double g = t - S.last_t;
        if (g < S.gap_lo) S.gap_lo = g;
        if (g > S.gap_hi) S.gap_hi = g;
        S.gap_sum += g; S.ngap++;
    }
    S.frames++;
    if (GetCurrentThreadId() != S.thread) S.thread_changed = 1;
    if (sp == S.sp_ref) S.sp_same++; else S.sp_other++;
    if (sp < S.sp_lo) S.sp_lo = sp;
    if (sp > S.sp_hi) S.sp_hi = sp;

    u32 alive, waiting, sleeping, nbusy; u64 busy[16];
    double t0 = now_ms(), waited;
    for (;;) {
        kernel_rb_census(c->thread, &alive, &waiting, &sleeping, busy, 16, &nbusy);
        waited = now_ms() - t0;
        if (!nbusy || waited >= quiet_limit_ms || rb_fast > 0 || ra_engaged) break;   /* (no waiting in the frames of a catch-up or of run-ahead: M1's measure only) */
        SwitchToThread();
    }
    fp_census_last = waited;
    if (!nbusy) { S.quiet++; if (S.nq < WINDOW) S.quiet_ms[S.nq++] = waited; }
    else { S.not_quiet++; for (u32 i = 0; i < nbusy && i < 16; i++) note_busy(busy[i]); }
    if (alive > S.alive_max) S.alive_max = alive;
    if (sleeping > S.sleeping_max) S.sleeping_max = sleeping;
    u32 live, running;
    fiber_rb_census(&live, &running);
    if (live < S.fib_live_lo) S.fib_live_lo = live;
    if (live > S.fib_live_hi) S.fib_live_hi = live;
    if (running > S.fib_run_max) S.fib_run_max = running;
    if (rb_log && S.frames <= 5)
        fprintf(stderr, "[rb] fight frame %lld: SP %#llx, %u threads, %u waiting, %u sleeping, %u busy after %.2f ms, fibers %u (%u running)\n",
                S.frames, (unsigned long long)sp, alive, waiting, sleeping, nbusy, waited, live, running);
    S.last_t = now_ms();   /* (after the census: the gap is the game's own time) */
    if (++S.win == WINDOW) summary("in fight");
}

/* M2: savestates, at the safe point. F7 saves, F8 loads (F1-F6 are the runtime's own keys).
 * SSBU_RB_AUTO=<frame>,<every>[,<count>]: in a fight, save at fight frame <frame>, then load it
 * every <every> frames, <count> times (default 100): the stress test. A load comes back through
 * rb_save's call (hle_rb_state.c), so nothing may follow these calls but the return. */
int rb_save(Cpu *c, u64 frame);   /* 1 saved, 0 not, 2 a load came back here */
void rb_load(Cpu *c, u64 frame);
void rb_load_age(Cpu *c, u64 frame, int age);
void rb_ring_size(int n);
int rb_ring_count(void);
void rb_quiet(int q);
int gfx_window_focused(void);
static long long auto_at = -1, auto_every, auto_left = 100, auto_next;
static long long bench_from = -1, bench_n;

/* SSBU_RB_RING=<frame>,<every>,<depth>,<count>[,<states kept>] (M3, the ring): from fight frame
 * <frame>, a save at every frame; every <every> frames, a load of the state <depth> frames back,
 * <count> times, and the game goes on from there (the frames since run again, with their input
 * from the tape: at normal speed here; rollback proper runs them within one frame, M4). Each fight
 * frame's state (fight_frame) is kept by logic frame the first time it runs; when it runs again
 * after a load, it must be the same. */
static long long ring_from = -1, ring_every, ring_depth, ring_left, ring_since, ring_loads;
static long long ring_same, ring_diff, ring_saves;
static double ring_save_ms, ring_save_max, ring_load_ms, ring_load_max, ring_load_t0;
static struct FFrame *ring_first;   /* by logic frame */
/* M4: after a load, the frames up to where the game was run again without pacing and unseen
 * (ipc_vi.c's vsync thread neither waits for the refresh nor presents while rb_fast > 0), as
 * rollback must, within a frame. SSBU_RB_FAST=0: at normal speed, shown (the M3 test). */
extern volatile LONG rb_fast;   /* (kernel.c) */
static int skip_mask = -1, lite_mask = -1;
static long long kicks_skipped, lite_skipped;
static int fast_on = -1;
static long long caught_up;
static double catchup_ms, catchup_max;
#define RING_FRAMES 65536
static void ring_test(Cpu *c, u64 frame);

/* SSBU_RB_VERIFY=<frame>,<k>[,<rounds>]: the replay check. In a fight, save at fight frame <frame>,
 * run <k> frames and keep a copy of guest memory (the reference), then <rounds> times (default 5):
 * load, run the same <k> frames, compare with the reference (hle_rb_state.c rb_ref_compare). The
 * pads must be the same each time: nobody touches the controls. */
void rb_ref_capture(Cpu *c);
void rb_ref_compare(Cpu *c, long long round);
static long long ver_at = -1, ver_k, ver_rounds = 5, ver_since, ver_round;
static int ver_phase;   /* 0 waiting, 1 running to the reference, 2 replaying, 3 done */

/* ---- the input tape --------------------------------------------------------------------
 * Rollback replays frames with the input they had. In a fight, each player's pad state
 * (hle_hid.c npad_state, after the pad, keyboard and SSBU_INPUT) is keyed by the logic frame:
 * the game's counter at 0x71052c5184, which moves once per logic frame and is guest memory, so
 * a load puts it back. The first read of a frame records what the pad gives; every later read of
 * that frame, in this pass or in a replay after a load, gets the recorded state. The sampling
 * number the game sees follows the same counter in a fight, so it is the same in a replay too.
 * SSBU_RB_TAPE=0: no tape (live input in replays: the control).
 * SSBU_RB_INPUT="<period>;<frame>:<button>[/<hold frames>],...": test input for player 1 on WALL
 * time (60ths of a second since start, modulo <period>), so a replay's live input differs from
 * the first pass's and only the tape can make them the same. Buttons: A B X Y L R ZL ZR, and
 * SLEFT SUP SRIGHT SDOWN for the left stick. */
typedef struct PadRec { u8 have; u64 buttons; i32 lx, ly, rx, ry, lt, rt; } PadRec;
static struct { PadRec *rec; u32 cap; } tape[8];
static SRWLOCK tape_lock = SRWLOCK_INIT;
static int tape_on = -1;
static long long tape_recorded, tape_replayed;
static volatile LONG64 rb_sampling, rb_sampling_base;
static struct { int at, hold; u64 button; int stick; } tin[64];
static int ntin = -1, tin_period;
#define LOGIC_FRAME 0x71052c5184ull

static void test_input_parse(void) {
    ntin = 0;
    const char *s = getenv("SSBU_RB_INPUT");
    if (!s || sscanf(s, "%d", &tin_period) != 1 || tin_period <= 0 || !(s = strchr(s, ';'))) return;
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR"};
    static const char *const sticks[] = {"SLEFT", "SUP", "SRIGHT", "SDOWN"};
    for (s++; *s && ntin < 64;) {
        char name[16] = {0}; int at, hold = 5, n = 0;
        if (sscanf(s, "%d:%15[A-Z]%n", &at, name, &n) < 2) break;
        s += n;
        if (*s == '/') { n = 0; sscanf(s + 1, "%d%n", &hold, &n); s += 1 + n; }
        tin[ntin].at = at; tin[ntin].hold = hold; tin[ntin].button = 0; tin[ntin].stick = -1;
        for (int b = 0; b < 10; b++) if (!strcmp(name, names[b])) tin[ntin].button = 1ull << b;
        for (int k = 0; k < 4; k++) if (!strcmp(name, sticks[k])) tin[ntin].stick = k;
        if (tin[ntin].button || tin[ntin].stick >= 0) ntin++;
        if (*s == ',') s++;
    }
    fprintf(stderr, "[rb] test input on wall time: %d presses, every %d frames\n", ntin, tin_period);
}

static void tape_clear(void) {
    AcquireSRWLockExclusive(&tape_lock);
    for (int i = 0; i < 8; i++) if (tape[i].rec) memset(tape[i].rec, 0, sizeof(PadRec) * tape[i].cap);
    ReleaseSRWLockExclusive(&tape_lock);
    rb_sampling_base = rb_sampling + 1000;
}

static double seg_poll, seg_fe_in, seg_fe_out;   /* (the profile of re-run frames: this frame's marks) */
static int net_pad_for(u32 id, u32 key, u64 *buttons, i32 *lx, i32 *ly, i32 *rx, i32 *ry, i32 *lt, i32 *rt);
/* (net mode: from a match set-up until the fight, players 1 and 2 hold nothing: in the entrance and countdown
 * each PC would read its own controllers in both slots - different devices, not exchanged.) */
static volatile LONG net_countdown;
static int net_n;   /* (players in net mode: defined with the net mode's state below) */
/* SSBU_RB_NET_EARLY=1: the extras made while two PCs' fights started apart for another reason (twopc2-5, the
 * lobbies wired differently): dropped refreshes cleared outside fights too, and the fixed clock from the match
 * set-up. Off by default from twopc13 on (to be shown unneeded). */
static int net_early = -1;
static int net_early_on(void) { if (net_early < 0) { const char *e = getenv("SSBU_RB_NET_EARLY"); net_early = e && *e == '1'; } return net_early; }
static int net_local;                 /* (this game's player: defined with the net mode's state below) */
volatile LONG net_results;            /* (the results screen: see net_results_frame) */
volatile LONG net_menu_mine;          /* (this game's player's A and B there, as rb_pad read them) */
volatile LONG net_menu_remote[8];     /* (each other player's, from its game) */
/* (offline) Is the game paused: the logic frame the same as at the last fight frame, outside a load's re-run frames?
 * Worked out once a fight frame; pz_extra counts paused frames, to keep the pads' sampling number moving. */
static u32 pz_logic = 0xffffffffu; static long long pz_frames = -1; static int pz_paused; static volatile LONG64 pz_extra;
static int rb_paused(void) {
    if (S.frames != pz_frames) {
        u32 lf = ld32(LOGIC_FRAME);
        pz_paused = rb_fast <= 0 && lf == pz_logic;
        if (pz_paused) InterlockedIncrement64(&pz_extra);
        pz_logic = lf; pz_frames = S.frames;
    }
    return pz_paused;
}
void rb_pad(u32 id, u64 *buttons, i32 *lx, i32 *ly, i32 *rx, i32 *ry, i32 *lt, i32 *rt) {
    if (net_countdown && !S.in_fight && id < (u32)net_n) { *buttons = 0; *lx = *ly = *rx = *ry = *lt = *rt = 0; return; }
    if (net_results && !S.in_fight && id < (u32)net_n) {   /* (the results screen: this player's A and B on every card) */
        if ((int)id == net_local) InterlockedExchange(&net_menu_mine, (LONG)(*buttons & 3));
        else { *buttons = (u64)(net_menu_mine | net_menu_remote[id]); *lx = *ly = *rx = *ry = *lt = *rt = 0; }
        return;
    }
    if (!rb_on || !S.in_fight || id >= 8) return;
    /* The pause menu (10-31: it hung, offline and online). Every pad in a fight is keyed by the logic frame, which
     * stands still while paused, so the + that opened it came back on every read and nothing got out. In net mode
     * + is taken out of every player's pads in a fight (as the official online play has no pause; version 11: every
     * game does it); offline, while the logic frame stands still the pads are live, not the tape. */
    /* (v0.9.3: only in an online fight - Training, or the controls screen's test fight, played here alone, pauses as
     * offline: + was taken there too, and Training has no other way out) */
    if (net_mode == 2 && net_armed) *buttons &= ~(1ull << 10);
    else if (rb_paused()) return;
    if (id == 0 && !seg_poll) seg_poll = now_ms();
    if (ntin < 0) test_input_parse();
    if (tape_on < 0) { const char *e = getenv("SSBU_RB_TAPE"); tape_on = !(e && *e == '0'); }
    if (id == 0 && ntin > 0) {
        int f = (int)(rt_now_ns() / 16666667ull % (u64)tin_period);
        for (int i = 0; i < ntin; i++) {
            if (f < tin[i].at || f >= tin[i].at + tin[i].hold) continue;
            *buttons |= tin[i].button;
            switch (tin[i].stick) {
            case 0: *lx = -32767; break;
            case 1: *ly = 32767; break;
            case 2: *lx = 32767; break;
            case 3: *ly = -32767; break;
            }
        }
    }
    if (net_pad_for(id, ld32(LOGIC_FRAME), buttons, lx, ly, rx, ry, lt, rt)) {   /* (loop mode: the bots, M5) */
        if (net_mode == 2) *buttons &= ~(1ull << 10);   /* (the other players' too, as they came) */
        return;
    }
    {   void ra_pad_read(u32 id, u64 buttons); ra_pad_read(id, *buttons); }
    if (!tape_on || ra_engaged) return;   /* (run-ahead: every read live; a tape would hand the real frame the predicted one's read) */
    u32 key = ld32(LOGIC_FRAME);
    if (key >= (1u << 20)) return;
    AcquireSRWLockExclusive(&tape_lock);
    if (key >= tape[id].cap) {
        u32 cap = tape[id].cap ? tape[id].cap : 4096;
        while (cap <= key) cap *= 2;
        PadRec *r = realloc(tape[id].rec, sizeof(PadRec) * cap);
        if (r) { memset(r + tape[id].cap, 0, sizeof(PadRec) * (cap - tape[id].cap)); tape[id].rec = r; tape[id].cap = cap; }
    }
    if (key < tape[id].cap) {
        PadRec *r = &tape[id].rec[key];
        if (r->have) {
            *buttons = r->buttons; *lx = r->lx; *ly = r->ly; *rx = r->rx; *ry = r->ry; *lt = r->lt; *rt = r->rt;
            tape_replayed++;
        } else {
            r->have = 1; r->buttons = *buttons; r->lx = *lx; r->ly = *ly; r->rx = *rx; r->ry = *ry; r->lt = *lt; r->rt = *rt;
            tape_recorded++;
        }
    }
    ReleaseSRWLockExclusive(&tape_lock);
    {   /* the first reads in a fight: which thread, which logic frame, at which safe point */
        static int shown; static long long fight_shown;
        if (fight_shown != S.fights) { fight_shown = S.fights; shown = 0; }
        if (id == 0 && shown < 12) {
            shown++;
            fprintf(stderr, "[rb] pad read: player 1, thread %lu, logic frame %u, fight frame %lld, buttons %#llx\n",
                    GetCurrentThreadId(), key, S.frames, (unsigned long long)*buttons);
        }
    }
}

/* In a fight: the logic frame on from a base above every number given before (monotonic). */
int rb_pad_sampling(u64 *sampling) {
    if (!rb_on) return 0;
    if (!S.in_fight) { *sampling = (u64)InterlockedIncrement64(&rb_sampling); return 1; }
    /* (offline, paused: on by one a frame, so the game sees each new press; never back after: the count stays) */
    if (net_mode != 2) rb_paused();
    *sampling = (u64)rb_sampling_base + ld32(LOGIC_FRAME) + (net_mode != 2 ? (u64)pz_extra : 0);
    if ((LONG64)*sampling > rb_sampling) InterlockedExchange64(&rb_sampling, (LONG64)*sampling);   /* (after the fight: on from here) */
    return 1;
}

/* ---- the fight hash: what the fight itself is, per frame -------------------------------
 * At each safe point of the replay check (and of every fight frame with SSBU_RB_HASH=1, one
 * line per fighter in build\rb_hash.txt), each fighter's state is read through the game's own
 * exported getters, as ssbu-recomp's fighter_host.c SSBU_FIGHT_HASH reads it: entry slot ->
 * app::fighter::get_id_from_entry_id -> app::sv_battle_object::module_accessor (the "boma"),
 * then status kind, situation, motion kind and frame, position, facing, speed and damage. Plus
 * the 9 script random generators (app::sv_math::rand: 9 x 20 bytes, [[[0x71052c35b0]]], 13.0.4
 * 0x71052c25b0 one page on as all of main's data). The calls run on the main thread at the
 * safe point, its registers kept around them (they use the guest stack below its pointer). */
typedef struct FRec { int entry, st, sit, lr; u64 mot; float mf, x, y, sx, sy, dmg; } FRec;
typedef struct FFrame { u32 n; u32 rng_calls; u64 rng; u64 h; FRec f[8]; } FFrame;
#define LB(s) "_ZN3app8lua_bind" s "EPNS_26BattleObjectModuleAccessorE"
static u64 hfnv(u64 h, const void *p, size_t n) { const u8 *b = p; while (n--) { h ^= *b++; h *= 0x100000001b3ull; } return h; }
static int readable(u64 a, u64 n) {
    MEMORY_BASIC_INFORMATION mi;
    return a && VirtualQuery((void *)(uintptr_t)G2H(a), &mi, sizeof mi) && mi.State == MEM_COMMIT &&
           !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (uintptr_t)G2H(a) + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}
/* (M6: the chain is found once a fight, then only followed: each VirtualQuery costs PC 2 some 5 ms over this
 * build's write-watched areas, and a fight hash asked 8 of them, 40 ms a hash there against 4 here (twopc19).
 * The pointers read the same as when they were checked: the same memory, still there, since nothing in a fight
 * gives the generators up.) */
double rb_rng_slow_ms; long long rb_rng_slow_n;
double rb_ff_get_ms; long long rb_ff_get_n;   /* (fight_frame's calls into the game: the getters) */
static u64 rng_base(void) {
    static long long fight = -1;
    static u64 chain[3];
    u64 p = 0x71052c35b0ull;
    if (fight == S.fights && chain[2] && ld64(p) == chain[0] && ld64(chain[0]) == chain[1] && ld64(chain[1]) == chain[2]) return chain[2];
    LARGE_INTEGER q0, q1, fq; QueryPerformanceCounter(&q0);
    u64 out = 0;
    for (int i = 0; i < 3; i++) { if (!readable(p, 8)) goto done; p = ld64(p); chain[i] = p; }
    if (readable(p, 180)) out = p;
done:
    fight = out ? S.fights : -1;
    if (!out) chain[2] = 0;
    QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&fq);
    rb_rng_slow_ms += (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart; rb_rng_slow_n++;
    return out;
}
static float getf(Cpu *c, const char *sym, u64 boma, int n, u64 arg) {
    rt_callsym(c, sym, n, boma, arg);
    u32 b = (u32)c->v[0].d[0]; float f; memcpy(&f, &b, 4); return f;
}
static void fight_frame(Cpu *c, FFrame *o) {
    static Cpu keep;   /* (the main thread's only) */
    keep = *c;
    memset(o, 0, sizeof *o);
    double t0 = now_ms();
    for (int e = 0; e < 8; e++) {
        u32 id = (u32)rt_callsym(c, "_ZN3app7fighter20get_id_from_entry_idEi", 1, (u64)e);
        if (id == 0x50000000u) continue;
        u64 boma = rt_callsym(c, "_ZN3app16sv_battle_object15module_accessorEj", 1, (u64)id);
        if (!boma) continue;
        FRec *r = &o->f[o->n++];
        r->entry = e;
        r->st = (int)rt_callsym(c, LB("30StatusModule__status_kind_impl"), 1, boma);
        r->sit = (int)rt_callsym(c, LB("33StatusModule__situation_kind_impl"), 1, boma);
        r->mot = rt_callsym(c, LB("30MotionModule__motion_kind_impl"), 1, boma);
        r->mf = getf(c, LB("24MotionModule__frame_impl"), boma, 1, 0);
        r->x = getf(c, LB("25PostureModule__pos_x_impl"), boma, 1, 0);
        r->y = getf(c, LB("25PostureModule__pos_y_impl"), boma, 1, 0);
        float lr = getf(c, LB("22PostureModule__lr_impl"), boma, 1, 0); r->lr = lr < 0 ? -1 : 1;
        r->sx = getf(c, LB("35KineticModule__get_sum_speed_x_impl") "i", boma, 2, (u64)(u32)-1);
        r->sy = getf(c, LB("35KineticModule__get_sum_speed_y_impl") "i", boma, 2, (u64)(u32)-1);
        r->dmg = getf(c, LB("25DamageModule__damage_impl") "i", boma, 2, 0);
        if (o->n == 8) break;
    }
    rb_ff_get_ms += now_ms() - t0; rb_ff_get_n++;
    *c = keep;
    u64 rb = rng_base();
    o->rng = 0xcbf29ce484222325ull;
    if (rb) { u32 w[45]; for (int i = 0; i < 45; i++) w[i] = ld32(rb + 4ull * i); for (int i = 0; i < 9; i++) o->rng_calls += w[i * 5 + 4]; o->rng = hfnv(o->rng, w, sizeof w); }
    o->h = hfnv(hfnv(0xcbf29ce484222325ull, o->f, sizeof(FRec) * o->n), &o->rng, 8);
}
static void frec_text(char *s, size_t n, const FRec *r) {
    snprintf(s, n, "P%d st %#x sit %d mot %#llx mf %.3f pos %.4f,%.4f lr %d speed %.4f,%.4f dmg %.2f%%",
             r->entry + 1, r->st, r->sit, (unsigned long long)r->mot, r->mf, r->x, r->y, r->lr, r->sx, r->sy, r->dmg);
}
static FFrame *ref_frames;      /* the reference run's frames 1..k */
static long long hash_first_bad;
static FILE *hash_out;
static void fight_hash(Cpu *c, long long verify_frame) {
    static int all = -1;
    if (all < 0) { const char *e = getenv("SSBU_RB_HASH"); all = e && *e == '1'; }
    if (!all && verify_frame <= 0) return;
    FFrame f;
    fight_frame(c, &f);
    if (all) {
        if (!hash_out) {
            hash_out = fopen("build\\rb_hash.txt", "w");
            fprintf(stderr, "[rb] fight hash: every fight frame, one line per fighter, in build\\rb_hash.txt; script random generators at %#llx\n", rng_base());
        }
        if (hash_out) {
            char t[256];
            for (u32 i = 0; i < f.n; i++) { frec_text(t, sizeof t, &f.f[i]); fprintf(hash_out, "F %lld %s rng %u.%08llx H %016llx\n", S.frames, t, f.rng_calls, (unsigned long long)(f.rng & 0xffffffff), (unsigned long long)f.h); }
            if (!(S.frames & 63)) fflush(hash_out);
        }
    }
    if (verify_frame <= 0 || verify_frame > ver_k) return;
    if (ver_phase == 1) {
        if (!ref_frames) ref_frames = calloc((size_t)ver_k + 1, sizeof *ref_frames);
        if (ref_frames) ref_frames[verify_frame] = f;
        if (verify_frame == 1) fprintf(stderr, "[rb] fight hash: %u fighters, script random generators %s\n", f.n, rng_base() ? "found" : "NOT found");
        return;
    }
    if (!ref_frames || hash_first_bad) return;
    const FFrame *r = &ref_frames[verify_frame];
    if (r->h == f.h) return;
    hash_first_bad = verify_frame;
    fprintf(stderr, "[rb] fight hash: FIRST DIFFERENCE at frame %lld after the load (reference %016llx, replay %016llx; generators %u calls %s)\n",
            verify_frame, (unsigned long long)r->h, (unsigned long long)f.h, f.rng_calls, r->rng == f.rng ? "same" : "DIFFER");
    char a[256], b[256];
    for (u32 i = 0; i < f.n || i < r->n; i++) {
        frec_text(a, sizeof a, &r->f[i]); frec_text(b, sizeof b, &f.f[i]);
        if (memcmp(&r->f[i], &f.f[i], sizeof(FRec))) fprintf(stderr, "[rb]   reference: %s\n[rb]   replay:    %s\n", a, b);
    }
}

extern void *rb_main_kthread;
extern volatile u64 rb_main_wait_ns, rb_main_waits, rb_vsync_gpu_wait_qpc, rb_vsync_gpu_waits;
static void tree_report(void);
static double seg_sum[4];
static long long seg_n;
static void seg_report(void) {
    if (!seg_n) return;
    fprintf(stderr, "[rb] profile, re-run frames by part (%lld): safe point -> pad poll %.2f ms, poll -> frame end (logic) %.2f, "
            "frame end call %.2f, frame end -> safe point %.2f (means)\n", seg_n, seg_sum[0] / seg_n, seg_sum[1] / seg_n, seg_sum[2] / seg_n, seg_sum[3] / seg_n);
}
static void fp_waits_report(void) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    long long frames = fp_n[0] + fp_n[1];
    if (!frames) return;
    fprintf(stderr, "[rb] profile, frames run again (%lld): the main thread asleep in kwait %.2f ms a frame (%.1f waits); "
            "the vsync thread waiting for the GPU %.2f ms a frame (%llu waits)\n", frames,
            rb_main_wait_ns / 1e6 / frames, (double)rb_main_waits / frames,
            rb_vsync_gpu_wait_qpc * 1000.0 / (double)f.QuadPart / frames, (unsigned long long)rb_vsync_gpu_waits);
}
static void fp_report(int k) {
    double n = (double)fp_n[k];
    fprintf(stderr, "[rb] profile, %s frames run again (%lld): game %.2f ms, census wait %.2f, save: hold + GPU drain %.2f, memory %.2f, "
            "the rest %.2f; the test's fight hash %.2f; other safe point work %.2f (means)\n", k ? "the last" : "lite", fp_n[k],
            fp_sum[k][0] / n, fp_sum[k][1] / n, fp_sum[k][2] / n, fp_sum[k][3] / n, fp_sum[k][4] / n, fp_sum[k][6] / n, fp_sum[k][5] / n);
}
/* SSBU_RB_RING_ROTATE=1: the loads go back 1, 2 .. <depth> frames in turn, with the catch-up
 * timed for each depth. */
static int ring_rotate = -1, cur_depth;
static double depth_catch[65], depth_load[65], depth_rerun[65];
static long long depth_n[65];
/* SSBU_RB_RERUN_SAVE=<n>: of the frames a catch-up runs again, every n-th is saved, and the last
 * (default 3; 1: all of them; 0: only the last). A later load back to an unsaved one takes the newest
 * state before it and runs the rest again: at most n - 1 frames more. States are found by their
 * logic frame (rb_save_tag / rb_ring_find), not by how many saves back. */
static int rerun_save = -1;
static LONG pending_fast, catch_total;
extern u32 rb_save_tag;
int rb_ring_find(u32 target, u32 *tag);
u32 rb_ring_tag(int age);
extern long long rb_fiber_refused, rb_fiber_retried_ok;   /* (hle_rb_state.c, M7) */
void rb_keep_hold_next(void);
void rb_release_held(void);
static void ring_test(Cpu *c, u64 frame) {
    if (fast_on < 0) { const char *e = getenv("SSBU_RB_FAST"); fast_on = !(e && *e == '0'); }
    if (ring_rotate < 0) { const char *e = getenv("SSBU_RB_RING_ROTATE"); ring_rotate = e && *e == '1'; }
    if (rb_fast > 0 && InterlockedDecrement(&rb_fast) == 0) {   /* caught up: the load and every frame since */
        rb_catchup = 0;
        double ms = now_ms() - ring_load_t0;
        caught_up++; catchup_ms += ms; if (ms > catchup_max) catchup_max = ms;
        if (cur_depth > 0 && cur_depth <= 64) { depth_catch[cur_depth] += ms; depth_n[cur_depth]++; }
    }
    /* this frame's state, against its first pass */
    if (!ring_first) ring_first = calloc(RING_FRAMES, sizeof *ring_first);
    u32 key = ld32(LOGIC_FRAME);
    FFrame f;
    /* (its getter calls cost some 5 ms a frame: in the middle of a catch-up only with
     * SSBU_RB_HASHALL=1; by default the last re-run frame and every normal one are checked) */
    static int hash_all = -1;
    if (hash_all < 0) { const char *e = getenv("SSBU_RB_HASHALL"); hash_all = e && *e == '1'; }
    int hash_now = hash_all || rb_fast <= 0;
    double th = now_ms();
    if (hash_now) fight_frame(c, &f);
    fp_hash_last = now_ms() - th;
    if (hash_now && ring_first && key < RING_FRAMES) {
        FFrame *r = &ring_first[key];
        if (!r->h) *r = f;
        else if (r->h == f.h) ring_same++;
        else if (++ring_diff <= 5) {
            char a[256], b[256];
            fprintf(stderr, "[rb] ring: logic frame %u runs again DIFFERENT (first %016llx, now %016llx; generators %s)\n",
                    key, (unsigned long long)r->h, (unsigned long long)f.h, r->rng == f.rng ? "same" : "DIFFER");
            for (u32 i = 0; i < f.n || i < r->n; i++) {
                frec_text(a, sizeof a, &r->f[i]); frec_text(b, sizeof b, &f.f[i]);
                if (memcmp(&r->f[i], &f.f[i], sizeof(FRec))) fprintf(stderr, "[rb]   first: %s\n[rb]   again: %s\n", a, b);
            }
        }
    }
    if (ring_left <= 0) {   /* done: a line, then nothing more */
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "[rb] ring test done: %lld loads, %lld saves; frames run again: %lld the same, %lld DIFFERENT; "
                    "save %.2f ms mean (max %.1f), load %.2f ms mean (max %.1f)\n", ring_loads, ring_saves, ring_same, ring_diff,
                    ring_saves ? ring_save_ms / ring_saves : 0, ring_save_max, ring_loads ? ring_load_ms / ring_loads : 0, ring_load_max);
            if (caught_up)
                fprintf(stderr, "[rb] ring test: back %lld frames and caught up again, unseen: %.1f ms mean (max %.1f) over %lld loads, "
                        "%.1f ms a frame run again (with its save); left out of the re-run frames: %lld kicks (mask %d), %lld presentation calls (mask %d)\n",
                        ring_depth, catchup_ms / caught_up, catchup_max, caught_up,
                        (catchup_ms / caught_up - (ring_loads ? ring_load_ms / ring_loads : 0)) / ring_depth, kicks_skipped, skip_mask, lite_skipped, lite_mask);
            for (int k = 0; k < 2; k++)
                if (fp_n[k])
                    fp_report(k);
            fp_waits_report();
            tree_report();
            seg_report();
            { void rb_stamp_report(void); rb_stamp_report(); }
            for (int d = 1; d <= 64; d++)
                if (depth_n[d])
                    fprintf(stderr, "[rb] ring test: back %d and caught up: %.1f ms mean over %lld loads (the load %.1f ms; %.1f frames run again)\n",
                            d, depth_catch[d] / depth_n[d], depth_n[d], depth_load[d] / depth_n[d], depth_rerun[d] / depth_n[d]);
        }
        return;
    }
    if (rerun_save < 0) { const char *e = getenv("SSBU_RB_RERUN_SAVE"); rerun_save = e && *e ? atoi(e) : 3; }
    if (rb_fast > 0) {   /* a frame in the middle of a catch-up: saved if it is an n-th; no load */
        LONG done = catch_total - rb_fast;   /* (re-run frames finished, this one included) */
        if (rerun_save != 1 && (rerun_save == 0 || done % rerun_save)) { rb_save_ms[0] = rb_save_ms[1] = rb_save_ms[2] = 0; return; }
    }
    double t0 = now_ms();
    rb_save_tag = key;
    /* (a load will follow this save at once if it is time and no catch-up runs: keep the hold) */
    static int keep_hold_on = -1;
    if (keep_hold_on < 0) { const char *e = getenv("SSBU_RB_KEEPHOLD"); keep_hold_on = !(e && *e == '0'); }
    if (keep_hold_on && ring_since + 1 >= ring_every && rb_fast <= 0) rb_keep_hold_next();
    int r = rb_save(c, frame);
    if (r == 2) {   /* a load came back here: the state `ring_depth` frames back */
        double ms = now_ms() - ring_load_t0;
        ring_load_ms += ms; if (ms > ring_load_max) ring_load_max = ms;
        if (cur_depth > 0 && cur_depth <= 64) depth_load[cur_depth] += ms;
        if (cur_depth > 0 && cur_depth <= 64) depth_rerun[cur_depth] += pending_fast;
        catch_total = pending_fast;
        if (fast_on) rb_fast = pending_fast;   /* the frames back to where the game was: fast and unseen */
        return;
    }
    if (r == 1) {
        double ms = now_ms() - t0;
        ring_saves++; ring_save_ms += ms; if (ms > ring_save_max) ring_save_max = ms;
    }
    int depth = ring_rotate ? 1 + (int)(ring_loads % ring_depth) : (int)ring_depth;
    u32 found = 0;
    int age = key >= (u32)depth ? rb_ring_find(key - (u32)depth, &found) : -1;
    int loading = ++ring_since >= ring_every && age >= 0 && rb_fast <= 0;
    if (!loading) rb_release_held();
    if (loading) {
        ring_since = 0; ring_left--; ring_loads++;
        cur_depth = depth;
        if (!(ring_loads % 25))
            fprintf(stderr, "[rb] ring: %lld loads; frames run again so far: %lld the same, %lld DIFFERENT; save %.2f ms mean (max %.1f), load %.2f ms mean (max %.1f); "
                    "caught up %.1f ms mean (max %.1f)\n",
                    ring_loads, ring_same, ring_diff, ring_save_ms / ring_saves, ring_save_max, ring_loads > 1 ? ring_load_ms / (ring_loads - 1) : 0, ring_load_max,
                    caught_up ? catchup_ms / caught_up : 0, catchup_max);
        ring_load_t0 = now_ms();
        fp_loaded = 1;
        {   static int profiled; const char *e = getenv("SSBU_RB_PROFILE");
            if (!profiled && e && atof(e) > 0) { profiled = 1; profile_start_gated(atof(e)); } }
        pending_fast = (LONG)(key - found);
        rb_catchup = fast_on;
        rb_load_age(c, frame, age);   /* (returns only if refused) */
        rb_catchup = 0;
        rb_release_held();
        ring_loads--; ring_left++;
    }
}

/* ---- M5 step 2: loop mode -----------------------------------------------------------------
 * SSBU_RB_NET=loop[,<delay>,<lag min>,<lag max>,<loss %>,<frames>] (defaults 2,1,6,10,1800): one game,
 * both players driven by bots by logic frame (net_pad: the same pads every run), player 2's arriving
 * through rb_net.c as if from another PC: the pad for frame F is read there at F - <delay> and gets
 * here <lag> frames later (each frame's lag drawn from F, between min and max; <loss> % lost, so the
 * next message brings it, 1-3 frames on), on wall time from the fight's start. Until then player 2
 * plays a guess (its last pad, held); a wrong guess loads the newest state at or before the frame
 * before it and runs the frames since again, fast and unseen, with the pads known now. A frame past
 * the 7 a rollback reaches waits for the pads instead. <lag max> -1: every pad known at once (no
 * rollbacks: the reference run). For the first <frames> fight frames each frame's fight hash, as it
 * stands once its pads are final, goes to build\rb_loop_<lag max>.txt: two runs (one with lag, one
 * with -1) compared frame by frame say whether rolling back gave the same fight. Both players must
 * be humans at character select (port 2 too: any controller), not CPUs; hands off. */
#include "hle_rb_net.h"
/* (v0.9) a player gone from this fight: its game said its fight was over (LEFT), or nothing came from it for
 * NET_GONE_MS while this game waited. Its pads past net_gone_after are none, and nobody waits for them. */
#define NET_GONE_MS 8000.0
#define NET_SILENT_MS 30000.0   /* (v0.9.3: nothing at all from a player since the fight began here: its game never started it) */
static u32 net_gone_after[RBN_PLAYERS];
static const char *net_gone_why[RBN_PLAYERS];
static int net_gone_shown[RBN_PLAYERS];
static double net_pads_heard_ms[RBN_PLAYERS];
static int net_left_take(const u8 *buf, int len);
static void net_gone_check(u32 key, double waited_ms);
static int net_mode = -1, net_delay = 2, net_lag_lo = 1, net_lag_hi = 6, net_loss = 10, net_frames = 1800;
static u32 net_key0;            /* the logic frame the fight started at */
static double net_t0;           /* and when (ms) */
static u8 *net_got;             /* by frame - net_key0: delivered */
static u32 net_low;             /* every frame below this delivered */
static u64 *net_final;          /* by frame - net_key0: the fight hash once final (0: none) */
static long long net_rollbacks, net_rerun, net_waits, net_too_deep, net_depth[16], net_saves_made, net_saves_skipped;
static long long net_older;   /* (M7: rollbacks done from an older state, the newest refused) */
static double net_catch_ms, net_catch_max, net_wait_ms;
static int net_done;
static long long net_reads[8];   /* (pad reads by slot since the last line, the diagnostic once a second) */
static u32 net_last_key[8];
static RbnPad net_last_pad[RBN_PLAYERS];
/* ---- M5 step 3: net mode, two games ---------------------------------------------------------
 * SSBU_RB_NET=net,<local player 0|1>,<my port>,<peer port>[,<delay>[,<frames>[,<peer host>]]]: two games
 * (on one PC: two instances, ports swapped), each driving its own player with its bot (net_pad) and
 * getting the other's pads from the other game: rb_net PADS messages over UDP (hle_rb_link.c), every
 * normal frame, with every pad the peer hasn't acknowledged. Each frame's fight hash, once its pads
 * are final, goes into the messages, and the peer's are compared with this game's own: a desync is
 * found, not assumed away. A frame too far past the peer's pads waits for them (up to 10 min: the other
 * game may still be loading, or its fight not started yet). Both games need the same seed (SSBU_RB_SEED), the same characters in
 * the same ports, the same stage; the starting sides are still random (step 3b): a start on different
 * sides shows up as a desync at once. */
static int net_local, net_port, net_peer_port;
static u64 hs_seed;    /* (the match seed of the handshake, below savestates) */
/* SSBU_RB_NET_CHECK_EVERY=<n>: the fight hash (some 5 ms of getter calls) only on logic frames
 * that are multiples of n (default 10), in both games alike; the others go unhashed and uncompared. The hash every
 * frame made the test's window play in slow motion ("responsive once the logger stopped"). */
static int net_every = -1;
/* SSBU_RB_NET_LIVE=1 (M5 step 4): this game's player is played by its own controllers (the pad of its
 * slot as hle_hid.c reads it: pad, keyboard), not by the bot: the newest live read is this game's pad for
 * the frame `delay` on, sent to the peer like the bot's. The other player's slot ignores the controllers
 * here (they may reach both windows): only the peer's pads count. */
static int net_live = -1;
static RbnPad net_live_pad;
static char net_host[64] = "127.0.0.1";
/* M7, 3+ players: what is kept of each other player (by player index; this game's own unused). Through the host:
 * player 1's game is the host, every other game talks to it only, and it passes each player's PADS and CHOICE on to
 * the rest (net_receive, net_choice_take). */
static int net_n = 2;                       /* players (the host's SSBU_RB_NET_PLAYERS; the others take it from its hello) */
static u32 net_acked_by[RBN_PLAYERS];      /* the newest frame of our pads each player has all of */
static u32 net_heard[RBN_PLAYERS];         /* the newest frame each player said it was running */
static u32 net_checked;                    /* (frames compared up to here) */
static u32 *net_peer_check[RBN_PLAYERS];   /* by frame - net_key0: each player's hash (low 32 bits); 0: none */
static long long net_checks, net_desyncs, net_msgs_in, net_msgs_out, net_sync_waits;
static long long net_first_desync = -1;
static long long net_link_base[3];   /* (the link's counts at the fight's start: the report is per fight) */
/* M6, the false desyncs at KO launches (twopc18 frame 2100, twopc19 frame 4430): PC 1 held two different hashes for
 * one frame (one sent, one its own later), neither the other PC's, while both fights went on alike. So a frame run
 * again can come out different from its first pass even with every pad the same: which part, is to see. Each
 * hashed frame is kept (its fighters and the generators' words) with whether its pads were all real then
 * (final); when a final frame is hashed again after a rollback and differs, both passes go to the log, field by
 * field. A DESYNC report shows the frame's fields too, to set beside the other PC's log. */
typedef struct CheckRec { u32 key; int final; u64 h; FFrame f; u32 w[45]; } CheckRec;
static CheckRec check_recs[64];
static long long net_again, net_again_bad;
static void check_words(u32 *w) { u64 rb = rng_base(); for (int k = 0; k < 45; k++) w[k] = rb ? ld32(rb + 4ull * k) : 0; }
static void check_show(const char *what, const CheckRec *r) {
    char t[256];
    for (u32 k = 0; k < r->f.n; k++) { frec_text(t, sizeof t, &r->f.f[k]); fprintf(stderr, "[rb]   %s: %s\n", what, t); }
    char g[512]; size_t n = 0;
    for (int q = 0; q < 9; q++)
        n += snprintf(g + n, sizeof g - n, " %d:%08x %08x %08x %08x #%u%s", q, r->w[q * 5], r->w[q * 5 + 1], r->w[q * 5 + 2], r->w[q * 5 + 3], r->w[q * 5 + 4],
                      q == 1 ? "(not hashed)" : "");
    fprintf(stderr, "[rb]   %s: generators%s\n", what, g);
}
/* the frame `key` was just hashed (h): kept, or, if it was final before and differs now, both passes logged */
static void check_again(u32 key, const FFrame *f, u64 h) {
    CheckRec *r = &check_recs[(key / (u32)(net_every > 0 ? net_every : 10)) % 64];
    if (r->key == key && r->final) {
        net_again++;
        if (r->h != h && ++net_again_bad <= 8) {
            CheckRec now = {key, 1, h, *f, {0}};
            check_words(now.w);
            fprintf(stderr, "[rb] net check: logic frame %u, its pads all real, ran again after a rollback and came out DIFFERENT (%08x, first %08x):\n",
                    key, (u32)h, (u32)r->h);
            check_show("first pass", r);
            check_show("run again ", &now);
        }
        return;   /* (the first final pass is what the other PC got) */
    }
    r->key = key; r->h = h; r->f = *f;
    r->final = rbn_confirmed() + 1 >= key;   /* (the old rule, on purpose: a frame final by it that changes shows what follows its own pads) */
    check_words(r->w);
}
static void check_desync_show(u32 key) {
    CheckRec *r = &check_recs[(key / (u32)(net_every > 0 ? net_every : 10)) % 64];
    if (r->key == key) { fprintf(stderr, "[rb]   (this game's frame %u, %s when hashed:)\n", key, r->final ? "its pads all real" : "on a guess"); check_show("ours", r); }
}
/* M6, time sync: a hold for the frame advantage leaves one refresh out (ipc_vi.c), so it costs the game a frame
 * for sure; SSBU_RB_NET_HOLD=sleep: Sleep(16) at the safe point, as before (twopc15: it did not hold). The
 * round trip comes from the acks: when the peer first says it has our pad for frame f, against when that pad
 * first went out (on top of the real trip: up to a frame of the peer's sending and of ours polling, so the
 * second's least is the one to read). Once a second the log says where both games are (net sync lines). */
extern volatile LONG rb_vsync_skip;
extern volatile LONG64 rb_vsync_skipped;
static int net_hold_sleep = -1;
static double net_sent_ms[RBN_RING]; static u32 net_sent_at[RBN_RING];
static double net_rtt_min_s = 1e9, net_rtt_sum; static long long net_rtt_n;
static int32_t net_adv_ours, net_adv_peer;
static long long net_bytes_s, net_msgs_s, net_bytes_max_s;   /* (the PADS messages sent this second: bytes, count, the longest) */
/* (M6: the rollback's own work on the frames of this second, to tell it from the game's: the fight hash every
 * net_every frames, the saves, the once-a-second fighter line; sum, worst, count) */
static double net_w_ms[3], net_w_max[3]; static long long net_w_n[3];
static void net_work(int k, double t0) { double ms = now_ms() - t0; net_w_ms[k] += ms; if (ms > net_w_max[k]) net_w_max[k] = ms; net_w_n[k]++; }
int rbl_open(int local, int my_port, const char *host, int peer_port);
#include "hle_rb_link.h"   /* (R7: the link through the netplay relay, SSBU_RB_NET=relay) */
/* (R7) SSBU_RB_NET=relay,auto: this game's number comes from the room's order (rbl_relay_number), once every player is
 * in it; until then net_local is -1 and the handshake waits (net_hello) */
static int net_pending;
/* SSBU_RB_NET=online: the game's own Online mode (Quickplay): each match's players come from its set-up (online_setup) */
static int net_online, online_delay_set = -1;
void rbl_players(int n);
void rbl_send(const void *buf, int len);
void rbl_send_to(int p, const void *buf, int len);
int rbl_recv(void *buf, int cap);
void rbl_stats(long long *s, long long *r, long long *e);
int rbl_accept(int p);
int rbl_has_peer(int p);
/* While a set-up waits for the other games the main thread draws nothing, so the game looks frozen: say what it
 * waits for (the game window's title, and build\rollback_status.txt, which the Host/Join window shows, on top) and
 * keep the freeze watchdog quiet (a known wait, not a hang). NULL: the wait is over. */
void watchdog_present(void);    /* debug_watchdog.c */
void gfx_set_status(const char *text);   /* gpu_d3d11.c */
static void net_wait_say(const char *what) {
    static double last; static int shown;
    double t = now_ms();
    if (what) watchdog_present();
    if (what && t - last < 500) return;
    if (!what && !shown) return;
    last = t; shown = what != NULL;
    char s[160] = "";
    if (what && net_local == 0) {
        int k = 1;
        for (int p = 1; p < net_n; p++) k += rbl_has_peer(p) ? 1 : 0;
        snprintf(s, sizeof s, "%s (%d of %d players here)", what, k, net_n);
    } else if (what) snprintf(s, sizeof s, "%s", what);
    gfx_set_status(s);
    FILE *f = fopen("build\\rollback_status.txt", "w");
    if (f) { fputs(s, f); fclose(f); }
}
int rbl_who(void);
void rbl_assign(int p);
long long rbl_dropped(void);
double rbl_last_arrival_ms(void);
static void net_hello_send(void);
/* M6, the round trip and the delay agreed in the handshake (HELLO version 2). SSBU_RB_NET's delay -1 (the
 * launcher's RB_DELAY=auto): player 1's game picks it from the round trip (the least of the first
 * HS_RTT_SAMPLES, taken over HS_RTT_MS at least), and only then proposes the seed; player 2's game takes
 * player 1's delay always, so the two never differ. (inet1: the first 12, 2 s of a phone link just woken, had
 * 112.6 ms at least and gave delay 3; later the least was ~85 ms, delay 2.) */
#define HS_RTT_SAMPLES 30
#define HS_RTT_MS 5000.0
static int hs_delay_ready;                 /* (player 1: net_delay is chosen; player 2: taken from player 1) */
static u64 hs_peer_t[RBN_PLAYERS], hs_peer_t_at[RBN_PLAYERS];   /* each player's newest hello clock, and when (our us) it came */
static double hs_rtt_min[RBN_PLAYERS], hs_rtt_t0[RBN_PLAYERS]; static int hs_rtt_n[RBN_PLAYERS];   /* (the least: once n > 0) */
static int hs_agreed_by[RBN_PLAYERS];      /* (the host: which players have the seed) */
static u64 now_us(void) { return (u64)(now_ms() * 1000.0); }
/* (hle_hid.c) the other PC's player's controller slot, in net mode: connected here as a Pro Controller always */
int rb_slot_remote(u32 id) { return net_mode == 2 && net_local >= 0 && id < (u32)net_n && id != (u32)net_local; }
/* (hle_hid.c, Host/Join) which of this PC's controller slots the game's slot `id` reads: a joining game's controller and
 * keyboard are its player 1 here, as on every PC; once the host gives it player N, the game's slot N reads them, and
 * its slot 1 (the host's player: a remote slot) the empty slot N */
u32 rb_slot_phys(u32 id) {
    if (net_mode != 2 || net_local <= 0) return id;
    if (id == (u32)net_local) return 0;
    if (id == 0) return (u32)net_local;
    return id;
}
/* M7, Host/Join: a joining game starts with no number (net_local -1) and a random join id; it says hello with player
 * -1 and the id, and the host gives it the lowest free player (rbl_assign), says which in every hello to it after (assign),
 * and knows it again by the id when its address changes. A slot whose game has said nothing for 5 s outside a fight goes
 * to a newcomer (a friend whose game started again: another port, another id). */
static u64 hs_join_id;                     /* (a joiner: its id) */
static u64 hs_slot_id[RBN_PLAYERS];        /* (the host: each player's join id) */
static double hs_heard_ms[RBN_PLAYERS];    /* (the host: when each player was last heard) */
static int hs_agreed;
static void net_forget_player(int p) {      /* (the host: a slot given to another game) */
    hs_rtt_n[p] = 0; hs_rtt_min[p] = 0; hs_peer_t[p] = 0; hs_agreed_by[p] = 0; hs_slot_id[p] = 0;
    hs_agreed = 0;   /* (the newcomer must take the seed too) */
}
static int net_join(u64 id) {
    int p = rbl_who();
    if (p >= 1 && p < net_n) { if (id) hs_slot_id[p] = id; return p; }   /* (this address's already) */
    for (int q = 1; q < net_n; q++)
        if (id && hs_slot_id[q] == id) { rbl_assign(q); return q; }      /* (the same game from another port) */
    for (int q = 1; q < net_n; q++)
        if (!rbl_has_peer(q)) {
            rbl_assign(q); net_forget_player(q); hs_slot_id[q] = id; hs_heard_ms[q] = now_ms();
            fprintf(stderr, "[rb] handshake: a game joined: it is player %d of %d\n", q + 1, net_n);
            return q;
        }
    if (!S.in_fight)
        for (int q = 1; q < net_n; q++)
            if (now_ms() - hs_heard_ms[q] > 5000) {
                rbl_assign(q); net_forget_player(q); hs_slot_id[q] = id; hs_heard_ms[q] = now_ms();
                fprintf(stderr, "[rb] handshake: player %d's game went quiet: a new joiner takes its place\n", q + 1);
                return q;
            }
    static int shown;
    if (!shown++) fprintf(stderr, "[rb] handshake: a game asked to join, but all %d players are here\n", net_n);
    return -1;
}
/* (a joiner) the host says this game is player p (of `players`) */
static void net_take_number(int p, int players) {
    if (p < 1 || p >= RBN_PLAYERS || S.in_fight) return;
    if (players >= 2 && players <= RBN_PLAYERS) net_n = players;
    if (net_local == p) return;
    if (net_local > 0) fprintf(stderr, "[rb] handshake: the host now says this game is player %d (was %d)\n", p + 1, net_local + 1);
    else fprintf(stderr, "[rb] handshake: the host made this game player %d of %d\n", p + 1, net_n);
    net_local = p;
}
/* A hello came: 1 if it is the other game's and speaks our version; then its sender is the peer (rbl_accept),
 * its clock is kept for our next hello's echo, and its echo of ours is a round trip. All by when it arrived
 * (the link's receive thread), not when the game read it: a frame's wait on each side is no part of the trip. */
static int net_hello_heard(const RbnHello *h) {
    int p = h->player;
    if (h->version != RBN_VERSION) return 0;
    if (net_local == 0 && p == -1) {                 /* (the host: a joiner asking for its number) */
        if ((p = net_join(h->seed)) < 0) return 0;
    } else {
        if (p < 0 || p >= RBN_PLAYERS || p == net_local) return 0;
        if (net_local != 0 && p != 0) return 0;      /* (M7: the others talk to the host only) */
        if (net_local == 0 && p >= net_n) return 0;  /* (the host: a player past the count) */
        if (rbl_accept(p) < 0) return 0;
    }
    if (net_local == 0) hs_heard_ms[p] = now_ms();
    if (net_local != 0 && h->assign) net_take_number(h->assign - 1, h->players);
    u64 now = (u64)(rbl_last_arrival_ms() * 1000.0);
    if (h->t) { hs_peer_t[p] = h->t; hs_peer_t_at[p] = now; }
    if (h->echo && h->echo + h->hold <= now) {
        double rtt = (double)(now - h->echo - h->hold) / 1000.0;
        if (!hs_rtt_n[p] || rtt < hs_rtt_min[p]) hs_rtt_min[p] = rtt;
        if (!hs_rtt_n[p]) hs_rtt_t0[p] = now_ms();
        if (hs_rtt_n[p]++ < 3 || !(hs_rtt_n[p] % 3000))   /* (a wait at the set-up measures thousands: M7 logs fewer) */
            fprintf(stderr, "[rb] handshake: round trip to player %d %.1f ms (least so far %.1f, %d measured)\n", p + 1, rtt, hs_rtt_min[p], hs_rtt_n[p]);
    }
    return 1;
}
/* the oldest frame any other player's pads are known up to (net mode); loop mode: player 2's */
static u32 net_remote_low(void) {
    if (net_mode != 2) return rbn_remote_newest(1);
    u32 low = ~0u;
    for (int p = 0; p < net_n; p++) {
        if (p == net_local || (rbn_gone_after(p) && rbn_remote_newest(p) >= rbn_gone_after(p))) continue;   /* (gone: all its pads known) */
        if (rbn_remote_newest(p) < low) low = rbn_remote_newest(p);
    }
    return low == ~0u ? 0 : low;
}
/* M5 step 6, the match's set-up agreed (CHOICE messages): each game's own player's fighter and colour,
 * player 1's game adds the stage; applied at the player record builder (its frame hook, record_before). */
static RbnChoice ch_mine, ch_peer[RBN_PLAYERS];
static int ch_have_mine, ch_have_peer[RBN_PLAYERS];
/* (hle_hid.c, version 12) the other PC's player's controller here: a GameCube controller if its game said so in its choice
 * (its analog triggers - light shield - and the GameCube controls, as on its own PC), else a Pro Controller */
int rb_slot_remote_gc(u32 id) { return id < RBN_PLAYERS && ch_have_peer[id] && ch_peer[id].gc; }
int hid_slot_is_gc(u32 id);   /* (hle_hid.c) this PC's slot plays a GameCube controller now */
static void net_choice_send(int ask);
static int net_choice_take(const u8 *buf, int len);
static int net_rules_take(const u8 *buf, int len);
static int net_ready_take(const u8 *buf, int len);
static int net_pre_take(const u8 *buf, int len);
static int net_menu_take(const u8 *buf, int len);
static u64 net_mix(u64 h, u64 v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdull; }
/* the bots: a stick direction and a button held a few frames at a time, as a frame's own function */
static RbnPad net_pad(int who, u32 f) {
    u64 h = net_mix((u64)who * 7919 + 1, f / (u32)(5 + who * 4));
    RbnPad p = {0};
    static const u64 press[8] = {0, 0, 1ull << 0, 1ull << 1, 1ull << 2, 0, 1ull << 0, 1ull << 7};   /* -, -, A, B, X (jump), -, A, R (shield) */
    p.buttons = press[h & 7];
    p.lx = (int16_t)((int)((h >> 8) % 3) - 1) * 32767;
    p.ly = (h >> 16) % 6 == 0 ? 32767 : (h >> 16) % 6 == 1 ? -32767 : 0;
    /* (SSBU_RB_NET_BOT=calm: no sideways stick, so the bots stay on the stage: an Online match kicks a player who
     * self-destructs too often, and the random bot does) */
    static int calm = -1;
    if (calm < 0) { const char *e = getenv("SSBU_RB_NET_BOT"); calm = e && !strcmp(e, "calm"); }
    if (calm) { p.lx = 0; if (p.ly > 0) p.ly = 0; p.buttons &= ~(1ull << 2); }   /* (no walking, no jumps: buttons where it stands) */
    return rbn_rest(p);
}
/* the logic frame at which player 2's pad for frame F gets here. (In this game's frames, not on
 * wall time: the bot keeps pace with this game as a real peer would through the frame advantage;
 * on wall time, a hitch at the fight's start left this game behind for good, every pad came
 * early and nothing was ever guessed: loop1.) */
static double net_arrival(u32 f) {
    if (net_lag_hi < 0) return -1e18;
    u64 h = net_mix(0xa11, f);
    int lag = net_lag_lo + (int)(h % (u64)(net_lag_hi - net_lag_lo + 1));
    if ((int)((h >> 20) % 100) < net_loss) lag += 1 + (int)((h >> 28) % 3);
    return (double)f - net_delay + lag;
}
/* every pad come by logic frame `now`; force: also the oldest missing ones until frame now + 1 may run
 * (the wait a game makes for a peer that is too far behind: here the peer is this game's own bot) */
static void net_deliver(u32 now, int force) {
    /* (at most 64 frames ahead of now: with every pad known at once (lag max -1), going 128 past
     * net_low each frame overran rb_net's 256-frame ring within two frames, and player 2 got
     * nothing but empty pads: seed2) */
    for (u32 f = net_low; f < net_low + 128 && f <= now + 64 && f - net_key0 < 1u << 20; f++) {
        u32 i = f - net_key0;
        if (net_got[i] || (net_arrival(f) > now && !(force && !rbn_can_run(now + 1)))) continue;
        rbn_remote(1, f, net_pad(1, f));
        net_got[i] = 1;
    }
    while (net_low - net_key0 < 1u << 20 && net_got[net_low - net_key0]) net_low++;
}
static void net_report(void) {
    char name[64];
    if (net_mode == 2) snprintf(name, sizeof name, "build\\rb_net_%d.txt", net_local);
    else snprintf(name, sizeof name, "build\\rb_loop_%d.txt", net_lag_hi);
    FILE *o = fopen(name, "w");
    long long n = 0;
    if (o) {
        for (u32 i = 0; i < (u32)net_frames + 600 && i < 1u << 20; i++) if (net_final[i]) { fprintf(o, "%u %016llx\n", i, (unsigned long long)net_final[i]); n++; }
        fclose(o);
    }
    char hist[200] = ""; size_t k = 0;
    for (int d = 1; d < 16; d++) if (net_depth[d]) k += snprintf(hist + k, sizeof hist - k, " %d:%lld", d, net_depth[d]);
    fprintf(stderr, "[rb] saves: %lld made, %lld left out (the other's next pad already there)\n", net_saves_made, net_saves_skipped);
    fprintf(stderr, "[rb] loads held up by a thread on another fiber: %lld went ahead after a wait, %lld refused; %lld rollbacks from an older state instead\n",
            rb_fiber_retried_ok, rb_fiber_refused, net_older);
    fprintf(stderr, "[rb] net check: %lld frames hashed again after a rollback with their pads all real, %lld of them DIFFERENT%s\n", net_again, net_again_bad,
            net_again_bad ? " (the passes are in the log: net check lines)" : "");
    fprintf(stderr, "[rb] loop mode done (delay %d, lag %d..%d, loss %d%%): %lld rollbacks (depths%s), %lld frames run again, caught up %.1f ms mean (max %.1f); "
            "%lld frames waited for pads (%.1f ms), %lld rollbacks too deep for the ring; %lld final hashes in %s\n",
            net_delay, net_lag_lo, net_lag_hi, net_loss, net_rollbacks, k ? hist : " none", net_rerun,
            net_rollbacks ? net_catch_ms / net_rollbacks : 0, net_catch_max, net_waits, net_wait_ms, net_too_deep, n, name);
    if (net_mode == 2) {
        long long s, r, e; rbl_stats(&s, &r, &e);
        s -= net_link_base[0]; r -= net_link_base[1]; e -= net_link_base[2];   /* (this fight's) */
        fprintf(stderr, "[rb] net mode (player %d): %lld hashes compared with the peer's, %lld DIFFERENT%s; messages %lld sent (%lld failed), %lld received; "
                "%lld frames waited for the peer's pads (%.1f ms), %lld frames held back for the frame advantage (%s); ack round trip %.1f ms mean\n",
                net_local + 1, net_checks, net_desyncs, net_desyncs ? "" : " (in sync)", s, e, r, net_waits, net_wait_ms, net_sync_waits,
                net_hold_sleep > 0 ? "slept" : "refreshes left out", net_rtt_n ? net_rtt_sum / net_rtt_n : -1.0);
        rbl_relay_report();   /* (R7, SSBU_RB_NET=relay: which way the messages went) */
    }
}
/* this game's newest frame whose hash is final (its pads all known), with that hash; 0: none yet.
 * (M6: final once the frame's OWN pads are real, not the frame before's. The false desyncs at KO launches
 * (twopc18, twopc19): a frame final by the old rule held one hash, then a wrong guess at that very frame rolled it
 * back and it came out another; with nothing before it changed, part of a frame's state follows its own pads
 * after all (player 1's pad is read after the safe point; the other's, or some part, not). A frame's hash
 * now waits one frame more, and a wrong guess at a frame clears its own hash too. check_again still flags
 * frames final by the old rule that change, to find which part.) */
static u32 net_final_frame(u32 key, u64 *h) {
    u32 c = rbn_confirmed();
    if (c > key) c = key;
    for (u32 f = c; f >= net_key0 && f + 8 + (u32)net_every > c; f--) {
        u32 i = f - net_key0;
        if (i < 1u << 20 && net_final[i]) { *h = net_final[i]; return f; }
    }
    return 0;
}
/* (the first desync: every player's pads for the frames before it, as this game played them - the other PC's log has
 * the same lines, so the two show whether the pads or the game went apart) */
/* (v0.9.6, diagnostics: desyncs with every pad alike on both PCs (three matches over the internet, 11-01): one fighter's action or position
 * apart, the PC that rolls back more being one side) The last RB_HIST rollbacks, and on the first desync every frame
 * kept (check_recs: the last 64, hashed every frame now) and those rollbacks, to set beside the other PC's log: the first
 * frame apart, and whether this PC ran it again after a rollback. */
#define RB_HIST 32
static struct { u32 at, to; int depth, older; } rb_hist[RB_HIST];
static long long rb_hist_n;
static void rb_hist_add(u32 at, u32 to, int older) {
    int k = (int)(rb_hist_n++ % RB_HIST);
    rb_hist[k].at = at; rb_hist[k].to = to; rb_hist[k].depth = (int)(at - to); rb_hist[k].older = older;
}
static void net_desync_dump(void) {
    fprintf(stderr, "[rb]   frames kept here (logic frame, pads all real when hashed or not, hash, each fighter):\n");
    u32 lo = ~0u;
    for (int k = 0; k < 64; k++) if (check_recs[k].key && check_recs[k].key < lo) lo = check_recs[k].key;
    for (u32 key = lo; lo != ~0u && key < lo + 64 * (u32)(net_every > 0 ? net_every : 1); key++) {
        const CheckRec *r = &check_recs[(key / (u32)(net_every > 0 ? net_every : 10)) % 64];
        if (r->key != key) continue;
        char t[256];
        fprintf(stderr, "[rb]   kept %u %s %08x\n", key, r->final ? "real" : "guess", (u32)r->h);
        for (u32 q = 0; q < r->f.n; q++) { frec_text(t, sizeof t, &r->f.f[q]); fprintf(stderr, "[rb]     %s\n", t); }
    }
    fprintf(stderr, "[rb]   the last rollbacks here (%lld in this fight):", rb_hist_n);
    for (long long k = rb_hist_n > RB_HIST ? rb_hist_n - RB_HIST : 0; k < rb_hist_n; k++) {
        int i = (int)(k % RB_HIST);
        fprintf(stderr, " %u->%u%s", rb_hist[i].at, rb_hist[i].to, rb_hist[i].older ? "(older)" : "");
    }
    fprintf(stderr, "\n");
}
static void net_desync_pads(u32 f) {
    for (int p = 0; p < net_n; p++) {
        fprintf(stderr, "[rb]   pads of player %d%s, frames %u..%u:", p + 1, p == net_local ? " (this game's)" : "", f > 20 ? f - 19 : 1, f);
        for (u32 k = f > 20 ? f - 19 : 1; k <= f; k++) {
            RbnPad d = rbn_get(p, k, NULL);
            fprintf(stderr, " %llx/%d,%d/%d,%d/%d,%d", (unsigned long long)d.buttons, d.lx, d.ly, d.rx, d.ry, d.lt, d.rt);
        }
        fprintf(stderr, "\n");
    }
}
static void net_compare(u32 key) {
    u32 c = rbn_confirmed();   /* (final: see net_final_frame) */
    if (c > key) c = key;
    for (u32 f = net_checked + 1; f <= c; f++) {
        u32 i = f - net_key0;
        if (i >= 1u << 20 || !net_final[i]) continue;
        for (int p = 0; p < net_n; p++) {
            if (p == net_local || !net_peer_check[p] || !net_peer_check[p][i]) continue;
            if (rbn_gone_after(p) && f > rbn_gone_after(p)) continue;   /* (a gone player's hashes past its cutoff: it played other pads) */
            net_checks++;
            if ((u32)net_final[i] != net_peer_check[p][i]) {
                if (!net_desyncs) net_first_desync = f;
                if (net_desyncs++ < 3) {   /* (the first few, each with this game's fields: M6) */
                    fprintf(stderr, "[rb] net mode: DESYNC with player %d at logic frame %u (ours %08x, theirs %08x)%s\n", p + 1, f, (u32)net_final[i],
                            net_peer_check[p][i], net_checks <= 1 ? ": the games did not start the same match (characters, colours, stage, rules or the seed differ)" : "");
                    check_desync_show(f);
                    if (net_desyncs == 1) { net_desync_pads(f); net_desync_dump(); rb_keep_dump("a desync"); }
                }
            }
        }
    }
    if (c > net_checked) net_checked = c;
}
static void net_receive(u32 key) {
    static u8 buf[2048];
    int n;
    while ((n = rbl_recv(buf, sizeof buf)) > 0) {
        RbnMsg m;
        RbnHello hh;
        if (rbn_hello_parse(buf, (size_t)n, &hh)) {   /* (a game still at its set-up: it waits for our answer) */
            if (net_hello_heard(&hh) && net_local != 0 && hs_agreed && hh.seed == hs_seed) net_hello_send();
            continue;
        }
        if (net_rules_take(buf, n)) continue;
        if (net_ready_take(buf, n)) continue;
        if (net_menu_take(buf, n)) continue;
        if (net_left_take(buf, n)) continue;
        if (net_pre_take(buf, n)) continue;
        if (net_choice_take(buf, n)) continue;
        if (!rbn_parse(buf, (size_t)n, &m) || m.player == net_local || m.player >= net_n) continue;
        if (rbl_accept(m.player) < 0) continue;
        int p = m.player;
        if (net_local == 0) hs_heard_ms[p] = now_ms();   /* (Host/Join: pads count as heard: a fight's players are no quiet slots) */
        if (net_local == 0) for (int k = 1; k < net_n; k++) if (k != p) rbl_send_to(k, buf, n);   /* (the host passes it on: M7) */
        net_msgs_in++;
        net_pads_heard_ms[p] = now_ms();
        for (int k = 0; k < m.count; k++) rbn_remote(p, m.first + (u32)k, m.pads[k]);
        u32 ack = m.ack[net_local];
        if (ack > net_acked_by[p] && net_sent_at[ack & (RBN_RING - 1)] == ack) {
            double rtt = rbl_last_arrival_ms() - net_sent_ms[ack & (RBN_RING - 1)];   /* (by arrival: the link's thread) */
            if (rtt < net_rtt_min_s) net_rtt_min_s = rtt;
            net_rtt_sum += rtt; net_rtt_n++;
        }
        if (ack > net_acked_by[p]) net_acked_by[p] = ack;
        if (m.frame > net_heard[p]) net_heard[p] = m.frame;
        if (m.check_frame >= net_key0 && m.check_frame - net_key0 < 1u << 20 && net_peer_check[p]) {
            u32 i = m.check_frame - net_key0;
            net_peer_check[p][i] = m.check ? m.check : 1;
            static u32 last_arrival[RBN_PLAYERS];   /* (a player repeats its newest final frame in several messages: counted once) */
            if (m.check_frame <= net_checked && net_final[i] && m.check_frame != last_arrival[p] && !(rbn_gone_after(p) && m.check_frame > rbn_gone_after(p))) {   /* (a frame compared already passed here: at once) */
                last_arrival[p] = m.check_frame;
                net_checks++;
                if ((u32)net_final[i] != net_peer_check[p][i]) {
                    if (!net_desyncs) net_first_desync = m.check_frame;
                    if (net_desyncs++ < 3) {
                        fprintf(stderr, "[rb] net mode: DESYNC with player %d at logic frame %u (ours %08x, theirs %08x)\n", p + 1, m.check_frame, (u32)net_final[i],
                                net_peer_check[p][i]);
                        check_desync_show(m.check_frame);
                        if (net_desyncs == 1) { net_desync_pads(m.check_frame); net_desync_dump(); rb_keep_dump("a desync"); }
                    }
                }
                net_peer_check[p][i] = 0;
            }
        }
        /* (each side's advantage as each measures it, late alike: the difference cancels the trip) */
        net_adv_ours = (int32_t)key - (int32_t)(net_heard[p] + 1); net_adv_peer = (int32_t)m.frame - (int32_t)(m.heard[net_local] + 1);
        rbn_sync_sample(p, net_adv_ours, net_adv_peer);
    }
    /* a wrong guess these pads showed: the frames after it are not what they will be once run again,
     * so their hashes go now, before anything is compared or sent (net3: the hashes of a pass about to
     * be rolled back went out as final and were "different" at frame 88, the rollback in the same
     * step still to come). The rollback itself comes at the end of net_step. */
    u32 bad = rbn_take_mismatch();
    if (bad) {
        for (u32 f = bad; f <= key; f++) if (f - net_key0 < 1u << 20) net_final[f - net_key0] = 0;   /* (bad too: M6, net_final_frame) */
        rbn_put_mismatch(bad);
    }
}
static void net_send(u32 key) {
    static RbnMsg m;
    static u8 buf[RBN_MSG_MAX];
    m.type = RBN_MSG_PADS; m.player = net_local; m.count = 0;
    memset(m.ack, 0, sizeof m.ack); memset(m.heard, 0, sizeof m.heard);
    u32 low = ~0u;   /* (from the oldest frame of ours any player lacks: M7) */
    for (int p = 0; p < net_n; p++) if (p != net_local) {
        if (net_acked_by[p] < low && !rbn_gone_after(p)) low = net_acked_by[p];   /* (not a gone player's: it acks no more) */
        m.ack[p] = rbn_remote_newest(p); m.heard[p] = net_heard[p];
    }
    m.first = (low == ~0u ? 0 : low) + 1;
    for (u32 f = m.first; f <= rbn_local_newest() && m.count < RBN_MAX_PADS; f++) rbn_local_get(f, &m.pads[m.count++]);
    m.frame = key;
    u64 h = 0;
    m.check_frame = net_final_frame(key, &h); m.check = (u32)h;
    size_t len = rbn_build(buf, sizeof buf, &m);
    if (len) { rbl_send(buf, (int)len); net_msgs_out++; net_bytes_s += (long long)len; net_msgs_s++; if ((long long)len > net_bytes_max_s) net_bytes_max_s = (long long)len; }
}
static void net_step(Cpu *c, u64 frame) {
    u32 key = ld32(LOGIC_FRAME);
    static long long net_fight = -1;
    if (net_fight != S.fights || !net_got) {   /* a fight starts (by its number: S.frames == 1 came twice once) */
        net_fight = S.fights;
        if (!net_got) { net_got = calloc(1u << 20, 1); net_final = calloc(1u << 20, sizeof *net_final); }
        else { memset(net_got, 0, 1u << 20); memset(net_final, 0, (1u << 20) * sizeof *net_final); }
        if (net_delay < 0) { net_delay = 2; fprintf(stderr, "[rb] net mode: no input delay agreed (no handshake): 2\n"); }
        if (net_mode == 2) {
            int measured = 0;
            for (int p = 0; p < RBN_PLAYERS; p++) measured |= hs_rtt_n[p] > 0;
            fprintf(stderr, "[rb] net mode: %d players, this game player %d; input delay %d this fight%s\n", net_n, net_local + 1, net_delay,
                    measured ? "" : " (no round trip measured)");
        }
        rbn_reset(net_mode == 2 ? net_n : 2, net_mode == 2 ? net_local : 0, net_delay);
        rbn_sync_reset();
        /* (the first frame's local pad is taken before this fight's first read: without this it was the last fight's last
         * controls, a press nobody made - a lone up + B at the start sent Steve off the stage) */
        net_live_pad = (RbnPad){0};
        memset(net_last_pad, 0, sizeof net_last_pad);
        for (int p = 0; p < net_n; p++) {
            if (p == net_local || net_mode != 2) continue;
            if (!net_peer_check[p]) net_peer_check[p] = calloc(1u << 20, sizeof *net_peer_check[p]);
            else memset(net_peer_check[p], 0, (1u << 20) * sizeof *net_peer_check[p]);
        }
        memset(net_acked_by, 0, sizeof net_acked_by); memset(net_heard, 0, sizeof net_heard); net_checked = key;
        net_countdown = 0;
        /* (every count of the report from zero: it added up over a session's fights, twopc14) */
        net_rollbacks = net_rerun = net_waits = net_too_deep = net_saves_made = net_saves_skipped = net_older = 0;
        rb_fiber_refused = rb_fiber_retried_ok = 0;
        memset(net_depth, 0, sizeof net_depth);
        net_catch_ms = net_catch_max = net_wait_ms = 0;
        net_checks = net_desyncs = net_msgs_in = net_msgs_out = net_sync_waits = 0;
        net_first_desync = -1;
        memset(check_recs, 0, sizeof check_recs); net_again = net_again_bad = 0;
        net_rtt_min_s = 1e9; net_rtt_sum = 0; net_rtt_n = 0; net_adv_ours = net_adv_peer = 0;
        memset(net_sent_at, 0, sizeof net_sent_at);
        rbl_stats(&net_link_base[0], &net_link_base[1], &net_link_base[2]);
        net_key0 = net_low = key; net_t0 = now_ms(); net_done = 0;
        fprintf(stderr, "[rb] loop mode: fight starts at logic frame %u\n", key);
        if (net_mode == 2) fprintf(stderr, "[rb] net mode: the match seed %s (%016llx)\n", hs_agreed ? "was agreed" : "was NOT agreed: the start may differ", (unsigned long long)hs_seed);
        /* SSBU_RB_SEED=<n>: the 9 script random generators (4 state words and a call count each) set
         * from <n> now, so two fights can start alike (two runs differed from frame 0: loop2). A
         * stand-in for step 3's handshake, which agrees on a seed between two PCs. */
        const char *sd = getenv("SSBU_RB_SEED"), *sf = getenv("SSBU_RB_SEED_FIGHT");
        u64 rb = rng_base();
        if (sf && *sf == '0') sd = NULL;   /* (step 3b's test: the match seed alone, net mode's global generator, outside fights) */
        if (sd && *sd && rb) {
            u64 z = strtoull(sd, NULL, 0);
            for (int g = 0; g < 9; g++) {
                for (int w = 0; w < 4; w++) {   /* (splitmix64: never all zero) */
                    z += 0x9e3779b97f4a7c15ull;
                    u64 x = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; x = (x ^ (x >> 27)) * 0x94d049bb133111ebull; x ^= x >> 31;
                    st32(rb + 20ull * g + 4ull * w, (u32)x | 1);
                }
                st32(rb + 20ull * g + 16, 0);
            }
            fprintf(stderr, "[rb] loop mode: script random generators seeded from %s\n", sd);
        }
    }
    if (S.frames > 8 && S.frames <= 300) {   /* (then, to 5 s: each generator's calls whenever they change) */
        static u32 prev[9];
        u64 rb = rng_base();
        int changed = 0;
        char g[200] = ""; size_t k = 0;
        for (int q = 0; q < 9 && rb; q++) { u32 v = ld32(rb + 20ull * q + 16); if (v != prev[q]) changed = 1; prev[q] = v; k += snprintf(g + k, sizeof g - k, " %u", v); }
        if (changed) fprintf(stderr, "[rb] loop mode, fight frame %lld (logic %u): generator calls now%s\n", S.frames, key, g);
    }
    if (S.frames <= 8) {   /* (what two runs' first frames hold, to see what differs: fighters, each generator's calls, the pads) */
        FFrame f; fight_frame(c, &f);
        char t[256];
        for (u32 k = 0; k < f.n; k++) { frec_text(t, sizeof t, &f.f[k]); fprintf(stderr, "[rb] loop mode, fight frame %lld: %s\n", S.frames, t); }
        char g[200] = ""; size_t k = 0;
        u64 rb = rng_base();
        for (int q = 0; q < 9 && rb; q++) k += snprintf(g + k, sizeof g - k, " %u", ld32(rb + 20ull * q + 16));
        fprintf(stderr, "[rb] loop mode, fight frame %lld (logic %u): generators %u calls (each:%s), %016llx; pads slot 0 %#llx %d,%d, slot 1 %#llx %d,%d\n",
                S.frames, key, f.rng_calls, g, (unsigned long long)f.rng, (unsigned long long)net_last_pad[0].buttons, net_last_pad[0].lx, net_last_pad[0].ly,
                (unsigned long long)net_last_pad[1].buttons, net_last_pad[1].lx, net_last_pad[1].ly);
    }
    if (rb_fast > 0 && InterlockedDecrement(&rb_fast) == 0) {
        rb_catchup = 0;
        double ms = now_ms() - ring_load_t0;
        net_catch_ms += ms; if (ms > net_catch_max) net_catch_max = ms;
    }
    u32 i = key - net_key0;
    if (i < 1u << 20) {
        if (net_every < 0) { const char *e = getenv("SSBU_RB_NET_CHECK_EVERY"); net_every = e && atoi(e) > 0 ? atoi(e) : 1; }   /* (v0.9.6: every frame, was 10: a desync's first frame exact) */
        if (rb_fast <= 0 && !net_done && key % (u32)net_every == 0) {
            /* the fighters and the generators, all but generator 1: something outside the fight (on Final
             * Destination; its background?) draws from it every other frame, starting at a different frame
             * each run, while the fighters stay the same to the last bit (dbg1-3, seed6: 30 s alike) */
            double t0 = now_ms();
            FFrame f; fight_frame(c, &f);
            u64 h = hfnv(0xcbf29ce484222325ull, f.f, sizeof(FRec) * f.n), rb = rng_base();
            for (int q = 0; q < 9 && rb; q++) if (q != 1) { u32 w[5]; for (int k = 0; k < 5; k++) w[k] = ld32(rb + 20ull * q + 4ull * k); h = hfnv(h, w, sizeof w); }
            net_final[i] = h ? h : 1;
            net_work(0, t0);
            check_again(key, &f, net_final[i]);
        }
        else net_final[i] = 0;   /* (run again in the middle of a catch-up: not hashed) */
    }
    if (!net_done && S.frames >= net_frames) { net_done = 1; net_report(); }
    if (rb_fast <= 0 && S.frames % 60 == 0 && S.frames <= net_frames) {   /* (once a second: who reads which pad, where the fighters are) */
        double t0 = now_ms();
        FFrame f; fight_frame(c, &f);
        char t[1024]; size_t k = 0;
        for (int p = 0; p < 8; p++) if (net_reads[p])
            k += snprintf(t + k, sizeof t - k, " | slot %d: %lld reads (key %u), last %#llx %d,%d", p, net_reads[p], net_last_key[p],
                          p < 2 ? (unsigned long long)net_last_pad[p].buttons : 0ull, p < 2 ? net_last_pad[p].lx : 0, p < 2 ? net_last_pad[p].ly : 0);
        for (u32 q = 0; q < f.n; q++) k += snprintf(t + k, sizeof t - k, " | P%d st %#x pos %.1f,%.1f", f.f[q].entry + 1, f.f[q].st, f.f[q].x, f.f[q].y);
        fprintf(stderr, "[rb] loop mode, fight frame %lld:%s\n", S.frames, t);
        memset(net_reads, 0, sizeof net_reads);
        net_work(2, t0);
    }
    if (rb_fast > 0) {   /* the middle of a catch-up: a save every n-th frame, nothing else */
        LONG done = catch_total - rb_fast;
        if (rerun_save != 1 && (rerun_save == 0 || done % rerun_save)) { rb_save_ms[0] = rb_save_ms[1] = rb_save_ms[2] = 0; return; }
    }
    /* A save only where a rollback could come back to it: a wrong guess at frame m loads a state tagged m - 1 or
     * before, so a state is needed at `key` only while the other player's pad for key + 1 hasn't come (it will be
     * guessed). When it has, no guess can go wrong before key + 2, whose own safe point saves if needed. The game
     * behind never guesses and saved every frame for nothing: some 9 ms a frame on PC 2 (GTX 1660 Ti), which ran at
     * about 20 ms a frame and held PC 1 back (twopc10, twopc14). SSBU_RB_NET_LAZY=0: a save every frame. */
    static int lazy = -1;
    if (lazy < 0) { const char *e = getenv("SSBU_RB_NET_LAZY"); lazy = !(e && *e == '0'); }
    int skip_save = lazy && rb_fast <= 0 && net_remote_low() >= key + 1;   /* (every other player's: M7) */
    if (skip_save) { net_saves_skipped++; rb_save_ms[0] = rb_save_ms[1] = rb_save_ms[2] = 0; }
    else {
        rb_save_tag = key;
        double t0 = now_ms();
        if (rb_save(c, frame) == 2) { catch_total = pending_fast; rb_fast = pending_fast; return; }
        if (rb_fast <= 0) net_work(1, t0);
        net_saves_made++;
    }
    if (rb_fast > 0) return;
    if (net_mode == 2) {
        /* this game's pad for the frame `delay` on; the peer's that came; ours to the peer; then the next
         * frame waits while it would run too far ahead of the peer's pads, or for the frame advantage */
        rbn_local(key, net_live == 1 ? net_live_pad : net_pad(net_local, key));
        u32 nw = rbn_local_newest();
        if (net_sent_at[nw & (RBN_RING - 1)] != nw) { net_sent_at[nw & (RBN_RING - 1)] = nw; net_sent_ms[nw & (RBN_RING - 1)] = now_ms(); }
        net_receive(key);
        net_compare(key);
        net_send(key);
        net_gone_check(key, 0);
        if (!rbn_can_run(key + 1)) {
            net_waits++;
            double w0 = now_ms(), last = w0;
            /* (10 min: two PCs a room apart, started by one person; but a player heard from this fight and quiet for
             * NET_GONE_MS, or whose fight is over, is gone and waited for no more: rb_net - v0.9) */
            while (!rbn_can_run(key + 1) && now_ms() - w0 < 600000) {
                Sleep(1);
                net_receive(key);
                if (now_ms() - last > 16) { net_send(key); last = now_ms(); }   /* (our pads and acks again: the peer may be waiting too) */
                net_gone_check(key, now_ms() - w0);
            }
            net_wait_ms += now_ms() - w0;
            if (!rbn_can_run(key + 1)) fprintf(stderr, "[rb] net mode: no pads from the peer for 10 min (logic frame %u): going on with guesses\n", key);
        }
        if (net_hold_sleep < 0) { const char *e = getenv("SSBU_RB_NET_HOLD"); net_hold_sleep = e && !strcmp(e, "sleep"); }
        if (rbn_sync_wait(key + 1)) {
            net_sync_waits++;
            if (net_hold_sleep) Sleep(16); else InterlockedIncrement(&rb_vsync_skip);
        }
        if (S.frames % 60 == 0) {
            fprintf(stderr, "[rb] net sync, fight frame %lld (logic %u): frame advantage ours %d, the peer's %d; %lld holds (%lld refreshes left out), "
                    "%lld rollbacks, %lld waits for pads; ack round trip %.1f ms least this second, %.1f mean; pads sent in %lld messages, %.0f bytes on average, %lld at most\n",
                    S.frames, key, net_adv_ours, net_adv_peer, net_sync_waits, (long long)rb_vsync_skipped, net_rollbacks, net_waits,
                    net_rtt_min_s < 1e9 ? net_rtt_min_s : -1.0, net_rtt_n ? net_rtt_sum / net_rtt_n : -1.0,
                    net_msgs_s, net_msgs_s ? (double)net_bytes_s / net_msgs_s : 0.0, net_bytes_max_s);
            net_bytes_s = net_msgs_s = net_bytes_max_s = 0;
            fprintf(stderr, "[rb] net work, the last second: hashes %lld (%.1f ms, worst %.1f), saves %lld (%.1f ms, worst %.1f), fighter line %lld (%.1f ms, worst %.1f); "
                    "of them the game's getters %lld (%.1f ms), generator look-ups that asked Windows %lld (%.1f ms)\n",
                    net_w_n[0], net_w_ms[0], net_w_max[0], net_w_n[1], net_w_ms[1], net_w_max[1], net_w_n[2], net_w_ms[2], net_w_max[2],
                    rb_ff_get_n, rb_ff_get_ms, rb_rng_slow_n, rb_rng_slow_ms);
            if (rb_save_count) {
                double k = (double)rb_save_count;
                fprintf(stderr, "[rb] net saves, the last second: %lld, a save %.2f ms on average: parking the threads %.2f, GPU drain %.2f, memory %.2f, the rest %.2f\n",
                        rb_save_count, rb_save_parts[3] / k, rb_save_parts[0] / k, rb_save_parts[1] / k, rb_save_parts[2] / k,
                        (rb_save_parts[3] - rb_save_parts[0] - rb_save_parts[1] - rb_save_parts[2]) / k);
            }
            memset(rb_save_parts, 0, sizeof rb_save_parts); rb_save_count = 0;
            memset(net_w_ms, 0, sizeof net_w_ms); memset(net_w_max, 0, sizeof net_w_max); memset(net_w_n, 0, sizeof net_w_n);
            rb_ff_get_ms = rb_rng_slow_ms = 0; rb_ff_get_n = rb_rng_slow_n = 0;
            net_rtt_min_s = 1e9;
        }
    } else {
        /* the pads that came; the next frame waits while it would run too far ahead of them */
        net_deliver(key, 0);
        if (!rbn_can_run(key + 1)) { net_waits++; net_deliver(key, 1); }
    }
    u32 m = rbn_take_mismatch();
    if (!m || m > key) { rb_release_held(); return; }
    u32 found = 0;
    int age = rb_ring_find(m - 1, &found);
    if (age < 0) { net_too_deep++; rb_release_held(); return; }
    int depth = (int)(key - found);
    net_rollbacks++; net_rerun += depth; net_depth[depth < 15 ? depth : 15]++;
    for (u32 f = found + 1; f <= key; f++) if (f - net_key0 < 1u << 20) net_final[f - net_key0] = 0;
    pending_fast = depth;
    ring_load_t0 = now_ms();
    rb_hist_add(key, found, 0);
    rb_catchup = 1;
    rb_load_age(c, frame, age);   /* (returns only if refused) */
    net_rollbacks--; net_rerun -= depth; net_depth[depth < 15 ? depth : 15]--;
    /* M7: refused (a thread on another fiber than at that save, even after waiting): an older state may have had
     * every thread where it is now. Up to 3 more, each a frame or so further back; the frames since go again. */
    for (int older = age + 1; older <= age + 3; older++) {
        u32 tag = rb_ring_tag(older);
        if (!tag || tag < net_key0) break;
        int d = (int)(key - tag);
        net_rollbacks++; net_rerun += d; net_depth[d < 15 ? d : 15]++; net_older++;
        for (u32 f = tag + 1; f <= key; f++) if (f - net_key0 < 1u << 20) net_final[f - net_key0] = 0;
        pending_fast = d;
        rb_hist_add(key, tag, 1);
        rb_load_age(c, frame, older);   /* (returns only if refused) */
        net_rollbacks--; net_rerun -= d; net_depth[d < 15 ? d : 15]--; net_older--;
    }
    rb_catchup = 0;   /* (every load refused) */
    rb_release_held();
    rbn_put_mismatch(m);
}
/* (rb_pad, in loop mode: both players' pads by logic frame) */
static int net_pad_for(u32 id, u32 key, u64 *buttons, i32 *lx, i32 *ly, i32 *rx, i32 *ry, i32 *lt, i32 *rt) {
    if (net_mode != 1 && net_mode != 2) return 0;
    if (net_mode == 2 && !net_armed) return 0;   /* (the controls screen's test fight: this PC's own pads) */
    if (!net_got) {   /* a fight's reads before its first safe point: they were live; now the bots' too (both from net_pad) */
        static int shown;
        if (shown < 16 && id < 2) {
            shown++;
            fprintf(stderr, "[rb] loop mode: pad read before the fight's first safe point: slot %u, logic frame %u, live %#llx %d,%d (lt %d rt %d)\n",
                    id, key, (unsigned long long)*buttons, *lx, *ly, *lt, *rt);
        }
        if ((int)id >= (net_mode == 2 ? net_n : 2)) return 0;
        RbnPad p = net_live == 1 ? (RbnPad){0} : net_pad((int)id, key);   /* (live: nothing held yet, in both games) */
        *buttons = p.buttons; *lx = p.lx; *ly = p.ly; *rx = p.rx; *ry = p.ry; *lt = p.lt; *rt = p.rt;
        return 1;
    }
    if (id < 8) { net_reads[id]++; net_last_key[id] = key; }
    if (net_mode == 2 && net_live == 1 && (int)id == net_local && rb_fast <= 0) {   /* (the controllers now: this game's next pad) */
        RbnPad l = {*buttons, (int16_t)*lx, (int16_t)*ly, (int16_t)*rx, (int16_t)*ry, (int16_t)*lt, (int16_t)*rt};
        net_live_pad = rbn_rest(l);
    }
    if ((int)id >= (net_mode == 2 ? net_n : 2)) return 0;
    RbnPad p = net_mode == 2 ? rbn_get((int)id, key, NULL) : id == 0 ? net_pad(0, key) : rbn_get(1, key, NULL);
    *buttons = p.buttons; *lx = p.lx; *ly = p.ly; *rx = p.rx; *ry = p.ry; *lt = p.lt; *rt = p.rt;
    net_last_pad[id] = p;
    return 1;
}

/* ---- M5 step 5: the handshake, in the menus (net mode) --------------------------------------------
 * Player 1's game proposes the match seed (SSBU_RB_SEED if set, for tests that must repeat; else from
 * the clock); player 2's takes the first one it hears and answers with it; once both have it, both set
 * the game's global generator from it at every safe point outside fights (savestates), so the match
 * set-up draws the same in both (step 3b). HELLO messages every 10 safe points until the fight; other
 * messages heard in the menus are dropped (the peer's pads come again in every message: nothing is
 * lost). The characters and the stage are still chosen by hand in both games; a match set up
 * differently shows as a desync on the fight's first frames, said as such (net_compare). */
/* The game's global xorshift128 (13.0.5 0x71052391c0) set from the agreed seed. */
static void net_seed_global(const char *why) {
    const u64 g = 0x71052391c0ull;
    if (!hs_agreed || !readable(g, 16)) return;
    static int shown;
    if (shown < 4) {
        shown++;
        fprintf(stderr, "[rb] net mode: the global generator %#llx was %08x %08x %08x %08x; set from the agreed seed (%s)\n",
                (unsigned long long)g, ld32(g), ld32(g + 4), ld32(g + 8), ld32(g + 12), why);
    }
    u64 z = hs_seed ^ 0x5eed5eedull;
    for (int k = 0; k < 4; k++) {
        z += 0x9e3779b97f4a7c15ull;
        u64 y = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; y = (y ^ (y >> 27)) * 0x94d049bb133111ebull; y ^= y >> 31;
        st32(g + 4ull * k, (u32)y | (k == 0));
    }
}
/* (M7: one hello to each game this one talks to - the host to each player, the others to the host - each with the
 * echo of that game's clock) */
static void net_hello_send(void) {
    for (int p = 0; p < net_n; p++) {
        if (p == net_local || (net_local != 0 && p != 0)) continue;
        RbnHello h = {net_local, RBN_VERSION, hs_agreed, 0, (net_local == 0 && hs_delay_ready) || hs_agreed ? hs_seed : 0};
        if (net_local == 0) { h.players = net_n; h.assign = p + 1; }   /* (Host/Join: its number, every time) */
        if (net_local < 0) {   /* (a joiner with no number yet: its join id) */
            if (!hs_join_id) { LARGE_INTEGER t; QueryPerformanceCounter(&t); hs_join_id = net_mix((u64)t.QuadPart, (u64)GetCurrentProcessId()) | 1; }
            h.seed = hs_join_id;
        }
        if (net_local == 0 && hs_delay_ready) h.input_delay = net_delay + 1;
        u64 now = now_us();
        h.t = now;
        if (hs_peer_t[p]) { h.echo = hs_peer_t[p]; h.hold = (u32)(now - hs_peer_t_at[p]); }
        u8 out[RBN_HELLO_BYTES];
        size_t k = rbn_hello_build(out, sizeof out, &h);
        if (k) rbl_send_to(p, out, (int)k);
    }
}
static void net_hello(int now) {
    static long long n;
    static int bad_version;
    if (net_pending) {   /* (R7, relay,auto: no hello before this game knows its number) */
        int p = rbl_relay_number();
        if (p < 0) return;
        net_pending = 0; net_local = p;
        if (p == 0) { rbl_players(net_n); fprintf(stderr, "[rb] net mode: %d players (SSBU_RB_NET_PLAYERS)\n", net_n); }
    }
    if (!hs_seed && net_local == 0) {
        const char *sd = getenv("SSBU_RB_SEED");
        if (sd && *sd) hs_seed = strtoull(sd, NULL, 0);
        else {
            LARGE_INTEGER t; QueryPerformanceCounter(&t);
            hs_seed = net_mix((u64)t.QuadPart, ((u64)GetCurrentProcessId() << 32) ^ (u64)time(NULL));
        }
        if (!hs_seed) hs_seed = 1;
        fprintf(stderr, "[rb] handshake: this game (player 1) proposes the match seed %016llx\n", (unsigned long long)hs_seed);
    }
    if (net_local == 0 && !hs_delay_ready) {   /* (the seed goes out with the delay, not before) */
        /* (M7: every other player measured, each over HS_RTT_MS at least; the delay from the longest of their trips:
         * two players' pads meet at the host, each about half its own trip away from it) */
        int ready = net_delay >= 0;
        double worst = 0;
        if (!ready) {
            ready = 1;
            for (int p = 1; p < net_n; p++) {
                /* (an Online match is set up on both games at once: its handshake measures for 1.5 s, not 5) */
                if (hs_rtt_n[p] < (net_online ? 20 : HS_RTT_SAMPLES) || now_ms() - hs_rtt_t0[p] < (net_online ? 1500.0 : HS_RTT_MS)) { ready = 0; break; }
                if (hs_rtt_min[p] > worst) worst = hs_rtt_min[p];
            }
        }
        if (ready) {
            if (net_delay < 0) {
                net_delay = net_online ? rbn_online_delay_for_rtt(worst) : rbn_delay_for_rtt(worst);
                char t[200] = ""; size_t k = 0;
                for (int p = 1; p < net_n; p++) k += snprintf(t + k, sizeof t - k, "%s player %d %.1f ms", p > 1 ? "," : "", p + 1, hs_rtt_min[p]);
                fprintf(stderr, "[rb] handshake: round trips (the least of %d over 5 s or more):%s; the longest %.1f ms: input delay %d\n", HS_RTT_SAMPLES, t, worst, net_delay);
            } else fprintf(stderr, "[rb] handshake: input delay %d (set)\n", net_delay);
            hs_delay_ready = 1;
        }
    }
    static u8 buf[2048];
    int len;
    while ((len = rbl_recv(buf, sizeof buf)) > 0) {
        RbnHello h;
        if (net_rules_take(buf, len)) continue;
        if (net_ready_take(buf, len)) continue;
        if (net_pre_take(buf, len)) continue;
        if (net_menu_take(buf, len)) continue;
        if (net_choice_take(buf, len)) continue;
        if (!rbn_hello_parse(buf, (size_t)len, &h)) continue;   /* (the peer's pads from a fight it is already in: they come again) */
        if (h.version != RBN_VERSION) {
            if (!bad_version++) fprintf(stderr, "[rb] handshake: the peer speaks version %d, this game %d: no agreement (both PCs need the same build)\n", h.version, RBN_VERSION);
            continue;
        }
        if (!net_hello_heard(&h)) continue;
        if (net_local > 0 && h.player == 0 && h.seed && h.seed != hs_seed && h.input_delay) {
            int was = net_delay;
            hs_seed = h.seed; hs_agreed = 1;
            net_delay = h.input_delay - 1; hs_delay_ready = 1;
            if (h.players >= 2 && h.players <= RBN_PLAYERS) net_n = h.players;   /* (M7: how many play, from the host) */
            if (net_local >= net_n) fprintf(stderr, "[rb] handshake: the host plays %d players, but this PC is set up as player %d: start it as player 2-%d\n",
                                            net_n, net_local + 1, net_n);
            fprintf(stderr, "[rb] handshake: took player 1's match seed %016llx, input delay %d, %d players%s\n", (unsigned long long)hs_seed, net_delay, net_n,
                    was >= 0 && was != net_delay ? " (this PC was set to another delay: player 1's counts)" : "");
        }
        if (net_local == 0 && h.player >= 1 && h.player < net_n && h.seed && h.seed == hs_seed && !hs_agreed_by[h.player]) {
            hs_agreed_by[h.player] = 1;
            int all = 1;
            for (int p = 1; p < net_n; p++) all &= hs_agreed_by[p];
            fprintf(stderr, "[rb] handshake: player %d has the match seed%s\n", h.player + 1, all ? ": every player has it, agreed" : "");
            if (all) hs_agreed = 1;
        }
    }
    if (n++ % 10 && !now) return;
    net_hello_send();
}
/* At a match set-up (player 1's record built): wait for the seed to be agreed, up to 10 min, hellos going
 * out and coming in. The handshake ran only while both games were in the menus: a game whose fight was
 * set up before the other had started never agreed (twopc4-7: one PC set up first, the other started a
 * room away), so the two began from different seeds and on different sides. */
/* A CHOICE from the peer: kept (the newest), and ours sent back (it may be waiting for it). 1: it was one. */
static int net_choice_take(const u8 *buf, int len) {
    RbnChoice c;
    if (!rbn_choice_parse(buf, (size_t)len, &c)) return 0;
    if (c.player == net_local || c.player >= net_n) return 1;
    if (rbl_accept(c.player) < 0) return 1;
    if (net_local == 0) for (int k = 1; k < net_n; k++) if (k != c.player) rbl_send_to(k, buf, len);   /* (the host passes it on: M7) */
    RbnChoice cmp = c; cmp.ask = 0;
    if (!ch_have_peer[c.player] || memcmp(&cmp, &ch_peer[c.player], sizeof cmp))
        fprintf(stderr, "[rb] handshake: player %d's choice came: ui_chara %016llx, colour %d%s%s\n", c.player + 1, (unsigned long long)c.chara, c.colour,
                c.has_stage ? ", and the stage" : "", c.gc ? " (a GameCube controller: its slot is one here too)" : "");
    int ask = c.ask;
    c.ask = 0;
    ch_peer[c.player] = c; ch_have_peer[c.player] = 1;
    if (ask) net_choice_send(0);   /* (only when asked: two games answering every answer ping-ponged some 30 000 messages, twopc11) */
    return 1;
}
static void net_choice_send(int ask) {
    if (!ch_have_mine) return;
    u8 out[RBN_CHOICE_BYTES];
    ch_mine.ask = ask;
    size_t k = rbn_choice_build(out, sizeof out, &ch_mine);
    if (k) rbl_send(out, (int)k);
}
/* ---- name tags (version 10) ------------------------------------------------------------------------------------
 * The save's name tags are a list of 0xF7D8-byte slots (the save at 0x13ac8; the game keeps two copies in memory,
 * found by searching: heap addresses). A slot's head (RBN_TAG_BYTES): used u32 (0/1), the name's hash u32 (an empty
 * slot: 0x811c9dc5, FNV-1a's basis), its length u32 (up to 10), the name in UTF-16 (10), then the tag's controls and
 * settings. A panel picks one as + 0x214 = slot + 1 (0: none; tag research, 10-31: the first slot's tag -> 1, the second's -> 2).
 * A tag's controls turn the same pads into other moves, so every game must play a player with the same tag: each game
 * sends its own player's tag head with its choice, and the others write it into a free slot of their list (both
 * copies) for the match and point that player's panel at it. The slots are put back as they were once the match is
 * over (character select again, or the next set-up): a friend's tag is never kept in this PC's list. */
#define TAG_STRIDE 0xF7D8ull
#define TAG_EMPTY_HASH 0x811c9dc5u
#define TAG_MAX_SLOTS 128
#define TAG_SLOTS 60
u32 kernel_rb_regions(u64 (*out)[2], u32 max);
static u64 tag_list[2]; static int tag_lists, tag_slots;   /* (the copies' first slots; slots in each) */
static struct { u64 at; u8 *was; } tag_kept[2 * RBN_PLAYERS]; static int tag_nkept;
static u32 tag_mine_slot;                  /* (this game's player's tag: its slot + 1 here) */
static u32 tag_given[RBN_PLAYERS];         /* (each player's panel + 0x214 this set-up, once worked out) */
static int tag_slot_ok(u64 a) {
    if (!readable(a, RBN_TAG_BYTES)) return 0;
    u32 used = ld32(a), hash = ld32(a + 4), len = ld32(a + 8);
    /* (every slot, used or not, holds a control layout at + 0x24 - the default one when empty: blank memory doesn't) */
    int controls = 0;
    for (u32 k = 0x24; k < 0x34; k++) controls |= ld8(a + k);
    if (!controls) return 0;
    /* (the rest lenient: a deleted tag's slot looked otherwise in one game's memory than in the save, so that game found
     * its list two slots late - gc4, gc6, desyncs; the control layout keeps blank memory out, gc5) */
    (void)hash;
    return used <= 1 && len <= 10;
}
/* A slot that holds a real tag: used, named. */
static int tag_slot_named(u64 a) {
    if (!tag_slot_ok(a) || ld32(a) != 1) return 0;
    u32 len = ld32(a + 8);
    if (len < 1) return 0;
    for (u32 k = 0; k < len; k++) if (!ld8(a + 12 + 2 * k)) return 0;
    return 1;
}
/* (both copies, from any empty slot's head: back to the first slot, then counted; some 200 ms) */
static void tag_find(void) {
    if (tag_lists) return;
    static u64 reg[16384][2];
    u32 nr = kernel_rb_regions(reg, 16384); if (nr > 16384) nr = 16384;
    static const u8 head[12] = {0, 0, 0, 0, 0xc5, 0x9d, 0x1c, 0x81, 0, 0, 0, 0};
    double t0 = now_ms();
    for (u32 r = 0; r < nr && tag_lists < 2; r++)
        for (u64 a = reg[r][0], end = reg[r][0] + reg[r][1]; a < end && tag_lists < 2;) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(G2H(a), &mi, sizeof mi)) break;
            u64 run_end = H2G((u8 *)mi.BaseAddress + mi.RegionSize);
            if (run_end > end) run_end = end;
            int ok = mi.State == MEM_COMMIT && (mi.Protect & PAGE_READWRITE) && !(mi.Protect & PAGE_GUARD);
            const u8 *lo = (const u8 *)G2H(a), *he = (const u8 *)G2H(run_end);
            for (const u8 *q = lo + 4; ok && q + 8 <= he && tag_lists < 2; q++) {
                q = memchr(q, 0xc5, (size_t)(he - q));
                if (!q || q + 8 > he) break;
                if (memcmp(q - 4, head, 12)) continue;
                u64 s = H2G(q - 4);
                if (((s - H2G(lo)) & 3) || !tag_slot_ok(s)) continue;
                while (s >= TAG_STRIDE && tag_slot_ok(s - TAG_STRIDE)) s -= TAG_STRIDE;
                int n = 0;
                while (n < TAG_MAX_SLOTS && tag_slot_ok(s + TAG_STRIDE * (u64)n)) n++;
                /* (always 60 slots, and the list's end is sure: the no-name slot just before it can pass for an empty one - its
                 * first word is whatever the save holds - which would put every tag's number one off) */
                if (n > TAG_SLOTS) { s += TAG_STRIDE * (u64)(n - TAG_SLOTS); n = TAG_SLOTS; }
                if (n < 40 || (tag_lists && s == tag_list[0])) { q = (const u8 *)G2H(s + TAG_STRIDE * (u64)n) - 1; if (q < lo + 4 || q >= he) break; continue; }
                tag_list[tag_lists++] = s;
                if (!tag_slots || n < tag_slots) tag_slots = n;
                q = (const u8 *)G2H(s + TAG_STRIDE * (u64)n) - 1;
                if (q < lo + 4 || q >= he) break;
            }
            a = run_end;
        }
    fprintf(stderr, "[rb] name tags: %d copies of the list found (%#llx, %#llx), %d slots, in %.0f ms\n", tag_lists,
            (unsigned long long)tag_list[0], (unsigned long long)tag_list[1], tag_slots, now_ms() - t0);
    for (int k = 0; k < 3 && tag_lists > 0; k++) {   /* (the first slots' heads: if a slot ever looks odd again) */
        u64 s = tag_list[0] + TAG_STRIDE * (u64)k;
        fprintf(stderr, "[rb] name tags: slot %d: used %u, hash %08x, length %u, controls %02x %02x %02x %02x\n", k, ld32(s), ld32(s + 4), ld32(s + 8),
                ld8(s + 0x24), ld8(s + 0x25), ld8(s + 0x26), ld8(s + 0x27));
    }
    if (!tag_lists) tag_lists = -1;   /* (not again this run) */
}
static void tag_name(const u8 *head, char *out, int cap) {   /* (for the log: the name in ASCII, ? for the rest) */
    u32 len = head[8] | head[9] << 8; int k = 0;
    for (u32 i = 0; i < len && i < 10 && k < cap - 1; i++) { u8 lo = head[12 + 2 * i], hi = head[13 + 2 * i]; out[k++] = !hi && lo >= 32 && lo < 127 ? (char)lo : '?'; }
    out[k] = 0;
}
/* This game's player's tag (its panel's + 0x214) into its choice. */
/* (v0.9.7) A player without a tag plays with its PC's "no name" controls, which a player can change too: the others
 * played it with theirs - the same C-stick push a tilt on its own PC and a smash on another (a match over the internet, 11-01). The no-name set
 * is a slot like a tag's, just before the list (the save: 0x42f0, the list 0x13ac8, one TAG_STRIDE apart; its controls
 * at + 0x24 as a tag's). It goes as a tag named after the player ("P2"), which the others put in a free slot for the
 * match as any tag; this game keeps playing it without one. SSBU_RB_TEST_NONAME_DEFAULT=1 (tests, two games on one PC,
 * one save): this game's no-name controls set to the defaults for the match, as on a PC whose player never changed them. */
static int tag_noname(u8 *head) {
    tag_find();
    if (tag_lists <= 0) return 0;
    u64 nn = tag_list[0] - TAG_STRIDE;
    /* (told by the empty name and its controls: the slot's first word is no flag here - 0xaa000301 in one save - so not tag_slot_ok) */
    int controls = 0;
    if (readable(nn, RBN_TAG_BYTES)) for (u32 k = 0x24; k < 0x34; k++) controls |= ld8(nn + k);
    if (!controls || ld32(nn + 4) != TAG_EMPTY_HASH || ld32(nn + 8) != 0) { fprintf(stderr, "[rb] name tags: the no-name controls aren't before the list here: not sent\n"); return 0; }
    static int test = -1;
    if (test < 0) { const char *e = getenv("SSBU_RB_TEST_NONAME_DEFAULT"); test = e && *e == '1'; }
    if (test) {   /* (from an empty slot's controls, the defaults; the no-name slots kept to be put back after the match) */
        for (int k = 0; k < tag_slots; k++) {
            u64 e = tag_list[0] + TAG_STRIDE * (u64)k;
            if (!tag_slot_ok(e) || ld32(e) != 0 || ld32(e + 4) != TAG_EMPTY_HASH) continue;
            for (int l = 0; l < tag_lists && tag_nkept < 2 * RBN_PLAYERS; l++) {
                u64 s = tag_list[l] - TAG_STRIDE;
                u8 *was = malloc(TAG_STRIDE);
                if (!was) break;
                memcpy(was, G2H(s), TAG_STRIDE);
                tag_kept[tag_nkept].at = s; tag_kept[tag_nkept].was = was; tag_nkept++;
                memcpy(G2H(s + 0x24), G2H(e + 0x24), RBN_TAG_BYTES - 0x24);
            }
            fprintf(stderr, "[rb] name tags: (test) this game's no-name controls set to the defaults for the match\n");
            break;
        }
    }
    for (int k = 0; k < RBN_TAG_BYTES; k++) head[k] = ld8(nn + (u64)k);
    fprintf(stderr, "[rb] name tags: the no-name controls here: +0x33 %02x, +0x3c %02x, +0x50 %02x%02x\n", head[0x33], head[0x3c], head[0x51], head[0x50]);
    memset(head, 0, 0x24);
    head[0] = 1;                                                       /* used */
    u32 id = 0x52420000u | (u32)(net_local + 1); memcpy(head + 4, &id, 4);   /* (an id of its own, as a tag's) */
    head[8] = 2;                                                       /* the name: "P<n>", what the game shows anyway */
    head[12] = 'P'; head[14] = (u8)('1' + (net_local < 0 ? 0 : net_local));
    return 1;
}
static void tag_mine(u64 st) {
    u32 n = ld32(st + 0x214);
    ch_mine.has_tag = 0; tag_mine_slot = 0;
    if (!n || n == 0xffffffffu || (tag_find(), tag_lists <= 0 || n > (u32)tag_slots || !tag_slot_named(tag_list[0] + TAG_STRIDE * (n - 1)))) {
        /* (a deleted tag's slot, say: no tag on this PC; v0.9.7: on the others, this PC's no-name controls) */
        if (n && n != 0xffffffffu) {
            fprintf(stderr, "[rb] name tags: this game's player has tag %u, but it isn't a named tag in the list here: played without one, here too\n", n);
            st32(st + 0x214, 0);
        }
        if (tag_noname(ch_mine.tag)) {
            ch_mine.has_tag = 1;   /* (tag_mine_slot stays 0: here, the player plays without a tag, on these very controls) */
            fprintf(stderr, "[rb] name tags: this game's player has no tag: its no-name controls sent with its choice, as tag \"P%d\"\n", net_local + 1);
        }
        return;
    }
    u64 s = tag_list[0] + TAG_STRIDE * (n - 1);
    for (int k = 0; k < RBN_TAG_BYTES; k++) ch_mine.tag[k] = ld8(s + (u64)k);
    ch_mine.has_tag = 1; tag_mine_slot = n;
    char nm[16]; tag_name(ch_mine.tag, nm, sizeof nm);
    fprintf(stderr, "[rb] name tags: this game's player plays with tag \"%s\" (slot %u): sent with its choice\n", nm, n - 1);
}
/* Every slot written for the match back as it was. */
static void tag_restore(const char *why) {
    if (!tag_nkept) return;
    for (int i = 0; i < tag_nkept; i++) {
        if (readable(tag_kept[i].at, TAG_STRIDE)) memcpy(G2H(tag_kept[i].at), tag_kept[i].was, TAG_STRIDE);
        free(tag_kept[i].was);
    }
    fprintf(stderr, "[rb] name tags: the %d slot copies written for the match put back (%s)\n", tag_nkept, why);
    tag_nkept = 0;
}
/* Another player's tag: into a free slot (the last free in both copies), for the match. The slot + 1 for the panel;
 * 0 if it couldn't be placed (that player then plays with default controls here: the match will differ). */
static u32 tag_place(const u8 *head, int player) {
    char nm[16]; tag_name(head, nm, sizeof nm);
    tag_find();
    for (int slot = tag_slots - 1; tag_lists > 0 && slot >= 0; slot--) {
        int free_all = 1;
        for (int l = 0; l < tag_lists; l++) {
            u64 s = tag_list[l] + TAG_STRIDE * (u64)slot;
            if (!tag_slot_ok(s) || ld32(s) != 0) free_all = 0;
            for (int i = 0; i < tag_nkept; i++) if (tag_kept[i].at == s) free_all = 0;   /* (another player's this match) */
        }
        if (!free_all) continue;
        for (int l = 0; l < tag_lists && tag_nkept < 2 * RBN_PLAYERS; l++) {
            u64 s = tag_list[l] + TAG_STRIDE * (u64)slot;
            u8 *was = malloc(TAG_STRIDE);
            if (!was) break;
            memcpy(was, G2H(s), TAG_STRIDE);
            tag_kept[tag_nkept].at = s; tag_kept[tag_nkept].was = was; tag_nkept++;
            memcpy(G2H(s), head, RBN_TAG_BYTES);
        }
        fprintf(stderr, "[rb] name tags: player %d's tag \"%s\" put in this PC's free slot %d for the match\n", player + 1, nm, slot);
        return (u32)slot + 1;
    }
    fprintf(stderr, "[rb] name tags: player %d's tag \"%s\" has no free slot here (%d lists, %d slots): DEFAULT controls here (the match will differ if the tag changes them)\n",
            player + 1, nm, tag_lists, tag_slots);
    return 0;
}
/* ---- streamlining, item 2: the host's rules (RULES messages) ----------------------------------------------------
 * As the match is set up (player 1's record), the host reads the game's rule header (the match block + 0, 0x588 bytes:
 * stocks, time, items and the rest; 13.0.4 0x71052c41b0, ssbu-recomp PR #116's MELEE_INFO, one page on as all of
 * main's data), the stage set-up's inputs (its seed and the history it picks stage variations from: the PR's hs12, a
 * desync without them) and its first panel's rule kind and stocks, and sends them; every joining game waits for them
 * there and writes them over its own, so a friend's save needs no matching ruleset. The header holds no pointers: the PR
 * found two runs' headers alike but for bytes 0 and 1 (the mode), which each game keeps. */
#define MELEE_INFO 0x71052c51b0ull
#define RULE_BYTES 0x588
#define STAGE_SEED 0x710530e380ull          /* (13.0.4 0x710530d380) */
#define STAGE_HIST 0x71050c6ad0ull          /* (13.0.4 0x71050c5ad0) */
#define STAGE_HIST_BYTES 0x60
#define RULES_TOTAL (RULE_BYTES + STAGE_HIST_BYTES + 4 + 2)   /* header | history | seed u32 | rule kind | stocks */
static u8 rules_blob[RULES_TOTAL];
static u32 rules_id;
static int rules_have;                       /* (the host: read at this set-up; a joiner: every chunk of rules_id come) */
static unsigned rules_chunks;                /* (a joiner: which chunks of rules_id came, a bit each) */
static void net_rules_send(int ask) {
    static u8 out[RBN_RULES_HEAD + RBN_RULES_CHUNK];
    static RbnRules r;
    memset(&r, 0, sizeof r);
    r.player = net_local < 0 ? 0 : net_local;
    if (ask) { r.ask = 1; size_t k = rbn_rules_build(out, sizeof out, &r); if (k) rbl_send(out, (int)k); return; }
    if (net_local != 0 || !rules_have) return;
    r.id = rules_id; r.total = RULES_TOTAL;
    for (int off = 0; off < RULES_TOTAL; off += RBN_RULES_CHUNK) {
        r.off = off; r.len = RULES_TOTAL - off < RBN_RULES_CHUNK ? RULES_TOTAL - off : RBN_RULES_CHUNK;
        memcpy(r.data, rules_blob + off, (size_t)r.len);
        size_t k = rbn_rules_build(out, sizeof out, &r);
        if (k) rbl_send(out, (int)k);
    }
}
/* a RULES message: the host answers an ask with every chunk; a joiner keeps the host's chunks. 1: it was one. */
static int net_rules_take(const u8 *buf, int len) {
    static RbnRules r;
    if (!rbn_rules_parse(buf, (size_t)len, &r)) return 0;
    if (net_local == 0) {
        if (r.ask && r.player > 0 && r.player < net_n && rbl_accept(r.player) >= 0) net_rules_send(0);
        return 1;
    }
    if (r.player != 0 || r.ask || r.total != RULES_TOTAL || rbl_accept(0) < 0) return 1;
    if (r.id != rules_id) { rules_id = r.id; rules_chunks = 0; rules_have = 0; }
    memcpy(rules_blob + r.off, r.data, (size_t)r.len);
    rules_chunks |= 1u << (r.off / RBN_RULES_CHUNK);
    unsigned all = (1u << ((RULES_TOTAL + RBN_RULES_CHUNK - 1) / RBN_RULES_CHUNK)) - 1;
    if (rules_chunks == all && !rules_have) {
        rules_have = 1;
        fprintf(stderr, "[rb] handshake: the host's rules came (rule kind %d, stocks %d)\n", rules_blob[RULES_TOTAL - 2], rules_blob[RULES_TOTAL - 1]);
    }
    return 1;
}
/* at player 1's record: the host reads and sends its rules; a joiner waits for them and writes them over its own */
static void net_rules_setup(u64 st) {
    if (net_local == 0) {
        rules_have = 0;
        if (!readable(MELEE_INFO, RULE_BYTES) || !readable(STAGE_HIST, STAGE_HIST_BYTES) || !readable(STAGE_SEED, 4)) {
            fprintf(stderr, "[rb] handshake: the rules aren't readable here: each game keeps its own\n");
            return;
        }
        for (u32 k = 0; k < RULE_BYTES; k++) rules_blob[k] = ld8(MELEE_INFO + k);
        for (u32 k = 0; k < STAGE_HIST_BYTES; k++) rules_blob[RULE_BYTES + k] = ld8(STAGE_HIST + k);
        u32 seed = ld32(STAGE_SEED);
        memcpy(rules_blob + RULE_BYTES + STAGE_HIST_BYTES, &seed, 4);
        rules_blob[RULES_TOTAL - 2] = ld8(st + 5); rules_blob[RULES_TOTAL - 1] = ld8(st + 0xa);
        rules_id = (u32)hfnv(0xcbf29ce484222325ull, rules_blob, RULES_TOTAL) | 1;
        rules_have = 1;
        fprintf(stderr, "[rb] handshake: this game's rules go to the others (rule kind %d, stocks %d, items %#x)\n",
                rules_blob[RULES_TOTAL - 2], rules_blob[RULES_TOTAL - 1], ld32(MELEE_INFO + 0x54));
        for (int k = 0; k < 3; k++) { net_rules_send(0); Sleep(5); }
        return;
    }
    if (!rules_have) {
        fprintf(stderr, "[rb] handshake: waiting for the host's rules\n");
        double t0 = now_ms();
        while (!rules_have && now_ms() - t0 < 600000) { net_hello(1); net_rules_send(1); net_wait_say("Waiting for the host's rules..."); Sleep(16); }   /* (ours asks for them) */
        net_wait_say(NULL);
    }
    if (!rules_have) { fprintf(stderr, "[rb] handshake: no rules from the host after 10 min: this game's own (the match may differ)\n"); return; }
    if (!readable(MELEE_INFO, RULE_BYTES) || !readable(STAGE_HIST, STAGE_HIST_BYTES) || !readable(STAGE_SEED, 4)) {
        fprintf(stderr, "[rb] handshake: the rules aren't writable here: this game's own (the match may differ)\n");
        return;
    }
    u32 differ = 0, first = 0, last = 0;
    for (u32 k = 2; k < RULE_BYTES; k++)   /* (bytes 0 and 1, the mode, stay this game's) */
        if (ld8(MELEE_INFO + k) != rules_blob[k]) { if (!differ++) first = k; last = k; }
    /* (two rulesets differ in their settings - stocks, time, item switches - not in most of the header: a header far from
     * this game's is no rule header, and stays unwritten) */
    if (differ > 256) {
        fprintf(stderr, "[rb] handshake: the host's rule header differs from this game's in %u of %u bytes: not rules as known, left alone\n",
                differ, RULE_BYTES);
        return;
    }
    for (u32 k = 2; k < RULE_BYTES; k++) if (ld8(MELEE_INFO + k) != rules_blob[k]) st8(MELEE_INFO + k, rules_blob[k]);
    u32 hist_differ = 0;
    for (u32 k = 0; k < STAGE_HIST_BYTES; k++)
        if (ld8(STAGE_HIST + k) != rules_blob[RULE_BYTES + k]) { hist_differ++; st8(STAGE_HIST + k, rules_blob[RULE_BYTES + k]); }
    u32 seed; memcpy(&seed, rules_blob + RULE_BYTES + STAGE_HIST_BYTES, 4);
    u32 seed_was = ld32(STAGE_SEED);
    st32(STAGE_SEED, seed);
    if (differ) fprintf(stderr, "[rb] handshake: the host's rules written over this game's: %u bytes of the rule header differed (from +%#x to +%#x)\n", differ, first, last);
    else fprintf(stderr, "[rb] handshake: the host's rules are this game's already\n");
    if (hist_differ || seed_was != seed)
        fprintf(stderr, "[rb] handshake: the stage set-up's inputs taken from the host (%u history bytes differed, seed %s)\n", hist_differ, seed_was != seed ? "differed" : "the same");
}

/* ---- the GO! freeze -----------------------------------------------------------------------------------------------
 * Two causes, both moved to the end of loading:
 * (1) A fight's first save copied all of memory (PC 2: 2 GB, 565 ms; later fights 935-1385 ms here, undo pages for all of
 *     it on top) on fight frame 1, as GO! appears. Now: at the end of loading the copy is taken (rb_save, every state let
 *     go after: rb_ring_drop), and through the entrances and the countdown kept current every PRE_EVERY frames, so the
 *     save at GO! copies a few frames' changes. (Outside fights the game keeps to real time by itself, catching up for
 *     frames that ran long: the entrance doesn't run late on a slower PC for it.)
 * (2) The games began the entrances apart (one loads faster), so the first came to GO! first and waited for the others'
 *     pads, then the frame advantage held it back: a stutter after GO!. Now a barrier at the end of loading: each game,
 *     once its fighters exist and its other threads have been at a wait for PRE_QUIET frames running (loading done),
 *     says READY to the host; the host waits for every player's and then tells them all to go on. The entrances run on
 *     logic frames after it, the same number on every game: they reach GO! together. (Not all at the barrier after
 *     PRE_WAIT_MS: each goes on, as before.)
 * Until that point, a line every 30 frames says what the frames before the fight hold (fighters, busy threads). */
/* (the GO! hitch) With the whole copy taken in the loading, the fight's first save still turned the state map on
 * (hle_rb_state.c sm_begin: every alias page read-only but the hot ones), 31-71 ms in the logs, so GO! came that much
 * late (a frame gap up to 84 ms in a fight's first 600 frames, two games on one PC). Now it is turned on at a refresh
 * of the copy in the entrances, once the logic frame has run SSBU_RB_SM_PRE frames since the set-up (default 60: it
 * stands still through the loading and runs from the entrances on; GO! comes some 216 after); 0: at the fight's first
 * save, as before. Left on with no fight after (a set-up given up), it is turned off at the next set-up. */
void rb_state_map_fight(int on);   /* (hle_rb_state.c) */
static int sm_pre = -1, sm_pre_armed;
static u32 sm_pre_logic, sm_pre_runs;
static void sm_pre_reset(void) {
    if (sm_pre_armed && !S.in_fight) rb_state_map_fight(0);
    sm_pre_armed = 0; sm_pre_runs = 0; sm_pre_logic = 0;
}
static void sm_pre_step(long long pre_frame) {
    if (sm_pre < 0) { const char *e = getenv("SSBU_RB_SM_PRE"); sm_pre = e && *e ? atoi(e) : 60; }
    u32 lf = ld32(LOGIC_FRAME);
    if (lf != sm_pre_logic) { sm_pre_logic = lf; sm_pre_runs++; }
    if (sm_pre <= 0 || sm_pre_armed || sm_pre_runs < (u32)sm_pre) return;
    sm_pre_armed = 1;
    rb_state_map_fight(1);   /* (the next refresh that holds the threads turns it on) */
    fprintf(stderr, "[rb] before the fight: frame %lld since the set-up, the logic frame has run %u times (the entrances): the state map goes on at the next refresh\n",
            pre_frame, sm_pre_runs);
}

#define PRE_EVERY 4
#define PRE_QUIET 3
#define PRE_WAIT_MS 30000.0
static int pre_done, pre_quiet;
static long long pre_frames;
static int ready_from[RBN_PLAYERS], all_ready;
/* (v0.9.4, "the sync hangs too long on GO!") Every game comes to GO! the same number of frames after the set-up (hj2),
 * but one whose frames ran late in the loading (a slower PC, its first whole copy: 897 ms against 328 on the host, in a match over the internet) got
 * there later, and the others waited for its pads at GO!: up to a second frozen on it. Now every game says its frame
 * count since the set-up every frame (PRE), and one more than PRE_SLACK frames (plus the input delay, for the trip)
 * ahead of a player waits for it at its next frame: the waiting happens in the loading screen, a little at a time, and
 * all come to GO! together. PRE_SYNC_MAX_MS of waiting in all at most, then each goes on as before. */
#define PRE_SLACK 1
#define PRE_SYNC_MAX_MS 20000.0
/* (pre1, one PC: GO! came at frame 449 on the host and 467 on the joiner - not the same count after all. The host went
 * into its fight and said no more, so the joiner waited on it the whole PRE_SYNC_MAX_MS, while the host waited at GO! for
 * its pads. Now a game at GO! says so (PRE_AT_GO) and nobody waits for it after; a player whose count stands still
 * PRE_STALL_MS is no longer waited for either; and the GO! line gives the logic frames since the set-up too, to see
 * whether those agree between the games better than the frames do.)
 * (pre3: they do. The logic frame stands still at 1 through the loading and runs from the entrances on, and GO! came at
 * logic frame 216 on both games - at frame 431 on one and 464 on the other. So PRE now carries the logic frames since the
 * set-up, and the game whose loading ended first waits at the start of its entrances until the others' loading ends:
 * the entrances then run together. "Stands still" is not quiet: a player is waited for while its PRE keep coming.) */
#define PRE_AT_GO 0xFFFFFFFFu
#define PRE_STALL_MS 2000.0
static long long pre_peer[RBN_PLAYERS];   /* (each player's newest frame count since the set-up, -1: none heard) */
static double pre_peer_ms[RBN_PLAYERS];   /* (when it last moved) */
static double pre_waited_ms;
static long long pre_waits;
static u32 pre_logic0, pre_logic_last;   /* (the logic frame at the set-up, and on the last frame before the fight) */
static void net_pre_reset(void) {
    pre_done = 0; pre_quiet = 0; pre_frames = 0; memset(ready_from, 0, sizeof ready_from); all_ready = 0;
    for (int p = 0; p < RBN_PLAYERS; p++) { pre_peer[p] = -1; pre_peer_ms[p] = 0; }
    pre_waited_ms = 0; pre_waits = 0;
    sm_pre_reset();
}
static void net_pre_send_n(u32 frames) {
    u8 out[RBN_PRE_BYTES];
    RbnPre r = {net_local, frames};
    size_t k = net_local >= 0 ? rbn_pre_build(out, sizeof out, &r) : 0;
    if (k) rbl_send(out, (int)k);
}
static void net_pre_send(void) { net_pre_send_n(pre_logic_last - pre_logic0); }   /* (pre3: the logic frames, not the frames) */
static int net_pre_take(const u8 *buf, int len) {
    RbnPre r;
    if (!rbn_pre_parse(buf, (size_t)len, &r)) return 0;
    if (r.player == net_local || r.player >= net_n || rbl_accept(r.player) < 0) return 1;
    if (net_local == 0) for (int k = 1; k < net_n; k++) if (k != r.player) rbl_send_to(k, buf, len);   /* (the host passes it on) */
    if (S.in_fight || !net_countdown) return 1;
    long long f = r.frames == PRE_AT_GO ? 0x7FFFFFFFFFFFll : (long long)r.frames;   /* (at GO!: never waited for again) */
    if (f > pre_peer[r.player]) pre_peer[r.player] = f;
    pre_peer_ms[r.player] = now_ms();   /* (heard: its logic frame stands still all through its loading) */
    return 1;
}
/* (each frame before the fight) this game's count to the others; then, while it is ahead of a player heard from, wait */
static void net_pre_step(void) {
    if (pre_frames == 1) pre_logic0 = ld32(LOGIC_FRAME);
    pre_logic_last = ld32(LOGIC_FRAME);
    if (pre_frames % 30 == 0) {   /* (the timeline: does the logic frame stand still through the loading, and the same on every game?) */
        long long other = -1;
        for (int p = 0; p < net_n && p < RBN_PLAYERS; p++) if (p != net_local && pre_peer[p] >= 0) { other = pre_peer[p]; break; }
        fprintf(stderr, "[rb] before the fight: frame %lld, logic frames %u since the set-up; the other game's last heard %lld; waited %.0f ms so far\n",
                pre_frames, pre_logic_last - pre_logic0, other, pre_waited_ms);
    }
    net_pre_send();
    double t0 = now_ms(), last = t0;
    for (;;) {
        long long low = -1;
        double now = now_ms();
        for (int p = 0; p < net_n && p < RBN_PLAYERS; p++)
            if (p != net_local && pre_peer[p] >= 0 && now - pre_peer_ms[p] < PRE_STALL_MS && (low < 0 || pre_peer[p] < low)) low = pre_peer[p];
        if (low < 0 || (long long)(pre_logic_last - pre_logic0) <= low + PRE_SLACK + net_delay || pre_waited_ms + (now_ms() - t0) > PRE_SYNC_MAX_MS) break;
        Sleep(1);
        net_hello(0);
        if (now_ms() - last > 16) { net_pre_send(); last = now_ms(); watchdog_present(); }
    }
    double w = now_ms() - t0;
    if (w > 0.5) { pre_waited_ms += w; pre_waits++; }
}
static void net_ready_send(int all) {
    u8 out[RBN_READY_BYTES];
    RbnReady r = {net_local < 0 ? 0 : net_local, all};
    size_t k = rbn_ready_build(out, sizeof out, &r);
    if (k) rbl_send(out, (int)k);
}
static int net_ready_take(const u8 *buf, int len) {
    RbnReady r;
    if (!rbn_ready_parse(buf, (size_t)len, &r)) return 0;
    if (net_local == 0) { if (!r.all && r.player > 0 && r.player < net_n && rbl_accept(r.player) >= 0) ready_from[r.player] = 1; }
    else if (r.all && r.player == 0 && rbl_accept(0) >= 0) all_ready = 1;
    return 1;
}
/* the fighters are made (the fighter manager there, player 1's entry a battle object) */
static int fighters_made(Cpu *c) {
    static u64 inst = 1;
    if (inst == 1) inst = rt_symbol("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
    if (!inst) return 0;
    u64 mgr = ld64(inst), impl = mgr ? ld64(mgr) : 0;
    if (!impl) return 0;
    static Cpu keep;
    keep = *c;
    u32 id = (u32)rt_callsym(c, "_ZN3app7fighter20get_id_from_entry_idEi", 1, (u64)0);
    *c = keep;
    return id != 0x50000000u && id != 0;
}
extern double rb_park_quick_ms;   /* (hle_rb_state.c) */
static long long pre_refreshed, pre_skipped;
static void net_pre_fight(Cpu *c, u64 frame) {
    pre_frames++;
    net_pre_step();   /* (v0.9.4: in step with the others through the loading) */
    sm_pre_step(pre_frames);   /* (the GO! hitch: the state map on in the entrances) */
    /* (hj2: "fighters not yet" on every frame to GO! - the game gives the players their fighters at GO!, though the
     * fighters do their entrances before - so the barrier never came, and the whole copy stayed at GO!: 358 ms here,
     * 526 ms on PC 2, whose GO! then came that much later: the stutter. The two games had come to GO! the same number
     * of frames after the set-up. So now the whole copy is taken at the set-up, as the loading screen starts, and kept
     * current every PRE_EVERY frames through loading, the entrances and the countdown, each refresh waiting a few ms
     * at most for the threads (a thread busy loading: that refresh left out). The barrier waits on a better sign of
     * loading done: SSBU_RB_PRE_BARRIER=1 tries it as it is.) */
    static int barrier = -1;
    if (barrier < 0) { const char *e = getenv("SSBU_RB_PRE_BARRIER"); barrier = e && *e == '1'; }
    if (!barrier) {
        void rb_ring_drop(void);
        if (pre_frames == 1) {
            double t0 = now_ms();
            rb_save_tag = 0xFFFFFFFFu;
            int saved = rb_save(c, frame);
            rb_ring_drop();
            pre_refreshed = pre_skipped = 0;
            fprintf(stderr, "[rb] before the fight: the whole copy at the set-up %s in %.0f ms\n", saved == 1 ? "taken" : "NOT taken (the threads didn't stop)", now_ms() - t0);
            return;
        }
        if (pre_frames % PRE_EVERY) return;
        rb_park_quick_ms = 4;
        rb_save_tag = 0xFFFFFFFFu;
        double t0 = now_ms();
        int saved = rb_save(c, frame);
        rb_park_quick_ms = 0;
        if (saved == 1) { rb_ring_drop(); pre_refreshed++; } else pre_skipped++;
        if (pre_frames % 120 == 0)
            fprintf(stderr, "[rb] before the fight: frame %lld since the set-up: the copy refreshed %lld times, %lld left out (threads busy); this one %.1f ms\n",
                    pre_frames, pre_refreshed, pre_skipped, now_ms() - t0);
        return;
    }
    if (pre_done) {   /* the entrances and the countdown: the copy kept current */
        if (pre_frames % PRE_EVERY == 0) { rb_save_tag = 0xFFFFFFFFu; if (rb_save(c, frame) == 1) { void rb_ring_drop(void); rb_ring_drop(); } }
        return;
    }
    u32 alive, waiting, sleeping, nbusy; u64 busy[1];
    kernel_rb_census(c->thread, &alive, &waiting, &sleeping, busy, 0, &nbusy);
    int made = fighters_made(c);
    pre_quiet = made && !nbusy ? pre_quiet + 1 : 0;
    if (pre_frames % 30 == 1 || pre_quiet == PRE_QUIET)
        fprintf(stderr, "[rb] before the fight: frame %lld since the set-up, fighters %s, %u threads busy%s\n", pre_frames, made ? "made" : "not yet", nbusy,
                pre_quiet == PRE_QUIET ? ": loading done" : "");
    if (pre_quiet < PRE_QUIET) return;
    pre_done = 1;
    /* (1) the whole copy, here at the end of loading */
    double t0 = now_ms();
    rb_save_tag = 0xFFFFFFFFu;   /* (no fight frame's: let go at once anyway) */
    int saved = rb_save(c, frame);
    { void rb_ring_drop(void); rb_ring_drop(); }
    double copy_ms = now_ms() - t0;
    /* (2) every game here before the entrances */
    double t1 = now_ms();
    if (net_local == 0) {
        for (;;) {
            int all = 1;
            for (int p = 1; p < net_n; p++) all &= ready_from[p];
            if (all || now_ms() - t1 > PRE_WAIT_MS) break;
            net_hello(0); Sleep(2);   /* (hellos at their own pace: one call in 10) */
        }
        for (int k = 0; k < 5; k++) { net_ready_send(1); Sleep(2); }
    } else {
        double last = 0;
        while (!all_ready && now_ms() - t1 < PRE_WAIT_MS) {
            if (now_ms() - last > 16) { net_ready_send(0); last = now_ms(); }
            net_hello(0); Sleep(2);
        }
    }
    int ok = net_local == 0 ? 1 : all_ready;
    if (net_local == 0) for (int p = 1; p < net_n; p++) ok &= ready_from[p];
    fprintf(stderr, "[rb] before the fight: loading done at frame %lld; the whole copy %s in %.0f ms; %s after %.0f ms\n", pre_frames,
            saved == 1 ? "taken" : "NOT taken (the threads didn't stop)", copy_ms, ok ? "every game ready, the entrances start" : "NOT every game came: going on", now_ms() - t1);
}

/* (the results screen, research: each game runs its menus alone after a fight, so the other players' cards never show
 * ready on it. To pass ready presses on there and stop before character select, a sign of the change is needed: after a
 * fight, until the next set-up, these candidates go to the log every 30 frames and whenever one changes) */
/* ---- the results screen -------------------------------------------------------------------------------------------
 * Each game runs its menus alone after a fight, so on a PC the other players' cards never showed ready and its results
 * screen never ended (hj3: stuck on it). hj4: passing each player's presses to the others got the host through, but the
 * host's presses never showed on PC 2 (sent before PC 2's own results began, it seems). So now (the user's idea) each
 * game also presses its own player's A and B on every other player's card: one press readies them all, each PC going on
 * when its own player does; the presses from the others still count too. Now from the fight's end each game sends its own player's A and B every frame
 * (MENU, through the host), and presses each other player's for it (rb_pad): a friend readies on its PC and its card is
 * ready on all of them; the screen goes on once every player has, as at one console. It stops (to keep character select
 * alone: a passed A there could add a player) when the fighter manager the fight left changes (the fight's scene gone:
 * hj3 saw it stay the same through the results), at the next match set-up, or MENU_MAX_MS after the fight; a player
 * not heard from for half a second has its buttons let go. Every change of the fighter manager is logged, to see
 * when the scene goes. */
#define MENU_MAX_MS 180000.0
static double menu_heard_ms[RBN_PLAYERS];
static int net_menu_take(const u8 *buf, int len) {
    RbnMenu m;
    if (!rbn_menu_parse(buf, (size_t)len, &m)) return 0;
    if (m.player == net_local || m.player >= net_n || rbl_accept(m.player) < 0) return 1;
    if (net_local == 0) for (int k = 1; k < net_n; k++) if (k != m.player) rbl_send_to(k, buf, len);   /* (the host passes it on) */
    if (net_results) {
        LONG was = net_menu_remote[m.player];
        InterlockedExchange(&net_menu_remote[m.player], m.buttons);
        menu_heard_ms[m.player] = now_ms();
        if (m.buttons & ~was & 1) fprintf(stderr, "[rb] results: player %d pressed A\n", m.player + 1);
    }
    return 1;
}
/* (v0.9) LEFT: this game's fight is over - a few times, it is UDP; the others stop waiting for pads past its last frame */
static void net_left_send(void) {
    if (net_local < 0) return;
    RbnLeft l = {net_local, ld32(LOGIC_FRAME)};
    u8 out[RBN_LEFT_BYTES];
    size_t k = rbn_left_build(out, sizeof out, &l);
    for (int i = 0; k && i < 3; i++) rbl_send(out, (int)k);
}
static double net_fight_t0;   /* (v0.9.3: when this fight began here: a player never heard from in it is gone after NET_SILENT_MS) */
static void net_gone_reset(void) {
    net_fight_t0 = now_ms();
    rb_hist_n = 0;
    {   /* (v0.9.6, diagnostics: the main thread's floating-point mode and the processor, to set beside the other PC's) */
        int r[4]; char brand[49] = "";
        __cpuid(r, (int)0x80000000);
        if ((unsigned)r[0] >= 0x80000004u) for (int k = 0; k < 3; k++) { __cpuid(r, (int)(0x80000002 + k)); memcpy(brand + 16 * k, r, 16); }
        fprintf(stderr, "[rb] fight start: this thread's MXCSR %08x; processor %s\n", _mm_getcsr(), brand);
    }
    if (pre_frames) {
        for (int k = 0; k < 3; k++) net_pre_send_n(PRE_AT_GO);   /* (nobody waits for this game before the fight now) */
        fprintf(stderr, "[rb] before the fight: GO! at frame %lld since the set-up (logic frames %u); waited %.0f ms in all for the others, over %lld frames\n",
                pre_frames, pre_logic_last - pre_logic0, pre_waited_ms, pre_waits);
    }
    memset(net_gone_after, 0, sizeof net_gone_after);
    memset(net_pads_heard_ms, 0, sizeof net_pads_heard_ms);
    memset(net_gone_shown, 0, sizeof net_gone_shown);
    memset(net_gone_why, 0, sizeof net_gone_why);
}
static DWORD WINAPI net_gone_box(LPVOID text) {   /* (its own thread: the game goes on behind it) */
    /* (as hle_env.c's error_dialog: never in test runs - SSBU_HEADLESS=1, SSBU_SEQ - or with SSBU_ERROR_DIALOG=0: the log
     * and the report only. Two headless games on one PC put two of these on the owner's desktop, 11-04) */
    const char *off = getenv("SSBU_ERROR_DIALOG"), *headless = getenv("SSBU_HEADLESS");
    if ((off && off[0] == '0') || getenv("SSBU_SEQ") || (headless && headless[0] == '1')) return 0;
    MessageBoxA(NULL, (const char *)text, "Smash Bros multiplayer", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}
static void net_gone_mark(int p, u32 after, const char *why) {
    if (p < 0 || p >= net_n || p == net_local) return;
    if (net_gone_after[p] && net_gone_after[p] <= after) return;   /* (the earliest cutoff heard holds, as in rb_net) */
    rbn_gone(p, after ? after : 1, ld32(LOGIC_FRAME));
    net_gone_after[p] = rbn_gone_after(p); if (!net_gone_why[p]) net_gone_why[p] = why;
    fprintf(stderr, "[rb] net mode: player %d is gone from this fight (%s) after logic frame %u; this game is at %u\n",
            p + 1, why, net_gone_after[p], ld32(LOGIC_FRAME));
    {   /* (what the link knew then: for the report) */
        long long s, r, e; rbl_stats(&s, &r, &e);
        int net_unreachable(void);
        fprintf(stderr, "[rb] net mode: the link then: %lld messages sent, %lld received, %lld send errors, %lld dropped (not the player's, or "
                "the queue full) in all; netplay %s; the last message from player %d %.1f s ago\n",
                s, r, e, rbl_dropped(), net_unreachable() ? "NOT reaching the lobby" : "reaching the lobby", p + 1,
                net_pads_heard_ms[p] > 0 ? (now_ms() - net_pads_heard_ms[p]) / 1000 : -1.0);
        { void rbl_relay_report(void); rbl_relay_report(); }
    }
    char why_all[160]; snprintf(why_all, sizeof why_all, "player %d lost: %s", p + 1, why);
    rb_keep_dump(why_all);
}
static void net_left_for(int p, u32 after) {   /* (the host: a joiner it gave up on, the same cutoff to every game) */
    RbnLeft l = {p, after};
    u8 out[RBN_LEFT_BYTES];
    size_t k = rbn_left_build(out, sizeof out, &l);
    for (int i = 0; k && i < 3; i++) rbl_send(out, (int)k);
}
static void net_gone_show(int p) {   /* (once this game's fight runs on past the gone player's: not at a normal end) */
    if (net_gone_shown[p]) return;
    net_gone_shown[p] = 1;
    fprintf(stderr, "[rb] net mode: this fight went on past player %d's end: the box shown\n", p + 1);
    static char text[768];
    snprintf(text, sizeof text,
             "Lost player %d: %s.\n\nThis fight goes on without them (their character stands still). Finish it, or pause and quit.\n\n"
             "If both games were still connected, the two games had gone out of sync: after the match, both players send their log (play.log) and the report below.%s%s",
             p + 1, net_gone_why[p], rb_kept_path[0] ? "\n\nThis game's rollback report: " : "", rb_kept_path);
    HANDLE h = CreateThread(NULL, 0, net_gone_box, text, 0, NULL);
    if (h) CloseHandle(h);
}
static void net_all_gone(const char *why) {   /* (a joiner without the host: every other player's pads came through it) */
    for (int p = 0; p < net_n && p < RBN_PLAYERS; p++) if (p != net_local) net_gone_mark(p, rbn_remote_newest(p), why);
}
static int net_left_take(const u8 *buf, int len) {
    RbnLeft l;
    if (!rbn_left_parse(buf, (size_t)len, &l)) return 0;
    if (l.player >= net_n || (l.player != net_local && rbl_accept(l.player) < 0)) return 1;
    if (net_local == 0) for (int k = 1; k < net_n; k++) if (k != l.player) rbl_send_to(k, buf, len);   /* (the host passes it on) */
    if (!S.in_fight) return 1;
    if (l.player == net_local) net_all_gone("the host stopped waiting for this game");   /* (it gave up on us: we are out) */
    else net_gone_mark(l.player, l.frame, "they left the fight or lost their connection");
    return 1;
}
/* (each frame of a fight) a player silent NET_GONE_MS is gone: the host decides for the joiners and tells every game the
 * cutoff, so the players left all play the same frames; a joiner only gives up on the host, and with it on everyone. And
 * a cutoff whose last real pads never came here (lost on the way) is moved to what came, after 3 s of waiting for them. */
static void net_gone_check(u32 key, double waited_ms) {
    double now = now_ms();
    /* (a stall of this game's own - a long frame, a loading hitch, this game's logic held - is not the others' silence:
     * the time since the last check, past 1 s, is not counted against them; brawlonline.net's GekkoNet fix of the same,
     * where a stall on one PC dropped a healthy peer. Their messages wait in the link's queue meanwhile) */
    static double last_check;
    if (last_check > 0 && now - last_check > 1000) {
        double gap = now - last_check;
        for (int p = 0; p < net_n && p < RBN_PLAYERS; p++) if (net_pads_heard_ms[p] > 0) net_pads_heard_ms[p] += gap;
        if (net_fight_t0 > 0) net_fight_t0 += gap;
        fprintf(stderr, "[rb] net mode: this game stalled %.1f s between two checks: not counted as the others' silence\n", gap / 1000);
    }
    last_check = now;
    for (int p = 0; p < net_n && p < RBN_PLAYERS; p++) {
        if (p == net_local) continue;
        int quiet = !net_gone_after[p] && (net_pads_heard_ms[p] > 0 ? now - net_pads_heard_ms[p] > NET_GONE_MS
                                                                   : net_fight_t0 > 0 && now - net_fight_t0 > NET_SILENT_MS);
        if (quiet && net_local == 0) {
            net_gone_mark(p, rbn_remote_newest(p), net_pads_heard_ms[p] > 0 ? "nothing came from their game for 8 seconds" : "their game never started this fight");
            net_left_for(p, net_gone_after[p]);
        } else if (quiet && p == 0) net_all_gone(net_pads_heard_ms[0] > 0 ? "nothing came from the host for 8 seconds" : "the host's game never started this fight");
        if (net_gone_after[p] && waited_ms > 3000 && rbn_remote_newest(p) < net_gone_after[p]) {
            fprintf(stderr, "[rb] net mode: player %d's last pads before its cutoff (%u) never came: cut at %u instead\n",
                    p + 1, net_gone_after[p], rbn_remote_newest(p));
            net_gone_mark(p, rbn_remote_newest(p), net_gone_why[p]);
        }
        if (net_gone_after[p] && key > net_gone_after[p] + 30) net_gone_show(p);   /* (half a second on) */
    }
}
/* (hj5: the logic frame stood a second in the middle of the results animation - the mirroring ended before anyone
 * readied, and neither PC got through. The game says itself whether it shows the results:
 * app::lua_bind::FighterManager__is_result_mode_impl (the fighters do their victory poses there, so the fighter manager
 * lives on through it). Mirroring is on while it says so, and ends when it stops; the next set-up and 3 minutes end it too.
 * Its changes are logged.) */
static u64 fm_ask(Cpu *c, u64 mgr, const char *sym) {   /* (a FighterManager query, the main thread's registers kept) */
    if (!mgr) return 0;
    static Cpu keep;
    keep = *c;
    u64 v = rt_callsym(c, sym, 1, mgr);
    *c = keep;
    return v;
}
static int net_result_mode(Cpu *c, u64 mgr) { return (int)(fm_ask(c, mgr, "_ZN3app8lua_bind35FighterManager__is_result_mode_implEPNS_14FighterManagerE") & 0xff); }
/* (hj6: is_result_mode came on 113 frames after the fight - mirroring began right as the results showed - but stayed on
 * in character select, so the mirroring went on there. The fighters are let go after the results, the manager staying:
 * the end is now its entry count or its fighter count changing from their values as the results began. Those, the
 * result mode, is_ready_go, the stage choice and the last set-up's first panel (a hash, every 30 frames) are logged
 * whenever one changes, to see character select come.) */
u64 net_panel0;   /* (the first panel state at the last set-up: the record builder's x0) */
static void net_results_frame(Cpu *c) {
    static long long fight_seen = -1, frames;
    static u64 mgr0;
    static double t0;
    static int ended, rm_last;
    static double t_end;
    static u64 cnt0, num0, seen[6];
    static u64 inst = 1;
    if (inst == 1) inst = rt_symbol("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
    u64 mgr = inst ? ld64(inst) : 0, impl = mgr ? ld64(mgr) : 0;
    if (fight_seen != S.fights) {   /* a fight just ended */
        fight_seen = S.fights; mgr0 = mgr; t0 = now_ms(); frames = 0; ended = 0; rm_last = -1; memset(seen, 0xff, sizeof seen);
        memset((void *)net_menu_remote, 0, sizeof net_menu_remote); InterlockedExchange(&net_menu_mine, 0);
        InterlockedExchange(&net_results, 0);
    }
    frames++;
    if (net_online && (mgr != mgr0 || now_ms() - t0 > 120000)) {   /* (an Online match's results: the scene gone, or 2 minutes on) */
        online_forget(mgr != mgr0 ? "the fight's scene is gone" : "2 minutes after the fight"); net_armed = 0;
        InterlockedExchange(&net_results, 0);
        return;
    }
    if (ended && now_ms() - t_end > 15000) return;   /* (then character select alone: the fighter manager no longer asked) */
    int rm = impl ? net_result_mode(c, mgr) : 0;
    u64 cnt = impl ? fm_ask(c, mgr, "_ZN3app8lua_bind32FighterManager__entry_count_implEPNS_14FighterManagerE") & 0xffffffff : 0, num = impl ? fm_ask(c, mgr, "_ZN3app8lua_bind38FighterManager__total_fighter_num_implEPNS_14FighterManagerE") & 0xffffffff : 0;
    u64 rg = impl ? fm_ask(c, mgr, "_ZN3app8lua_bind32FighterManager__is_ready_go_implEPNS_14FighterManagerE") & 0xff : 0;
    static u64 ph;
    if (frames % 30 == 1) ph = net_panel0 && readable(net_panel0, 0x460) ? hfnv(0xcbf29ce484222325ull, G2H(net_panel0), 0x460) : 0;
    u64 v[6] = {(u64)rm, cnt, num, rg, ld32(0x710530a968ull), ph};
    if (memcmp(v, seen, sizeof v) && frames < 20000)
        fprintf(stderr, "[rb] results: frame %lld after the fight: result mode %llu, entry count %llu, fighters %llu, ready-go %llu, stage choice %llu, panel 1 %016llx\n",
                frames, v[0], v[1], v[2], v[3], v[4], v[5]);
    memcpy(seen, v, sizeof v);
    rm_last = rm;
    if (!net_results) {
        if (!ended && rm) {
            InterlockedExchange(&net_results, 1); cnt0 = cnt; num0 = num;
            fprintf(stderr, "[rb] results: this game's A and B pressed on every player's card too (entry count %llu, fighters %llu)\n", cnt, num);
        }
        return;
    }
    const char *end = !rm ? "the results are over" : cnt != cnt0 || num != num0 ? "the fighters are let go (character select)" : mgr != mgr0 ? "the fight's scene is gone"
                    : now_ms() - t0 > MENU_MAX_MS ? "3 minutes on" : NULL;
    if (end) {
        InterlockedExchange(&net_results, 0); ended = 1; t_end = now_ms();
        fprintf(stderr, "[rb] results: %s: A and B no longer mirrored\n", end);
        tag_restore("the results are over");
        if (net_online) { online_forget("the results are over"); net_armed = 0; }   /* (the next Online match: other players, maybe) */
        return;
    }
    for (int p = 0; p < net_n; p++)
        if (p != net_local && net_menu_remote[p] && now_ms() - menu_heard_ms[p] > 500) InterlockedExchange(&net_menu_remote[p], 0);
    RbnMenu m = {net_local < 0 ? 0 : net_local, (int)net_menu_mine};
    u8 out[RBN_MENU_BYTES];
    size_t k = rbn_menu_build(out, sizeof out, &m);
    if (k && net_local >= 0) rbl_send(out, (int)k);
    static LONG mine_was;
    if (m.buttons & ~mine_was & 1) fprintf(stderr, "[rb] results: this game's player pressed A\n");
    mine_was = m.buttons;
}

static void net_hello_wait(void) {
    if (hs_agreed) return;
    fprintf(stderr, "[rb] handshake: the match is being set up but the seed is not agreed: waiting for the other games (up to 10 min)\n");
    double t0 = now_ms();
    while (!hs_agreed && now_ms() - t0 < 600000) {
        net_hello(1);
        net_wait_say(net_pending ? "Waiting for the room's players..." : net_local == 0 ? "Waiting for players to join..." : "Waiting for the host...");
        Sleep(16);
    }
    net_wait_say(NULL);
    if (hs_agreed) {
        fprintf(stderr, "[rb] handshake: agreed after %.1f s of waiting\n", (now_ms() - t0) / 1000);
        for (int k = 0; k < 5; k++) { net_hello_send(); Sleep(20); }   /* (player 2's answer, again: one lost would leave player 1 waiting) */
    } else fprintf(stderr, "[rb] handshake: no agreement after 10 min: going on (the start may differ)\n");
}

static void savestates(Cpu *c, u64 frame) {
    static int ready, prev_save, prev_load;
    if (!ready) {
        ready = 1;
        const char *e = getenv("SSBU_RB_AUTO");
        if (e && sscanf(e, "%lld,%lld,%lld", &auto_at, &auto_every, &auto_left) >= 2)
            fprintf(stderr, "[rb] auto test: save at fight frame %lld, then load every %lld frames, %lld times\n", auto_at, auto_every, auto_left);
        else auto_at = -1;
        e = getenv("SSBU_RB_VERIFY");
        if (e && sscanf(e, "%lld,%lld,%lld", &ver_at, &ver_k, &ver_rounds) >= 2)
            fprintf(stderr, "[rb] replay check: save at fight frame %lld, reference %lld frames later, %lld replays\n", ver_at, ver_k, ver_rounds);
        else ver_at = -1;
        e = getenv("SSBU_RB_BENCH");
        if (e && sscanf(e, "%lld,%lld", &bench_from, &bench_n) == 2)
            fprintf(stderr, "[rb] save benchmark: a save at every fight frame from %lld, %lld times\n", bench_from, bench_n);
        else bench_from = -1;
        e = getenv("SSBU_RB_RING");
        int size = 0;
        if (e && sscanf(e, "%lld,%lld,%lld,%lld,%d", &ring_from, &ring_every, &ring_depth, &ring_left, &size) >= 4 && ring_depth >= 1) {
            if (size <= ring_depth) size = (int)ring_depth + 2;
            rb_ring_size(size);
            rb_quiet(1);
            fprintf(stderr, "[rb] ring test: from fight frame %lld a save every frame (%d kept); every %lld frames back %lld, %lld times; "
                    "each frame's fight hash checked against its first pass\n", ring_from, size, ring_every, ring_depth, ring_left);
        } else ring_from = -1;
        e = getenv("SSBU_RB_NET");
        /* (started online, net_rollback_default: SSBU_RB_NET=online's mode unless another is set - the game's own matches,
         * Quickplay and Local Wireless rooms, with rollback when both games have it; SSBU_RB_NET=off for none) */
        {   const char *rb = getenv("SSBU_RB");
            if (!(e && *e) && !(rb && *rb == '0') && net_rollback_default()) {
                e = "online";
                fprintf(stderr, "[rb] net mode: started online: Quickplay and Local Wireless room matches with rollback (SSBU_RB_NET=online's)\n");
            }
        }
        /* (R7: relay[,<player>[,<delay>[,<frames>]]], the link through the netplay relay: hle_rb_relay.c) */
        int via_relay = e && !strncmp(e, "relay", 5) && (e[5] == 0 || e[5] == ',');
        net_online = e && !strncmp(e, "online", 6) && (e[6] == 0 || e[6] == ',');
        net_mode = e && !strncmp(e, "loop", 4) ? 1 : e && (!strncmp(e, "net,", 4) || via_relay || net_online) ? 2 : 0;
        if (net_mode == 2) {
            net_lag_hi = -2;
            /* (M7: SSBU_RB_NET_PLAYERS=<n>, the host's: how many play, 2-4; the others take it from the host's hello.
             * R7, relay,auto: every game's, the room's order waiting for that many) */
            const char *np = getenv("SSBU_RB_NET_PLAYERS");
            if (np && atoi(np) >= 2 && atoi(np) <= 4) net_n = atoi(np);
            int ok;
            if (net_online) {
                /* (the game's Online mode: online[,<delay>[,<frames>]]; each match's players come from its set-up, online_setup) */
                net_delay = -1;
                if (e[6] == ',') sscanf(e + 7, "%d,%d", &net_delay, &net_frames);
                online_delay_set = net_delay;
                net_pending = 1; net_local = -1; net_n = 2;
                ok = rbl_open_relay(RBL_ONLINE, net_n);
            } else
            if (via_relay) {
                /* <player>: auto (the default: by virtual address in the room, hle_rb_relay.c), 0-3, or -1 (the host gives one) */
                char who[16] = "auto", *end = NULL;
                if (e[5] == ',') sscanf(e + 6, "%15[^,],%d,%d", who, &net_delay, &net_frames);
                long v = strtol(who, &end, 10);
                net_pending = !strcmp(who, "auto");
                ok = net_pending || (end != who && !*end && v >= -1 && v <= 3);
                net_local = net_pending ? -1 : (int)v;
                ok = ok && rbl_open_relay(net_pending ? RBL_AUTO : net_local, net_n);
            } else
            /* (Host/Join: player -1, a joining game: its number comes from the host; it needs the host's address) */
            ok = sscanf(e + 4, "%d,%d,%d,%d,%d,%63s", &net_local, &net_port, &net_peer_port, &net_delay, &net_frames, net_host) >= 3 && net_local >= -1 &&
                 net_local <= 3 && !(net_local < 0 && !strcmp(net_host, "*")) && rbl_open(net_local, net_port, net_host, net_peer_port);
            if (!ok) {
                if (via_relay) fprintf(stderr, "[rb] net mode: SSBU_RB_NET=relay[,<player auto|0-3|-1>[,<delay>,<frames>]] not usable (%s): off\n", e);
                else fprintf(stderr, "[rb] net mode: SSBU_RB_NET=net,<player 0-3, -1 to join>,<my port>,<peer port>[,<delay>,<frames>,<host>] not usable (%s): off\n", e);
                net_mode = 0; net_pending = 0;
            } else {
                if (rerun_save < 0) { const char *r = getenv("SSBU_RB_RERUN_SAVE"); rerun_save = r && *r ? atoi(r) : 3; }
                rb_ring_size(RBN_MAX_BACK + 3);
                rb_quiet(1);
                if (net_frames < (1 << 19)) net_frames = 1 << 19;   /* (the whole fight: some 2.4 hours of frames) */
                /* (#145: players play with their own controllers; the bot only when a test asks for it, SSBU_RB_NET_LIVE=0
                 * or SSBU_RB_NET_BOT=<kind>. Before, the bot was the default and a real player's controls did nothing) */
                {   const char *l = getenv("SSBU_RB_NET_LIVE"), *b = getenv("SSBU_RB_NET_BOT");
                    net_live = l && *l ? *l != '0' : !(b && *b); }
                fprintf(stderr, "[rb] net mode: this game's player plays with %s\n", net_live ? "its controllers (live)" : "its bot");
                char dl[48];
                if (net_delay < 0) snprintf(dl, sizeof dl, "%s", net_local == 0 ? "from the round trip" : "the host's");
                else snprintf(dl, sizeof dl, "%d%s", net_delay, net_local != 0 ? " (the host's counts)" : "");
                if (net_local == 0) rbl_players(net_n);
                if (net_online) fprintf(stderr, "[rb] net mode: the game's Online mode: each match's players and numbers from its set-up; input delay %s\n", dl);
                else if (net_pending) fprintf(stderr, "[rb] net mode: this game's player number comes from the room (%d players); input delay %s\n", net_n, dl);
                else if (net_local < 0) fprintf(stderr, "[rb] net mode: this game joins: its player number comes from the host; input delay %s\n", dl);
                else fprintf(stderr, "[rb] net mode: this game plays player %d%s; input delay %s; fight hashes of the first %d frames compared\n",
                             net_local + 1, net_local == 0 ? " (the host)" : "", dl, net_frames);
                if (net_local == 0) fprintf(stderr, "[rb] net mode: %d players (SSBU_RB_NET_PLAYERS)\n", net_n);
            }
        }
        if (net_mode == 1) {
            if (e[4] == ',') sscanf(e + 5, "%d,%d,%d,%d,%d", &net_delay, &net_lag_lo, &net_lag_hi, &net_loss, &net_frames);
            if (rerun_save < 0) { const char *r = getenv("SSBU_RB_RERUN_SAVE"); rerun_save = r && *r ? atoi(r) : 3; }
            rb_ring_size(RBN_MAX_BACK + 3);
            rb_quiet(1);
            fprintf(stderr, "[rb] loop mode (M5): player 2's pads through rb_net, delay %d, lag %d..%d frames, %d%% lost; fight hashes of the first %d frames\n",
                    net_delay, net_lag_lo, net_lag_hi, net_loss, net_frames);
        }
    }
    if (net_mode && S.in_fight && (net_mode != 2 || net_armed)) { net_step(c, frame); return; }
    /* (net mode: a fight that ended before <frames> still gets its report, and the whole fight is checked:
     * the launcher's 7200 is a floor, not a window - twopc8's report never came, its fight ending sooner) */
    if (net_mode == 2 && !S.in_fight && net_got && !net_done) {
        net_done = 1; net_report(); ch_have_mine = 0; memset(ch_have_peer, 0, sizeof ch_have_peer);
        rules_have = 0; rules_id = 0; rules_chunks = 0;   /* (the next set-up's rules again: the host's may change) */
        /* (an Online match: its players are forgotten once its results are over, net_results_frame: until then the other
         * player's controller slot stays one here, or the game would stop at its "connect a controller" screen) */
    }
    /* M5 step 3b: the same start on both games. The match set-up (13.0.4 0x71014d8620) stores the next
     * value of the game's global xorshift128 (4 words, 13.0.5 0x71052391c0: 13.0.4's 0x71052381c0 one page
     * on, as all of main's data) in the match block, and the fight's random set-up, the starting sides
     * among it, comes from that; the menus advance the generator, so two games had different starts
     * about half the time (ssbu-recomp PR #116's handshake found the store: debug_mode.c). Outside fights,
     * with SSBU_RB_SEED, it is set from the seed at every safe point, so the set-up draws the same in both
     * games. (Until a handshake agrees on the seed between two PCs: the same SSBU_RB_SEED on both.) */
    if (net_mode == 2 && !S.in_fight) net_hello(0);
    if (net_mode == 2 && !S.in_fight) net_seed_global("menus");
    /* (net mode, from a match set-up until the fight: each frame waits for the game's other threads to be at a
     * wait - loading done - up to SSBU_RB_NET_SETTLE_MS (default 500). The fighters' entrances begin once their
     * files are loaded, checked each frame, so the frame they began on followed each PC's disk: the slower PC
     * had Steve's entrance 90 frames later and Kazuya's a frame apart, the same in three runs (twopc2-4).) */
    static double settle_limit = -1;
    if (settle_limit < 0) { const char *e = getenv("SSBU_RB_NET_SETTLE_MS"); settle_limit = e && *e ? atof(e) : 0; }   /* (off by default from twopc13: it made the countdown stutter and didn't help) */
    if (net_mode == 2 && !S.in_fight && net_countdown && settle_limit > 0) {
        double limit = settle_limit;
        static long long settled, timed_out;
        static double settle_ms;
        u32 alive, waiting, sleeping, nbusy; u64 busy[16];
        double t0 = now_ms();
        for (;;) {
            kernel_rb_census(c->thread, &alive, &waiting, &sleeping, busy, 16, &nbusy);
            if (!nbusy || now_ms() - t0 >= limit) break;
            Sleep(0);
        }
        settle_ms += now_ms() - t0;
        if (nbusy) timed_out++; else settled++;
        if ((settled + timed_out) % 60 == 1 || nbusy)
            if (timed_out <= 20)
                fprintf(stderr, "[rb] net mode, before the fight: frame %lld settled, %lld timed out (%.0f ms waited in all)%s\n",
                        settled, timed_out, settle_ms, nbusy ? ": a thread stayed busy" : "");
    }
    if (net_mode == 2 && !S.in_fight && net_countdown) net_pre_fight(c, frame);   /* (the GO! freeze) */
    {   int ra_pre_point(Cpu *c, u64 frame);
        if (!S.in_fight && ra_pre_point(c, frame)) return; }
    if (net_mode == 2 && net_armed && !S.in_fight && !net_countdown && net_got) net_results_frame(c);   /* (the results screen; not after the controls screen's test fight: no match was set up) */
    {   int runahead_point(Cpu *c, u64 frame);
        if (S.in_fight && runahead_point(c, frame)) return; }
    if (ring_from >= 0 && S.in_fight && S.frames >= ring_from) { ring_test(c, frame); return; }
    /* SSBU_RB_BENCH=<frame>,<count> (M3): a save at every fight frame from <frame>, <count> of them,
     * as rollback's per-frame capture would do; before a replay check's save, that save is a
     * page-by-page one too, which the check then tests. */
    if (bench_from >= 0 && S.in_fight && S.frames >= bench_from && S.frames < bench_from + bench_n) {
        rb_save(c, frame);
        return;
    }
    if (ver_at >= 0 && S.in_fight && ver_phase < 3) {
        if (ver_phase) ver_since++;
        /* A game counter, 0x71052c5184, that moves once per logic frame: which safe points ran more
         * than one (the game catching up), as a list of "frame:+n" after each save or load. */
        static u32 prev_count; static char steps[512]; static size_t ns;
        u32 count = ld32(0x71052c5184ull);
        if (ver_phase && ver_since > 0 && count - prev_count != 1 && ns < sizeof steps - 16)
            ns += snprintf(steps + ns, sizeof steps - ns, " %lld:+%d", ver_since, (int)(count - prev_count));
        prev_count = count;
        if (ver_phase && ver_since == ver_k) {
            fprintf(stderr, "[rb] verify phase %d: counter %u after %lld frames; safe points that were not one logic frame:%s\n",
                    ver_phase, count, ver_since, ns ? steps : " none");
            ns = 0; steps[0] = 0;
        }
        fight_hash(c, ver_phase ? ver_since : 0);
        if (ver_phase == 2 && ver_since == ver_k) {
            if (!hash_first_bad) fprintf(stderr, "[rb] fight hash: replay %lld matches the reference on all %lld frames\n", ver_round + 1, ver_k);
            fprintf(stderr, "[rb] input tape: %lld pad reads recorded, %lld played back so far\n", tape_recorded, tape_replayed);
            hash_first_bad = 0;
        }
        if (ver_phase == 0 && S.frames == ver_at) { ver_phase = 1; ver_since = 0; rb_save(c, frame); return; }
        if (ver_phase == 1 && ver_since == ver_k) { rb_ref_capture(c); ver_phase = 2; ver_since = 0; rb_load(c, frame); return; }
        if (ver_phase == 2 && ver_since == ver_k) {
            rb_ref_compare(c, ++ver_round);
            if (ver_round >= ver_rounds) { ver_phase = 3; return; }
            ver_since = 0;
            rb_load(c, frame);
            return;
        }
        return;   /* (no hotkeys while it runs) */
    }
    if (S.in_fight) fight_hash(c, 0);
    if (auto_at >= 0 && S.in_fight) {
        if (S.frames == auto_at) { auto_next = S.frames + auto_every; rb_save(c, frame); return; }
        if (S.frames > auto_at && S.frames >= auto_next && auto_left > 0) { auto_left--; auto_next = S.frames + auto_every; rb_load(c, frame); return; }
    }
    int focused = gfx_window_focused();
    int save = focused && (GetAsyncKeyState(VK_F7) & 0x8000), load = focused && (GetAsyncKeyState(VK_F8) & 0x8000);
    int new_save = save && !prev_save, new_load = load && !prev_load;
    prev_save = save; prev_load = load;
    if (new_save) rb_save(c, frame);
    else if (new_load) rb_load(c, frame);
}

/* FUN 0x71038701e0: the graphics module's frame end (13.0.4 0x710386fc30, as ssbu-recomp's
 * fighter_host.c fh_frame_end, SSBU_DETERMINISTIC), through its frame hook (FRAME_HOOK_END). It compares the refresh count of the game's
 * vsync count thread with the count at the last frame and reports the difference less one,
 * "dropped" refreshes, through its third argument (result code 0xF0044007 / 0xF0044005): the game
 * then runs that many EXTRA logic frames. That count follows wall time, so a replay ran 0-3 more
 * logic frames than the run it repeats. In a fight the report is cleared: one logic frame per
 * main loop iteration, always. SSBU_RB_DROPS=1 leaves it (the game's own catching up). */
static int keep_drops = -1;
static long long drops_cleared, drop_reports;
static void frame_end_before(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)args; (void)ctx;
    seg_fe_in = now_ms();
}
static void frame_end_after(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)ctx;
    u64 p1 = args[0], p3 = args[2];   /* (as the game called it) */
    seg_fe_out = now_ms();
    /* (net mode: outside fights too: the entrance and the countdown before "GO" run on logic frames as well,
     * and a slower PC caught up there with extra ones: two PCs began the fight a frame apart, animations and
     * an idle variant's random draw out of step (twopc3: Kazuya's wait motion at frame 63 on one PC, 64 on the
     * other, Steve on another motion and one draw more). In the menus a lagging PC then runs slower instead.) */
    /* (v0.9.4b: and from the match set-up to GO!, always. v0.9.4's waits there for the others (net_pre_step) left
     * refreshes undone, which the game then ran as extra logic frames: one more just before GO! on the game that had
     * waited, so its fight began at logic frame 2 and the other's at 1, and they went apart 4.5 s in, a match over the internet, 11-01) */
    if (!rb_on || !p3 || !(S.in_fight || (net_mode == 2 && (net_early_on() || net_countdown)))) return;
    if (keep_drops < 0) { const char *e = getenv("SSBU_RB_DROPS"); keep_drops = e && *e == '1'; }
    u64 n = ld64(p3);
    if (!n || keep_drops) return;
    drop_reports++; drops_cleared += (long long)n;
    if (rb_log || drop_reports <= 5) fprintf(stderr, "[rb] frame end reported %llu dropped refreshes (fight frame %lld): cleared (%lld reports, %lld frames so far)\n",
                                             (unsigned long long)n, S.frames, drop_reports, drops_cleared);
    st64(p3, 0);
    if (p1 && (ld32(p1) == 0xF0044007u || ld32(p1) == 0xF0044005u)) st32(p1, 0);
}

/* ---- M4: re-run frames without their drawing --------------------------------------------
 * While rb_fast > 1 (the frames a load runs again, all but the last, whose picture is shown next),
 * the main loop's presentation work is left out, as ssbu-recomp's rollback re-run passes do
 * (runahead.c ra_kick2 and ring_simulation.c's "lite" list, 13.0.4 addresses mapped by that
 * project's verified table, hostrt.c g_code_13_0_5):
 *  - SSBU_RB_SKIP=<mask> (default 0; was 49, the PR's, which crashed long fights): the job-group kicks (FUN 0x7103549720) called
 *    from: 1 the render preparation (0x710374a0d4 - the safe point's own call - 0x710374baa8,
 *    0x710374bad8, 0x710374bb08); 16 preparation list A (0x7103749aac); 32 list B (0x7103749b48);
 *  - SSBU_RB_LITE=<mask> (default 57: all but 2, 4 and 64, see skip_flags): the presentation callees of the main loop (FUN
 *    0x7103747820, 0x6000 bytes), by group: 1 camera, 2 UI, 4 lighting, 8 render layers,
 *    16 recorder, 32 dynamic resolution, 64 effect emitter tick; each returns a fixed value.
 * The fight hash then says whether the frames after are still the same. */
#define MAIN_LOOP_LO 0x7103747820ull
#define MAIN_LOOP_HI 0x710374d820ull
static void skip_flags(void) {
    const char *e = getenv("SSBU_RB_SKIP"), *l = getenv("SSBU_RB_LITE");
    skip_mask = e && *e ? atoi(e) : 0;   /* (was 49, the PR's: leaving the job-group kicks out of re-run frames crashed long
                                          * fights - a null read in the job system or the game after some 4 000 - 14 000 logic
                                          * frames of rollbacks, often as the fight ended; the very same fight (stress3 / stress4,
                                          * bots, 6359 frames) crashed at the winner screen with 49 and finished with 0) */
    lite_mask = l && *l ? atoi(l) : 57;    /* (not 64 either, the effect emitter tick: effects made in re-run frames and never ticked crashed the
                                            * fight's end after many rollbacks - main+0x35c4580 - the same fight crashing with it skipped (stress5)
                                            * and finishing twice without (stress7) and with no lite skips at all (stress6)) */   /* (not 4, lighting: fighters and items drew flat green from then on, and it saved nothing measurable;
                                            * not 2, UI: the HUD's animations ("GO!", the stock counter after a KO) jittered and stuck in the game
                                            * that rolled back most, every load putting them back and the re-run frames not moving them on: ui1) */
}
static int kick_skipped(u64 lr) {
    if (rb_fast <= 1) return 0;
    if (skip_mask < 0) skip_flags();
    int prep = lr == SAFE_POINT || lr == 0x710374baa8ull || lr == 0x710374bad8ull || lr == 0x710374bb08ull;
    int la = lr == 0x7103749aacull, lb = lr == 0x7103749b48ull;
    if (((skip_mask & 1) && prep) || ((skip_mask & 16) && la) || ((skip_mask & 32) && lb)) { kicks_skipped++; return 1; }
    /* 2: the render list's kick (0x710374c688). Its culling job sets a byte the main loop spins on
     * (13.0.4 0x7103749f48), [0x71053375a8] + 0x140 (13.0.4 0x71053365a8, data one page on): set
     * here instead, as the PR's runahead.c ra_kick2 does. The PR measured 0.3% of the picture changed.
     * HERE IT CRASHES (rend_crash: a task worker calls a stale pointer from the job runner in the
     * first rollbacks): not in the default mask. */
    if ((skip_mask & 2) && lr == 0x710374c688ull) {
        u64 p = ld64(0x71053375a8ull);
        if (p) st8(p + 0x140, 1);
        kicks_skipped++;
        return 1;
    }
    return 0;
}
static int lite_skipped_now(Cpu *c, int grp) {
    if (rb_fast <= 1) return 0;
    if (lite_mask < 0) skip_flags();
    u64 lr = c->x[30];
    if (!(lite_mask & grp) || lr < MAIN_LOOP_LO || lr >= MAIN_LOOP_HI) return 0;
    lite_skipped++;
    return 1;
}
/* (hle_rb_wraps.c: the presentation callees' wraps ask this) */
int rb_lite_skip(Cpu *c, int group) { return lite_skipped_now(c, group); }

/* ---- a catch-up's frames get the buffers their first pass got ----------------------------------
 * The game keeps data per display buffer (the camera's among it) and picks it by the slot DequeueBuffer
 * gives. The buffer queue is the host's (ipc_vi.c): a load puts its slot states back (vi_rb_load), but a
 * catch-up's frames are freed at once and not paced, so they were given the slots in another order than
 * their first pass. The per-buffer data was then a buffer out of step, and every rollback moved the
 * camera back and forth (the "earthquake screen"; measured with ring tests 300,10,4 and 300,8,2: the
 * camera's block differed from the first pass on 20-70% of the frames shown, none with this). ipc_vi.c
 * asks these. SSBU_RB_SLOT_ORDER=0: as before. */
static int slot_order_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_RB_SLOT_ORDER"); on = !(e && *e == '0'); }
    return on;
}
/* (a rollback's load or catch-up is under way, and the slot order is kept) */
int rb_vi_catchup(void) { return rb_catchup && slot_order_on(); }
/* DequeueBuffer, under the queue's lock: `dflt` the slot it would give, `freemask` the free ones. A
 * normal fight frame's slot is kept by logic frame; a catch-up's frame is given that slot again when free. */
int rb_dequeue_pick(u64 freemask, int dflt) {
    static u32 key_at[4096]; static u8 slot_at[4096], kept[4096];
    if (!S.in_fight || !slot_order_on()) return dflt;
    u32 key = ld32(LOGIC_FRAME), i = key % 4096;
    if (rb_fast <= 0) { key_at[i] = key; slot_at[i] = (u8)dflt; kept[i] = 1; return dflt; }
    if (rb_catchup && kept[i] && key_at[i] == key && slot_at[i] < 64 && (freemask >> slot_at[i] & 1)) return slot_at[i];
    return dflt;
}

/* ---- M4 profile: the game's task tree (SSBU_RB_TREE=1) --------------------------------------
 * FUN 0x7103725030 (13.0.4 0x7103724a80, ssbu-recomp's ring_simulation.c profile 2) walks the
 * game's task tree once per logic step: node->vt[0x28], its children (re-entering here), node->vt[0x30].
 * Timed per node class (its vtable), exclusive of its children, on the guest main thread, apart for
 * normal frames and re-run ones; the busiest classes are listed at the end of the ring test. */
typedef struct TreeE { u64 vt; double incl[2], excl[2]; long long n[2]; int dmin; } TreeE;
#define TREE_N 2048
static TreeE tree_e[TREE_N];
static int tree_on = -1, tree_depth;
static double tree_child[256];
static TreeE *tree_slot(u64 vt) {
    u32 h = (u32)((vt * 0x9e3779b97f4a7c15ull) >> 52) % TREE_N;
    for (int i = 0; i < TREE_N; i++) { TreeE *e = &tree_e[(h + (u32)i) % TREE_N]; if (e->vt == vt) return e; if (!e->vt) { e->vt = vt; e->dmin = 999; return e; } }
    return NULL;
}
static int tree_cmp(const void *a, const void *b) { double x = ((const TreeE *)a)->excl[1], y = ((const TreeE *)b)->excl[1]; return x < y ? 1 : x > y ? -1 : 0; }
static void tree_report(void) {
    if (tree_on != 1) return;
    TreeE *v = malloc(sizeof tree_e);
    int n = 0;
    for (int i = 0; i < TREE_N; i++) if (tree_e[i].vt) v[n++] = tree_e[i];
    qsort(v, n, sizeof *v, tree_cmp);
    double tot[2] = {0, 0};
    for (int i = 0; i < n; i++) { tot[0] += v[i].excl[0]; tot[1] += v[i].excl[1]; }
    fprintf(stderr, "[rb] task tree: %d node classes; exclusive time in all: normal frames %.1f ms, re-run frames %.1f ms (sums)\n", n, tot[0], tot[1]);
    for (int i = 0; i < n && i < 30; i++)
        fprintf(stderr, "[rb]   vt %#llx depth %d: re-run %6.1f ms excl (%5.1f%%) %6.1f incl, %lld calls | normal %6.1f ms excl, %lld calls\n",
                (unsigned long long)v[i].vt, v[i].dmin, v[i].excl[1], tot[1] ? 100 * v[i].excl[1] / tot[1] : 0, v[i].incl[1], v[i].n[1], v[i].excl[0], v[i].n[0]);
    free(v);
}
void rb_tree_node(Cpu *c, GuestFn *orig) {   /* (hle_rb_wraps.c, the wrap of FUN 0x7103725030) */
    if (!rb_on) { orig(c); return; }
    if (tree_on < 0) { const char *e = getenv("SSBU_RB_TREE"); tree_on = e && *e == '1'; }
    if (tree_on != 1 || !S.in_fight || c->thread != rb_main_kthread || tree_depth >= 255) { orig(c); return; }
    u64 node = c->x[0], vt = node ? ld64(node) : 0;
    int kd = rb_fast > 0, d = tree_depth;
    tree_child[d + 1] = 0; tree_depth++;
    double t0 = now_ms();
    orig(c);
    double dt = now_ms() - t0;
    tree_depth--;
    TreeE *e = tree_slot(vt ? vt : 1);
    if (e) { e->n[kd]++; e->incl[kd] += dt; e->excl[kd] += dt - tree_child[d + 1]; if (d < e->dmin) e->dmin = d; }
    tree_child[d] += dt;
}

/* ---- M5: who draws from the script random generators at a fight's start ----------------------
 * app::sv_math::rand(Hash40, int) and randf(Hash40, float): the Hash40 picks one of the 9 generators.
 * Two runs differed in when generator 1 was drawn (every other frame, on even frames in one run and
 * odd in another: dbg1). In loop mode, each draw of the first 12 fight frames is logged with its
 * caller, its Hash40 and which generator moved. */
static void rand_log(Cpu *c, const char *fn, u64 hash, u64 lr, const u32 *before) {
    u64 rb = rng_base();
    int moved = -1;
    for (int q = 0; q < 9 && rb; q++) if (ld32(rb + 20ull * q + 16) != before[q]) moved = q;
    fprintf(stderr, "[rb] draw: %s(%#llx) from %#llx, generator %d, logic frame %u, fight frame %lld%s\n", fn, (unsigned long long)hash,
            (unsigned long long)lr, moved, ld32(LOGIC_FRAME), S.frames, rb_fast > 0 ? " (re-run)" : "");
    (void)c;
}
void rb_rand_draw(Cpu *c, GuestFn *orig, int which) {   /* (hle_rb_wraps.c: 0 rand, 1 randf) */
    static int logged[2];
    if (!rb_on) { orig(c); return; }
    int log = net_mode >= 1 && S.in_fight && S.frames <= 300 && logged[which & 1] < 60;
    if (log) logged[which & 1]++;
    u64 hash = c->x[0] & 0xFFFFFFFFFFull, lr = c->x[30];
    u32 before[9] = {0};
    u64 rb = log ? rng_base() : 0;
    for (int q = 0; q < 9 && rb; q++) before[q] = ld32(rb + 20ull * q + 16);
    orig(c);
    if (log) rand_log(c, which & 1 ? "randf" : "rand", hash, lr, before);
}

/* M5: the player record builder (13.0.5 0x71017880c0, 13.0.4 0x7101788260: ssbu-recomp PR #116's
 * FN_PLAYER_RECORD; x0 the player's panel state, x1 its index). The match set-up draws from the global
 * generator right after the records are built; setting it only at the menus' safe points left a gap: two
 * PCs agreed on the seed and still started on different sides (twopc2, desync at the first check) - a
 * slower PC's menus run extra logic frames between safe points (dropped refreshes), and those can draw.
 * So in net mode it is set here too, as player 1's record is built, as the PR's handshake does.
 * Through its frame hook (FRAME_HOOK_RECORD: the fight test's debug match uses the function too). */
/* The stage choice (13.0.4 0x71053097c0: three entries of 0x20 bytes, the current one's index at
 * 0x7105309968; the hazards byte 0x7105309985: the PR's debug_mode.c), one page on in 13.0.5. */
#define SEL_BASE 0x710530a7c0ull
#define SEL_CUR  0x710530a968ull
#define SEL_HAZ  0x710530a985ull
/* (v0.9.7 research: desyncs where the same C-stick push made a different attack for one player on the two PCs, every
 * pad and the frame before alike. On its own PC a player's panel is a human's, set up as its controller joined; on the
 * others it is a CPU's panel turned human here. The whole panel state, as the select screen left it and as it goes to
 * the record builder, to set the two PCs' panels for one player side by side.) */
static void panel_dump(const char *when, int idx, u64 st) {
    for (u32 o = 0; o < 0x460; o += 0x40) {
        char t[200]; size_t n = 0;
        for (u32 k = 0; k < 0x40; k += 4) n += snprintf(t + n, sizeof t - n, " %08x", ld32(st + o + k));
        fprintf(stderr, "[rb] panel %d %s +%03x:%s\n", idx + 1, when, o, t);
    }
}
/* ---- the game's Online mode (SSBU_RB_NET=online) ---------------------------------------------------------------
 * A Quickplay match is set up by both games at once from the match the game server made (tools/nex): the same players
 * in the same order on both (player 1 the gathering's owner), the same rules, stage and seed, melee mode 14 in the match
 * block, and the other game's player a network player (its panel's + 0x451 = 1, controller slot -1), whose pads the
 * game's own online netcode (Pia, delay-based) would bring. Here the match is played as rollback's: every player a
 * local one on both games (the controller slot of its number; the melee mode stays 14, so the scene stays the Online
 * one, with its results, GSP and next search), the other's pads through rollback's link to the address the game's
 * Pia already talks to (net_recent_peers). The game's own records come back as the fight ends (online_restore). The
 * Pia session and the NEX connection live through the rollbacks: a load gives the game's sockets again what they
 * took since its save (hle_net.c net_rb_load, hle_online.c online_rb_load). Both games must agree within ONLINE_WAIT_MS
 * (the handshake: the seed and the delay); a game whose opponent has rollback off (or an older build) never hears it,
 * and the match is left to the game's own netcode on both. Online matches of more than 2 players are left to it too. */
int net_recent_peers(u32 *out, int max, u32 within_ms);   /* (net.h) */
#define ONLINE_MELEE 14
#define ONLINE_WAIT_MS 15000.0
static int online_match;   /* (the match set up now is an Online one played with rollback) */
/* (what the game set up, put back as the fight ends: its own results and what follows them - the GSP, the next
 * search - run on the Online match, melee mode 14 and the other player a network player, as without rollback) */
#define ONLINE_HDR 0x580   /* (the match block's rule header: the records follow, 0x260 bytes each) */
#define ONLINE_REC 0x260
static u8 online_hdr[ONLINE_HDR];
static u8 online_panel[RBN_PLAYERS][0x460];
static u8 online_rec[RBN_PLAYERS][ONLINE_REC];
static int online_rec_have[RBN_PLAYERS];
static u64 fm_impl(void) {
    static u64 inst = 1;
    if (inst == 1) inst = rt_symbol("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
    u64 mgr = inst ? ld64(inst) : 0;
    return mgr ? ld64(mgr) : 0;
}
static void online_fight_start(void) {
    if (!online_match) return;
    u64 impl = fm_impl();
    fprintf(stderr, "[rb] online: GO!: melee mode %u in the match block, the fighter manager's +0xc0 %08x\n", ld8(MELEE_INFO), impl ? ld32(impl + 0xc0) : 0);
}
static void online_restore(void) {
    if (!online_match) return;
    memcpy(P(u8, MELEE_INFO), online_hdr, ONLINE_HDR);
    {   /* (and the fighter manager's copy of the melee mode, which it took at the match's start: + 0xc0; only
         * another with SSBU_RB_ONLINE_MODE) */
        u64 impl = fm_impl();
        if (impl && readable(impl + 0xc0, 4) && ld8(impl + 0xc0) != online_hdr[0]) {
            fprintf(stderr, "[rb] online: the fighter manager's melee mode %u -> %u\n", ld32(impl + 0xc0), online_hdr[0]);
            st8(impl + 0xc0, online_hdr[0]);
        }
    }
    int n = 0;
    for (int p = 0; p < RBN_PLAYERS; p++)
        if (online_rec_have[p]) { memcpy(P(u8, MELEE_INFO + ONLINE_HDR + (u64)p * ONLINE_REC), online_rec[p], ONLINE_REC); n++; }
    fprintf(stderr, "[rb] online: the fight is over: the game's own match set-up back (melee mode %u, %d players' records) for its results\n",
            ld8(MELEE_INFO), n);
    { void net_rb_counts_report(void); net_rb_counts_report(); }
    online_match = 0;
}
static void online_forget(const char *why) {
    if (!net_online) return;
    if (net_local >= 0 || online_match) fprintf(stderr, "[rb] online: %s: this game's number and players forgotten\n", why);
    rbl_relay_online_end();
    online_match = 0;
    net_local = -1; net_pending = 1;
    hs_agreed = 0; hs_seed = 0; hs_delay_ready = 0; net_delay = online_delay_set;
    memset(hs_agreed_by, 0, sizeof hs_agreed_by); memset(hs_peer_t, 0, sizeof hs_peer_t); memset(hs_peer_t_at, 0, sizeof hs_peer_t_at);
    memset(hs_rtt_min, 0, sizeof hs_rtt_min); memset(hs_rtt_t0, 0, sizeof hs_rtt_t0); memset(hs_rtt_n, 0, sizeof hs_rtt_n);
    ch_have_mine = 0; memset(ch_have_peer, 0, sizeof ch_have_peer);
    rules_have = 0; rules_id = 0; rules_chunks = 0;
}
/* (player 1's record about to be built) 1: this set-up is an Online match, now this game's rollback match */
int ldn_in_network(u32 ip);   /* (net.h) */
u32 net_local_ip(void);
int net_room_members(u32 *out, int max);
/* (a match of another melee mode) the other players of this game's Local Wireless room: the room's members (the lobby's
 * room, or a private room's) that are nodes of the game's local wireless network now; 0 outside a room */
static int lw_room_others(u32 *out, int max) {
    u32 m[16], me = net_local_ip();
    int n = net_room_members(m, 16), k = 0;
    if (!me) return 0;
    for (int i = 0; i < n && k < max; i++)
        if (m[i] != me && ldn_in_network(m[i])) out[k++] = m[i];
    return k;
}
static int online_setup(u64 st) {
    online_forget("a new match set-up");
    u8 mode = ld8(MELEE_INFO);
    u32 ips[RBN_PLAYERS] = {0}, seen[8];
    int peers;
    const char *what = "an Online match";
    if (mode != ONLINE_MELEE) {
        /* (a Local Wireless room's match: set up by both games from the room's own, as an Online one is; the same
         * rollback, the room's other game the peer) */
        peers = lw_room_others(seen, 8);
        if (!peers) { fprintf(stderr, "[rb] online: a match of melee mode %u (not an Online one, no Local Wireless room): played here alone\n", mode); return 0; }
        what = "a Local Wireless room's match";
    } else peers = net_recent_peers(seen, 8, 30000);
    if (!readable(st, 0x460)) return 0;
    if (peers != 1) {
        fprintf(stderr, "[rb] online: %s with %d other players' games: left to the game's own netcode (rollback plays 2)\n", what, peers);
        return 0;
    }
    int own = ld32(st + 0x78) == 0 && !ld8(st + 0x451) ? 0 : 1;   /* (player 1's panel is this game's own, or the network player's) */
    ips[1 - own] = seen[0];
    net_n = 2;
    if (!rbl_relay_online(own, net_n, ips)) return 0;
    net_local = own; net_pending = 0;
    if (own == 0) rbl_players(net_n);
    fprintf(stderr, "[rb] online: %s (melee mode %u): this game is player %d; waiting for the other game's rollback (up to %.0f s)\n",
            what, mode, own + 1, ONLINE_WAIT_MS / 1000);
    double t0 = now_ms();
    while (!hs_agreed && now_ms() - t0 < ONLINE_WAIT_MS) {
        net_hello(1);
        net_wait_say("Connecting rollback...");
        Sleep(16);
    }
    net_wait_say(NULL);
    if (!hs_agreed) {
        fprintf(stderr, "[rb] online: no answer from the other game's rollback in %.0f s (rollback off there, or another build): the game's own netcode plays this match\n",
                ONLINE_WAIT_MS / 1000);
        online_forget("no rollback on the other side");
        return 0;
    }
    for (int k = 0; k < 5; k++) { net_hello_send(); Sleep(20); }   /* (the other's answer, again) */
    fprintf(stderr, "[rb] online: agreed after %.1f s (input delay %d): %s is played with rollback, both players local on both games (melee mode %u)\n",
            (now_ms() - t0) / 1000, net_delay, what, mode);
    memcpy(online_hdr, P(u8, MELEE_INFO), ONLINE_HDR);
    memset(online_rec_have, 0, sizeof online_rec_have);
    /* (the melee mode stays the game's, 14: the scene loads as an Online match - its results, the GSP and the next search
     * run as without rollback, and so do its rules for the players, as the kick for too many self-destructs. Only the
     * players are local ones here. SSBU_RB_ONLINE_MODE=<n>, research: the fight set up with melee mode n instead; 0, a
     * local melee, plays alike but ends on a local results screen the Online scene never leaves) */
    {   const char *e = getenv("SSBU_RB_ONLINE_MODE"); if (e && *e) st8(MELEE_INFO, (u8)atoi(e)); }
    online_match = 1;
    return 1;
}
static u64 record_caller;
/* (FRAME_HOOK_RECORD's after, SSBU_RB_LOG: the record as built and the match block's melee mode) */
void f_71017880c0_orig(Cpu *c);   /* (the builder itself: hle_each_frame.c's wrap) */
static void record_after(Cpu *c, const u64 args[8], void *ctx) {
    (void)c; (void)ctx;
    u64 rec = args[3];
    int idx = (int)args[1];
    if (net_online && online_match && net_armed && idx >= 0 && idx < net_n && rec && readable(rec, ONLINE_REC) && readable(args[0], 0x460)) {
        /* (the game's own record of this player, for after the fight: built again from the panel as the game left it) */
        static u8 conv_rec[ONLINE_REC], conv_panel[0x460];
        static Cpu keep;
        memcpy(conv_rec, P(u8, rec), ONLINE_REC);
        memcpy(conv_panel, P(u8, args[0]), 0x460);
        memcpy(P(u8, args[0]), online_panel[idx], 0x460);
        keep = *c;
        for (int k = 0; k < 8; k++) c->x[k] = args[k];
        f_71017880c0_orig(c);
        *c = keep;
        memcpy(online_rec[idx], P(u8, rec), ONLINE_REC); online_rec_have[idx] = 1;
        memcpy(P(u8, rec), conv_rec, ONLINE_REC);
        memcpy(P(u8, args[0]), conv_panel, 0x460);
    }
    if (!rb_log) return;
    fprintf(stderr, "[rb] record %d built (caller %#llx): melee mode %u, record at %#llx\n", (int)args[1],
            (unsigned long long)record_caller, ld8(MELEE_INFO), (unsigned long long)rec);
    if (rec && readable(rec, 0x100))
        for (u32 o = 0; o < 0x100; o += 0x40) {
            char t[200]; size_t n = 0;
            for (u32 k = 0; k < 0x40; k += 4) n += snprintf(t + n, sizeof t - n, " %08x", ld32(rec + o + k));
            fprintf(stderr, "[rb] record %d +%03x:%s\n", (int)args[1], o, t);
        }
}
/* (FRAME_HOOK_RECORD's before: the game's builder runs next, on the panel as left here) */
static void record_before(Cpu *c, const u64 args[8], void *ctx) {
    (void)args; (void)ctx;
    int idx = (int)c->x[1];
    u64 st = c->x[0];
    record_caller = c->x[30];
    if (rb_log && readable(st, 0x460))
        fprintf(stderr, "[rb] record %d: panel type %u, slot %d, ui_chara %016llx, colour %u, +0x450 %02x %02x\n", idx, ld32(st + 0x78),
                (int)ld32(st + 0x210), (unsigned long long)ld64(st + 0x88), ld8(st + 0x90), ld8(st + 0x450), ld8(st + 0x451));
    if (idx == 0) {   /* (research, 10-31: the controls screen builds records too; who calls, to tell a match's set-up apart) */
        char chain[160]; int k = snprintf(chain, sizeof chain, "%#llx", (unsigned long long)c->x[30]);
        u64 fp = c->x[29];
        for (int d = 0; d < 5 && fp && readable(fp, 16) && k < (int)sizeof chain - 20; d++) {
            k += snprintf(chain + k, sizeof chain - k, " %#llx", (unsigned long long)ld64(fp + 8));
            fp = ld64(fp);
        }
        fprintf(stderr, "[rb] record build: player 1's, called from %s\n", chain);
        if (c->x[30] == MATCH_SETUP_CALLER) { void ra_set_up(void); ra_set_up(); }   /* (run-ahead: its first save's copy in the loading) */
        if (net_mode == 2) {
            net_armed = c->x[30] == MATCH_SETUP_CALLER;
            if (net_online) net_armed = net_armed && online_setup(st);   /* (Online mode: only its own matches, agreed) */
            if (net_armed && fight_flag()) { net_leftover = 1; fprintf(stderr, "[rb] net mode: the controls screen's fight flag is still set: the fight starts once the game sets it anew\n"); }
            if (!net_armed) fprintf(stderr, "[rb] net mode: not a match's set-up (another screen builds records: controls?): no wait, played here alone\n");
        }
    }
    if (net_mode == 2 && idx == 0 && net_armed) {
        fprintf(stderr, "[rb] net mode: player 1's record is built (a match set-up): the %d players hold nothing until the fight\n", net_n);
        tag_restore("a new set-up");
        memset(tag_given, 0, sizeof tag_given);
        net_hello_wait();
        net_seed_global("match set-up");
        net_rules_setup(st);   /* (streamlining 2: the host's rules on every game) */
        net_countdown = 1;
        net_pre_reset();       /* (the GO! freeze: a new set-up's loading watched again) */
        if (net_results) { InterlockedExchange(&net_results, 0); fprintf(stderr, "[rb] results: a new match set up: A and B no longer passed\n"); }
    }
    if (net_mode == 2 && idx == 0) net_panel0 = st;   /* (the results' research: character select's panel, watched after the fight) */
    if (net_mode == 2 && net_armed && idx >= 0 && idx < net_n && readable(st, 0x460)) {
        /* (the panel state, read off the PR's runs: + 0x78 type (0 human), + 0x88 ui_chara, + 0x90 colour,
         * + 0x210 the controller slot, + 0x450 "human with a name tag") */
        u32 cur = ld32(SEL_CUR);
        u64 sel = SEL_BASE + 0x20ull * (cur < 3 ? cur : 0);
        u32 type = ld32(st + 0x78), slot = ld32(st + 0x210);
        fprintf(stderr, "[rb] handshake: panel %d as the select screen left it: type %u (0 human, 1 CPU, 3 none), controller slot %d, ui_chara %016llx, colour %d\n",
                idx + 1, type, (int)slot, (unsigned long long)ld64(st + 0x88), ld8(st + 0x90));
        panel_dump("left", idx, st);
        if (idx == 0) ch_have_mine = 0;   /* (a new set-up: this game's own choice read again) */
        if (net_local != 0 && rules_have) { st8(st + 5, rules_blob[RULES_TOTAL - 2]); st8(st + 0xa, rules_blob[RULES_TOTAL - 1]); }   /* (the host's rule kind and stocks) */
        /* This game's own player is the human panel its own controller joined (controller slot = this game's
         * player, as controllers_rollback_p<n>.ini puts it), at whichever index: the other slot may be a CPU,
         * added first or after (no keyboard stand-in needed). Its choice goes to the peer. */
        /* (v0.9.3) on a joiner any human panel is its own player - the others never join its select screen. One its
         * controller joined on another slot (as player 1, before the host's game was up and gave it its number) is held
         * as a spare: its choice if no panel on its own slot comes by its own panel (it may join again once numbered) */
        static RbnChoice ch_spare; static int ch_have_spare;
        if (idx == 0) ch_have_spare = 0;
        if (net_online) memcpy(online_panel[idx], P(u8, st), 0x460);   /* (as the game left it: record_after builds its own record too) */
        if (net_online) {   /* (an Online match: this game's own panel is the one not a network player's; it plays on its number's slot) */
            slot = type == 0 && !ld8(st + 0x451) ? (u32)net_local : (u32)idx;
            st32(st + 0x210, slot); st8(st + 0x451, 0);
        }
        int exact = type == 0 && slot == (u32)net_local, spare = !exact && type == 0 && net_local > 0 && !ch_have_spare && !net_online;
        if (!ch_have_mine && (exact || spare)) {
            memset(&ch_mine, 0, sizeof ch_mine);
            ch_mine.player = net_local; ch_mine.chara = ld64(st + 0x88); ch_mine.colour = ld8(st + 0x90);
            tag_mine(st);
            ch_mine.gc = hid_slot_is_gc((u32)net_local);
            if (ch_mine.gc) fprintf(stderr, "[rb] handshake: this game's player plays a GameCube controller: its slot is one on every PC\n");
            if (net_local == 0) {
                ch_mine.has_stage = 1; ch_mine.hazards = ld8(SEL_HAZ);
                for (int k = 0; k < 0x20; k++) ch_mine.stage[k] = ld8(sel + (u64)k);
            }
            if (spare) {
                ch_spare = ch_mine; ch_have_spare = 1; memset(&ch_mine, 0, sizeof ch_mine);
                fprintf(stderr, "[rb] handshake: this game's player picked on panel %d with controller slot %d, not %d: its choice unless its own slot's panel comes\n",
                        idx + 1, (int)slot, net_local);
            } else {
                ch_have_mine = 1;
                fprintf(stderr, "[rb] handshake: this game's player %d (panel %d): ui_chara %016llx, colour %d%s (stage entry %u: %016llx, hazards %d)\n", net_local + 1, idx + 1,
                        (unsigned long long)ch_mine.chara, ch_mine.colour, net_local == 0 ? ", sent with the stage" : "", cur, (unsigned long long)ld64(sel), ld8(SEL_HAZ));
                for (int k = 0; k < 3; k++) { net_choice_send(0); Sleep(5); }
            }
        }
        if (idx == net_local && !ch_have_mine && ch_have_spare) {
            ch_mine = ch_spare; ch_have_mine = 1;
            fprintf(stderr, "[rb] handshake: this game's player %d: the choice it made on another panel (ui_chara %016llx, colour %d)\n",
                    net_local + 1, (unsigned long long)ch_mine.chara, ch_mine.colour);
            for (int k = 0; k < 3; k++) { net_choice_send(0); Sleep(5); }
        }
        if (idx == net_local) {   /* this game's own player's slot: its own choice */
            if (ch_have_mine && net_online) {   /* (an Online match: the game server's set-up is the same on both: kept) */
            } else if (ch_have_mine) {
                st64(st + 0x88, ch_mine.chara); st8(st + 0x90, (u8)ch_mine.colour);
            } else fprintf(stderr, "[rb] handshake: player %d's own controller hasn't joined before this panel: join with your own controller FIRST (this match will not be the agreed one)\n", idx + 1);
        } else {   /* the other player: its game's choice, waited for */
            if (!ch_have_peer[idx]) {
                fprintf(stderr, "[rb] handshake: waiting for player %d's choice from its game\n", idx + 1);
                double t0 = now_ms();
                while (!ch_have_peer[idx] && now_ms() - t0 < 600000) { net_hello(1); net_choice_send(1); net_wait_say("Waiting for the others to pick their fighters..."); Sleep(16); }   /* (ours asks for theirs) */
                net_wait_say(NULL);
            }
            if (ch_have_peer[idx] && net_online) {   /* (an Online match: both games have the same players, colours and stage) */
                if (ch_peer[idx].chara != ld64(st + 0x88))
                    fprintf(stderr, "[rb] online: player %d's fighter differs between the games (%016llx here, %016llx there)\n", idx + 1,
                            (unsigned long long)ld64(st + 0x88), (unsigned long long)ch_peer[idx].chara);
            } else if (ch_have_peer[idx]) {
                const RbnChoice *pc = &ch_peer[idx];
                u64 was = ld64(st + 0x88);
                st64(st + 0x88, pc->chara); st8(st + 0x90, (u8)pc->colour);
                if (pc->has_stage) {
                    for (int k = 0; k < 0x20; k++) st8(sel + (u64)k, pc->stage[k]);
                    st8(SEL_HAZ, (u8)pc->hazards);
                }
                fprintf(stderr, "[rb] handshake: player %d set from its game's choice (ui_chara %016llx, was %016llx; colour %d)%s\n", idx + 1,
                        (unsigned long long)pc->chara, (unsigned long long)was, pc->colour, pc->has_stage ? "; the stage too" : "");
            } else fprintf(stderr, "[rb] handshake: no choice from the other game after 10 min: player %d as this game has it\n", idx + 1);
        }
        /* every game builds the same players: human, each on its own controller slot (whatever joined the panel
         * first), each with its own game's name tag (a tag brings its own controls): this game's from its list, the
         * others' put in a free slot here (version 10; before, no tag for anyone) */
        if (!tag_given[idx]) {
            if (idx == net_local) tag_given[idx] = ch_have_mine && ch_mine.has_tag ? tag_mine_slot : 0;
            else if (ch_have_peer[idx] && ch_peer[idx].has_tag) tag_given[idx] = tag_place(ch_peer[idx].tag, idx);
        }
        /* (+ 0x450: 1 for a human's panel, 0 for a CPU's - tag research, 10-31; the PR's 0 for everyone left every
         * player on the default controls, the tag's own never applied: R jumped nowhere, gc1) */
        st32(st + 0x78, 0); st32(st + 0x210, (u32)idx); st8(st + 0x450, 1);
        st8(st + 0x454, 0); st8(st + 0x455, 0); st32(st + 0x214, tag_given[idx]);
        panel_dump("built", idx, st);
    }
}

/* FUN 0x7103549720: kicks the task workers (see the top of this file). */
/* (hle_rb_wraps.c's wrap of it calls this. Never inlined: the frame below is this function's own,
 * the same every frame, reached through the same calls.) */
__attribute__((noinline)) void rb_job_kick(Cpu *c, GuestFn *orig) {
    if (!rb_on) { orig(c); return; }
    u64 lr = c->x[30];
    if (!kick_skipped(lr)) orig(c);
    if (rb_on && lr == SAFE_POINT) {
        uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
        rb_main_kthread = c->thread;
        {   /* the frame now ending: safe point -> poll -> frame end -> frame end's return -> here */
            double now = now_ms();
            if (rb_fast > 0 && fp_out && seg_poll > fp_out && seg_fe_in > seg_poll && seg_fe_out >= seg_fe_in) {
                seg_sum[0] += seg_poll - fp_out; seg_sum[1] += seg_fe_in - seg_poll;
                seg_sum[2] += seg_fe_out - seg_fe_in; seg_sum[3] += now - seg_fe_out; seg_n++;
            }
            seg_poll = 0;
        }
        fp_in = now_ms(); fp_fast = rb_fast > 0 ? (rb_fast > 1 ? 1 : 2) : 0; fp_loaded = 0;
        double game = fp_out ? fp_in - fp_out : 0;
        safe_point(c, frame);
        double census = fp_census_last;
        /* (net mode: from a match set-up, the entrance and countdown too: on real time there, the two PCs
         * began the fight with the fighters at different points of their entrances (twopc2-5)) */
        if (clock_on) rb_clock_frame(S.in_fight || (net_countdown && net_early_on()), rt_now_ns());
        savestates(c, frame);
        /* (after a load this runs in the save's frame: the statics, not the locals of then, are now's) */
        double out = now_ms();
        if (fp_fast && !fp_loaded) {
            int k = fp_fast - 1;
            fp_n[k]++;
            fp_sum[k][0] += game; fp_sum[k][1] += census;
            fp_sum[k][2] += rb_save_ms[0]; fp_sum[k][3] += rb_save_ms[1]; fp_sum[k][4] += rb_save_ms[2] - rb_save_ms[0] - rb_save_ms[1];
            fp_sum[k][5] += out - fp_in - census - rb_save_ms[2] - fp_hash_last;
            fp_sum[k][6] += fp_hash_last;
        }
        fp_out = out;
    }
}

/* ---- Run-ahead (local play) ------------------------------------------------------------------
 * SSBU_RUNAHEAD=<n> (0-3), or display.ini's runahead (the settings menu's Display page: off, 1, 2):
 * in a fight, each frame shown runs n frames ahead of the game's own pipeline (runahead.h has the
 * schedule): a real frame hidden, a save, n - 1 frames predicted on the controls held now, the frame
 * shown, a load back to the save. 1 predicts nothing (the picture shown is the real logic frame's, one
 * frame sooner); 2 shows one logic frame ahead. Only in fights (the fighter manager's ready-go flag, as
 * ssbu-recomp's ra_fight_on), never with the net mode, the ring, the replay check, the save benchmark
 * or the auto test (they own the savestates); every pad is read live then (no tape). The hidden frames
 * run as a catch-up's (rb_fast: not shown, not paced; with rb_fast 1, so no presentation work is left
 * out: the camera and the HUD go on in the real frames); the sound queued in the frames thrown away
 * goes at the load (ipc_audio.c's splice, SSBU_RB_AUDIO_SPLICE). A load refused leaves the predicted
 * frames as the real ones (counted); a save refused makes that frame a normal one.
 * SSBU_RA_TRACE=<file> (tests, with or without run-ahead): per fight safe point, "R <logic frame> <fight
 * hash> <player 1's status> <motion>" for a real state, "S <frame shown> <logic frame> <hash> <status>
 * <motion>" for the state the next frame shown draws, and "P <frame shown> <logic frame> <buttons>"
 * when player 1's buttons change (tools/runahead_trace.py reads them: the delay from a press to its
 * picture, and the real frames against a run without run-ahead). Its fight hash costs some 5 ms.
 * The "[rb] run-ahead" lines, every 10 s of frames shown, time each part; SSBU_RA_STATS=1 times the frames shown
 * without run-ahead too. At normal speed a fight whose frames shown come more than 18.5 ms apart for 2 s goes on
 * without run-ahead (SSBU_RA_KEEP=1: kept on). */
static RaSched ra;
static FILE *ra_tf;
static long long ra_shown, ra_seg_host;   /* frames shown in fights; the frame shown the segment running now belongs to */
static double ra_t_start, ra_t_save0, ra_t_save1, ra_t_load0;
static double ra_ms[5], ra_ms_max[5]; static long long ra_ms_n;   /* real frame, save, the frames after, load; [4] the time between frames shown */
static double ra_shown_t; static long long ra_gaps, ra_gaps_late;
static int ra_slow;   /* (this fight: too slow with run-ahead, off) */
static long long ra_ended_ahead;
static u64 ra_last_buttons = ~0ull;

static int ra_frames(void) {
    static int env = -2;
    if (env == -2) { const char *e = getenv("SSBU_RUNAHEAD"); env = e && *e ? atoi(e) : -1; }
    int display_runahead(void);
    int n = env >= 0 ? env : display_runahead();
    return n < 0 ? 0 : n > 3 ? 3 : n;
}
int rb_ra_predicting(void) { return rb_on && ra_predicting(&ra); }

static void ra_trace(Cpu *c, char kind, u32 key) {
    if (!ra_tf) return;
    static FFrame f;
    static long long at = -1;
    if (at != S.points) { fight_frame(c, &f); at = S.points; }   /* (R and S of one safe point: one hash) */
    int i = 0;
    while (i < (int)f.n && f.f[i].entry != 0) i++;
    if (i == (int)f.n) i = 0;
    int st = f.n ? f.f[i].st : -1;
    u64 mot = f.n ? f.f[i].mot : 0;
    if (kind == 'S') fprintf(ra_tf, "S %lld %u %016llx %d %llx\n", ra_shown, key, (unsigned long long)f.h, st, (unsigned long long)mot);
    else fprintf(ra_tf, "R %u %016llx %d %llx\n", key, (unsigned long long)f.h, st, (unsigned long long)mot);
}
/* (rb_pad: player 1's reads, live; only the real frames' count) */
void ra_pad_read(u32 id, u64 buttons) {
    if (!ra_tf || id != 0 || ra_predicting(&ra) || buttons == ra_last_buttons) return;
    ra_last_buttons = buttons;
    fprintf(ra_tf, "P %lld %u %llx\n", ra_seg_host, ld32(LOGIC_FRAME), (unsigned long long)buttons);
}

static void ra_report(const char *why);
/* the next segment hidden or shown */
static void ra_next(Cpu *c, u32 key, int hide) {
    extern volatile int gpu_hide_fast;
    static int draw_hidden = -1;
    if (draw_hidden < 0) { const char *e = getenv("SSBU_RA_DRAW_HIDDEN"); draw_hidden = e && *e == '1'; }
    gpu_hide_fast = hide && !draw_hidden;
    InterlockedExchange(&rb_fast, hide ? 1 : 0);
    ra_seg_host = ra_shown;
    if (!hide) {
        ra_trace(c, 'S', key); ra_shown++;
        double t = now_ms();
        if (ra_shown_t && (ra.phase != RA_IDLE || !ra.n)) { double g = t - ra_shown_t; ra_ms[4] += g; if (g > ra_ms_max[4]) ra_ms_max[4] = g; ra_gaps++; ra_gaps_late += g > 20; }
        ra_shown_t = t;
        /* (too slow for 60 frames a second: run-ahead off for the rest of this fight, rather than the game at half speed.
         * At normal speed only; SSBU_RA_KEEP=1 keeps it on, to measure) */
        static int keep = -1; static double win_t0; static long long win_n;
        if (keep < 0) { const char *e = getenv("SSBU_RA_KEEP"); double display_speed(void); keep = (e && *e == '1') || display_speed() != 1; }
        if (!keep && ra.phase != RA_IDLE && !ra_slow) {
            if (!win_n++) win_t0 = t;
            else if (win_n > 120) {
                double mean = (t - win_t0) / (double)(win_n - 1);
                if (mean > 18.5) {
                    ra_slow = 1;
                    fprintf(stderr, "[rb] run-ahead: a frame shown every %.1f ms over the last 2 s, too slow for 60 a second: off for the rest of this fight\n", mean);
                }
                win_n = 0;
            }
        }
        if (ra_gaps && ra_gaps % 600 == 0) ra_report("10 s");
    }
}

extern double rb_load_parts[6]; extern long long rb_load_count;   /* (hle_rb_state.c) */
extern long long rb_save_away_waits, rb_save_away_refused;
static void ra_report(const char *why) {
    if (!ra_ms_n && !ra_gaps && !ra.paused && !ra.save_failed && !ra.load_refused) return;
    double n = ra_ms_n ? (double)ra_ms_n : 1;
    {   /* the save's and the load's parts since the last line */
        static double sp[4], lp[6]; static long long sn, ln;
        double ds = rb_save_count - sn ? (double)(rb_save_count - sn) : 1, dl = rb_load_count - ln ? (double)(rb_load_count - ln) : 1;
        fprintf(stderr, "[rb] run-ahead: a save's parts: hold %.2f ms, GPU drain %.2f, memory %.2f, the rest %.2f; a load's: hold %.2f, check and stacks %.2f, "
                "GPU drain %.2f, memory %.2f, host state %.2f, the rest %.2f (means)\n",
                (rb_save_parts[0] - sp[0]) / ds, (rb_save_parts[1] - sp[1]) / ds, (rb_save_parts[2] - sp[2]) / ds,
                (rb_save_parts[3] - sp[3] - (rb_save_parts[0] - sp[0]) - (rb_save_parts[1] - sp[1]) - (rb_save_parts[2] - sp[2])) / ds,
                (rb_load_parts[0] - lp[0]) / dl, (rb_load_parts[1] - lp[1]) / dl, (rb_load_parts[2] - lp[2]) / dl, (rb_load_parts[3] - lp[3]) / dl,
                (rb_load_parts[4] - lp[4]) / dl, (rb_load_parts[5] - lp[5] - (rb_load_parts[0] - lp[0]) - (rb_load_parts[1] - lp[1]) - (rb_load_parts[2] - lp[2])
                                                   - (rb_load_parts[3] - lp[3]) - (rb_load_parts[4] - lp[4])) / dl);
        memcpy(sp, rb_save_parts, sizeof sp); memcpy(lp, rb_load_parts, sizeof lp); sn = rb_save_count; ln = rb_load_count;
    }
    {
        extern double gpu_q_busy_ms[2]; extern long long gpu_q_jobs[2];
        static double gb[2]; static long long gj[2];
        double fr = ra_gaps ? (double)ra_gaps : 1;
        fprintf(stderr, "[rb] run-ahead: the GPU thread per frame shown: %.2f ms in %.1f submissions of hidden frames, %.2f ms in %.1f of shown ones\n",
                (gpu_q_busy_ms[1] - gb[1]) / fr, (gpu_q_jobs[1] - gj[1]) / fr, (gpu_q_busy_ms[0] - gb[0]) / fr, (gpu_q_jobs[0] - gj[0]) / fr);
        memcpy(gb, gpu_q_busy_ms, sizeof gb); memcpy(gj, gpu_q_jobs, sizeof gj);
        extern double watch_prof_ms[2][2]; extern long long watch_prof_n[2][3];
        static double wm[2][2]; static long long wn[2][3];
        for (int w = 0; w < 2; w++)
            fprintf(stderr, "[rb] run-ahead: the write watch per frame shown, %s: asked %.1f times in %.2f ms, reset %.1f times in %.2f ms (%.0f pages)\n", w ? "the savestates" : "the renderer",
                    (watch_prof_n[w][0] - wn[w][0]) / fr, (watch_prof_ms[w][0] - wm[w][0]) / fr, (watch_prof_n[w][1] - wn[w][1]) / fr, (watch_prof_ms[w][1] - wm[w][1]) / fr, (watch_prof_n[w][2] - wn[w][2]) / fr);
        memcpy(wm, watch_prof_ms, sizeof wm); memcpy(wn, watch_prof_n, sizeof wn);
    }
    fprintf(stderr, "[rb] run-ahead %d (%s): %lld frames shown ahead; per frame: the real frame %.2f ms (max %.1f), save %.2f (max %.1f), "
            "the frames after %.2f (max %.1f), load %.2f (max %.1f); a frame shown every %.2f ms (max %.1f, %lld over 20 ms); "
            "%lld paused frames, %lld saves and %lld loads refused (%lld saves waited for a job, %lld of them in vain), %lld fights ended ahead\n",
            ra.n, why, ra.cycles, ra_ms[0] / n, ra_ms_max[0], ra_ms[1] / n, ra_ms_max[1], ra_ms[2] / n, ra_ms_max[2], ra_ms[3] / n, ra_ms_max[3],
            ra_gaps ? ra_ms[4] / (double)ra_gaps : 0, ra_ms_max[4], ra_gaps_late, ra.paused, ra.save_failed, ra.load_refused, rb_save_away_waits, rb_save_away_refused, ra_ended_ahead);
    memset(ra_ms, 0, sizeof ra_ms); memset(ra_ms_max, 0, sizeof ra_ms_max); ra_ms_n = 0; ra_gaps = ra_gaps_late = 0;
}
static void ra_add(int k, double ms) { ra_ms[k] += ms; if (ms > ra_ms_max[k]) ra_ms_max[k] = ms; }

static void ra_fight_over(void) {
    if (ra.phase == RA_AHEAD) ra_ended_ahead++;   /* (the predicted frames stay: the fight is over) */
    if (ra.phase != RA_IDLE) InterlockedExchange(&rb_fast, 0);
    { extern volatile int gpu_hide_fast; gpu_hide_fast = 0; }
    ra_stop(&ra);
    ra_shown_t = 0; ra_slow = 0;
    if (ra_engaged) ra_report("the fight is over");
    ra_engaged = 0;
    if (ra_tf) fflush(ra_tf);
}

/* (savestates, at a fight's safe point) 1: run-ahead (or its trace) has this safe point. */
static int ra_blocked(void) { return net_mode || ring_from >= 0 || ver_at >= 0 || bench_from >= 0 || auto_at >= 0; }

/* The fight's first save copied all of memory (2 GB, about a second) at GO!, as the net mode's did (its "GO! freeze",
 * net_pre_fight): here too that copy is taken at the match set-up, in the loading screen, and kept current every
 * RA_PRE_EVERY frames through the loading, the entrances and the countdown (each refresh waiting 4 ms at most for the
 * threads, else left out), every state let go after; the save at GO! then copies a few frames' changes. */
#define RA_PRE_EVERY 4
static int ra_pre;
static long long ra_pre_frames, ra_pre_ok, ra_pre_skipped;
/* (the ring test, SSBU_RB_RING, offline: the same copy at the set-up, so that its first save at <frame> is a fight
 * save as a match's is, not the whole 2 GB copy - 376-538 ms in its logs, a freeze no match has) */
void ra_set_up(void) {
    if (net_mode < 0) return;
    if (ring_from >= 0 ? net_mode != 0 : (ra_blocked() || !ra_frames())) return;
    ra_pre = 1; ra_pre_frames = 0;
    sm_pre_reset();
}
extern double rb_park_quick_ms;   /* (hle_rb_state.c) */
int ra_pre_point(Cpu *c, u64 frame) {
    if (!ra_pre) return 0;
    if (ra_pre_frames > 1800) { ra_pre = 0; return 0; }   /* (a set-up left without a fight, or a long loading: 30 s, then no more) */
    void rb_ring_drop(void);
    sm_pre_step(ra_pre_frames + 1);   /* (the GO! hitch: the state map on in the entrances) */
    if (ra_pre_frames++ % RA_PRE_EVERY) return 1;
    double t0 = now_ms();
    if (ra_pre_frames > 1) rb_park_quick_ms = 4;
    rb_quiet(1);
    rb_save_tag = 0xFFFFFFFFu;
    int saved = rb_save(c, frame);
    rb_park_quick_ms = 0;
    if (saved == 1) { rb_ring_drop(); ra_pre_ok++; } else ra_pre_skipped++;
    if (ra_pre_frames == 1)
        fprintf(stderr, "[rb] run-ahead, before the fight: the whole copy at the set-up %s in %.0f ms\n", saved == 1 ? "taken" : "NOT taken (the threads didn't stop)", now_ms() - t0);
    return 1;
}

int runahead_point(Cpu *c, u64 frame) {
    static int blocked = -1;
    if (ra_pre) {
        fprintf(stderr, "[rb] run-ahead, before the fight: the copy refreshed %lld times in %lld frames, %lld left out (threads busy)\n", ra_pre_ok, ra_pre_frames, ra_pre_skipped);
        ra_pre = 0; ra_pre_ok = ra_pre_skipped = 0;
    }
    if (blocked < 0) {
        blocked = ra_blocked();
        const char *t = getenv("SSBU_RA_TRACE");
        if (t && *t && !blocked) { ra_tf = fopen(t, "w"); if (!ra_tf) fprintf(stderr, "[rb] SSBU_RA_TRACE: cannot write %s\n", t); }
        if (ra_frames() || ra_tf)
            fprintf(stderr, "[rb] run-ahead: %d frame(s)%s%s\n", ra_frames(), blocked ? ": OFF, another test mode has the savestates" : "",
                    ra_tf ? "; the trace (SSBU_RA_TRACE) on" : "");
    }
    if (blocked) return 0;
    ra.n = ra_slow ? 0 : ra_frames();
    static int stats = -1;   /* (SSBU_RA_STATS=1: the frames shown timed without run-ahead too, to compare) */
    if (stats < 0) { const char *e = getenv("SSBU_RA_STATS"); stats = e && *e == '1'; }
    if (!ra.n && ra.phase == RA_IDLE && !ra_tf && !stats) {
        if (ra_engaged) { ra_report("off"); ra_engaged = 0; }
        return 0;
    }
    if (!ra_engaged) {
        ra_engaged = 1;
        static int quiet;
        if (!quiet++) rb_quiet(1);   /* (a save and a load a frame: their lines only when slow) */
    }
    u32 key = ld32(LOGIC_FRAME);
    int hide = 0, act = ra_point(&ra, key, &hide);
    if (act == RA_SAVE) {
        ra_trace(c, 'R', key);
        ra_t_save0 = now_ms();
        rb_save_tag = key;
        extern int rb_save_home_only;
        rb_save_home_only = 1;
        int r = rb_save(c, frame);
        rb_save_home_only = 0;
        if (r == 2) {   /* a load came back here: the real game, at the logic frame after R's */
            double t = now_ms();
            ra_add(3, t - ra_t_load0);
            ra_ms_n++;
            ra_loaded(&ra, ld32(LOGIC_FRAME), &hide);
            ra_t_start = t;
            ra_next(c, ld32(LOGIC_FRAME), hide);
            return 1;
        }
        if (r == 1) { ra_t_save1 = now_ms(); ra_add(0, ra_t_save0 - ra_t_start); ra_add(1, ra_t_save1 - ra_t_save0); }
        ra_saved(&ra, r == 1, &hide);
        if (r != 1 && (ra.save_failed <= 5 || ra.save_failed % 600 == 0))
            fprintf(stderr, "[rb] run-ahead: save refused at logic frame %u (%lld so far): a normal frame\n", key, ra.save_failed);
    } else if (act == RA_LOAD) {
        ra_t_load0 = now_ms();
        ra_add(2, ra_t_load0 - ra_t_save1);
        rb_load(c, frame);   /* (returns only if refused) */
        ra_refused(&ra, &hide);
        if (ra.load_refused <= 5 || ra.load_refused % 600 == 0)
            fprintf(stderr, "[rb] run-ahead: load refused at logic frame %u (%lld so far): the predicted frames stay\n", key, ra.load_refused);
    } else if (ra.phase != RA_AHEAD) {
        ra_trace(c, 'R', key);   /* (a normal frame's state, or R's start: real) */
        if (ra.phase == RA_REAL) ra_t_start = now_ms();
    }
    ra_next(c, key, hide);
    return 1;
}

#endif   /* _WIN32 */
