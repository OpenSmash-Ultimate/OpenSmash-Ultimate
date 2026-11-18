/* The clock behind host.c's rt_clock_ns (included there once; tools/dev/clocktest.c checks it).
 *
 * What tells time asks it: the game's system tick, the kernel's timeouts, rt_now_ns and the
 * renderer's own timing (twice a draw) - tens of thousands of times a second. That is
 * QueryPerformanceCounter, some 20 ns a call, except on a PC that Windows was told to time
 * with the HPET (bcdedit useplatformclock, a common "tweak") or the ACPI timer: there a call
 * takes a microsecond or more, and a player's 8-fighter match spent some 40% of the game's
 * main thread in it. So a slow one is measured at start and, where the CPU's time stamp
 * counter runs at a constant rate (CPUID: invariant TSC - x64 CPUs of the last decade), that
 * counter is read instead, scaled to the same nanoseconds: measured against the system timer
 * at start, and again after a few seconds, half a minute and ten minutes, each time from
 * where the clock stands then (it never steps back). SSBU_CLOCK=qpc or tsc picks one.
 *
 * On arm64 (the macOS port) there is nothing to switch to: the system timer is the processor's
 * own counter there (mach_absolute_time, a few nanoseconds a read, at a constant rate), so it
 * is always the one, and only x64 builds have the time stamp counter's part.
 *
 * Needs <windows.h> (or the Linux port's) first. */
#ifndef SSBU_CLOCK_H
#define SSBU_CLOCK_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#define CLOCK_TSC 1
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif
#else
#define CLOCK_TSC 0
#endif

typedef struct ClockScale { uint64_t tsc0, ns0, mul; } ClockScale;   /* ns = ns0 + (tsc - tsc0) * mul / 2^32 */
static const ClockScale *volatile clock_scale;   /* NULL: the system timer */
static INIT_ONCE clock_once = INIT_ONCE_STATIC_INIT;
static volatile LONG clock_ready;
static uint64_t qpc_hz, qpc_mul, clock_start_ns;
/* clock_steady: the CPU has a counter of its own that may stand in for the system timer (never
 * on arm64, where the system timer is that counter) */
static int clock_refines, clock_steady;
static double clock_read_ns;   /* what a system timer read took, at start */
#if CLOCK_TSC
static volatile LONG clock_refining;
static uint64_t cal_tsc, cal_ns, next_refine;   /* (cal_*: the first measurement) */
static int clock_has_rdtscp;
#endif

#if CLOCK_TSC
/* The CPU's counter, read in order (plain RDTSC may run ahead of earlier loads: a thread could
 * then see a time before one it had just read from another thread - as the system timer never does). */
static inline uint64_t clock_tsc(void) {
    if (clock_has_rdtscp) {
        unsigned aux;
        return __rdtscp(&aux);
    }
    _mm_lfence();
    return __rdtsc();
}

#endif

static uint64_t qpc_ns(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    uint64_t q = (uint64_t)t.QuadPart;
    /* Windows normally exposes 10 MHz, Linux 1 GHz. An integral ns/tick ratio is exact
     * with one multiply; avoid two variable divisions on every guest clock read. Other
     * frequencies (including Apple's 24 MHz) retain the quotient/remainder conversion. */
    if (qpc_mul) return q * qpc_mul;
    return q / qpc_hz * 1000000000ull + q % qpc_hz * 1000000000ull / qpc_hz;
}

#if CLOCK_TSC
/* CPUID's extended leaf's EDX (0 where there is none) */
static unsigned cpuid_ext_edx(unsigned leaf) {
#ifdef _MSC_VER
    int r[4];
    __cpuid(r, (int)0x80000000);
    if ((unsigned)r[0] < leaf) return 0;
    __cpuid(r, (int)leaf);
    return (unsigned)r[3];
#else
    unsigned a, b, c, d;
    return __get_cpuid(leaf, &a, &b, &c, &d) ? d : 0;
#endif
}

/* A time stamp counter reading and the system timer's time at the same moment (the middle of the read). */
static void clock_pair(uint64_t *tsc, uint64_t *ns) {
    uint64_t t0 = clock_tsc();
    *ns = qpc_ns();
    uint64_t t1 = clock_tsc();
    *tsc = t0 + (t1 - t0) / 2;
}

static uint64_t clock_scaled(const ClockScale *s, uint64_t tsc) {
    if ((int64_t)(tsc - s->tsc0) < 0) tsc = s->tsc0;   /* (a core whose counter is a little behind) */
    return s->ns0 + (uint64_t)((unsigned __int128)(tsc - s->tsc0) * s->mul >> 32);
}

/* A new scale from the first measurement to now, taking over from where the clock is. */
static void clock_rescale(void) {
    uint64_t tsc, ns;
    clock_pair(&tsc, &ns);
    ClockScale *n = malloc(sizeof *n);
    if (!n || tsc <= cal_tsc) { free(n); return; }
    n->mul = (uint64_t)((double)(ns - cal_ns) / (double)(tsc - cal_tsc) * 4294967296.0 + 0.5);
    const ClockScale *old = __atomic_load_n(&clock_scale, __ATOMIC_ACQUIRE);
    n->tsc0 = tsc;
    n->ns0 = old ? clock_scaled(old, tsc) : ns;
    __atomic_store_n(&clock_scale, n, __ATOMIC_RELEASE);   /* (the old one stays: another thread may be reading it) */
}
#endif

