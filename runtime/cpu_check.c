/* The processor check, before anything else of the program runs.
 *
 * A program compiled for AVX2 (tools/isa.py) started on a processor without it would stop at its
 * first AVX instruction, with a crash report that helps nobody. This asks the processor (CPUID, and
 * XGETBV for whether the system saves the YMM registers) for each extension the program's code may
 * use - host.c's rt_cpu_needs, from the compiler's own macros (cpu_check.h) - and if one is
 * missing, says which and what to do (on Windows in a message box too, as the game is started from
 * Play SSBU.bat with its output in build\play.log), then stops.
 *
 * It must not need more of the processor than any x86-64 one has, so this file alone is compiled
 * for plain x86-64 whatever the build's instruction set (tools/cc.py's CHECK_CFLAGS, tools/linux/
 * toolchain.py's), and it runs from the C runtime's start-up, before any of the program's own
 * code: on Windows among the C initialisers (.CRT$XID: after the static C runtime's own, which are
 * in .CRT$XIC and set up stdio among others, and before every C++ initialiser and main), on Linux
 * from .preinit_array (before every constructor). The macOS build (arm64) has none of this. */
#if defined(__x86_64__) || defined(_M_X64)
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#else
#include <cpuid.h>
#include <unistd.h>
#endif
#include "cpu_check.h"

extern const unsigned rt_cpu_needs;

static void cpuid(unsigned leaf, unsigned sub, unsigned r[4]) {
#ifdef _WIN32
    int v[4];
    __cpuidex(v, (int)leaf, (int)sub);
    memcpy(r, v, sizeof v);
#else
    __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#endif
}

/* The extensions this processor has (those using the YMM registers only if the system saves them). */
unsigned rt_cpu_has(void) {
#ifdef RT_CPU_TEST   /* (the tests: a made-up processor's) */
    return RT_CPU_TEST;
#else
    unsigned r[4], has = 0;
    cpuid(0, 0, r);
    unsigned top = r[0];
    cpuid(1, 0, r);
    unsigned c = r[2];
    static const struct { unsigned char bit; unsigned has; } leaf1[] = {
        {0, RT_CPU_SSE3}, {9, RT_CPU_SSSE3}, {13, RT_CPU_CX16}, {19, RT_CPU_SSE41}, {20, RT_CPU_SSE42},
        {22, RT_CPU_MOVBE}, {23, RT_CPU_POPCNT}};
    for (unsigned i = 0; i < sizeof leaf1 / sizeof leaf1[0]; i++)
        if (c >> leaf1[i].bit & 1) has |= leaf1[i].has;
    int ymm = 0;
    if (c >> 27 & 1) {   /* OSXSAVE: XCR0 says what the system saves; bits 1 and 2 are the XMM and YMM registers */
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        (void)hi;
        ymm = (lo & 6) == 6;
    }
    if (ymm) {
        if (c >> 28 & 1) has |= RT_CPU_AVX;
        if (c >> 12 & 1) has |= RT_CPU_FMA;
        if (c >> 29 & 1) has |= RT_CPU_F16C;
    }
    if (top >= 7) {
        cpuid(7, 0, r);
        if (r[1] >> 3 & 1) has |= RT_CPU_BMI1;
        if (r[1] >> 5 & 1 && ymm) has |= RT_CPU_AVX2;
        if (r[1] >> 8 & 1) has |= RT_CPU_BMI2;
    }
    cpuid(0x80000000u, 0, r);
    if (r[0] >= 0x80000001u) {
        cpuid(0x80000001u, 0, r);
        if (r[2] & 1) has |= RT_CPU_LAHF;
        if (r[2] >> 5 & 1) has |= RT_CPU_LZCNT;
    }
    return has;
#endif
}

/* What to say about a processor with `has` for a program that needs `needs` (into out, `size`
 * bytes), or 0 when it has all of it. */
int rt_cpu_message(unsigned needs, unsigned has, char *out, size_t size) {
    static const char *const names[RT_CPU_COUNT] = RT_CPU_NAMES;
    unsigned lacks = needs & ~has;
    if (!lacks) return 0;
    char list[160] = "";
    for (int i = 0; i < RT_CPU_COUNT; i++)
        if (lacks >> i & 1) {
            if (list[0]) strcat(list, ", ");
            strcat(list, names[i]);
        }
#ifdef _WIN32
#define RT_CPU_AGAIN "Run OpenSmash.exe again: it builds the game for this processor"
#else
#define RT_CPU_AGAIN "Run install-windowsmash.sh again: it builds the game for this processor"
#endif
    /* (the second sentence for when the first does not help: a Windows older than 10 version 2004 cannot be asked
     * about AVX2, and the installer then goes by the processor's name, which may say AVX2 where a virtual machine
     * has none) */
    if ((has & RT_CPU_V2) == RT_CPU_V2)
        snprintf(out, size, "This copy of WindowSmash was built for processors with AVX2, and this processor lacks %s. "
                 RT_CPU_AGAIN " (the build for older processors, x86-64-v2). If this message comes back, build it with "
                 "SSBU_ISA=x86-64-v2 set (docs/BUILDING.md, The instruction set).", list);
    else
        snprintf(out, size, "This processor lacks %s, which WindowSmash needs (at the least SSE4.2 and POPCNT: "
                 "x86-64-v2, Intel processors since 2008 and AMD since 2011). It cannot run on this PC.", list);
    return 1;
}

/* Stops the program with the message if the processor lacks what it needs. */
void rt_cpu_check(void) {
    char text[640];
    if (!rt_cpu_message(rt_cpu_needs, rt_cpu_has(), text, sizeof text)) return;
#ifdef _WIN32   /* (WriteFile, not stdio: the message needs nothing of the C runtime set up) */
    char line[sizeof text + 16];
    int n = snprintf(line, sizeof line, "[boot] %s\n", text);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD wrote;
    if (n > 0 && err && err != INVALID_HANDLE_VALUE)
        WriteFile(err, line, (DWORD)((size_t)n < sizeof line ? (size_t)n : sizeof line - 1), &wrote, NULL);
#ifndef RT_CPU_TEST   /* (and no box for a test to click away) */
    MessageBoxA(NULL, text, "WindowSmash", MB_OK | MB_ICONERROR);
#endif
    ExitProcess(5);
#else   /* (before the C library's own constructors: write, not stdio's stderr) */
    char line[sizeof text + 16];
    int n = snprintf(line, sizeof line, "[boot] %s\n", text);
    if (n > 0 && write(2, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1) < 0) _exit(5);
    _exit(5);
#endif
}

#ifdef _WIN32
static int __cdecl cpu_check_start(void) {
    rt_cpu_check();
    return 0;
}
#pragma section(".CRT$XID", long, read)
__declspec(allocate(".CRT$XID")) __attribute__((used)) static int (__cdecl *const cpu_check_entry)(void) = cpu_check_start;
#elif defined(__ELF__)
static void cpu_check_start(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    rt_cpu_check();
}
__attribute__((section(".preinit_array"), used)) static void (*cpu_check_entry)(int, char **, char **) = cpu_check_start;
#endif
#endif
