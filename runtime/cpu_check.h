/* The x86-64 extensions the processor check (cpu_check.c) knows, as bits.
 *
 * With RT_CPU_DEFINE_NEEDS defined first, this also defines rt_cpu_needs: the extensions the
 * compiler was told the including file's code may use, from its own macros. host.c does that, and
 * the game's code is compiled for the same instruction set (tools/isa.py), so it is what the
 * program needs: the check follows the compile flags, whatever they are. */
#ifndef SSBU_CPU_CHECK_H
#define SSBU_CPU_CHECK_H

#define RT_CPU_SSE3   (1u << 0)
#define RT_CPU_SSSE3  (1u << 1)
#define RT_CPU_SSE41  (1u << 2)
#define RT_CPU_SSE42  (1u << 3)
#define RT_CPU_POPCNT (1u << 4)
#define RT_CPU_CX16   (1u << 5)
#define RT_CPU_LAHF   (1u << 6)
#define RT_CPU_AVX    (1u << 7)    /* AVX, AVX2, FMA and F16C: only with the system saving the YMM registers */
#define RT_CPU_AVX2   (1u << 8)
#define RT_CPU_FMA    (1u << 9)
#define RT_CPU_F16C   (1u << 10)
#define RT_CPU_BMI1   (1u << 11)
#define RT_CPU_BMI2   (1u << 12)
#define RT_CPU_LZCNT  (1u << 13)
#define RT_CPU_MOVBE  (1u << 14)
#define RT_CPU_COUNT  15
#define RT_CPU_NAMES {"SSE3", "SSSE3", "SSE4.1", "SSE4.2", "POPCNT", "CMPXCHG16B", "LAHF-SAHF", "AVX", "AVX2", "FMA", \
                      "F16C", "BMI1", "BMI2", "LZCNT", "MOVBE"}
/* x86-64-v2: what WindowSmash needs at the least */
#define RT_CPU_V2 (RT_CPU_SSE3 | RT_CPU_SSSE3 | RT_CPU_SSE41 | RT_CPU_SSE42 | RT_CPU_POPCNT | RT_CPU_CX16 | RT_CPU_LAHF)

#endif

#if defined(RT_CPU_DEFINE_NEEDS) && !defined(SSBU_CPU_NEEDS_DEFINED)
#define SSBU_CPU_NEEDS_DEFINED
const unsigned rt_cpu_needs = 0u
#ifdef __SSE3__
    | RT_CPU_SSE3
#endif
#ifdef __SSSE3__
    | RT_CPU_SSSE3
#endif
#ifdef __SSE4_1__
    | RT_CPU_SSE41
#endif
#ifdef __SSE4_2__
    | RT_CPU_SSE42
#endif
#ifdef __POPCNT__
    | RT_CPU_POPCNT
#endif
#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16
    | RT_CPU_CX16
#endif
#ifdef __LAHF_SAHF__
    | RT_CPU_LAHF
#endif
#ifdef __AVX__
    | RT_CPU_AVX
#endif
#ifdef __AVX2__
    | RT_CPU_AVX2
#endif
#ifdef __FMA__
    | RT_CPU_FMA
#endif
#ifdef __F16C__
    | RT_CPU_F16C
#endif
#ifdef __BMI__
    | RT_CPU_BMI1
#endif
#ifdef __BMI2__
    | RT_CPU_BMI2
#endif
#ifdef __LZCNT__
    | RT_CPU_LZCNT
#endif
#ifdef __MOVBE__
    | RT_CPU_MOVBE
#endif
    ;
#endif
