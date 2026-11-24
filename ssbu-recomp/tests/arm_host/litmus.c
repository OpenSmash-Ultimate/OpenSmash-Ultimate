/* Multi-threaded litmus tests of the code recomp.py emits for guest LDAR / STLR / LDXR / LDAXR / STXR / STLXR /
 * DMB / DSB / CLREX, run on an AArch64 host. The guest programs are in test_lifter_atomics.py;
 * litmus_gen.h is their lifted C (python3 test_lifter_atomics.py --gen litmus_gen.h).
 *
 *   cc -O1 -fno-strict-aliasing -ffp-contract=off -I../../runtime -I. litmus.c -o litmus -lpthread
 *   ./litmus [iterations per thread = 1000000] [threads = 8]
 *
 * Must hold (exit status 1 otherwise):
 *   MP  (message passing)   str data; stlr flag   ||  ldar flag; ldr data          data seen once flag is seen
 *                           str data; dmb ishst; str flag || ldr flag; dmb ishld; ldr data
 *   SB  (store buffering)   str x; dmb ish / dsb sy; ldr y  ||  str y; dmb ish; ldr x   not both 0
 *                           stlr x; ldar y  ||  stlr y; ldar x                           not both 0 (STLR->LDAR is ordered)
 *   lock counters           ldaxr/stxr ... stlr,  ldxr/stxr+dmb ... ldxr/stlxr,  byte lock,  ldaxr/stlxr fetch-add,
 *                           ldxr/stxr fetch-add: the count is exactly threads x iterations
 *   exclusive pairs         ldaxp (2 x X / 2 x W) as the acquiring reader of MP, ldxp/stlxp as the releasing writer;
 *                           ldaxp/stxp .. ldxp/stlxp locks (2 x X and 2 x W); 16-byte fetch-add with ldaxp/stlxp and
 *                           ldxp/stxp (both doublewords counted exactly)
 *   clrex                   a STXR after CLREX fails, the next LDXR/STXR pair succeeds
 * Controls (plain str/ldr, no barrier): the number of reorderings seen is printed for information only; a non-zero
 * SB count shows the harness is able to observe reordering on this machine.
 * Meant for an AArch64 host. On x86-64 runtime.h keeps the lifted code of before (empty barriers, plain LDAR / STLR,
 * empty CLREX): the SB tests can fail there by design, and the CLREX check is skipped; -DRECOMP_X86_STRICT_FENCES=1
 * makes them exact. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t g_stx_fail;
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits)
{
    (void)c; (void)a; (void)expected; (void)seen; (void)v; (void)bits;
    __atomic_fetch_add(&g_stx_fail, 1, __ATOMIC_RELAXED);
}

#include "litmus_gen.h"

typedef void (*GFn)(Ctx *);
static long g_iters = 1000000;
static int g_threads = 8;
static int g_failed;

static void result(const char *name, long bad, int must_be_zero, const char *what)
{
    if (must_be_zero) {
        printf("%-4s %-44s %ld %s\n", bad ? "FAIL" : "ok", name, bad, what);
        if (bad) g_failed = 1;
    } else
        printf("info %-44s %ld %s (control, informational)\n", name, bad, what);
}

/* ------------------------------------------------------------------ message passing */
#define MP_SLOTS (1 << 20)
static uint64_t *mp_data, *mp_flag;
static GFn mp_w, mp_r;
static long mp_bad;

static void *mp_writer(void *arg)
{
    (void)arg;
    Ctx c;
    memset(&c, 0, sizeof c);
    for (long i = 0; i < MP_SLOTS; i++) {
        c.x[0] = (uint64_t)(uintptr_t)&mp_data[i];
        c.x[1] = (uint64_t)(uintptr_t)&mp_flag[2 * i];       /* 16-byte slots: ldaxp / stlxp of two X registers */
        c.x[2] = (uint64_t)i + 1;
        mp_w(&c);
    }
    return NULL;
}
static void *mp_reader(void *arg)
{
    (void)arg;
    Ctx c;
    long bad = 0;
    memset(&c, 0, sizeof c);
    for (long i = 0; i < MP_SLOTS; i++) {
        c.x[0] = (uint64_t)(uintptr_t)&mp_data[i];
        c.x[1] = (uint64_t)(uintptr_t)&mp_flag[2 * i];       /* 16-byte slots: ldaxp / stlxp of two X registers */
        mp_r(&c);
        if (c.x[4] != (uint64_t)i + 1) bad++;
    }
    mp_bad = bad;
    return NULL;
}
static void run_mp(const char *name, GFn w, GFn r, int must)
{
    long rounds = (g_iters + MP_SLOTS - 1) / MP_SLOTS * 4, bad = 0;     /* 4 pairs' worth of slots per thread pair */
    mp_w = w; mp_r = r;
    for (long k = 0; k < rounds; k++) {
        memset(mp_data, 0, MP_SLOTS * sizeof *mp_data);
        memset(mp_flag, 0, 2 * MP_SLOTS * sizeof *mp_flag);
        pthread_t a, b;
        pthread_create(&b, NULL, mp_reader, NULL);
        pthread_create(&a, NULL, mp_writer, NULL);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        bad += mp_bad;
    }
    char what[96];
    snprintf(what, sizeof what, "stale reads in %ld messages", rounds * MP_SLOTS);
    result(name, bad, must, what);
}

