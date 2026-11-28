/* tp.h - tiny test helpers for the tests/port suites (the clock is CLOCK_MONOTONIC, which macOS has too) */
#ifndef TP_H
#define TP_H
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int tp_fail, tp_checks;
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define TP_BUMP(x) __atomic_add_fetch(&(x), 1, __ATOMIC_RELAXED)
#define CHECK(c) do { TP_BUMP(tp_checks); if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); TP_BUMP(tp_fail); } } while (0)
#define CHECKF(c, ...) do { TP_BUMP(tp_checks); if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); TP_BUMP(tp_fail); } } while (0)
static inline int64_t tp_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static inline int tp_done(const char *name) {
    printf("%-14s %s (%d checks, %d failed)\n", name, tp_fail ? "FAIL" : "PASS", tp_checks, tp_fail);
    fflush(stdout);
    return tp_fail ? 1 : 0;
}
#define SECTION(s) do { printf("  - %s\n", s); fflush(stdout); } while (0)
#endif
