"""Helpers of runtime/recomp.h checked against their definitions, built with the game build's compiler and flags
for this OS (tools/dev/difftest.py's toolchain: clang-cl on Windows, the Linux build's x86-64 flags, the macOS
build's arm64 flags; on an Apple silicon Mac with Rosetta 2, the Linux build's flags under it as well), at -O2 and
-O0, and outside Windows as C++ too (as the build compiles the generated files with landing sites):

  rt_rsqrt_est     (frsqrte's estimate) equal to the architecture's loop, which it replaced, for every input
  rt_crc32*        the CRC-32 and CRC-32C check values (of "123456789"), and the host's instruction or CRC-32's
                   tables (whichever is used) equal to the bitwise definition, rt_crc32_bits, for random operands
  NaN results      fnan*, rt_vnan*, the fmax / fmin and step helpers, in the forms the lifter emits: Arm's NaNs
                   (FPCR.DN = 0) for made-up operands, with the x86 builds' checks (RT_EXACT_NAN 1) on every host;
                   on arm64 the instructions themselves must give the same"""
from pathlib import Path
import importlib.util
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location('difftest', ROOT / 'tools' / 'dev' / 'difftest.py')
difftest = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(difftest)

CHECK = r'''
#include <stdio.h>
#define RT_EXACT_NAN 1
#include "recomp.h"

/* rt_rsqrt_est as it was: the loop of the Arm manual's RecipSqrtEstimate */
static u32 rsqrt_est_loop(u32 a) {
    a = a < 256 ? a * 2 + 1 : ((a & ~1u) + 1) * 2;
    u32 b = 512;
    while ((u64)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (b + 1) / 2;
}

static u64 next(u64 *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }   /* xorshift64 */
static int bad;
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); bad++; } } while (0)

/* NaN results, from made-up operands: a, b the operands (a multiply-add's Rn, Rm), c its addend. The lifted forms
   are lift.py's and lift_simd.py's; on arm64 the instructions must agree with the expected values too. */
enum { F_ADD, F_SUB, F_MUL, F_DIV, F_NMUL, F_SQRT, F_MADD, F_MSUB, F_NMADD, F_NMSUB, F_MAX, F_MIN, F_MAXNM, F_MINNM,
       F_MULX, F_RECPS, F_RSQRTS };
#define LIFTED(P, T, W, FMA, SQRT) \
static T lifted##W(int op, T a, T b, T c) { \
    switch (op) { \
    case F_ADD: return fnan2##P(a + b, a, b); \
    case F_SUB: return fnan2##P(a - b, a, b); \
    case F_MUL: return fnan2##P(a * b, a, b); \
    case F_DIV: return fnan2##P(a / b, a, b); \
    case F_NMUL: return -fnan2##P(a * b, a, b); \
    case F_SQRT: return fnan1##P(SQRT(a), a); \
    case F_MADD: return fnan3##P(FMA(a, b, c), c, a, b); \
    case F_MSUB: return fnan3##P(FMA(-a, b, c), c, -a, b); \
    case F_NMADD: return fnan3##P(FMA(-a, b, -c), -c, -a, b); \
    case F_NMSUB: return fnan3##P(FMA(a, b, -c), -c, a, b); \
    case F_MAX: return fmax##P(a, b); \
    case F_MIN: return fmin##P(a, b); \
    case F_MAXNM: return fmaxnm##P(a, b); \
    case F_MINNM: return fminnm##P(a, b); \
    case F_MULX: return rt_fmulx##W(a, b); \
    case F_RECPS: return rt_frecps##W(a, b); \
    default: return rt_frsqrts##W(a, b); \
    } \
}
LIFTED(S, f32, 32, fmaf, sqrtf)
LIFTED(D, f64, 64, fma, sqrt)

#if defined(__aarch64__)
#define HW1(ins, R) __asm__(ins " %" R "0, %" R "1" : "=w"(r) : "w"(a))
#define HW2(ins, R) __asm__(ins " %" R "0, %" R "1, %" R "2" : "=w"(r) : "w"(a), "w"(b))
#define HW3(ins, R) __asm__(ins " %" R "0, %" R "1, %" R "2, %" R "3" : "=w"(r) : "w"(a), "w"(b), "w"(c))
#define HW(T, W, R) \
static T hw##W(int op, T a, T b, T c) { \
    T r = 0; \
    switch (op) { \
    case F_ADD: HW2("fadd", R); break; \
    case F_SUB: HW2("fsub", R); break; \
    case F_MUL: HW2("fmul", R); break; \
    case F_DIV: HW2("fdiv", R); break; \
    case F_NMUL: HW2("fnmul", R); break; \
    case F_SQRT: HW1("fsqrt", R); break; \
    case F_MADD: HW3("fmadd", R); break; \
    case F_MSUB: HW3("fmsub", R); break; \
    case F_NMADD: HW3("fnmadd", R); break; \
    case F_NMSUB: HW3("fnmsub", R); break; \
    case F_MAX: HW2("fmax", R); break; \
    case F_MIN: HW2("fmin", R); break; \
    case F_MAXNM: HW2("fmaxnm", R); break; \
    case F_MINNM: HW2("fminnm", R); break; \
    case F_MULX: HW2("fmulx", R); break; \
    case F_RECPS: HW2("frecps", R); break; \
    default: HW2("frsqrts", R); break; \
    } \
    return r; \
}
HW(f32, 32, "s")
HW(f64, 64, "d")
#endif

/* quiet NaNs 7fc12345 and ffd55555, signalling 7f800001 and ffa00000 (quieted: 7fc00001, ffe00000); 3f800000 is 1 */
static const struct { int op; u32 a, b, c, want; } NAN32[] = {
    {F_ADD, 0x3F800000, 0x3F800000, 0, 0x40000000},
    {F_ADD, 0x7F800000, 0xFF800000, 0, 0x7FC00000},           /* inf - inf: the default NaN (x86's is ffc00000) */
    {F_DIV, 0, 0x80000000, 0, 0x7FC00000},                    /* 0 / -0 */
    {F_MUL, 0x80000000, 0x7F800000, 0, 0x7FC00000},           /* -0 * inf */
    {F_SQRT, 0xBF800000, 0, 0, 0x7FC00000},                   /* the square root of -1 */
    {F_ADD, 0x7FC12345, 0x7F800001, 0, 0x7FC00001},           /* a signalling NaN before a quiet one, quieted */
    {F_SUB, 0xFFD55555, 0x3F800000, 0, 0xFFD55555},           /* sign and payload kept */
    {F_ADD, 0x3F800000, 0xFFD55555, 0, 0xFFD55555},
    {F_MUL, 0x7FC12345, 0xFFD55555, 0, 0x7FC12345},           /* two quiet NaNs: the first */
    {F_ADD, 0xFFD55555, 0x7FC12345, 0, 0xFFD55555},
    {F_DIV, 0xFFA00000, 0x7FC12345, 0, 0xFFE00000},
    {F_SQRT, 0x7FBFFFFF, 0, 0, 0x7FFFFFFF},
    {F_NMUL, 0x7F800000, 0, 0, 0xFFC00000},                   /* fnmul: inf * 0's default NaN, negated */
    {F_NMUL, 0x7FC12345, 0x3F800000, 0, 0xFFC12345},
    {F_MADD, 0x7F800000, 0, 0x7FC12345, 0x7FC00000},          /* inf * 0 + a quiet NaN: the default NaN */
    {F_MADD, 0, 0xFF800000, 0xFFD55555, 0x7FC00000},
    {F_MADD, 0x7F800000, 0, 0x7FA00000, 0x7FE00000},          /* (+ a signalling one: that one) */
    {F_MADD, 0x7FC12345, 0x3F800000, 0xFFD55555, 0xFFD55555}, /* the addend first */
    {F_MADD, 0x7FC12345, 0x7F800001, 0xFFD55555, 0x7FC00001}, /* but a signalling NaN before it */
    {F_MSUB, 0x7FC12345, 0x3F800000, 0x3F800000, 0xFFC12345}, /* fmsub: Rn negated first */
    {F_NMADD, 0x3F800000, 0x3F800000, 0x7FC12345, 0xFFC12345}, /* fnmadd: the addend negated, */
    {F_NMADD, 0xFFD55555, 0x3F800000, 0x3F800000, 0x7FD55555}, /* and Rn */
    {F_NMSUB, 0x7F800000, 0, 0xFFD55555, 0x7FC00000},         /* fnmsub: the addend negated, still quiet */
    {F_NMSUB, 0x7FC12345, 0x3F800000, 0x3F800000, 0x7FC12345},
    {F_MAX, 0, 0x80000000, 0, 0},
    {F_MAX, 0x3F800000, 0x7FC12345, 0, 0x7FC12345},
    {F_MIN, 0xFFD55555, 0x7F800001, 0, 0x7FC00001},
    {F_MAXNM, 0x7FC12345, 0x3F800000, 0, 0x3F800000},         /* a quiet NaN against a number: the number */
    {F_MINNM, 0xFF800000, 0xFFD55555, 0, 0xFF800000},
    {F_MAXNM, 0x7FC12345, 0x7F800001, 0, 0x7FC00001},         /* against a signalling NaN: that one */
    {F_MINNM, 0xFFA00000, 0x3F800000, 0, 0xFFE00000},
    {F_MAXNM, 0xFFD55555, 0x7FC12345, 0, 0xFFD55555},         /* two quiet NaNs: the first */
    {F_MULX, 0xFF800000, 0, 0, 0xC0000000},                   /* fmulx: -inf * 0 is -2 */
    {F_MULX, 0x3F800000, 0xFFA00000, 0, 0xFFE00000},
    {F_RECPS, 0x7F800000, 0x80000000, 0, 0x40000000},         /* frecps: inf * -0 gives 2 */
    {F_RECPS, 0x7FC12345, 0x3F800000, 0, 0xFFC12345},         /* the first operand negated first */
    {F_RECPS, 0x3F800000, 0x7FC12345, 0, 0x7FC12345},
    {F_RSQRTS, 0, 0x7F800000, 0, 0x3FC00000},                 /* frsqrts: 0 * inf gives 1.5 */
    {F_RSQRTS, 0xFFA00000, 0x3F800000, 0, 0x7FE00000},
};
/* quiet 7ff8000000012345 and fffaaaaaaaaaaaaa, signalling 7ff0000000000001 and fff4000000000000 */
static const struct { int op; u64 a, b, c, want; } NAN64[] = {
    {F_SUB, 0x7FF0000000000000ull, 0x7FF0000000000000ull, 0, 0x7FF8000000000000ull},
    {F_SQRT, 0xBFF0000000000000ull, 0, 0, 0x7FF8000000000000ull},
    {F_ADD, 0xFFFAAAAAAAAAAAAAull, 0x7FF0000000000001ull, 0, 0x7FF8000000000001ull},
    {F_DIV, 0x3FF0000000000000ull, 0xFFFAAAAAAAAAAAAAull, 0, 0xFFFAAAAAAAAAAAAAull},
    {F_NMUL, 0, 0xFFF0000000000000ull, 0, 0xFFF8000000000000ull},
    {F_MADD, 0x7FF0000000000000ull, 0, 0x7FF8000000012345ull, 0x7FF8000000000000ull},
    {F_MSUB, 0xFFF4000000000000ull, 0x3FF0000000000000ull, 0x7FF8000000012345ull, 0x7FFC000000000000ull},
    {F_MAXNM, 0x3FF0000000000000ull, 0x7FF8000000012345ull, 0, 0x3FF0000000000000ull},
    {F_MIN, 0x7FF8000000012345ull, 0xFFFAAAAAAAAAAAAAull, 0, 0x7FF8000000012345ull},
    {F_MULX, 0x8000000000000000ull, 0xFFF0000000000000ull, 0, 0x4000000000000000ull},
    {F_RSQRTS, 0x7FF8000000012345ull, 0x3FF0000000000000ull, 0, 0xFFF8000000012345ull},
};

static f32 S(u32 x) { f32 f; memcpy(&f, &x, 4); return f; }
static u32 BS(f32 f) { u32 x; memcpy(&x, &f, 4); return x; }
static f64 D(u64 x) { f64 f; memcpy(&f, &x, 8); return f; }
static u64 BD(f64 f) { u64 x; memcpy(&x, &f, 8); return x; }

/* lift_simd.py's vfp: the lanes worked out natively, then rt_vnanS / D for NaN lanes. form 3: fmla (fmls with
   RT_VNAN_NEG), 2: fmul (fabd with RT_VNAN_ABS) */
static V128 lifted_v32(int form, int index, V128 a, V128 b, V128 d) {
    V128 r;
    memset(&r, 0, 16);
    for (int i = 0; i < 4; i++) {
        f32 x = a.f[i], y = b.f[index < 0 ? i : index];
        r.f[i] = (form & 3) == 3 ? fmaf(form & RT_VNAN_NEG ? -x : x, y, d.f[i]) : form & RT_VNAN_ABS ? fabsf(x - y) : x * y;
    }
    if (rt_nan_any32(&r)) r = rt_vnanS(r, a, b, d, form, index);
    return r;
}
static V128 lifted_v64(V128 a, V128 b, V128 d) {   /* fmla .2d */
    V128 r;
    for (int i = 0; i < 2; i++) r.g[i] = fma(a.g[i], b.g[i], d.g[i]);
    if (rt_nan_any64(&r)) r = rt_vnanD(r, a, b, d, 3, -1);
    return r;
}
static V128 v32(u32 x0, u32 x1, u32 x2, u32 x3) { V128 v; v.s[0] = x0; v.s[1] = x1; v.s[2] = x2; v.s[3] = x3; return v; }
static V128 v64(u64 x0, u64 x1) { V128 v; v.d[0] = x0; v.d[1] = x1; return v; }
#define CHECK_V(got, want) do { V128 g_ = (got), w_ = (want); if (memcmp(&g_, &w_, 16)) { \
    printf("line %d: %016llx%016llx, want %016llx%016llx\n", __LINE__, (unsigned long long)g_.d[1], \
           (unsigned long long)g_.d[0], (unsigned long long)w_.d[1], (unsigned long long)w_.d[0]); bad++; } } while (0)

static void nan_rules(void) {
    for (unsigned i = 0; i < sizeof NAN32 / sizeof NAN32[0]; i++) {
        u32 got = BS(lifted32(NAN32[i].op, S(NAN32[i].a), S(NAN32[i].b), S(NAN32[i].c)));
        if (got != NAN32[i].want) { printf("NAN32[%u]: %08x, want %08x\n", i, got, NAN32[i].want); bad++; }
#if defined(__aarch64__)
        got = BS(hw32(NAN32[i].op, S(NAN32[i].a), S(NAN32[i].b), S(NAN32[i].c)));
        if (got != NAN32[i].want) { printf("NAN32[%u]: the instruction's %08x, want %08x\n", i, got, NAN32[i].want); bad++; }
#endif
    }
    for (unsigned i = 0; i < sizeof NAN64 / sizeof NAN64[0]; i++) {
        u64 got = BD(lifted64(NAN64[i].op, D(NAN64[i].a), D(NAN64[i].b), D(NAN64[i].c)));
        if (got != NAN64[i].want) { printf("NAN64[%u]: %016llx, want %016llx\n", i, (unsigned long long)got, (unsigned long long)NAN64[i].want); bad++; }
#if defined(__aarch64__)
        got = BD(hw64(NAN64[i].op, D(NAN64[i].a), D(NAN64[i].b), D(NAN64[i].c)));
        if (got != NAN64[i].want) { printf("NAN64[%u]: the instruction's %016llx, want %016llx\n", i, (unsigned long long)got, (unsigned long long)NAN64[i].want); bad++; }
#endif
    }

    /* fmls: Rn's lane negated first; the addend first; inf * 0 + a quiet NaN; a lane that is not NaN */
    V128 a = v32(0x7FC12345, 0x3F800000, 0x7F800000, 0x3F800000), b = v32(0x3F800000, 0x7F800001, 0, 0x3F800000);
    V128 d = v32(0x3F800000, 0x7FC12345, 0xFFD55555, 0x3F800000);
    V128 fmls = v32(0xFFC12345, 0x7FC00001, 0x7FC00000, 0);
    CHECK_V(lifted_v32(3 | RT_VNAN_NEG, -1, a, b, d), fmls);
    /* fabd: the NaN's sign cleared after */
    V128 a2 = v32(0xFFD55555, 0x3F800000, 0xFFA00000, 0x40000000), b2 = v32(0x3F800000, 0xFFD55555, 0x3F800000, 0x40400000);
    V128 fabd = v32(0x7FD55555, 0x7FD55555, 0x7FE00000, 0x3F800000);
    CHECK_V(lifted_v32(2 | RT_VNAN_ABS, -1, a2, b2, d), fabd);
    /* fmul by element 1 */
    V128 a3 = v32(0x3F800000, 0xFFD55555, 0x7F800001, 0x7F800000), b3 = v32(0x40000000, 0x7FC12345, 0, 0);
    V128 fmul = v32(0x7FC12345, 0xFFD55555, 0x7FC00001, 0x7FC12345);
    CHECK_V(lifted_v32(2, 1, a3, b3, d), fmul);
    /* fmla .2d */
    V128 a4 = v64(0x7FF0000000000000ull, 0x7FF8000000012345ull), b4 = v64(0, 0x3FF0000000000000ull);
    V128 d4 = v64(0xFFFAAAAAAAAAAAAAull, 0x3FF0000000000000ull);
    V128 fmla = v64(0x7FF8000000000000ull, 0x7FF8000000012345ull);
    CHECK_V(lifted_v64(a4, b4, d4), fmla);
#if defined(__aarch64__)
    RtV4f x, y, z;
    RtV2d x2, y2, z2;
    memcpy(&x, &a, 16); memcpy(&y, &b, 16); memcpy(&z, &d, 16);
    __asm__("fmls %0.4s, %1.4s, %2.4s" : "+w"(z) : "w"(x), "w"(y));
    memcpy(&a, &z, 16); CHECK_V(a, fmls);
    memcpy(&x, &a2, 16); memcpy(&y, &b2, 16);
    __asm__("fabd %0.4s, %1.4s, %2.4s" : "=w"(z) : "w"(x), "w"(y));
    memcpy(&a, &z, 16); CHECK_V(a, fabd);
    memcpy(&x, &a3, 16); memcpy(&y, &b3, 16);
    __asm__("fmul %0.4s, %1.4s, %2.s[1]" : "=w"(z) : "w"(x), "w"(y));
    memcpy(&a, &z, 16); CHECK_V(a, fmul);
    memcpy(&x2, &a4, 16); memcpy(&y2, &b4, 16); memcpy(&z2, &d4, 16);
    __asm__("fmla %0.2d, %1.2d, %2.2d" : "+w"(z2) : "w"(x2), "w"(y2));
    memcpy(&a, &z2, 16); CHECK_V(a, fmla);
#endif
}

int main(void) {
    for (u32 a = 0; a < 4096; a++)   /* (frsqrte gives it 128 .. 511) */
        if (rt_rsqrt_est(a) != rsqrt_est_loop(a)) { printf("rt_rsqrt_est(%u) = %u, the loop's %u\n", a, rt_rsqrt_est(a), rsqrt_est_loop(a)); bad++; }

    /* the check values: "123456789" from all ones, the result inverted */
    const char *s = "123456789";
    u32 b = ~0u, cb = ~0u, bits = ~0u, cbits = ~0u, h = ~0u, ch = ~0u, w = ~0u, cw = ~0u, x = ~0u, cx = ~0u;
    u64 lo = 0;
    for (int i = 0; i < 8; i++) lo |= (u64)(u8)s[i] << (8 * i);
    for (int i = 0; i < 9; i++) {
        b = rt_crc32b(b, (u8)s[i]); cb = rt_crc32cb(cb, (u8)s[i]);
        bits = rt_crc32_bits(bits, (u8)s[i], 8, RT_CRC32_POLY); cbits = rt_crc32_bits(cbits, (u8)s[i], 8, RT_CRC32C_POLY);
    }
    for (int i = 0; i < 4; i++) { h = rt_crc32h(h, (u16)(lo >> (16 * i))); ch = rt_crc32ch(ch, (u16)(lo >> (16 * i))); }
    for (int i = 0; i < 2; i++) { w = rt_crc32w(w, (u32)(lo >> (32 * i))); cw = rt_crc32cw(cw, (u32)(lo >> (32 * i))); }
    x = rt_crc32x(x, lo); cx = rt_crc32cx(cx, lo);
    h = rt_crc32b(h, '9'); ch = rt_crc32cb(ch, '9'); w = rt_crc32b(w, '9'); cw = rt_crc32cb(cw, '9'); x = rt_crc32b(x, '9'); cx = rt_crc32cb(cx, '9');
    CHECK(~b == 0xCBF43926u && ~bits == 0xCBF43926u && ~h == 0xCBF43926u && ~w == 0xCBF43926u && ~x == 0xCBF43926u);
    CHECK(~cb == 0xE3069283u && ~cbits == 0xE3069283u && ~ch == 0xE3069283u && ~cw == 0xE3069283u && ~cx == 0xE3069283u);

    /* each size against the definition, the same operands for both polynomials */
    u64 st = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 200000; i++) {
        u32 acc = (u32)next(&st);
        u64 v = next(&st);
        if (i < 64) { acc = i & 1 ? ~0u : 0; v = i & 2 ? ~0ull : i & 4 ? 1ull << (i >> 3) : 0; }
        CHECK(rt_crc32b(acc, (u8)v) == rt_crc32_bits(acc, (u8)v, 8, RT_CRC32_POLY));
        CHECK(rt_crc32h(acc, (u16)v) == rt_crc32_bits(acc, (u16)v, 16, RT_CRC32_POLY));
        CHECK(rt_crc32w(acc, (u32)v) == rt_crc32_bits(acc, (u32)v, 32, RT_CRC32_POLY));
        CHECK(rt_crc32x(acc, v) == rt_crc32_bits(acc, v, 64, RT_CRC32_POLY));
        CHECK(rt_crc32cb(acc, (u8)v) == rt_crc32_bits(acc, (u8)v, 8, RT_CRC32C_POLY));
        CHECK(rt_crc32ch(acc, (u16)v) == rt_crc32_bits(acc, (u16)v, 16, RT_CRC32C_POLY));
        CHECK(rt_crc32cw(acc, (u32)v) == rt_crc32_bits(acc, (u32)v, 32, RT_CRC32C_POLY));
        CHECK(rt_crc32cx(acc, v) == rt_crc32_bits(acc, v, 64, RT_CRC32C_POLY));
        if (bad > 20) break;
    }
    nan_rules();
#if defined(__ARM_FEATURE_CRC32)
    puts("crc32: arm; crc32c: arm");
#elif defined(__SSE4_2__)
    CHECK(rt_crc32_tables() != 0);
    puts("crc32: tables; crc32c: sse4.2");
#else
    CHECK(rt_crc32_tables() != 0);
    puts("crc32: tables; crc32c: bitwise");
#endif
    puts(bad ? "FAILED" : "ok");
    return bad != 0;
}
'''

