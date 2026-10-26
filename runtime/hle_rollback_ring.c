/* The rollback ring (rollback_ring.h): the guest's writable memory at each of the last frames,
 * and any of them put back. SSBU_ROLLBACK_RING=1 (8 frames) or a number of frames from 2 to 64
 * turns it on; it is off unless set so.
 *
 * In a run it only saves, at each frame end (FRAME_HOOK_END, after the game's function and the
 * fight test's handler: frame_hooks.h), and says what that costs: the pages written and changed
 * a frame, a save's time, the memory held. It never puts a frame back there. The game's other
 * threads run on through the save (audio, loading, the GPU's), so a frame is not one moment of
 * every thread; and putting one back under a running game would change memory under them. That
 * needs the guest's threads stopped at known places first, which is later work, as is the state
 * that is not guest memory at all (docs/TECHNICAL.md, "Rollback ring"). Restoring is for
 * tools/dev/ringtest.c until then.
 *
 * What a frame holds: every page of the memory map's writable regions (heap, alias memory, the
 * stacks and thread-local regions, the modules' data), as kernel_writable_ranges gives them. The
 * newest frame is a table with a copy for each page (none for a page of zeros). Each other frame
 * is an undo log: for each page that changed between it and the next, the copy it had. A save
 * compares the pages that may have changed with their copies and makes a new copy only of those
 * that differ, logging the old one; putting frame f back undoes the logs from the newest down to
 * f's and writes each page they name once (zeros in a page mapped now that f did not have: the
 * memory map is not put back). A page copy belongs to exactly one place, the table
 * or a log entry, and goes back to the pool when that is dropped: the oldest frame's next log,
 * once the oldest is dropped, or a newer frame's table copies when a restore undoes them.
 *
 * Which pages may have changed: those the write watch reports for the ring (kernel_watch.h: the
 * kernel shares the system's bits with the renderer, so neither misses the other's news), every
 * page of a region no watch sees (the modules' data, which host.c maps outside the watched
 * areas: compared in full at each save), and pages mapped or unmapped since. The kernel's lock
 * is held through a save (kernel_memory_hold), so no region comes or goes in the middle. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_hooks.h"
#include "host.h"
#include "kernel_watch.h"
#include "rollback_ring.h"

#define RING_PAGE 0x1000ull
#define RING_CHUNK 512u                                                   /* pages a chunk of the table: 2 MB */
#define RING_CHUNKS ((ASPACE_BASE + ASPACE_SIZE) / RING_PAGE / RING_CHUNK)
#define RING_BLOCK 256u                                                   /* page copies a block: 1 MiB */
#define RING_ASK 65536u                                                   /* pages a question of the write watch */

typedef u32 Ref;   /* a page copy: 0 a page of zeros, else 1 + its place in the blocks */
typedef struct Undo { u32 page; Ref had; } Undo;   /* a page that changed in a frame: its copy in the frame before */
typedef struct RingFrame { u64 number; Undo *undo; u32 nundo, capundo; } RingFrame;
typedef struct Ranges { KernelRange *r; u32 n, cap; } Ranges;

static SRWLOCK ring_lock = SRWLOCK_INIT;
static Ref **ring_table;               /* RING_CHUNKS of them: each page's copy in the newest frame */
static u64 ring_chunks;
static u8 **ring_blocks;
static u32 ring_nblocks, ring_capblocks, ring_next = 1;   /* (ring_next: the first copy never handed out) */
static Ref *ring_spare;                /* copies given back, for the next ones */
static u32 ring_nspare, ring_capspare;
static RingFrame *ring_frames;         /* ring_frames[(ring_first + i) % ring_depth], i < ring_held: the oldest first */
static int ring_depth, ring_first, ring_held;
static u64 ring_numbered;              /* frames saved so far */
static Ranges ring_now, ring_newest;   /* the writable regions now, and at the newest frame */
static void **ring_found;              /* the write watch's answers */
static u32 *ring_pend;                 /* (restore) the pages to write back */
static u32 ring_npend, ring_cappend;
static RingStats ring_st;

static void *ring_grow(void *p, u32 *cap, u32 need, size_t each) {
    if (need <= *cap) return p;
    u32 to = *cap ? *cap : 64;
    while (to < need) to *= 2;
    if (!(p = realloc(p, (size_t)to * each))) rt_fatal("rollback ring: out of memory (%llu MiB held)", (unsigned long long)(ring_st.bytes >> 20));
    *cap = to;
    return p;
}