static BOOL CALLBACK clock_setup(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once; (void)param; (void)context;
    LARGE_INTEGER f, a, b, t;
    QueryPerformanceFrequency(&f);
    qpc_hz = (uint64_t)f.QuadPart;
    qpc_mul = 1000000000ull % qpc_hz ? 0 : 1000000000ull / qpc_hz;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 256; i++) QueryPerformanceCounter(&t);
    QueryPerformanceCounter(&b);
    clock_read_ns = (double)(b.QuadPart - a.QuadPart) * 1e9 / (double)qpc_hz / 257;
    const char *want = getenv("SSBU_CLOCK");
    int slow = clock_read_ns > 250;
#if CLOCK_TSC
    clock_steady = cpuid_ext_edx(0x80000007) >> 8 & 1;    /* invariant TSC */
    clock_has_rdtscp = cpuid_ext_edx(0x80000001) >> 27 & 1;
    int tsc = want && !strcmp(want, "tsc") ? clock_steady : want && !strcmp(want, "qpc") ? 0 : slow && clock_steady;
#endif
    clock_start_ns = qpc_ns();
#if CLOCK_TSC
    if (tsc) {   /* a first scale over 20 ms */
        clock_pair(&cal_tsc, &cal_ns);
        while (qpc_ns() - cal_ns < 20000000) {}
        clock_rescale();
        __atomic_store_n(&next_refine, cal_tsc + (uint64_t)((double)(clock_tsc() - cal_tsc) * 100), __ATOMIC_RELAXED);   /* after some 2 s */
    }
#endif
#ifdef _WIN32
#define CLOCK_SLOW_WHY "Windows set to use the HPET? \"bcdedit /deletevalue useplatformclock\" undoes that"
#elif defined(__APPLE__)   /* (the macOS port: mach_absolute_time, the processor's counter itself) */
#define CLOCK_SLOW_WHY "on a Mac it is the processor's own counter: was the machine very busy as the game started?"
#else   /* (the Linux port: clock_gettime, slow when the kernel's clock source is) */
#define CLOCK_SLOW_WHY "the kernel's clock source, in /sys/devices/system/clocksource/clocksource0/current_clocksource, hpet or acpi_pm?"
#endif
#if CLOCK_TSC
    fprintf(stderr, "[clock] system timer %.2f MHz, %.0f ns a read%s\n", qpc_hz / 1e6, clock_read_ns,
            tsc ? (slow ? ": slow (" CLOCK_SLOW_WHY ") - timing from the CPU's counter instead"
                        : ": timing from the CPU's counter (SSBU_CLOCK=tsc)")
                : slow ? (clock_steady ? " (SSBU_CLOCK=qpc)" : ": slow, and this CPU's counter does not run at a constant rate - expect a lower frame rate")
                       : want && !strcmp(want, "tsc") ? " (SSBU_CLOCK=tsc, but this CPU's counter does not run at a constant rate)" : "");
#else   /* (the system timer is the counter: nothing faster to take) */
    fprintf(stderr, "[clock] system timer %.2f MHz, %.0f ns a read%s\n", qpc_hz / 1e6, clock_read_ns,
            slow ? ": slow (" CLOCK_SLOW_WHY ")" : want && !strcmp(want, "tsc") ? " (SSBU_CLOCK=tsc: here the system timer is the processor's counter)" : "");
#endif
    __atomic_store_n(&clock_ready, 1, __ATOMIC_RELEASE);   /* (after all of the above, for readers that skip InitOnce) */
    return TRUE;
}

/* Nanoseconds on the system timer's time line (since the PC started), cheaply. */
static uint64_t clock_now_ns(void) {
    if (!__atomic_load_n(&clock_ready, __ATOMIC_ACQUIRE)) InitOnceExecuteOnce(&clock_once, clock_setup, NULL, NULL);
#if !CLOCK_TSC
    return qpc_ns();
#else
    const ClockScale *s = __atomic_load_n(&clock_scale, __ATOMIC_ACQUIRE);
    if (!s) return qpc_ns();
    uint64_t tsc = clock_tsc();
    /* The schedule is read outside the calibration lock: those reads and its updates
     * must be atomic too. Recheck under the lock, so a stale fast-path read is harmless.
     * The scale again over a longer time: after 2 s, 30 s and 10 min. */
    if (tsc >= __atomic_load_n(&next_refine, __ATOMIC_RELAXED) &&
        __atomic_load_n(&clock_refines, __ATOMIC_RELAXED) < 3 && !InterlockedExchange(&clock_refining, 1)) {
        int refined = __atomic_load_n(&clock_refines, __ATOMIC_RELAXED);
        if (tsc >= __atomic_load_n(&next_refine, __ATOMIC_RELAXED) && refined < 3) {
            clock_rescale();
            static const uint32_t factor[3] = {15, 20, 0};
            __atomic_store_n(&next_refine, cal_tsc + (tsc - cal_tsc) * factor[refined], __ATOMIC_RELAXED);
            __atomic_store_n(&clock_refines, refined + 1, __ATOMIC_RELAXED);
        }
        InterlockedExchange(&clock_refining, 0);
        s = __atomic_load_n(&clock_scale, __ATOMIC_ACQUIRE);
    }
    return clock_scaled(s, tsc);
#endif
}

#endif
