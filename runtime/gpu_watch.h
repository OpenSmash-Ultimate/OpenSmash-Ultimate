/* The write watch's stamps (gpu_watch.c): which pages of the alias area the CPU wrote, and since
 * when, for every user of the write watch at once - the renderer's textures, kept constant buffers,
 * compute inputs and resident regions, and rollback's savestates (watch_rb_*, hle_rb_state.c).
 * Not drawing: any back end behind gpu_backend.h uses the same stamps (macOS' Metal back end keeps
 * its own copy). */
#pragma once
#include "host.h"

/* The write watch keeps one bit a page of the alias area, and a query that resets it takes the
 * news from everyone else watching that page. Textures, kept constant buffers, compute inputs
 * and resident regions share pages - memory a scene gives up is handed out again for something
 * else - and whoever asked first kept the news for good: the No Contest screen skinned the
 * fighters' arms from inputs never taken again (stretched over the screen). So every question
 * goes through here: pages found written get the next stamp, kept per page, and each user keeps
 * the clock as it was when it last asked ("seen"): its data is current unless one of its pages
 * has a newer stamp. */
#define WATCH_CHUNK 512   /* pages a chunk: 2 MB */
typedef struct WatchChunk { u64 newest, page[WATCH_CHUNK]; } WatchChunk;
extern WatchChunk *watch_chunks[ALIAS_SIZE / (WATCH_CHUNK * 0x1000ull)];
extern u64 watch_clock;   /* stamps given so far */

/* Take the write watch's news for guest pages [lo, hi) (page aligned, watched): resetting the
 * bits as they are read, so a write after this is news next time. (Through the kernel, which
 * shares the system's bits with the rollback ring when that is on: kernel_watch.h.) One caller at
 * a time (a lock: rollback's savestates ask from the game's main thread). */
void watch_take(u64 lo, u64 hi);

/* The first page in [p, hi) stamped after `seen` (hi if none). */
static inline u64 watch_next(u64 p, u64 hi, u64 seen) {
    while (p < hi) {
        u64 i = (p - ALIAS_BASE) >> 12, end = (i / WATCH_CHUNK + 1) * WATCH_CHUNK;
        const WatchChunk *c = watch_chunks[i / WATCH_CHUNK];
        if (c && c->newest > seen)
            for (; i < end && ALIAS_BASE + (i << 12) < hi; i++)
                if (c->page[i % WATCH_CHUNK] > seen) return ALIAS_BASE + (i << 12);
        p = ALIAS_BASE + (end << 12);
    }
    return hi;
}

/* Has guest memory [g, g + size) been written since *seen? (*seen moves on to now.) Ask before
 * reading the memory, so that a write meanwhile is news next time. */
static inline int watch_written(u64 g, u64 size, u64 *seen) {
    u64 lo = g & ~0xFFFull, hi = (g + size + 0xFFF) & ~0xFFFull;
    watch_take(lo, hi);
    int written = watch_next(lo, hi, *seen) < hi;
    *seen = watch_clock;
    return written;
}

/* (rollback netplay, SSBU_RB: hle_rb_state.c declares its own) */
u64 watch_rb_take(u64 lo, u64 hi);
u64 watch_rb_next(u64 p, u64 hi, u64 seen);
u64 watch_rb_begin(void);
void watch_rb_end(void);
void watch_rb_scan(u64 lo, u64 hi, u64 stamp, void **pages);