/* ---- page copies -------------------------------------------------------------------------- */
static u8 *ring_copy(Ref r) { return ring_blocks[(r - 1) / RING_BLOCK] + (size_t)((r - 1) % RING_BLOCK) * RING_PAGE; }

static Ref ring_new_copy(void) {
    ring_st.copies++;
    if (ring_nspare) return ring_spare[--ring_nspare];
    if ((ring_next - 1) / RING_BLOCK == ring_nblocks) {
        ring_blocks = ring_grow(ring_blocks, &ring_capblocks, ring_nblocks + 1, sizeof *ring_blocks);
        if (!(ring_blocks[ring_nblocks] = malloc(RING_BLOCK * RING_PAGE)))
            rt_fatal("rollback ring: out of memory (%llu MiB held)", (unsigned long long)(ring_st.bytes >> 20));
        ring_nblocks++;
    }
    return ring_next++;
}

static void ring_give_back(Ref r) {
    if (!r) return;
    ring_st.copies--;
    ring_spare = ring_grow(ring_spare, &ring_capspare, ring_nspare + 1, sizeof *ring_spare);
    ring_spare[ring_nspare++] = r;
}

static Ref *ring_slot(u32 page) {   /* (made if need be) */
    Ref **c = &ring_table[page / RING_CHUNK];
    if (!*c) {
        if (!(*c = calloc(RING_CHUNK, sizeof **c))) rt_fatal("rollback ring: out of memory (%llu MiB held)", (unsigned long long)(ring_st.bytes >> 20));
        ring_chunks++;
    }
    return &(*c)[page % RING_CHUNK];
}

static Ref ring_peek(u32 page) {   /* (none made) */
    const Ref *c = ring_table[page / RING_CHUNK];
    return c ? c[page % RING_CHUNK] : 0;
}

static int ring_zeros(const u8 *p) {
    const u64 *w = (const u64 *)p;
    for (u32 i = 0; i < RING_PAGE / 8; i++)
        if (w[i]) return 0;
    return 1;
}

/* Whether a page differs from its copy in the newest frame. */
static int ring_differs(u32 page, Ref r) {
    const u8 *live = G2H((u64)page * RING_PAGE);
    return r ? memcmp(live, ring_copy(r), RING_PAGE) != 0 : !ring_zeros(live);
}

/* ---- frames ------------------------------------------------------------------------------- */
static RingFrame *ring_frame(int i) { return &ring_frames[(ring_first + i) % ring_depth]; }

static void ring_log(RingFrame *f, u32 page, Ref had) {
    f->undo = ring_grow(f->undo, &f->capundo, f->nundo + 1, sizeof *f->undo);
    f->undo[f->nundo++] = (Undo){page, had};
}

/* A page as it is now against its copy in the newest frame: if it changed, a new copy (none for
 * zeros), and the old one into f's log - or given back, for the first frame, which has none. */
static void ring_check(RingFrame *f, u32 page, int log) {
    Ref had = ring_peek(page);
    ring_st.checked++;
    if (!ring_differs(page, had)) return;
    Ref *slot = ring_slot(page);
    const u8 *live = G2H((u64)page * RING_PAGE);
    Ref r = 0;
    if (!had || !ring_zeros(live)) {   /* (had none: it is not zeros, or it would not differ) */
        r = ring_new_copy();
        memcpy(ring_copy(r), live, RING_PAGE);
    }
    *slot = r;
    ring_st.changed++;
    if (log) ring_log(f, page, had);
    else ring_give_back(had);
}

/* A page the memory map no longer has: zeros in this frame, its copy into the log. */
static void ring_gone(RingFrame *f, u32 page) {
    if (!ring_peek(page)) return;
    Ref *slot = ring_slot(page);
    ring_log(f, page, *slot);
    *slot = 0;
    ring_st.changed++;
}

/* The oldest frame dropped: the next one's log held its copies, which nothing needs now. */
static void ring_drop_oldest(void) {
    if (ring_held > 1) {
        RingFrame *next = ring_frame(1);
        for (u32 i = 0; i < next->nundo; i++) ring_give_back(next->undo[i].had);
        next->nundo = 0;
    }
    ring_first = (ring_first + 1) % ring_depth;
    ring_held--;
}

