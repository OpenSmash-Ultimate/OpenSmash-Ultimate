/* Rollback (SSBU_RB): the fight state map by OWNER - which allocation a page a fight writes belongs to, and the
 * allocations left out of the savestates because they are presentation only (docs/TECHNICAL.md, "The fight's state
 * map"). hle_rb_state.c asks the write watch about, compares, copies and loads back every page a fight writes (the
 * hot pages, ~5,000-7,000 a save); a page of an allocation that only draws, plays or shows (render lists, effect
 * vertex buffers, the HUD's) is no fight state: leaving it out of the states makes every save and load cheaper.
 *
 * Who owns a page is known from the game's allocator: its jemalloc entry points in main (13.0.5: posix_memalign
 * 0x710392e1f0, aligned_alloc 0x710392e290 - malloc and operator new reach it -, realloc 0x710392e330, free
 * 0x710392eb40) are hooked (rt_hook_function) when the game mounts its RomFS, before the scenes allocate, and every
 * block is tagged with its SITE: the caller's return address and the caller's caller's (the frame record at
 * [x29 + 8]), as R4 named them (plan R4_state_map.md; 13.0.4 there).
 *
 * SSBU_RB_MAP_CENSUS=<save>[,<saves>[,<file>]] (diagnostic, off): every block is tracked; from that fight save on,
 * for <saves> saves (default 300 then 600), each hot alias page is counted (hot, steady, written); then every page is
 * given to the live block covering most of it and the sites are listed with their pages (file default
 * build\rb_map_census.txt, the top of the list in the log). The map this file's list comes from.
 *
 * Left out (the exclusion): the live blocks of the sites in the list - built in (rb_map_sites below, each one proven
 * by the replay check and pairs) or SSBU_RB_MAP_SKIP=<file> (lines "<site> [<caller's caller>]", hex; 0: none). Every
 * page wholly inside such a block is left out of the fight's saves and loads (hle_rb_state.c's sm_skip: not asked
 * about, not compared, not copied, not put back). The live blocks of those sites are host state that follows the
 * game's allocator, so each savestate keeps their list (rb_map_save / rb_map_load): after a load the left-out set is
 * that state's, as jemalloc's own bookkeeping (guest memory, in the state) is. A block joins the set only once every
 * state kept is younger than it (map_oldest below). A page that leaves the set (its block freed) is copied into the
 * shadow at the next save (hle_rb_state.c, as for SSBU_RB_SKIP's ranges).
 * SSBU_RB_MAP=0: no hooks, nothing left out (the fallback: every page in, as before). */
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <windows.h>

#include "host.h"

#define JE_POSIX_MEMALIGN 0x710392e1f0ull
#define JE_ALIGNED_ALLOC  0x710392e290ull
#define JE_REALLOC        0x710392e330ull
#define JE_FREE           0x710392eb40ull

/* ---- sites ---- */
typedef struct Site { u64 lr, lr2; } Site;
#define SITE_MAX 65536
static Site sites[SITE_MAX];
static u32 nsites;
static u32 *site_hash;                  /* (lr, lr2) -> index + 1, open addressing, 2 * SITE_MAX */
static SRWLOCK site_lock = SRWLOCK_INIT;
static u32 site_of(u64 lr, u64 lr2) {   /* its index, made if new; SITE_MAX - 1 when full */
    u64 h = (lr * 0x9e3779b97f4a7c15ull) ^ (lr2 * 0xc2b2ae3d27d4eb4full);
    u32 m = 2 * SITE_MAX - 1, i = (u32)(h >> 40) & m;
    AcquireSRWLockShared(&site_lock);
    for (u32 k = i;; k = (k + 1) & m) {
        u32 v = site_hash[k];
        if (!v) break;
        if (sites[v - 1].lr == lr && sites[v - 1].lr2 == lr2) { ReleaseSRWLockShared(&site_lock); return v - 1; }
    }
    ReleaseSRWLockShared(&site_lock);
    AcquireSRWLockExclusive(&site_lock);
    u32 r = SITE_MAX - 1;
    for (u32 k = i;; k = (k + 1) & m) {
        u32 v = site_hash[k];
        if (v && sites[v - 1].lr == lr && sites[v - 1].lr2 == lr2) { r = v - 1; break; }
        if (!v) { if (nsites < SITE_MAX - 1) { sites[nsites] = (Site){lr, lr2}; site_hash[k] = ++nsites; r = nsites - 1; } break; }
    }
    ReleaseSRWLockExclusive(&site_lock);
    return r;
}

