/* SSBU_RB_DCDEFER's page bits (kernel.c): memory the game gives back during a rollback fight, kept committed.
 * Two bits a page of the alias area, numbered from its base: KEPT (given back, still committed) and ZERO (given back
 * in this fight: cleared when the game maps it anew, also after a load brought back a frame from before an earlier
 * clear, so it stays until the fight ends). Plain bit work, with no system call, so that tests/test_rb_dcdefer.py
 * can run it alone; the caller holds its lock, asks whether a page is mapped in the guest's memory map now, and
 * clears or decommits the runs it is given. */
#ifndef RB_DCDEFER_H
#define RB_DCDEFER_H
#include <stdint.h>

typedef struct DcDefer {
    uint64_t *kept, *zero;                 /* a bit a page, as many as the area has */
    uint64_t lo, hi;                       /* the pages ever marked since the bits were last empty: [lo, hi) */
    long long now, peak;                   /* KEPT pages now, at most at once */
    long long calls, pages, load_pages;    /* give-backs kept and their pages; pages a load would have decommitted */
    long long zeroed, zero_calls;          /* pages cleared when mapped anew, in how many runs */
} DcDefer;

static inline int dcd_bit(const uint64_t *b, uint64_t i) { return (int)(b[i >> 6] >> (i & 63) & 1); }
static inline void dcd_set(uint64_t *b, uint64_t i) { b[i >> 6] |= 1ull << (i & 63); }
static inline void dcd_unset(uint64_t *b, uint64_t i) { b[i >> 6] &= ~(1ull << (i & 63)); }

/* Pages [i0, i1) given back and kept committed. */
static inline void dcd_mark(DcDefer *d, uint64_t i0, uint64_t i1) {
    for (uint64_t i = i0; i < i1; i++) {
        if (!dcd_bit(d->kept, i)) d->now++;
        dcd_set(d->kept, i);
        dcd_set(d->zero, i);
    }
    if (i0 < d->lo) d->lo = i0;
    if (i1 > d->hi) d->hi = i1;
    if (d->now > d->peak) d->peak = d->now;
}

/* Pages [i0, i1) decommitted for real: neither bit any more. */
static inline void dcd_unmark(DcDefer *d, uint64_t i0, uint64_t i1) {
    for (uint64_t i = i0; i < i1; i++) {
        if (dcd_bit(d->kept, i)) { dcd_unset(d->kept, i); d->now--; }
        dcd_unset(d->zero, i);
    }
}

/* The game maps [i0, i1) (after the commit, before the region is added): each run of ZERO pages not mapped before
 * is handed to clear(first, end) to be zeroed, and is not KEPT any more. Outside a fight (fight 0) ZERO goes too: no
 * load can bring back a frame from before the clear then. */
static inline void dcd_take(DcDefer *d, uint64_t i0, uint64_t i1, int fight, int (*mapped)(uint64_t, void *),
                            void (*clear)(uint64_t, uint64_t, void *), void *ctx) {
    if (i0 < d->lo) i0 = d->lo;
    if (i1 > d->hi) i1 = d->hi;
    for (uint64_t i = i0; i < i1;) {
        if (!dcd_bit(d->zero, i) || mapped(i, ctx)) { i++; continue; }
        uint64_t j = i;
        while (j < i1 && dcd_bit(d->zero, j) && !mapped(j, ctx)) {
            if (dcd_bit(d->kept, j)) { dcd_unset(d->kept, j); d->now--; }
            if (!fight) dcd_unset(d->zero, j);
            j++;
        }
        clear(i, j, ctx);
        d->zeroed += (long long)(j - i); d->zero_calls++;
        i = j;
    }
}

/* The fight's end. mode 1: each run of KEPT pages the game has not mapped again goes to release(first, end) to be
 * decommitted, and loses both bits; mode 2: they stay KEPT and ZERO (zeros when the game maps them). Pages mapped
 * again lose their ZERO bit either way. Returns the pages released. */
static inline long long dcd_end(DcDefer *d, int mode, int (*mapped)(uint64_t, void *),
                                void (*release)(uint64_t, uint64_t, void *), void *ctx) {
    long long freed = 0;
    for (uint64_t i = d->lo; i < d->hi;) {
        if (!(i & 63) && !d->kept[i >> 6] && !d->zero[i >> 6]) { i += 64; continue; }   /* (a word of nothing) */
        int k = dcd_bit(d->kept, i);
        if (!k && !dcd_bit(d->zero, i)) { i++; continue; }
        if (!k) { dcd_unset(d->zero, i); i++; continue; }                     /* (mapped again since) */
        if (mode == 2 || mapped(i, ctx)) { if (mapped(i, ctx)) { dcd_unset(d->kept, i); dcd_unset(d->zero, i); d->now--; } i++; continue; }
        uint64_t j = i;
        while (j < d->hi && dcd_bit(d->kept, j) && !mapped(j, ctx)) { dcd_unset(d->kept, j); dcd_unset(d->zero, j); d->now--; j++; }
        release(i, j, ctx);
        freed += (long long)(j - i);
        i = j;
    }
    if (!d->now) { d->lo = ~0ull; d->hi = 0; }
    return freed;
}
#endif