# the CRC instructions each build uses
EXPECTED = {'windows': 'crc32: tables; crc32c: sse4.2', 'linux': 'crc32: tables; crc32c: sse4.2',
            'rosetta': 'crc32: tables; crc32c: sse4.2', 'mac': 'crc32: arm; crc32c: arm'}
HOST = {'win32': 'windows', 'darwin': 'mac'}.get(sys.platform, 'linux')


def rosetta():
    """Whether this is an Apple silicon Mac that runs x86-64 programs."""
    if sys.platform != 'darwin' or platform.machine() != 'arm64' or not shutil.which('clang'):
        return False
    try:
        return subprocess.run(['arch', '-x86_64', '/usr/bin/true'], capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


class HelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp(prefix='recomp-helpers-'))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def build_and_run(self, name):
        llvm = os.environ.get('SSBU_LLVM_BIN')
        try:
            tc = difftest.toolchain(name)
        except SystemExit as e:
            self.skipTest(f'{name}: {e}')
        finally:   # (the rosetta toolchain sets it for tools/linux/toolchain.py; other tests see it as it was)
            if llvm is None:
                os.environ.pop('SSBU_LLVM_BIN', None)
        out = self.dir / name
        out.mkdir()
        builds = [('c', 'O2'), ('c', 'O0')] + ([('cpp', 'O2')] if name != 'windows' else [])
        for lang, level in builds:
            with self.subTest(toolchain=name, lang=lang, level=level):
                src = out / f'check_{level}.{lang}'
                src.write_text(CHECK, encoding='utf-8')
                obj, exe = out / f'check_{lang}_{level}{tc.obj}', out / f'check_{lang}_{level}{tc.exe}'
                cmd = tc.compile_cmd(str(src), str(obj), tc.opts[level])
                if lang == 'cpp':
                    cmd[1:1] = ['-x', 'c++', '-std=gnu++20', '-fexceptions']
                for step in (cmd, tc.link_cmd([str(obj)], str(exe))):
                    r = subprocess.run(step, capture_output=True, text=True, timeout=300)
                    self.assertEqual(r.returncode, 0, f'{" ".join(step)}\n{r.stdout}{r.stderr}')
                r = subprocess.run([*tc.run, str(exe)], capture_output=True, text=True, timeout=300)
                self.assertEqual((r.returncode, r.stdout.splitlines()[-2:]), (0, [EXPECTED[name], 'ok']), r.stdout + r.stderr)

    def test_this_os(self):
        self.build_and_run(HOST)

    @unittest.skipUnless(rosetta(), 'needs an Apple silicon Mac with Rosetta 2 and clang')
    def test_x86_64_under_rosetta(self):
        self.build_and_run('rosetta')


if __name__ == '__main__':
    unittest.main()