/* ---- the memory map ----------------------------------------------------------------------- */
static void ring_read_map(Ranges *to) {
    u32 n = kernel_writable_ranges(to->r, to->cap);
    if (n > to->cap) {
        to->r = ring_grow(to->r, &to->cap, n, sizeof *to->r);
        n = kernel_writable_ranges(to->r, to->cap);
    }
    to->n = n;
    for (u32 i = 0; i < n; i++) {   /* (whole pages) */
        to->r[i].lo &= ~(RING_PAGE - 1);
        to->r[i].hi = (to->r[i].hi + RING_PAGE - 1) & ~(RING_PAGE - 1);
    }
}

static void ring_keep_map(void) {
    ring_newest.r = ring_grow(ring_newest.r, &ring_newest.cap, ring_now.n, sizeof *ring_newest.r);
    memcpy(ring_newest.r, ring_now.r, ring_now.n * sizeof *ring_now.r);
    ring_newest.n = ring_now.n;
}

typedef void RingPage(RingFrame *f, u32 page);

/* Each page that a's ranges have and b's do not (both sorted and apart). */
static void ring_each_missing(const Ranges *a, const Ranges *b, RingPage *fn, RingFrame *f) {
    u32 j = 0;
    for (u32 i = 0; i < a->n; i++) {
        u64 at = a->r[i].lo, hi = a->r[i].hi;
        while (at < hi) {
            while (j < b->n && b->r[j].hi <= at) j++;
            u64 to = j == b->n || b->r[j].lo >= hi ? hi : b->r[j].lo;
            if (to > at) {
                for (; at < to; at += RING_PAGE) fn(f, (u32)(at / RING_PAGE));
            } else {
                at = b->r[j].hi < hi ? b->r[j].hi : hi;   /* (b has these) */
            }
        }
    }
}

static int ring_mapped(const Ranges *m, u32 page) {
    u64 a = (u64)page * RING_PAGE;
    u32 lo = 0, hi = m->n;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (m->r[mid].hi <= a) lo = mid + 1;
        else hi = mid;
    }
    return lo < m->n && m->r[lo].lo <= a;
}

/* The pages of a watched range written since the ring last asked, their bits taken. */
static void ring_each_written(const KernelRange *k, RingPage *fn, RingFrame *f) {
    for (u64 at = k->lo; at < k->hi;) {
        ULONG_PTR n = RING_ASK;
        ULONG granularity;
        if (kernel_write_watch(WATCH_RING, WRITE_WATCH_FLAG_RESET, G2H(at), k->hi - at, ring_found, &n, &granularity) != 0 || !n) return;
        ring_st.written += n;
        for (ULONG_PTR i = 0; i < n; i++) fn(f, (u32)(H2G(ring_found[i]) / RING_PAGE));
        if (n < RING_ASK) return;
        at = H2G(ring_found[n - 1]) + RING_PAGE;
    }
}

static void ring_check_logged(RingFrame *f, u32 page) { ring_check(f, page, 1); }
static void ring_check_first(RingFrame *f, u32 page) { ring_check(f, page, 0); }
static void ring_ignore(RingFrame *f, u32 page) { (void)f; (void)page; }

/* ---- the ring ----------------------------------------------------------------------------- */
int rollback_ring_open(int frames) {
    AcquireSRWLockExclusive(&ring_lock);
    int ok = !ring_frames && frames >= 2;
    if (ok) {
        ring_frames = calloc((size_t)frames, sizeof *ring_frames);
        ring_found = malloc(RING_ASK * sizeof *ring_found);
        ring_table = calloc(RING_CHUNKS, sizeof *ring_table);
        if (!ring_frames || !ring_found || !ring_table) rt_fatal("rollback ring: out of memory");
        ring_depth = frames;
    }
    ReleaseSRWLockExclusive(&ring_lock);
    return ok;
}

static u64 ring_bytes(void) {
    u64 b = (u64)ring_nblocks * RING_BLOCK * RING_PAGE + ring_chunks * RING_CHUNK * sizeof(Ref) + RING_CHUNKS * sizeof *ring_table +
            (u64)ring_capspare * sizeof(Ref) + (u64)ring_cappend * sizeof(u32) + (u64)(ring_now.cap + ring_newest.cap) * sizeof(KernelRange) +
            (u64)ring_depth * sizeof(RingFrame) + RING_ASK * sizeof(void *);
    for (int i = 0; i < ring_depth; i++) b += (u64)ring_frames[i].capundo * sizeof(Undo);
    return b;
}