/* ---- block tables: address -> size and site (linear probing, deletion by backward shift) ---- */
typedef struct Blk { u64 p; u64 size : 40, site : 24; long long born; } Blk;   /* born: rb_map_save's clock at the allocation */
typedef struct Tab { Blk *b; u64 cap, n; SRWLOCK lock; long long full; } Tab;
static u64 tab_slot(const Tab *t, u64 p) { return ((p >> 4) * 0x9e3779b97f4a7c15ull >> 20) & (t->cap - 1); }
static int tab_init(Tab *t, u64 cap) {
    t->b = VirtualAlloc(NULL, sizeof(Blk) * cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    t->cap = cap; t->n = 0; InitializeSRWLock(&t->lock);
    return t->b != NULL;
}
static void tab_put(Tab *t, u64 p, u64 size, u32 site, long long born) {   /* (lock held) */
    if (t->n * 4 >= t->cap * 3) { t->full++; return; }
    for (u64 k = tab_slot(t, p);; k = (k + 1) & (t->cap - 1)) {
        if (t->b[k].p == p) { t->b[k].size = size; t->b[k].site = site; t->b[k].born = born; return; }
        if (!t->b[k].p) { t->b[k].p = p; t->b[k].size = size; t->b[k].site = site; t->b[k].born = born; t->n++; return; }
    }
}
static int tab_take(Tab *t, u64 p, Blk *out) {   /* (lock held) removed; 1 if it was there */
    u64 m = t->cap - 1, k = tab_slot(t, p);
    for (;; k = (k + 1) & m) { if (!t->b[k].p) return 0; if (t->b[k].p == p) break; }
    if (out) *out = t->b[k];
    for (u64 j = (k + 1) & m;; j = (j + 1) & m) {   /* (backward shift: no tombstones) */
        if (!t->b[j].p) break;
        u64 h = tab_slot(t, t->b[j].p);
        if ((j > k && (h <= k || h > j)) || (j < k && (h <= k && h > j))) { t->b[k] = t->b[j]; k = j; }
    }
    t->b[k].p = 0; t->n--;
    return 1;
}

static int map_on = -1;                 /* hooks installed */
static int census_on, census_from = 300, census_len = 600;
static char census_file[512] = "build\\rb_map_census.txt";
static Tab all;                         /* census: every live block */
static Tab out;                         /* the live blocks of the left-out sites */
static u8 *site_out;                    /* per site: left out */
static Site *want; static u32 nwant;    /* the list of sites to leave out (resolved to indices as they appear) */
volatile long rb_map_gen;               /* (hle_rb_state.c) the left-out set changed: it is built again */
/* A block is left out only once every state kept was saved after it was made (born < map_oldest, the clock of the
 * oldest state kept): its pages are then the block's - presentation - in all of them, so a load to any of them may
 * leave them as they are. A block made since (young) is saved and loaded as any page: else a rollback to a state from
 * before it, when its pages may have been fight state (a block freed there and the range given to this one), would
 * not put them back. A freed block leaves the set at once. */
static long long map_clock, map_oldest, young_min = LLONG_MAX;   /* (out.lock) */
static long long n_alloc, n_free, n_out_alloc, n_out_free;
static u64 orig_pm, orig_aa, orig_ra, orig_fr;

/* The built-in list: allocation sites proven presentation only. R4 (plan R4_state_map.md, 13.0.4, the marked ring:
 * "rt_big3", IDENTICAL on 12 matches of 11 fighters) found them; here at their 13.0.5 addresses, with the census's
 * pages and the pairs that passed with them left out (docs/TECHNICAL.md, "The fight's state map"). The GPU's memory
 * pools (nvmap: NVN's sync objects and their semaphores live there) are NOT in it: left out, a rollback's main thread
 * spun for good in nvnSyncWait (sdk+0x427010), as SSBU_RB_SKIP_GPU=1 did. */
static const Site rb_map_sites[] = {
    {0x7103816ef4ull, 0},                 /* the graphics module's 16 MB buffers (posix_memalign; 13.0.4 0x7103816944) */
    {0x710355a3b8ull, 0x71000a3fd8ull},   /* an 8 MB block (13.0.4 0x7103559e08 / 0x71000a3fd8) */
};

static int listed(u64 lr, u64 lr2) {
    for (u32 i = 0; i < nwant; i++) if (want[i].lr == lr && (!want[i].lr2 || want[i].lr2 == lr2)) return 1;
    return 0;
}
/* fp + 8 readable: x29 is the caller's frame record only by convention (a function without one may hold anything
 * there), so the committed region around it is asked once and kept per thread (a thread's stack: one region). */
static __declspec(thread) u64 rd_lo, rd_hi;
static int readable8(u64 a) {
    if (a >= rd_lo && a + 8 <= rd_hi) return 1;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(G2H(a), &mi, sizeof mi) || mi.State != MEM_COMMIT || !mi.Protect || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    u64 lo = (u64)(uintptr_t)mi.BaseAddress - (u64)(uintptr_t)G2H(0), hi = lo + mi.RegionSize;
    if (a + 8 > hi) return 0;
    rd_lo = lo; rd_hi = hi;
    return 1;
}
static u32 site_now(Cpu *c) {
    u64 lr = c->x[30], fp = c->x[29], lr2 = 0;
    /* (a frame record on a guest stack: the stack area, or a stack the game made in its heap) */
    if (!(fp & 7) && ((fp >= STACK_BASE && fp < KERNEL_BASE + 0x1000000) || (fp >= HEAP_BASE && fp < HEAP_BASE + HEAP_LIMIT)) && readable8(fp + 8))
        lr2 = ld64(fp + 8);
    u32 s = site_of(lr, lr2);
    if (s < SITE_MAX - 1 && site_out[s] == 0) site_out[s] = listed(lr, lr2) ? 2 : 1;   /* (1 kept, 2 left out) */
    return s;
}
static void noted(u64 p, u64 size, u32 s) {
    if (!p) return;
    InterlockedIncrement64(&n_alloc);
    if (census_on) { AcquireSRWLockExclusive(&all.lock); tab_put(&all, p, size, s, 0); ReleaseSRWLockExclusive(&all.lock); }
    if (site_out[s] == 2) {   /* (young: not left out yet, the set is the same) */
        AcquireSRWLockExclusive(&out.lock);
        tab_put(&out, p, size, s, map_clock);
        if (map_clock < young_min) young_min = map_clock;
        ReleaseSRWLockExclusive(&out.lock);
        InterlockedIncrement64(&n_out_alloc);
    }
}
static void gone(u64 p) {
    if (!p) return;
    InterlockedIncrement64(&n_free);
    if (census_on) { AcquireSRWLockExclusive(&all.lock); tab_take(&all, p, NULL); ReleaseSRWLockExclusive(&all.lock); }
    if (out.n) {
        Blk b;
        AcquireSRWLockExclusive(&out.lock);
        int was = tab_take(&out, p, &b), left = was && b.born < map_oldest;
        ReleaseSRWLockExclusive(&out.lock);
        if (was) InterlockedIncrement64(&n_out_free);
        if (left) InterlockedIncrement(&rb_map_gen);
    }
}
static void hook_aa(Cpu *c) {   /* aligned_alloc(align, size) */
    u32 s = site_now(c); u64 size = c->x[1];
    rt_call(c, orig_aa);
    noted(c->x[0], size, s);
}
static void hook_pm(Cpu *c) {   /* posix_memalign(&p, align, size) */
    u32 s = site_now(c); u64 at = c->x[0], size = c->x[2];
    rt_call(c, orig_pm);
    if ((u32)c->x[0] == 0 && at) noted(ld64(at), size, s);
}
static void hook_ra(Cpu *c) {   /* realloc(p, size) */
    u32 s = site_now(c); u64 p = c->x[0], size = c->x[1];
    rt_call(c, orig_ra);
    u64 r = c->x[0];
    if (p && (r || !size)) gone(p);
    if (r && size) noted(r, size, s);
}
static void hook_fr(Cpu *c) {   /* free(p) */
    gone(c->x[0]);
    rt_call(c, orig_fr);
}

static void read_list(void) {
    const char *f = getenv("SSBU_RB_MAP_SKIP");
    static Site buf[4096];
    nwant = 0;
    if (f && *f && strcmp(f, "0")) {
        FILE *h = fopen(f, "r");
        if (!h) { fprintf(stderr, "[rb] state map: site list %s not found: nothing left out\n", f); want = buf; return; }
        char ln[256];
        unsigned long long a, b;
        while (fgets(ln, sizeof ln, h) && nwant < 4096) {
            if (ln[0] == '#') continue;
            int k = sscanf(ln, "%llx %llx", &a, &b);
            if (k >= 1 && a) buf[nwant++] = (Site){a, k > 1 ? b : 0};
        }
        fclose(h);
    } else if (!(f && !strcmp(f, "0")))
        for (u32 i = 0; i < sizeof rb_map_sites / sizeof *rb_map_sites; i++) if (rb_map_sites[i].lr) buf[nwant++] = rb_map_sites[i];
    want = buf;
}

/* (hle_fs.c, the RomFS mounted: the modules are loaded, the scenes not yet made) */
void rb_map_install(void) {
    extern int rb_on;   /* (hle_rb_core.c) */
    if (map_on >= 0) return;
    map_on = 0;
    const char *e = getenv("SSBU_RB_MAP"), *cz = getenv("SSBU_RB_MAP_CENSUS");
    if (!rb_on || (e && *e == '0')) return;
    if (cz && *cz && *cz != '0') {
        char file[512] = "";
        int n = sscanf(cz, "%d,%d,%511s", &census_from, &census_len, file);
        if (n >= 3 && file[0]) snprintf(census_file, sizeof census_file, "%s", file);
        if (census_len <= 0) census_len = 600;
        census_on = 1;
    }
    read_list();
    if (!nwant && !census_on) { fprintf(stderr, "[rb] state map by owner: no site left out (SSBU_RB_MAP_SKIP / the built-in list empty): no hooks\n"); return; }
    site_hash = calloc(2 * SITE_MAX, sizeof *site_hash);
    site_out = calloc(SITE_MAX, 1);
    if (!site_hash || !site_out || !tab_init(&out, 1u << 16) || (census_on && !tab_init(&all, 1u << 23))) { fprintf(stderr, "[rb] state map by owner: out of memory: off\n"); census_on = 0; return; }
    orig_aa = rt_hook_function(JE_ALIGNED_ALLOC, rt_native(hook_aa, "rollback map: aligned_alloc"));
    orig_pm = rt_hook_function(JE_POSIX_MEMALIGN, rt_native(hook_pm, "rollback map: posix_memalign"));
    orig_ra = rt_hook_function(JE_REALLOC, rt_native(hook_ra, "rollback map: realloc"));
    orig_fr = rt_hook_function(JE_FREE, rt_native(hook_fr, "rollback map: free"));
    map_on = orig_aa && orig_pm && orig_ra && orig_fr;
    fprintf(stderr, "[rb] state map by owner: allocator hooks %s; %u sites left out (%s)%s\n", map_on ? "on" : "MISSED (nothing left out)", nwant,
            getenv("SSBU_RB_MAP_SKIP") ? getenv("SSBU_RB_MAP_SKIP") : "the built-in list", census_on ? "; census on" : "");
    if (!map_on) { nwant = 0; census_on = 0; }
}

/* (hle_rb_state.c, the left-out set built again) Every alias page wholly inside a live left-out block: its bit set in
 * bits (one a page from ALIAS_BASE). The pages. */
u64 rb_map_skip_pages(u64 *bits) {
    if (map_on <= 0 || !out.n) return 0;
    u64 n = 0;
    AcquireSRWLockShared(&out.lock);
    for (u64 i = 0; i < out.cap; i++) {
        const Blk *b = &out.b[i];
        if (!b->p || b->born >= map_oldest) continue;   /* (young) */
        u64 lo = (b->p + 0xFFF) & ~0xFFFull, hi = (b->p + b->size) & ~0xFFFull;
        if (lo < ALIAS_BASE || hi > ALIAS_BASE + ALIAS_SIZE) continue;
        for (u64 g = lo; g < hi; g += 0x1000) { u64 a = (g - ALIAS_BASE) >> 12; bits[a >> 6] |= 1ull << (a & 63); n++; }
    }
    ReleaseSRWLockShared(&out.lock);
    return n;
}
int rb_map_active(void) { return map_on > 0 && nwant > 0; }

/* The left-out blocks' list, kept with each savestate (every guest thread held), with the save's clock. oldest: the
 * oldest state kept besides this one (its list; NULL: none), whose clock says which blocks may be left out. */
typedef struct MapSave { long long stamp; u64 n; Blk b[]; } MapSave;
static u64 set_sum(void) {   /* (out.lock held) the left-out set's fingerprint: its old blocks */
    u64 h = 0, k = 0;
    for (u64 i = 0; i < out.cap; i++) {
        const Blk *b = &out.b[i];
        if (b->p && b->born < map_oldest) { h += (b->p * 0x9e3779b97f4a7c15ull) ^ ((u64)b->size * 0xc2b2ae3d27d4eb4full); k++; }
    }
    return h ^ (k << 48);
}
void *rb_map_save(const void *oldest) {
    if (map_on <= 0 || !nwant) return NULL;
    AcquireSRWLockExclusive(&out.lock);
    long long stamp = ++map_clock, old = oldest ? ((const MapSave *)oldest)->stamp : stamp;
    if (old > map_oldest) {
        map_oldest = old;
        if (young_min < map_oldest) {   /* blocks old enough now: the set grows */
            young_min = LLONG_MAX;
            for (u64 i = 0; i < out.cap; i++) if (out.b[i].p && out.b[i].born >= map_oldest && out.b[i].born < young_min) young_min = out.b[i].born;
            InterlockedIncrement(&rb_map_gen);
        }
    }
    MapSave *s = malloc(sizeof *s + sizeof(Blk) * (out.n ? out.n : 1));
    if (s) { s->stamp = stamp; s->n = 0; for (u64 i = 0; i < out.cap; i++) if (out.b[i].p) s->b[s->n++] = out.b[i]; }
    ReleaseSRWLockExclusive(&out.lock);
    return s;
}
void rb_map_load(const void *p) {
    const MapSave *s = p;
    if (map_on <= 0 || !nwant || !s) return;
    AcquireSRWLockExclusive(&out.lock);
    u64 before = set_sum();
    memset(out.b, 0, sizeof(Blk) * out.cap); out.n = 0;
    young_min = LLONG_MAX;
    for (u64 i = 0; i < s->n; i++) {
        tab_put(&out, s->b[i].p, s->b[i].size, (u32)s->b[i].site, s->b[i].born);
        if (s->b[i].born >= map_oldest && s->b[i].born < young_min) young_min = s->b[i].born;
    }
    u64 after = set_sum();
    ReleaseSRWLockExclusive(&out.lock);
    if (after != before) InterlockedIncrement(&rb_map_gen);   /* (most loads: the same set, nothing to build again) */
}
void rb_map_free(void *p) { free(p); }

/* ---- the census ---- */
static u16 *c_hot, *c_steady, *c_wr;   /* per alias page: saves hot, steady, written (copied: changed) */
static u8 *c_gpu, *c_out;
static long long c_saves;
static int c_done;
/* (hle_rb_state.c, each fight save's sync, before the streaks) fight save number; the hot page a with its flags */
int rb_map_census_want(long long fight_save) {
    if (!census_on || c_done || fight_save < census_from) return 0;
    if (!c_hot) {
        c_hot = VirtualAlloc(NULL, (ALIAS_SIZE >> 12) * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        c_steady = VirtualAlloc(NULL, (ALIAS_SIZE >> 12) * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        c_wr = VirtualAlloc(NULL, (ALIAS_SIZE >> 12) * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        c_gpu = VirtualAlloc(NULL, ALIAS_SIZE >> 12, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        c_out = VirtualAlloc(NULL, ALIAS_SIZE >> 12, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!c_hot || !c_steady || !c_wr || !c_gpu || !c_out) { census_on = 0; return 0; }
    }
    c_saves++;
    return 1;
}
void rb_map_census_page(u64 a, int steady, int written, int gpu, int left_out) {
    if (c_hot[a] < 0xFFFF) c_hot[a]++;
    if (steady && c_steady[a] < 0xFFFF) c_steady[a]++;
    if (written && c_wr[a] < 0xFFFF) c_wr[a]++;
    if (gpu) c_gpu[a] = 1;
    if (left_out) c_out[a] = 1;
}
typedef struct Agg { u32 site; u64 pages, gpu, out, hot, steady, wr, blocks, bytes; } Agg;
static int blk_cmp(const void *x, const void *y) { u64 a = ((const Blk *)x)->p, b = ((const Blk *)y)->p; return (a > b) - (a < b); }
static int agg_cmp(const void *x, const void *y) { const Agg *a = x, *b = y; return (a->hot < b->hot) - (a->hot > b->hot); }
/* (after the save that ends the window: every guest thread held) the report */
void rb_map_census_end(void) {
    if (!census_on || c_done || c_saves < census_len) return;
    c_done = 1;
    double t0 = (double)GetTickCount64();
    AcquireSRWLockShared(&all.lock);
    u64 nb = 0;
    Blk *bl = malloc(sizeof *bl * (all.n ? all.n : 1));
    for (u64 i = 0; bl && i < all.cap; i++) if (all.b[i].p) bl[nb++] = all.b[i];
    ReleaseSRWLockShared(&all.lock);
    if (!bl) return;
    qsort(bl, nb, sizeof *bl, blk_cmp);
    Agg *ag = calloc(SITE_MAX + 1, sizeof *ag);   /* (SITE_MAX: pages no live block covers) */
    for (u32 s = 0; s <= SITE_MAX; s++) ag[s].site = s;
    for (u64 i = 0; i < nb; i++) { u32 s = (u32)bl[i].site; ag[s].blocks++; ag[s].bytes += bl[i].size; }
    u64 total[4] = {0};
    u64 lo = 0;
    for (u64 a = 0; a < (ALIAS_SIZE >> 12); a++) {
        if (!c_hot[a]) continue;
        u64 g = ALIAS_BASE + (a << 12), ge = g + 0x1000;
        /* the live block covering most of the page */
        while (lo < nb && bl[lo].p + bl[lo].size <= g) lo++;   /* (blocks sorted by start; a block ending before g is past for good... */
        u64 best = 0; u32 bs = SITE_MAX;
        /* (...mostly: a long block starting before may still cover it) look back a little and forward over the page */
        u64 j = lo > 64 ? lo - 64 : 0;
        for (; j < nb && bl[j].p < ge; j++) {
            u64 s0 = bl[j].p > g ? bl[j].p : g, e0 = bl[j].p + bl[j].size < ge ? bl[j].p + bl[j].size : ge;
            if (e0 > s0 && e0 - s0 > best) { best = e0 - s0; bs = (u32)bl[j].site; }
        }
        if (bs == SITE_MAX) {   /* (a block starting far before: binary search for the last start <= g) */
            u64 l = 0, h = nb;
            while (l < h) { u64 m = (l + h) / 2; if (bl[m].p <= g) l = m + 1; else h = m; }
            if (l && bl[l - 1].p + bl[l - 1].size > g) bs = (u32)bl[l - 1].site;
        }
        Agg *x = &ag[bs];
        x->pages++; x->gpu += c_gpu[a]; x->out += c_out[a]; x->hot += c_hot[a]; x->steady += c_steady[a]; x->wr += c_wr[a];
        total[0]++; total[1] += c_hot[a]; total[2] += c_wr[a]; total[3] += c_gpu[a];
    }
    qsort(ag, SITE_MAX + 1, sizeof *ag, agg_cmp);
    FILE *f = fopen(census_file, "w");
    char d1[96], d2[96];
    fprintf(stderr, "[rb] state map census, %lld saves: %llu hot pages (%.0f a save, %.0f written a save; %llu in GPU memory); %llu live blocks of %u sites; file %s (%.0f ms)\n",
            c_saves, total[0], (double)total[1] / c_saves, (double)total[2] / c_saves, total[3], nb, nsites, census_file, (double)GetTickCount64() - t0);
    if (f) fprintf(f, "# site lr lr2 | pages hot a save | written a save | steady a save | distinct pages | GPU pages | left out | live blocks | MB | lr | lr2\n");
    for (u32 i = 0; i <= SITE_MAX; i++) {
        Agg *x = &ag[i];
        if (!x->pages) continue;
        u64 lr = x->site < SITE_MAX ? sites[x->site].lr : 0, lr2 = x->site < SITE_MAX ? sites[x->site].lr2 : 0;
        if (f) fprintf(f, "%llx %llx | %.1f | %.1f | %.1f | %llu | %llu | %llu | %llu | %.2f | %s | %s\n", lr, lr2, (double)x->hot / c_saves, (double)x->wr / c_saves,
                       (double)x->steady / c_saves, x->pages, x->gpu, x->out, x->blocks, x->bytes / 1048576.0,
                       lr ? rt_describe(lr, d1, sizeof d1) : "(no live block)", lr2 ? rt_describe(lr2, d2, sizeof d2) : "-");
        if (i < 40) fprintf(stderr, "[rb]   site %llx %llx: %.0f hot a save, %.0f written, %llu pages (%llu GPU, %llu left out), %llu blocks %.1f MB\n", lr, lr2,
                            (double)x->hot / c_saves, (double)x->wr / c_saves, x->pages, x->gpu, x->out, x->blocks, x->bytes / 1048576.0);
    }
    if (f) fclose(f);
    free(ag); free(bl);
}

/* (the fight's end line) */
void rb_map_report(void) {
    if (map_on <= 0) return;
    fprintf(stderr, "[rb] state map by owner: %lld blocks made, %lld freed; left out: %llu live blocks now (%lld made, %lld freed), %u sites known%s\n",
            n_alloc, n_free, out.n, n_out_alloc, n_out_free, nsites, out.full || all.full ? " (a block table was full: some blocks not tracked)" : "");
}
#endif   /* _WIN32 */
