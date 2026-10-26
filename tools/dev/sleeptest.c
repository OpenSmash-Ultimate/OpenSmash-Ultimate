/* Offline checks of the sleep behind the guest's svcSleepThread (runtime/sleep.h): that it never
 * ends before the time asked, on one thread or on several at once, that a sleep of 200 us takes
 * well under the millisecond the old one rounded it up to, and that SSBU_SLEEP=ms brings that back.
 *
 *   Windows: python -c "import sys; sys.path.insert(0,'tools'); import cc, subprocess; subprocess.run([cc.CLANG, '/nologo', '/O2', *cc.BASE_CFLAGS, 'tools/dev/sleeptest.c', '/Febuild/sleeptest.exe'], check=True)"
 *   Linux:   clang -O2 tools/dev/sleeptest.c -o build/sleeptest -pthread
 *   macOS:   clang -O2 -I runtime/macos/include -I runtime/linux/include tools/dev/sleeptest.c runtime/macos/platform.c -o build/sleeptest
 *   sleeptest early|fine, SSBU_SLEEP=ms sleeptest ms
 *
 * early: every sleep at least as long as asked, from 1 us to 2.5 ms, and on four threads at once.
 * fine: the median of 401 sleeps of 200 us under 900 us; on a Windows without high-resolution
 * timers (before 10 1803) it prints "skipped:" and why. Both print what they measured, fine also
 * the old whole-millisecond sleep beside it. Both want SSBU_SLEEP unset.
 * ms: with SSBU_SLEEP=ms, sleeps of 1 us, 200 us and 1.5 ms are whole milliseconds again: on Linux
 * and macOS each at least the milliseconds rounded up; on Windows by Sleep(), without a timer
 * (Sleep() may end early by its own rules: only printed there).
 * Exit code 0 when every check passes. tests/test_sleep.py runs all three. */
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "winmm.lib")
#else
#define _GNU_SOURCE 1
#include <pthread.h>
#include <time.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/sleep.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the clock sleep.h keeps to (the one the guest's system tick follows) */
static uint64_t now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    uint64_t q = (uint64_t)t.QuadPart, hz = (uint64_t)f.QuadPart;
    return q / hz * 1000000000ull + q % hz * 1000000000ull / hz;
#elif defined(__APPLE__)
    return rt_uptime_ns();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* what svcSleepThread did before: whole milliseconds, rounded up */
static void old_sleep(uint64_t ns) {
#ifdef _WIN32
    Sleep((DWORD)((ns + 999999) / 1000000));
#else   /* (the shim's Sleep: that many whole milliseconds on the same timers) */
    sleep_ns((ns + 999999) / 1000000 * 1000000);
#endif
}

static int by_value(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

typedef struct Stats { uint64_t min, median, p90, max; int short_ones; } Stats;

/* n sleeps of ns each, by `how`: how long each took, and how many ended early */
static Stats measure(void (*how)(uint64_t), uint64_t ns, int n) {
    uint64_t *took = malloc(sizeof *took * (size_t)n);
    Stats s = {0};
    for (int i = 0; i < n; i++) {
        uint64_t t0 = now_ns();
        how(ns);
        took[i] = now_ns() - t0;
        s.short_ones += took[i] < ns;
    }
    qsort(took, (size_t)n, sizeof *took, by_value);
    s.min = took[0];
    s.median = took[n / 2];
    s.p90 = took[n * 9 / 10];
    s.max = took[n - 1];
    free(took);
    return s;
}

static void show(const char *what, uint64_t ns, int n, Stats s) {
    printf("%s, asked %7.1f us: %3d sleeps, min %8.1f, median %8.1f, 90%% %8.1f, max %8.1f us; %d short\n", what, ns / 1e3, n,
           s.min / 1e3, s.median / 1e3, s.p90 / 1e3, s.max / 1e3, s.short_ones);
}

/* ---- four threads at once ---------------------------------------------------------------- */
#define THREADS 4
typedef struct Worker { Stats s; void *timer; } Worker;

static void work(Worker *w) {
    w->s = measure(sleep_ns, 100000, 100);
#ifdef _WIN32
    w->timer = sleep_timer;   /* (its own) */
#endif
}

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID p) { work(p); return 0; }
#else
static void *worker(void *p) { work(p); return NULL; }
#endif

