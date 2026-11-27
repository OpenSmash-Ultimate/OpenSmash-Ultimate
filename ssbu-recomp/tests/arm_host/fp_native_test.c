/* Floating-point helpers of runtime.h / fpexact.h / neon.h against the AArch64 instructions themselves (inline asm),
 * bit for bit, on an AArch64 host (FPCR = 0 as on Linux threads: round to nearest, no flush-to-zero, no default NaN).
 *
 *   cc -O1 -fno-strict-aliasing -ffp-contract=off -I../../runtime fp_native_test.c -o fp_native_test -lm -lpthread
 *   ./fp_native_test [random pairs = 20000000] [--exhaustive]
 *
 * Checked, each as software version (= what an x86 host runs) and as the routed version (= what an ARM host runs):
 *   FRECPS / FRSQRTS (s, d, and the 4s vector form through neon.h v_f3v)   special x special pairs + random pairs
 *     (exception: an x86 host does not run fp_step*_soft yet; neon.h keeps its fma of before there, so x86 results do
 *     not change. The soft versions are checked here so that a later change can switch x86 to them.)
 *   FRECPE / FRSQRTE (s, d)                                              specials + random (+ all 2^32 floats)
 *   FCVT single/double -> half, half -> single/double                    all 65536 halves, specials + random
 *   FMAX / FMIN / FMAXNM / FMINNM (s, d)                                 special x special + random pairs
 *   FCVTZS / FCVTZU to 32 / 64 bits (f2s32 / f2u32 / f2s64 / f2u64)      boundaries + random doubles
 *   fnan32 / fnan64 on lifted-style arithmetic (fadd, fmul, fdiv, fsub, fsqrt, fmadd, fneg, fabs) vs the
 *   instruction, with NaN / infinity / zero operands
 * Every run also prints a hash of the software results over the same deterministic inputs: build with
 * -DSOFT_ONLY on an x86 host and compare (run_frame.sh does), to check the x86 build computes them the same. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#if !RECOMP_A64_NATIVE
/* runtime.h: without RECOMP_A64_NATIVE the software versions are the helpers themselves (RT_SOFT) */
#define f2s32_soft f2s32
#define f2u32_soft f2u32
#define f2s64_soft f2s64
#define f2u64_soft f2u64
#define recpe_f_soft recpe_f
#define recpe_d_soft recpe_d
#define rsqrte_f_soft rsqrte_f
#define rsqrte_d_soft rsqrte_d
#define f32_to_half_soft f32_to_half
#define f64_to_half_soft f64_to_half
#define half_to_f32_soft half_to_f32
#define half_to_f64_soft half_to_f64
#define fp_maxmin32_soft fp_maxmin32
#define fp_maxmin64_soft fp_maxmin64
#ifndef SOFT_ONLY
#define SOFT_ONLY 1
#endif
#endif

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static inline uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static inline uint32_t fb(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static inline uint64_t db(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static inline float bf(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline double bd(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }

static uint64_t hash = 1469598103934665603ull, line_hash[1024];
static inline void Hl(int line, uint64_t v) { hash = (hash ^ v) * 1099511628211ull; line_hash[line & 1023] = (line_hash[line & 1023] ^ v) * 1099511628211ull; }
#define H(v) Hl(__LINE__, (v))       /* per source line too: --hash-lines tells which helper differs between hosts */

static long n_checks, n_fail;
static void fail(const char *what, uint64_t a, uint64_t b, uint64_t got, uint64_t want)
{
    n_fail++;
    if (n_fail <= 40) printf("FAIL %-22s a=%016llx b=%016llx got=%016llx want=%016llx\n", what, (unsigned long long)a,
                             (unsigned long long)b, (unsigned long long)got, (unsigned long long)want);
}
#define CHECK(what, a, b, got, want) do { n_checks++; if ((uint64_t)(got) != (uint64_t)(want)) fail(what, a, b, got, want); } while (0)

#ifndef SOFT_ONLY
#define A2S(name, insn) static inline uint32_t name(uint32_t a, uint32_t b) { float r; __asm__ volatile(insn " %s0, %s1, %s2" : "=w"(r) : "w"(bf(a)), "w"(bf(b))); return fb(r); }
#define A2D(name, insn) static inline uint64_t name(uint64_t a, uint64_t b) { double r; __asm__ volatile(insn " %d0, %d1, %d2" : "=w"(r) : "w"(bd(a)), "w"(bd(b))); return db(r); }
#define A1S(name, insn) static inline uint32_t name(uint32_t a) { float r; __asm__ volatile(insn " %s0, %s1" : "=w"(r) : "w"(bf(a))); return fb(r); }
#define A1D(name, insn) static inline uint64_t name(uint64_t a) { double r; __asm__ volatile(insn " %d0, %d1" : "=w"(r) : "w"(bd(a))); return db(r); }
A2S(i_frecps_s, "frecps") A2S(i_frsqrts_s, "frsqrts") A2D(i_frecps_d, "frecps") A2D(i_frsqrts_d, "frsqrts")
A2S(i_fmax_s, "fmax") A2S(i_fmin_s, "fmin") A2S(i_fmaxnm_s, "fmaxnm") A2S(i_fminnm_s, "fminnm")
A2D(i_fmax_d, "fmax") A2D(i_fmin_d, "fmin") A2D(i_fmaxnm_d, "fmaxnm") A2D(i_fminnm_d, "fminnm")
A2S(i_fadd_s, "fadd") A2S(i_fsub_s, "fsub") A2S(i_fmul_s, "fmul") A2S(i_fdiv_s, "fdiv")
A2D(i_fadd_d, "fadd") A2D(i_fsub_d, "fsub") A2D(i_fmul_d, "fmul") A2D(i_fdiv_d, "fdiv")
A1S(i_frecpe_s, "frecpe") A1S(i_frsqrte_s, "frsqrte") A1D(i_frecpe_d, "frecpe") A1D(i_frsqrte_d, "frsqrte")
A1S(i_fsqrt_s, "fsqrt") A1S(i_fneg_s, "fneg") A1S(i_fabs_s, "fabs") A1D(i_fsqrt_d, "fsqrt") A1D(i_fneg_d, "fneg")
static inline uint32_t i_fmadd_s(uint32_t a, uint32_t b, uint32_t c) { float r; __asm__ volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(bf(a)), "w"(bf(b)), "w"(bf(c))); return fb(r); }
static inline uint32_t i_fmsub_s(uint32_t a, uint32_t b, uint32_t c) { float r; __asm__ volatile("fmsub %s0, %s1, %s2, %s3" : "=w"(r) : "w"(bf(a)), "w"(bf(b)), "w"(bf(c))); return fb(r); }
static inline uint16_t i_f2h(uint32_t a) { float r; __asm__ volatile("fcvt %h0, %s1" : "=w"(r) : "w"(bf(a))); return (uint16_t)fb(r); }
static inline uint16_t i_d2h(uint64_t a) { float r; __asm__ volatile("fcvt %h0, %d1" : "=w"(r) : "w"(bd(a))); return (uint16_t)fb(r); }
static inline uint32_t i_h2f(uint16_t h) { float r; __asm__ volatile("fcvt %s0, %h1" : "=w"(r) : "w"(bf(h))); return fb(r); }
static inline uint64_t i_h2d(uint16_t h) { double r; __asm__ volatile("fcvt %d0, %h1" : "=w"(r) : "w"(bf(h))); return db(r); }
static inline uint64_t i_cvt(double v, int k)
{
    uint64_t r;
    switch (k) {
    case 0: __asm__ volatile("fcvtzs %w0, %d1" : "=r"(r) : "w"(v)); return (uint32_t)r;
    case 1: __asm__ volatile("fcvtzu %w0, %d1" : "=r"(r) : "w"(v)); return (uint32_t)r;
    case 2: __asm__ volatile("fcvtzs %x0, %d1" : "=r"(r) : "w"(v)); return r;
    default: __asm__ volatile("fcvtzu %x0, %d1" : "=r"(r) : "w"(v)); return r;
    }
}
#endif

/* ------------------------------------------------------------------ inputs */
static uint32_t SPEC_S[160];
static int nspec_s;
static uint64_t SPEC_D[160];
static int nspec_d;

static void add_s(uint32_t v) { if (nspec_s < 160) { SPEC_S[nspec_s++] = v; } if (nspec_s < 160) SPEC_S[nspec_s++] = v ^ 0x80000000u; }
static void add_d(uint64_t v) { if (nspec_d < 160) { SPEC_D[nspec_d++] = v; } if (nspec_d < 160) SPEC_D[nspec_d++] = v ^ 0x8000000000000000ull; }

static void make_specials(void)
{
    uint32_t s[] = { 0, 1, 2, 0x007fffff, 0x00400000, 0x00200000, 0x00100000, 0x00800000, 0x00800001, 0x3f800000,
                     0x40000000, 0x40400000, 0x3f000000, 0x3fc00000, 0x7f7fffff, 0x7f000000, 0x5f800000, 0x5f7fffff,
                     0x1f800000, 0x20000000, 0x7f800000, 0x7fc00000, 0x7fc00001, 0x7fffffff, 0x7f800001, 0x7fa00000,
                     0x7fbfffff, 0x3f7fffff, 0x3f800001, 0x3effffff, 0x7e800000, 0x00000003, 0x0080ffff, 0x3e800000,
                     0x41200000, 0x4f000000, 0x4f800000, 0x5f000000, 0x7e7fffff, 0x01000000 };
    for (unsigned i = 0; i < sizeof s / sizeof *s; i++) add_s(s[i]);
    uint64_t d[] = { 0, 1, 2, 0x000fffffffffffffull, 0x0008000000000000ull, 0x0004000000000000ull, 0x0010000000000000ull,
                     0x0010000000000001ull, 0x3ff0000000000000ull, 0x4000000000000000ull, 0x4008000000000000ull,
                     0x3fe0000000000000ull, 0x3ff8000000000000ull, 0x7fefffffffffffffull, 0x7fe0000000000000ull,
                     0x5ff0000000000000ull, 0x5fefffffffffffffull, 0x1ff0000000000000ull, 0x2000000000000000ull,
                     0x7ff0000000000000ull, 0x7ff8000000000000ull, 0x7ff8000000000001ull, 0x7fffffffffffffffull,
                     0x7ff0000000000001ull, 0x7ff4000000000000ull, 0x7ff7ffffffffffffull, 0x3fefffffffffffffull,
                     0x3ff0000000000001ull, 0x41dfffffffc00000ull, 0x41e0000000000000ull, 0x41efffffffe00000ull,
                     0x41f0000000000000ull, 0x43e0000000000000ull, 0x43dfffffffffffffull, 0x43f0000000000000ull,
                     0x43efffffffffffffull, 0xc1e0000000200000ull, 0x3fe0000000000001ull, 0x47efffffe0000000ull,
                     0x380fffffffffffffull, 0x3810000000000000ull };
    for (unsigned i = 0; i < sizeof d / sizeof *d; i++) add_d(d[i]);
}
/* random float bits, with a bias towards the interesting parts of the range */
static uint32_t rand_s(void)
{
    uint64_t r = rnd();
    switch (r & 7) {
    case 0: return SPEC_S[(r >> 8) % (unsigned)nspec_s];
    case 1: return (uint32_t)(r >> 32) & 0x807fffffu;                                   /* subnormal / zero */
    case 2: return ((uint32_t)(r >> 32) & 0x80000000u) | 0x7f800000u | ((uint32_t)(r >> 9) & 0x7fffffu);   /* inf/NaN */
    case 3: return ((uint32_t)(r >> 32) & 0x807fffffu) | ((uint32_t)(0x7e + ((r >> 12) & 3)) << 23);       /* ~0.5..4 */
    default: return (uint32_t)(r >> 32);
    }
}
static uint64_t rand_d(void)
{
    uint64_t r = rnd(), m = rnd();
    switch (r & 7) {
    case 0: return SPEC_D[(r >> 8) % (unsigned)nspec_d];
    case 1: return m & 0x800fffffffffffffull;
    case 2: return (m & 0x800fffffffffffffull) | 0x7ff0000000000000ull;
    case 3: return (m & 0x800fffffffffffffull) | ((uint64_t)(0x3fe + ((r >> 12) & 3)) << 52);
    default: return m;
    }
}
/* pairs whose product is near 2 / 3 (cancellation) or near the overflow threshold of the step results */
static void rand_pair_s(uint32_t *a, uint32_t *b)
{
    uint64_t r = rnd();
    *a = rand_s();
    switch (r & 3) {
    case 0: { float x = bf(*a); float t = ((r >> 2) & 1 ? 3.0f : 2.0f) / x;           /* NaN results differ by host: not used */
              *b = t != t ? rand_s() : fb(t) + (uint32_t)((int)((r >> 3) & 7) - 3); break; }
    case 1: { uint32_t ea = (*a >> 23) & 0xff; int eb = 254 + 127 - (int)ea + (int)((r >> 4) & 3) - 1;
              if (eb > 0 && eb < 255) *b = ((uint32_t)(r >> 32) & 0x807fffffu) | ((uint32_t)eb << 23); else *b = rand_s(); break; }
    default: *b = rand_s();
    }
}
static void rand_pair_d(uint64_t *a, uint64_t *b)
{
    uint64_t r = rnd();
    *a = rand_d();
    switch (r & 3) {
    case 0: { double t = ((r >> 2) & 1 ? 3.0 : 2.0) / bd(*a);
              *b = t != t ? rand_d() : db(t) + (uint64_t)((int64_t)((r >> 3) & 7) - 3); break; }
    case 1: { uint64_t ea = (*a >> 52) & 0x7ff; int64_t eb = 2046 + 1023 - (int64_t)ea + (int64_t)((r >> 4) & 3) - 1;
              if (eb > 0 && eb < 2047) *b = (rnd() & 0x800fffffffffffffull) | ((uint64_t)eb << 52); else *b = rand_d(); break; }
    default: *b = rand_d();
    }
}

/* ------------------------------------------------------------------ checks */
static void pair_s(uint32_t a, uint32_t b)
{
    uint32_t s0 = fb(fp_step32_soft(bf(a), bf(b), 0)), s1 = fb(fp_step32_soft(bf(a), bf(b), 1));
    uint32_t m[4];
    for (int k = 0; k < 4; k++) m[k] = fb(fp_maxmin32_soft(bf(a), bf(b), k >> 1, k & 1));
    uint32_t c3 = rand_s();                  /* drawn on every host: the inputs must not depend on SOFT_ONLY */
    H(s0); H(s1); H(m[0]); H(m[1]); H(m[2]); H(m[3]);
#ifndef SOFT_ONLY
    CHECK("frecps.s soft", a, b, s0, i_frecps_s(a, b));
    CHECK("frsqrts.s soft", a, b, s1, i_frsqrts_s(a, b));
    CHECK("frecps.s routed", a, b, fb(fp_recps32(bf(a), bf(b))), i_frecps_s(a, b));
    CHECK("frsqrts.s routed", a, b, fb(fp_rsqrts32(bf(a), bf(b))), i_frsqrts_s(a, b));
    CHECK("fmax.s soft", a, b, m[0], i_fmax_s(a, b));
    CHECK("fmaxnm.s soft", a, b, m[1], i_fmaxnm_s(a, b));
    CHECK("fmin.s soft", a, b, m[2], i_fmin_s(a, b));
    CHECK("fminnm.s soft", a, b, m[3], i_fminnm_s(a, b));
    CHECK("fmax.s routed", a, b, fb(fp_maxmin32(bf(a), bf(b), 0, 0)), i_fmax_s(a, b));
    CHECK("fmaxnm.s routed", a, b, fb(fp_maxmin32(bf(a), bf(b), 0, 1)), i_fmaxnm_s(a, b));
    CHECK("fmin.s routed", a, b, fb(fp_maxmin32(bf(a), bf(b), 1, 0)), i_fmin_s(a, b));
    CHECK("fminnm.s routed", a, b, fb(fp_maxmin32(bf(a), bf(b), 1, 1)), i_fminnm_s(a, b));
    /* lifted scalar arithmetic: '{ float t = fnan32(a + b); ... }' must be the instruction, NaNs included */
    float fa = bf(a), fbv = bf(b);
    CHECK("fadd.s lifted", a, b, fb(fnan32(fa + fbv)), i_fadd_s(a, b));
    CHECK("fsub.s lifted", a, b, fb(fnan32(fa - fbv)), i_fsub_s(a, b));
    CHECK("fmul.s lifted", a, b, fb(fnan32(fa * fbv)), i_fmul_s(a, b));
    CHECK("fdiv.s lifted", a, b, fb(fnan32(fa / fbv)), i_fdiv_s(a, b));
    CHECK("fmadd.s lifted", a, b, fb(fnan32(fmaf(fa, fbv, bf(c3)))), i_fmadd_s(a, b, c3));
    CHECK("fmsub.s lifted", a, b, fb(fnan32(fmaf(-fa, fbv, bf(c3)))), i_fmsub_s(a, b, c3));
#endif
}
static void pair_d(uint64_t a, uint64_t b)
{
    uint64_t s0 = db(fp_step64_soft(bd(a), bd(b), 0)), s1 = db(fp_step64_soft(bd(a), bd(b), 1));
    uint64_t m[4];
    for (int k = 0; k < 4; k++) m[k] = db(fp_maxmin64_soft(bd(a), bd(b), k >> 1, k & 1));
    H(s0); H(s1); H(m[0]); H(m[1]); H(m[2]); H(m[3]);
#ifndef SOFT_ONLY
    CHECK("frecps.d soft", a, b, s0, i_frecps_d(a, b));
    CHECK("frsqrts.d soft", a, b, s1, i_frsqrts_d(a, b));
    CHECK("frecps.d routed", a, b, db(fp_recps64(bd(a), bd(b))), i_frecps_d(a, b));
    CHECK("frsqrts.d routed", a, b, db(fp_rsqrts64(bd(a), bd(b))), i_frsqrts_d(a, b));
    CHECK("fmax.d soft", a, b, m[0], i_fmax_d(a, b));
    CHECK("fmaxnm.d soft", a, b, m[1], i_fmaxnm_d(a, b));
    CHECK("fmin.d soft", a, b, m[2], i_fmin_d(a, b));
    CHECK("fminnm.d soft", a, b, m[3], i_fminnm_d(a, b));
    CHECK("fmax.d routed", a, b, db(fp_maxmin64(bd(a), bd(b), 0, 0)), i_fmax_d(a, b));
    CHECK("fminnm.d routed", a, b, db(fp_maxmin64(bd(a), bd(b), 1, 1)), i_fminnm_d(a, b));
    double da = bd(a), dbv = bd(b);
    CHECK("fadd.d lifted", a, b, db(fnan64(da + dbv)), i_fadd_d(a, b));
    CHECK("fsub.d lifted", a, b, db(fnan64(da - dbv)), i_fsub_d(a, b));
    CHECK("fmul.d lifted", a, b, db(fnan64(da * dbv)), i_fmul_d(a, b));
    CHECK("fdiv.d lifted", a, b, db(fnan64(da / dbv)), i_fdiv_d(a, b));
#endif
}
static void one_s(uint32_t a)
{
    uint32_t e = fb(recpe_f_soft(bf(a))), q = fb(rsqrte_f_soft(bf(a)));
    uint16_t h = f32_to_half_soft(bf(a));
    H(e); H(q); H(h);
#ifndef SOFT_ONLY
    CHECK("frecpe.s soft", a, 0, e, i_frecpe_s(a));
    CHECK("frsqrte.s soft", a, 0, q, i_frsqrte_s(a));
    CHECK("frecpe.s routed", a, 0, fb(recpe_f(bf(a))), i_frecpe_s(a));
    CHECK("frsqrte.s routed", a, 0, fb(rsqrte_f(bf(a))), i_frsqrte_s(a));
    CHECK("fcvt h,s soft", a, 0, h, i_f2h(a));
    CHECK("fcvt h,s routed", a, 0, f32_to_half(bf(a)), i_f2h(a));
    CHECK("fsqrt.s lifted", a, 0, fb(fnan32(sqrtf(bf(a)))), i_fsqrt_s(a));
    CHECK("fneg.s lifted", a, 0, fb(fnan32(-bf(a))), i_fneg_s(a));
    CHECK("fabs.s lifted", a, 0, fb(fnan32(fabsf(bf(a)))), i_fabs_s(a));
#endif
}
static void one_d(uint64_t a)
{
    uint64_t e = db(recpe_d_soft(bd(a))), q = db(rsqrte_d_soft(bd(a)));
    uint16_t h = f64_to_half_soft(bd(a));
    H(e); H(q); H(h);
    for (int k = 0; k < 4; k++) H(k == 0 ? f2s32_soft(bd(a)) : k == 1 ? f2u32_soft(bd(a)) : k == 2 ? f2s64_soft(bd(a)) : f2u64_soft(bd(a)));
#ifndef SOFT_ONLY
    CHECK("frecpe.d soft", a, 0, e, i_frecpe_d(a));
    CHECK("frsqrte.d soft", a, 0, q, i_frsqrte_d(a));
    CHECK("frecpe.d routed", a, 0, db(recpe_d(bd(a))), i_frecpe_d(a));
    CHECK("frsqrte.d routed", a, 0, db(rsqrte_d(bd(a))), i_frsqrte_d(a));
    CHECK("fcvt h,d soft", a, 0, h, i_d2h(a));
    CHECK("fcvt h,d routed", a, 0, f64_to_half(bd(a)), i_d2h(a));
    CHECK("fsqrt.d lifted", a, 0, db(fnan64(sqrt(bd(a)))), i_fsqrt_d(a));
    CHECK("fneg.d lifted", a, 0, db(fnan64(-bd(a))), i_fneg_d(a));
    double v = bd(a);
    CHECK("fcvtzs w soft", a, 0, f2s32_soft(v), i_cvt(v, 0));
    CHECK("fcvtzu w soft", a, 0, f2u32_soft(v), i_cvt(v, 1));
    CHECK("fcvtzs x soft", a, 0, f2s64_soft(v), i_cvt(v, 2));
    CHECK("fcvtzu x soft", a, 0, f2u64_soft(v), i_cvt(v, 3));
    /* the lifter calls f2xx on an already rounded value: f2s32(trunc(x)) etc. */
    double t = trunc(v);
    CHECK("fcvtzs w routed", a, 0, f2s32(t), i_cvt(t, 0));
    CHECK("fcvtzu w routed", a, 0, f2u32(t), i_cvt(t, 1));
    CHECK("fcvtzs x routed", a, 0, f2s64(t), i_cvt(t, 2));
    CHECK("fcvtzu x routed", a, 0, f2u64(t), i_cvt(t, 3));
#endif
}
/* the vector path of neon.h: v_f3v(F_RECPS / F_RSQRTS) on 4 lanes against FRECPS / FRSQRTS Vd.4S. The hash takes the
 * software versions lane by lane (the same on every host); v_f3v itself is checked on AArch64 only (on x86-64 it is the
 * fma of before, see fpexact.h). */
static void vec_s(const uint32_t *a, const uint32_t *b)
{
    for (int i = 0; i < 4; i++) { H(fb(fp_step32_soft(bf(a[i]), bf(b[i]), 0))); H(fb(fp_step32_soft(bf(a[i]), bf(b[i]), 1))); }
#ifndef SOFT_ONLY
    V128 n, m, d, r0, r1;
    memcpy(n.w, a, 16); memcpy(m.w, b, 16);
    d = n; v_f3v(&d, &n, &m, F_RECPS, 0, 4); r0 = d;
    d = n; v_f3v(&d, &n, &m, F_RSQRTS, 0, 4); r1 = d;
    float32x4_t vn, vm, x0, x1;
    memcpy(&vn, a, 16); memcpy(&vm, b, 16);
    __asm__ volatile("frecps %0.4s, %1.4s, %2.4s" : "=w"(x0) : "w"(vn), "w"(vm));
    __asm__ volatile("frsqrts %0.4s, %1.4s, %2.4s" : "=w"(x1) : "w"(vn), "w"(vm));
    uint32_t w0[4], w1[4];
    memcpy(w0, &x0, 16); memcpy(w1, &x1, 16);
    for (int i = 0; i < 4; i++) {
        CHECK("v_f3v F_RECPS 4s", a[i], b[i], r0.w[i], w0[i]);
        CHECK("v_f3v F_RSQRTS 4s", a[i], b[i], r1.w[i], w1[i]);
    }
#endif
}

#ifndef SOFT_ONLY
/* all 2^32 single-precision inputs of the unary helpers, on all cores */
static long ex_fail[16];
static void *exhaustive_part(void *arg)
{
    long k = (long)(intptr_t)arg, bad = 0;
    uint64_t lo = (uint64_t)k << 29, hi = lo + (1ull << 29);
    for (uint64_t i = lo; i < hi; i++) {
        uint32_t a = (uint32_t)i;
        bad += fb(recpe_f_soft(bf(a))) != i_frecpe_s(a);
        bad += fb(rsqrte_f_soft(bf(a))) != i_frsqrte_s(a);
        bad += f32_to_half_soft(bf(a)) != i_f2h(a);
    }
    ex_fail[k] = bad;
    return NULL;
}
#endif

int main(int argc, char **argv)
{
    long npairs = 20000000;
    int exhaustive = 0, hash_lines = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exhaustive")) exhaustive = 1;
        else if (!strcmp(argv[i], "--hash-lines")) hash_lines = 1;
        else npairs = atol(argv[i]);
    }
    make_specials();
    /* special x special */
    for (int i = 0; i < nspec_s; i++) for (int j = 0; j < nspec_s; j++) pair_s(SPEC_S[i], SPEC_S[j]);
    for (int i = 0; i < nspec_d; i++) for (int j = 0; j < nspec_d; j++) pair_d(SPEC_D[i], SPEC_D[j]);
    for (int i = 0; i < nspec_s; i++) one_s(SPEC_S[i]);
    for (int i = 0; i < nspec_d; i++) one_d(SPEC_D[i]);
    /* every half value */
    for (uint32_t h = 0; h < 65536; h++) {
        uint32_t f = fb(half_to_f32_soft((uint16_t)h));
        uint64_t d = db(half_to_f64_soft((uint16_t)h));
        H(f); H(d);
#ifndef SOFT_ONLY
        CHECK("fcvt s,h soft", h, 0, f, i_h2f((uint16_t)h));
        CHECK("fcvt d,h soft", h, 0, d, i_h2d((uint16_t)h));
        CHECK("fcvt s,h routed", h, 0, fb(half_to_f32((uint16_t)h)), i_h2f((uint16_t)h));
        CHECK("fcvt d,h routed", h, 0, db(half_to_f64((uint16_t)h)), i_h2d((uint16_t)h));
#endif
    }
    /* random */
    for (long i = 0; i < npairs; i++) {
        uint32_t a, b;
        uint64_t c, d;
        rand_pair_s(&a, &b);
        pair_s(a, b);
        one_s(a);
        rand_pair_d(&c, &d);
        pair_d(c, d);
        one_d(c);
        /* integer-valued doubles near the conversion limits */
        if ((i & 15) == 0) {
            static const double lim[] = { 2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0, 4294967295.0, 4294967296.0,
                                          9223372036854775807.0, -9223372036854775808.0, 18446744073709551615.0, -1.0, 0.5, -0.5 };
            double v = lim[(c >> 3) % 12];
            uint64_t vb = db(v) + (uint64_t)((int64_t)(d & 7) - 3);
            one_d(vb);
        }
        if ((i & 3) == 0) {
            uint32_t va[4], vb[4];
            for (int k = 0; k < 4; k++) rand_pair_s(&va[k], &vb[k]);
            vec_s(va, vb);
        }
    }
    printf("soft-hash %016llx (pairs %ld)\n", (unsigned long long)hash, npairs);
    if (hash_lines)
        for (int l = 0; l < 1024; l++) if (line_hash[l]) printf("line %d %016llx\n", l, (unsigned long long)line_hash[l]);
#ifndef SOFT_ONLY
    if (exhaustive) {
        pthread_t t[8];
        for (long k = 0; k < 8; k++) pthread_create(&t[k], NULL, exhaustive_part, (void *)(intptr_t)k);
        long bad = 0;
        for (int k = 0; k < 8; k++) { pthread_join(t[k], NULL); bad += ex_fail[k]; }
        n_checks += 3ll << 32;
        n_fail += bad;
        printf("%s exhaustive frecpe.s / frsqrte.s / fcvt h,s over all 2^32 inputs: %ld mismatches\n", bad ? "FAIL" : "ok", bad);
    }
    printf("%s: %ld checks, %ld failed\n", n_fail ? "FP NATIVE FAILED" : "FP NATIVE PASSED", n_checks, n_fail);
    return n_fail != 0;
#else
    (void)exhaustive;
    return 0;
#endif
}