u64 rollback_ring_save(void) {
    AcquireSRWLockExclusive(&ring_lock);
    if (!ring_frames) { ReleaseSRWLockExclusive(&ring_lock); return 0; }
    u64 t0 = rt_clock_ns();
    kernel_memory_hold();
    ring_read_map(&ring_now);
    if (ring_held == ring_depth) ring_drop_oldest();
    RingFrame *f = ring_frame(ring_held++);
    f->number = ++ring_numbered;
    f->nundo = 0;
    ring_st.written = ring_st.checked = ring_st.changed = 0;
    if (f->number == 1) {   /* every page, the write watch's news until now taken and dropped */
        for (u32 i = 0; i < ring_now.n; i++) {
            if (ring_now.r[i].watched) ring_each_written(&ring_now.r[i], ring_ignore, f);
            for (u64 at = ring_now.r[i].lo; at < ring_now.r[i].hi; at += RING_PAGE) ring_check_first(f, (u32)(at / RING_PAGE));
        }
    } else {
        ring_each_missing(&ring_newest, &ring_now, ring_gone, f);
        ring_each_missing(&ring_now, &ring_newest, ring_check_logged, f);
        for (u32 i = 0; i < ring_now.n; i++) {
            if (ring_now.r[i].watched) ring_each_written(&ring_now.r[i], ring_check_logged, f);
            else
                for (u64 at = ring_now.r[i].lo; at < ring_now.r[i].hi; at += RING_PAGE) ring_check_logged(f, (u32)(at / RING_PAGE));
        }
    }
    ring_keep_map();
    kernel_memory_release();
    ring_st.saves++;
    ring_st.frames = (u64)ring_held;
    ring_st.pages = 0;
    for (u32 i = 0; i < ring_now.n; i++) ring_st.pages += (ring_now.r[i].hi - ring_now.r[i].lo) / RING_PAGE;
    ring_st.bytes = ring_bytes();
    ring_st.save_ns = rt_clock_ns() - t0;
    u64 n = f->number;
    ReleaseSRWLockExclusive(&ring_lock);
    return n;
}

static void ring_pend_page(RingFrame *f, u32 page) {
    (void)f;
    ring_pend = ring_grow(ring_pend, &ring_cappend, ring_npend + 1, sizeof *ring_pend);
    ring_pend[ring_npend++] = page;
}

static void ring_pend_if_changed(RingFrame *f, u32 page) {
    if (ring_differs(page, ring_peek(page))) ring_pend_page(f, page);
}