static void on_threads(void) {
    Worker w[THREADS];
    memset(w, 0, sizeof w);
#ifdef _WIN32
    HANDLE t[THREADS];
    for (int i = 0; i < THREADS; i++) t[i] = CreateThread(NULL, 0, worker, &w[i], 0, NULL);
    WaitForMultipleObjects(THREADS, t, TRUE, INFINITE);
    for (int i = 0; i < THREADS; i++) CloseHandle(t[i]);
#else
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, worker, &w[i]);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
#endif
    for (int i = 0; i < THREADS; i++) {
        char what[32];
        snprintf(what, sizeof what, "thread %d of %d", i + 1, THREADS);
        show(what, 100000, 100, w[i].s);
        CHECK(w[i].s.short_ones == 0, "thread %d: %d sleeps of 100 us ended early (shortest %llu ns)", i + 1, w[i].s.short_ones,
              (unsigned long long)w[i].s.min);
#ifdef _WIN32
        if (!sleep_untimed) {
            CHECK(w[i].timer != NULL, "thread %d slept without a timer of its own", i + 1);
            for (int k = 0; k < i; k++) CHECK(w[i].timer != w[k].timer, "threads %d and %d slept on the same timer", k + 1, i + 1);
        }
#endif
    }
}

/* ---- the checks --------------------------------------------------------------------------- */
static void never_early(void) {
    CHECK(!sleep_whole_ms(), "SSBU_SLEEP=ms is set: this mode wants it unset");
    static const struct { uint64_t ns; int n; } spans[] = {{1000, 200}, {50000, 200}, {200000, 200}, {1000000, 100}, {2500000, 40}};
    for (size_t i = 0; i < sizeof spans / sizeof spans[0]; i++) {
        Stats s = measure(sleep_ns, spans[i].ns, spans[i].n);
        show("sleep_ns", spans[i].ns, spans[i].n, s);
        CHECK(s.short_ones == 0, "%d of %d sleeps of %llu ns ended early (shortest %llu ns)", s.short_ones, spans[i].n,
              (unsigned long long)spans[i].ns, (unsigned long long)s.min);
    }
    on_threads();
}

static int fine(void) {
    CHECK(!sleep_whole_ms(), "SSBU_SLEEP=ms is set: this mode wants it unset");
    sleep_ns(1000);   /* (this thread's timer made) */
#ifdef _WIN32
    if (sleep_untimed) {
        printf("skipped: no high-resolution waitable timer here (Windows 10 1803 and later have one): sleeps are whole milliseconds, as before\n");
        return 0;
    }
#endif
    measure(sleep_ns, 200000, 20);   /* (warm-up) */
    Stats s = measure(sleep_ns, 200000, 401);
    show("sleep_ns", 200000, 401, s);
    Stats old = measure(old_sleep, 200000, 20);
    show("before (whole ms)", 200000, 20, old);
#ifdef _WIN32   /* (the game's vsync thread asks for the 1 ms period) */
    timeBeginPeriod(1);
    old = measure(old_sleep, 200000, 20);
    show("before, timeBeginPeriod(1)", 200000, 20, old);
    timeEndPeriod(1);
#endif
    printf("200 us asked: median %.1f us (bound 900 us)\n", s.median / 1e3);
    CHECK(s.median < 900000, "a sleep of 200 us took %.1f us at the median", s.median / 1e3);
    return 1;
}

static void whole_ms(void) {
    const char *e = getenv("SSBU_SLEEP");
    CHECK(sleep_whole_ms(), "SSBU_SLEEP=ms not taken up (SSBU_SLEEP=%s)", e ? e : "(unset)");
    static const uint64_t spans[] = {1000, 200000, 1500000};
    for (size_t i = 0; i < sizeof spans / sizeof spans[0]; i++) {
        Stats s = measure(sleep_ns, spans[i], 10);
        show("SSBU_SLEEP=ms", spans[i], 10, s);
#ifndef _WIN32
        uint64_t ms = (spans[i] + 999999) / 1000000;
        CHECK(s.min >= ms * 1000000, "a sleep of %llu ns took %llu ns, under the whole %llu ms", (unsigned long long)spans[i],
              (unsigned long long)s.min, (unsigned long long)ms);
#endif
    }
#ifdef _WIN32
    CHECK(!sleep_timer && !sleep_untimed, "SSBU_SLEEP=ms slept on the high-resolution timer");
#endif
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "early";
    if (!strcmp(mode, "early")) never_early();
    else if (!strcmp(mode, "fine")) { if (!fine()) return 0; }
    else if (!strcmp(mode, "ms")) whole_ms();
    else { fprintf(stderr, "usage: sleeptest early|fine|ms\n"); return 2; }
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
