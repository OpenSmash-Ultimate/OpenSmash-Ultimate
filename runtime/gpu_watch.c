/* The write watch's stamps (gpu_watch.h): shared by the back end behind gpu_backend.h and by
 * rollback's savestates. Moved here from gpu_d3d11.c unchanged, so a second back end has them.
 * (macOS: macos/gpu_metal.m keeps its own; this file is left out of the Mac program.) */
#ifndef __APPLE__
#include <stdlib.h>

#include "gpu_watch.h"
#include "kernel_watch.h"   /* (kernel_write_watch) */

WatchChunk *watch_chunks[ALIAS_SIZE / (WATCH_CHUNK * 0x1000ull)];
u64 watch_clock;

static void watch_take_locked(u64 lo, u64 hi);
/* (rollback netplay, SSBU_RB: hle_rb_state.c's savestates ask too, from the game's main thread, so
 * one at a time: watch_rb_take and the others below) */
static SRWLOCK watch_lock = SRWLOCK_INIT;
void watch_take(u64 lo, u64 hi) {
    AcquireSRWLockExclusive(&watch_lock);
    watch_take_locked(lo, hi);
    ReleaseSRWLockExclusive(&watch_lock);
}
double watch_prof_ms[2][2]; long long watch_prof_n[2][3]; static int watch_who;   /* [0] the renderer's questions, [1] the savestates' */   /* (a test's profile: asking, resetting; calls, resets, pages reset) */
static void watch_take_locked(u64 lo, u64 hi) {
    static void *pages[65536];   /* (256 MB: the largest range asked about, in one call) */
    void *first;
    ULONG_PTR count = 1;
    ULONG granularity;
    u64 t0 = rt_now_ns();
    watch_prof_n[watch_who][0]++;
    /* (asking is cheap, resetting is not - even where nothing was written: so ask first) */
    UINT q = kernel_write_watch(WATCH_RENDERER, 0, (void *)lo, hi - lo, &first, &count, &granularity);
    watch_prof_ms[watch_who][0] += (double)(rt_now_ns() - t0) / 1e6;
    if (q != 0 || !count) return;
    for (u64 at = (u64)first; at < hi;) {
        count = 65536;
        u64 t1 = rt_now_ns();
        UINT r = kernel_write_watch(WATCH_RENDERER, WRITE_WATCH_FLAG_RESET, (void *)at, hi - at, pages, &count, &granularity);
        watch_prof_ms[watch_who][1] += (double)(rt_now_ns() - t1) / 1e6; watch_prof_n[watch_who][1]++; watch_prof_n[watch_who][2] += count;
        if (r != 0 || !count) return;
        u64 stamp = ++watch_clock;
        for (ULONG_PTR k = 0; k < count; k++) {
            u64 p = ((u64)pages[k] - ALIAS_BASE) >> 12;
            WatchChunk **c = &watch_chunks[p / WATCH_CHUNK];
            if (!*c && !(*c = calloc(1, sizeof **c))) continue;
            (*c)->page[p % WATCH_CHUNK] = (*c)->newest = stamp;
        }
        if (count < 65536) return;
        at = (u64)pages[count - 1] + 0x1000;
    }
}

/* (rollback netplay, SSBU_RB) The savestate's own questions about the alias area, through the same
 * stamps, so the textures' news stays theirs: take the news for [lo, hi) and say the clock; the
 * first page in [p, hi) stamped after `seen`. */
u64 watch_rb_take(u64 lo, u64 hi) {
    AcquireSRWLockExclusive(&watch_lock);
    watch_who = 1;
    watch_take_locked(lo, hi);
    watch_who = 0;
    u64 now = watch_clock;
    ReleaseSRWLockExclusive(&watch_lock);
    return now;
}
u64 watch_rb_next(u64 p, u64 hi, u64 seen) { return watch_next(p, hi, seen); }
/* (SSBU_RB_STAMPS=3) The same, its ranges asked on several threads at once: watch_rb_begin takes
 * the lock and the stamp, then watch_rb_scan from any thread (ranges whole watch chunks apart,
 * pages: room for 65536), then watch_rb_end. */
u64 watch_rb_begin(void) { AcquireSRWLockExclusive(&watch_lock); return ++watch_clock; }
void watch_rb_end(void) { ReleaseSRWLockExclusive(&watch_lock); }
/* (the savestates' state map, a load) The pages of [lo, hi) written since their bits were last reset,
 * asked without resetting them (their news stays for whoever takes it): a bit each set in `bits`, a
 * bit per page of the alias area. */
void watch_rb_dirty(u64 lo, u64 hi, u64 *bits) {
    static void *pages[65536];
    AcquireSRWLockExclusive(&watch_lock);
    for (u64 at = lo; at < hi;) {
        ULONG_PTR count = 65536;
        ULONG granularity;
        u64 t0 = rt_now_ns();
        UINT r = kernel_write_watch(WATCH_RENDERER, 0, (void *)at, hi - at, pages, &count, &granularity);
        watch_prof_ms[1][0] += (double)(rt_now_ns() - t0) / 1e6; watch_prof_n[1][0]++;
        if (r != 0 || !count) break;
        for (ULONG_PTR k = 0; k < count; k++) {
            u64 p = ((u64)pages[k] - ALIAS_BASE) >> 12;
            bits[p >> 6] |= 1ull << (p & 63);
        }
        if (count < 65536) break;
        at = (u64)pages[count - 1] + 0x1000;
    }
    ReleaseSRWLockExclusive(&watch_lock);
}
/* (hle_rb_state.c, SSBU_RB_SM_PAR) The stamp chunks of [lo, hi) made now, on one thread, before watch_rb_scan
 * stamps ranges that may share one on several. */
void watch_rb_prepare(u64 lo, u64 hi) {
    for (u64 p = (lo - ALIAS_BASE) >> 12; p < (hi - ALIAS_BASE + 0xFFF) >> 12; p = (p / WATCH_CHUNK + 1) * WATCH_CHUNK) {
        WatchChunk **c = &watch_chunks[p / WATCH_CHUNK];
        if (!*c) *c = calloc(1, sizeof **c);
    }
}
void watch_rb_scan(u64 lo, u64 hi, u64 stamp, void **pages) {
    void *first;
    ULONG_PTR count = 1;
    ULONG granularity;
    if (kernel_write_watch(WATCH_RENDERER, 0, (void *)lo, hi - lo, &first, &count, &granularity) != 0 || !count) return;
    for (u64 at = (u64)first; at < hi;) {
        count = 65536;
        if (kernel_write_watch(WATCH_RENDERER, WRITE_WATCH_FLAG_RESET, (void *)at, hi - at, pages, &count, &granularity) != 0 || !count) return;
        for (ULONG_PTR k = 0; k < count; k++) {
            u64 p = ((u64)pages[k] - ALIAS_BASE) >> 12;
            WatchChunk **c = &watch_chunks[p / WATCH_CHUNK];
            if (!*c && !(*c = calloc(1, sizeof **c))) continue;
            (*c)->page[p % WATCH_CHUNK] = (*c)->newest = stamp;
        }
        if (count < 65536) return;
        at = (u64)pages[count - 1] + 0x1000;
    }
}
#endif