static int ring_by_page(const void *a, const void *b) {
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

int rollback_ring_restore(u64 frame) {
    AcquireSRWLockExclusive(&ring_lock);
    if (!ring_held || frame < ring_frame(0)->number || frame > ring_frame(ring_held - 1)->number) {
        ReleaseSRWLockExclusive(&ring_lock);
        return 0;
    }
    kernel_memory_hold();
    ring_read_map(&ring_now);
    ring_npend = 0;
    /* what changed since the newest frame */
    ring_each_missing(&ring_newest, &ring_now, ring_pend_page, NULL);
    ring_each_missing(&ring_now, &ring_newest, ring_pend_page, NULL);
    for (u32 i = 0; i < ring_now.n; i++) {
        if (ring_now.r[i].watched) ring_each_written(&ring_now.r[i], ring_pend_page, NULL);
        else
            for (u64 at = ring_now.r[i].lo; at < ring_now.r[i].hi; at += RING_PAGE) ring_pend_if_changed(NULL, (u32)(at / RING_PAGE));
    }
    /* the newer frames undone, the newest first, each log from its last entry back (a page
     * logged twice in a frame - written again while it was saved - ends at its first copy) */
    while (ring_frame(ring_held - 1)->number > frame) {
        RingFrame *f = ring_frame(ring_held - 1);
        for (u32 i = f->nundo; i-- > 0;) {
            Ref *slot = ring_slot(f->undo[i].page);
            ring_give_back(*slot);
            *slot = f->undo[i].had;
            ring_pend_page(NULL, f->undo[i].page);
        }
        f->nundo = 0;
        ring_held--;
    }
    /* each page once: back as the frame had it, where it is mapped */
    qsort(ring_pend, ring_npend, sizeof *ring_pend, ring_by_page);
    ring_st.put_back = ring_st.unmapped = 0;
    for (u32 i = 0; i < ring_npend; i++) {
        u32 page = ring_pend[i];
        if (i && page == ring_pend[i - 1]) continue;
        if (!ring_mapped(&ring_now, page)) {   /* (forgotten: the newest frame has what is mapped now) */
            if (ring_peek(page)) {
                Ref *slot = ring_slot(page);
                ring_st.unmapped++;
                ring_give_back(*slot);
                *slot = 0;
            }
            continue;
        }
        Ref *slot = ring_slot(page);
        if (!ring_differs(page, *slot)) continue;
        u8 *live = G2H((u64)page * RING_PAGE);
        if (*slot) memcpy(live, ring_copy(*slot), RING_PAGE);
        else memset(live, 0, RING_PAGE);
        ring_st.put_back++;
    }
    ring_keep_map();
    kernel_memory_release();
    ring_st.restores++;
    ring_st.frames = (u64)ring_held;
    ring_st.bytes = ring_bytes();
    ReleaseSRWLockExclusive(&ring_lock);
    return 1;
}

u64 rollback_ring_oldest(void) {
    AcquireSRWLockShared(&ring_lock);
    u64 n = ring_held ? ring_frame(0)->number : 0;
    ReleaseSRWLockShared(&ring_lock);
    return n;
}

u64 rollback_ring_newest(void) {
    AcquireSRWLockShared(&ring_lock);
    u64 n = ring_held ? ring_frame(ring_held - 1)->number : 0;
    ReleaseSRWLockShared(&ring_lock);
    return n;
}

void rollback_ring_stats(RingStats *out) {
    AcquireSRWLockShared(&ring_lock);
    *out = ring_st;
    ReleaseSRWLockShared(&ring_lock);
}

/* ---- in a run: a save at each frame end, and its cost --------------------------------------- */
#define RING_REPORT 300   /* saves a report line covers: 5 s at 60 frames a second */
static int ring_switch_frames;
static struct { u64 saves, written, written_max, changed, changed_max, checked, ns, ns_max; } ring_window;

static void ring_at_frame_end(Cpu *c, const u64 args[8], void *unused) {
    (void)c; (void)args; (void)unused;
    static int started;   /* (the frame end runs on one thread) */
    if (!started) {   /* (here, not at start-up: by now stderr is where SSBU_LOG puts it) */
        started = 1;
        rollback_ring_open(ring_switch_frames);   /* (0 if open already: tools/dev/ringtest.c) */
        fprintf(stderr, "[ring] SSBU_ROLLBACK_RING: the guest's writable memory saved at each frame end, the last %d frames kept; "
                        "never put back in a run (that needs the game's threads stopped first)\n", ring_switch_frames);
    }
    u64 n = rollback_ring_save();
    RingStats s;
    rollback_ring_stats(&s);
    if (n == 1) {
        fprintf(stderr, "[ring] frame 1: %llu writable pages (%.1f MiB), %llu of them not zeros, saved in %.1f ms; %.1f MiB held\n",
                (unsigned long long)s.pages, (double)s.pages * RING_PAGE / (1 << 20), (unsigned long long)s.changed, (double)s.save_ns / 1e6,
                (double)s.bytes / (1 << 20));
        return;
    }
    ring_window.saves++;
    ring_window.written += s.written;
    ring_window.changed += s.changed;
    ring_window.checked += s.checked;
    ring_window.ns += s.save_ns;
    if (s.written > ring_window.written_max) ring_window.written_max = s.written;
    if (s.changed > ring_window.changed_max) ring_window.changed_max = s.changed;
    if (s.save_ns > ring_window.ns_max) ring_window.ns_max = s.save_ns;
    if (ring_window.saves < RING_REPORT) return;
    double k = (double)ring_window.saves;
    fprintf(stderr, "[ring] frames %llu-%llu: a frame %.0f pages written (at most %llu), %.0f changed (at most %llu), %.0f compared; "
                    "a save %.2f ms (at most %.2f); %.1f MiB held for %llu frames of %llu writable pages\n",
            (unsigned long long)(n - ring_window.saves + 1), (unsigned long long)n, ring_window.written / k, (unsigned long long)ring_window.written_max,
            ring_window.changed / k, (unsigned long long)ring_window.changed_max, ring_window.checked / k, ring_window.ns / k / 1e6,
            (double)ring_window.ns_max / 1e6, (double)s.bytes / (1 << 20), (unsigned long long)s.frames, (unsigned long long)s.pages);
    memset(&ring_window, 0, sizeof ring_window);
}

/* (before main: registered only with the switch on, so with it off the frame end has nothing of
 * the ring's to run) */
__attribute__((constructor)) static void ring_switch(void) {
    ring_switch_frames = kernel_ring_frames();
    if (ring_switch_frames) frame_hook_add(FRAME_HOOK_END, FRAME_ORDER_ROLLBACK, NULL, ring_at_frame_end, NULL);
}