/* ------------------------------------------------------------------ store buffering */
typedef struct { uint64_t v; char pad[120]; } Line;
static Line *sb_x, *sb_y;
static uint64_t *sb_r0, *sb_r1;
static GFn sb_f;
static struct { volatile long pos; char pad[120]; } sb_pos[2];

static void *sb_thread(void *arg)
{
    int me = (int)(intptr_t)arg;
    Ctx c;
    memset(&c, 0, sizeof c);
    for (long i = 0; i < g_iters; i++) {
        /* loose lock-step with relaxed accesses only (they add no ordering to the test's own store -> load) */
        __atomic_store_n(&sb_pos[me].pos, i, __ATOMIC_RELAXED);
        while (__atomic_load_n(&sb_pos[!me].pos, __ATOMIC_RELAXED) < i) { }
        Line *mine = me ? &sb_y[i & 1023] : &sb_x[i & 1023], *other = me ? &sb_x[i & 1023] : &sb_y[i & 1023];
        c.x[0] = (uint64_t)(uintptr_t)&mine->v;
        c.x[1] = (uint64_t)(uintptr_t)&other->v;
        sb_f(&c);
        (me ? sb_r1 : sb_r0)[i] = c.x[3];
        /* both threads have run slot i before it is cleared for slot i + 1024 */
        __atomic_store_n(&sb_pos[me].pos, i + 1, __ATOMIC_RELAXED);
        while (__atomic_load_n(&sb_pos[!me].pos, __ATOMIC_RELAXED) < i + 1) { }
        if (!me) { sb_x[i & 1023].v = 0; sb_y[i & 1023].v = 0; }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    return NULL;
}
static void run_sb(const char *name, GFn f, int must)
{
    sb_f = f;
    memset(sb_x, 0, 1024 * sizeof *sb_x);
    memset(sb_y, 0, 1024 * sizeof *sb_y);
    sb_pos[0].pos = sb_pos[1].pos = -1;
    pthread_t a, b;
    pthread_create(&a, NULL, sb_thread, (void *)0);
    pthread_create(&b, NULL, sb_thread, (void *)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    long bad = 0;
    for (long i = 0; i < g_iters; i++) bad += sb_r0[i] == 0 && sb_r1[i] == 0;
    char what[96];
    snprintf(what, sizeof what, "both-zero outcomes in %ld runs", g_iters);
    result(name, bad, must, what);
}

/* ------------------------------------------------------------------ lock / atomic counters */
/* 16-byte aligned lock and counter: ldaxp / stxp of two X registers on them (CASP needs it, as the guest's pair does) */
static struct { union { uint64_t lock; uint8_t lock8; uint64_t lock2[2]; }; char pad[112]; uint64_t counter, counter_hi; char pad2[112]; }
    __attribute__((aligned(128))) cnt;
static GFn cnt_f;
static pthread_barrier_t cnt_start;

static void *cnt_thread(void *arg)
{
    (void)arg;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.ex_addr = ~0ull;
    c.x[0] = (uint64_t)(uintptr_t)&cnt.lock;
    c.x[5] = (uint64_t)(uintptr_t)&cnt.counter;
    c.x[6] = (uint64_t)g_iters;
    pthread_barrier_wait(&cnt_start);
    cnt_f(&c);
    return NULL;
}
/* hi_step: the pair programs also add hi_step to the second doubleword each time (exact count checked too) */
static void run_counter_pair(const char *name, GFn f, int width32, uint64_t hi_step)
{
    cnt.lock2[0] = cnt.lock2[1] = 0;
    cnt.counter = cnt.counter_hi = 0;
    cnt_f = f;
    g_stx_fail = 0;
    pthread_t t[64];
    pthread_barrier_init(&cnt_start, NULL, (unsigned)g_threads);
    for (int i = 0; i < g_threads; i++) pthread_create(&t[i], NULL, cnt_thread, NULL);
    for (int i = 0; i < g_threads; i++) pthread_join(t[i], NULL);
    pthread_barrier_destroy(&cnt_start);
    uint64_t want = (uint64_t)g_iters * (uint64_t)g_threads, got = width32 ? (uint32_t)cnt.counter : cnt.counter;
    char what[160];
    snprintf(what, sizeof what, "lost updates: counter %llu of %llu (%d threads x %ld; %llu failed stxr)",
             (unsigned long long)got, (unsigned long long)want, g_threads, g_iters, (unsigned long long)g_stx_fail);
    long bad = (long)(want - got);
    if (hi_step && cnt.counter_hi != want * hi_step) bad += 1 + (long)(want * hi_step - cnt.counter_hi) / (long)hi_step;
    result(name, bad, 1, what);
}
static void run_counter(const char *name, GFn f, int width32) { run_counter_pair(name, f, width32, 0); }

static void run_clrex(void)
{
#if !RT_ORDERED_ATOMIC
    puts("skip clrex: RT_CLREX is empty on this host (x86-64 without RECOMP_X86_STRICT_FENCES, as before)");
    return;
#endif
    Ctx c;
    uint64_t word = 0x1234;
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)&word;
    c.x[2] = c.x[3] = 77;
    g_clrex_check(&c);
    long bad = (c.x[2] != 1) + (c.x[3] != 0) + (word != 0x1234);
    result("clrex: stxr after clrex fails, next pair ok", bad, 1, "wrong results");
}

int main(int argc, char **argv)
{
    if (argc > 1) g_iters = atol(argv[1]);
    if (argc > 2) g_threads = atoi(argv[2]);
    if (g_threads < 2 || g_threads > 64) g_threads = 8;
    mp_data = calloc(MP_SLOTS, sizeof *mp_data);
    mp_flag = aligned_alloc(16, 2 * MP_SLOTS * sizeof *mp_flag);
    sb_x = calloc(1024, sizeof *sb_x);
    sb_y = calloc(1024, sizeof *sb_y);
    sb_r0 = calloc((size_t)g_iters, sizeof *sb_r0);
    sb_r1 = calloc((size_t)g_iters, sizeof *sb_r1);
    if (!mp_data || !mp_flag || !sb_x || !sb_y || !sb_r0 || !sb_r1) { puts("out of memory"); return 2; }

    run_clrex();
    run_mp("MP  stlr / ldar", g_mp_rel_writer, g_mp_acq_reader, 1);
    run_mp("MP  dmb ishst / dmb ishld", g_mp_fence_writer, g_mp_fence_reader, 1);
    run_mp("MP  plain str / ldr", g_mp_plain_writer, g_mp_plain_reader, 0);
    run_mp("MP  stlr / ldaxp (2 x X, no stxp)", g_mp_rel_writer, g_mp_ldaxp_reader, 1);
    run_mp("MP  stlr / ldaxp (2 x W, no stxp)", g_mp_rel_writer, g_mp_ldaxp_w_reader, 1);
    run_mp("MP  ldxp/stlxp (2 x X) / ldar", g_mp_stlxp_writer, g_mp_acq_reader, 1);
    run_mp("MP  ldxp/stlxp (2 x W) / ldar", g_mp_stlxp_w_writer, g_mp_acq_reader, 1);
    run_sb("SB  str; dmb ish; ldr", g_sb_fence, 1);
    run_sb("SB  str; dsb sy; ldr", g_sb_dsb, 1);
    run_sb("SB  stlr; ldar", g_sb_rel_acq, 1);
    run_sb("SB  plain str; ldr", g_sb_plain, 0);
    run_counter("lock ldaxr/stxr .. stlr (32-bit)", g_lock_inc_a, 0);
    run_counter("lock ldxr/stxr+dmb ish .. ldxr/stlxr", g_lock_inc_b, 0);
    run_counter("lock ldaxrb/stxrb .. stlrb (8-bit)", g_lock_inc_byte, 0);
    run_counter("fetch-add ldaxr/stlxr (64-bit)", g_atomic_inc_sc, 0);
    run_counter("fetch-add ldxr/stxr (32-bit)", g_atomic_inc_relaxed, 1);
    run_counter("lock ldaxp/stxp .. ldxp/stlxp (2 x X)", g_lock_pair_x, 0);
    run_counter("lock ldaxp/stxp .. ldxp/stlxp (2 x W)", g_lock_pair_w, 0);
    run_counter_pair("fetch-add ldaxp/stlxp (2 x X: +1, +3)", g_fetch_add_pair, 0, 3);
    run_counter_pair("fetch-add ldxp/stxp (2 x X: +1, +3)", g_fetch_add_pair_relaxed, 0, 3);
    printf("%s\n", g_failed ? "LITMUS FAILED" : "LITMUS PASSED");
    return g_failed;
}
