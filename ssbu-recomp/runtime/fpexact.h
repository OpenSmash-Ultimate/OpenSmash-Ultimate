/* Bit-exact AArch64 behaviour for the few floating-point operations where
 * x86 differs: reciprocal estimates and the sign of generated NaNs.
 * The estimate algorithms follow the pseudocode in the Arm architecture manual. */
#ifndef RECOMP_FPEXACT_H
#define RECOMP_FPEXACT_H

#if RECOMP_A64_NATIVE
#include <arm_neon.h>
#endif

/* x86 produces a negative quiet NaN for invalid operations, AArch64 a positive one.
 * On an AArch64 host (RECOMP_A64_NATIVE, runtime.h) the lifted arithmetic already runs on the guest's own instructions
 * (FPCR = 0: no FZ, no DN, round to nearest), so its NaNs are the guest's and fnan is the identity.
 * Known limit of the x86 version (not fixed here: it needs the operands): a NaN that is PROPAGATED from an input, not
 * generated, and happens to be 0xffc00000 / 0xfff8000000000000 is wrongly made positive too (e.g. FNEG / FABS / FMOV /
 * FCSEL of such a NaN, or x + y with that NaN as an operand); x86 also orders two NaN operands differently from
 * FPProcessNaNs (Arm picks a signalling NaN before a quiet one, first operand first). */
#if RECOMP_A64_NATIVE
static inline float fnan32(float r) { return r; }
static inline double fnan64(double r) { return r; }
#else
static inline float fnan32(float r) {
    uint32_t b;
    memcpy(&b, &r, 4);
    if (b == 0xffc00000u) { b = 0x7fc00000u; memcpy(&r, &b, 4); }
    return r;
}
static inline double fnan64(double r) {
    uint64_t b;
    memcpy(&b, &r, 8);
    if (b == 0xfff8000000000000ull) { b = 0x7ff8000000000000ull; memcpy(&r, &b, 8); }
    return r;
}
#endif

static inline uint32_t recip_estimate(uint32_t a) {            /* a in 256..511 */
    a = a * 2 + 1;
    uint32_t b = (1u << 19) / a;
    return (b + 1) / 2;
}
static inline uint32_t recip_sqrt_estimate(uint32_t a) {       /* a in 128..511 */
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    /* The manual's loop (b = 512; while (a * (b + 1)^2 < 2^28) b++; return (b + 1) / 2) took up to ~500 steps and was
     * 8.7% of the game thread in a 2-fighter match (2026-10-15): k = b + 1 is found from sqrt and corrected exactly
     * instead (identical for every a, checked for 1..4095). */
    uint64_t k = (uint64_t)sqrt((double)(1u << 28) / a);
    if (k < 513) k = 513;
    while (k > 513 && (uint64_t)a * (k - 1) * (k - 1) >= (1ull << 28)) k--;
    while ((uint64_t)a * k * k < (1ull << 28)) k++;
    return (uint32_t)(k / 2);
}

/* Generic implementations on (sign, biased exponent, fraction left-aligned to 52 bits). */
static inline uint64_t fp_recpe_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = bits >> (ebits + fbits);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1;
    uint64_t signbit = sign << (ebits + fbits);
    if (exp == emax) {
        if (frac) return bits | (1ull << (fbits - 1));          /* NaN: quieten */
        return signbit;                                         /* infinity -> zero */
    }
    if (exp == 0 && frac == 0) return signbit | ((uint64_t)emax << fbits);       /* zero -> infinity */
    if (exp == 0 && frac < (1ull << (fbits - 2))) return signbit | ((uint64_t)emax << fbits);  /* overflow */
    uint64_t f52 = frac << (52 - fbits);
    if (exp == 0) {
        if ((f52 >> 51) == 0) { exp = -1; f52 = (f52 << 2) & ((1ull << 52) - 1); }
        else f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    uint32_t scaled = 0x100 | (uint32_t)(f52 >> 44);
    int64_t rexp = (dbl ? 2045 : 253) - exp;
    uint64_t rf = (uint64_t)(recip_estimate(scaled) & 0xff) << 44;
    if (rexp == 0) rf = (1ull << 51) | (rf >> 1);
    else if (rexp == -1) { rf = (1ull << 50) | (rf >> 2); rexp = 0; }
    return signbit | ((uint64_t)rexp << fbits) | (rf >> (52 - fbits));
}
static inline uint64_t fp_rsqrte_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = bits >> (ebits + fbits);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1;
    uint64_t default_nan = ((uint64_t)emax << fbits) | (1ull << (fbits - 1));
    if (exp == emax && frac) return bits | (1ull << (fbits - 1));                 /* NaN: quieten */
    if (exp == 0 && frac == 0) return (sign << (ebits + fbits)) | ((uint64_t)emax << fbits);  /* zero -> infinity */
    if (sign) return default_nan;                                                 /* negative -> NaN */
    if (exp == emax) return 0;                                                    /* +infinity -> +0 */
    uint64_t f52 = frac << (52 - fbits);
    if (exp == 0) {
        while ((f52 >> 51) == 0) { f52 = (f52 << 1) & ((1ull << 52) - 1); exp--; }
        f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    uint32_t scaled = (exp & 1) == 0 ? 0x100 | (uint32_t)(f52 >> 44) : 0x80 | (uint32_t)(f52 >> 45);
    int64_t rexp = ((dbl ? 3068 : 380) - exp) / 2;
    uint64_t est = recip_sqrt_estimate(scaled) & 0xff;
    return ((uint64_t)rexp << fbits) | (est << (fbits - 8));
}

static inline float RT_SOFT(recpe_f)(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_recpe_bits(b, 0); memcpy(&x, &b, 4); return x; }
static inline double RT_SOFT(recpe_d)(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_recpe_bits(b, 1); memcpy(&x, &b, 8); return x; }
static inline float RT_SOFT(rsqrte_f)(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_rsqrte_bits(b, 0); memcpy(&x, &b, 4); return x; }
static inline double RT_SOFT(rsqrte_d)(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_rsqrte_bits(b, 1); memcpy(&x, &b, 8); return x; }

/* FRECPS / FRSQRTS (FPRecipStepFused / FPRSqrtStepFused of the Arm manual, FPCR = 0): 2 - a*b and (3 - a*b) / 2 with one
 * rounding, except that infinity times zero gives exactly 2.0 / 1.5 (fma would give NaN). The first operand is negated
 * BEFORE NaN processing, so a NaN coming from it changes sign. NaN choice as FPProcessNaNs: a signalling NaN first
 * (first operand first), returned quiet; then a quiet NaN (first operand first).
 * fp_recps* / fp_rsqrts* (the instructions) exist with RECOMP_A64_NATIVE only; every other host keeps the fma of
 * before in neon.h, so its results do not change here. fp_step*_soft are what such a host would run to give Arm's
 * results for 0 x inf, NaN operands and an overflowing FRSQRTS sum: tests/arm_host/fp_native_test.c checks them against
 * the instructions bit for bit, ready for a separate change of the other hosts. */
static inline int fp_nan2_32(uint32_t a, uint32_t b, uint32_t *r) {
    int na = (a & 0x7fffffffu) > 0x7f800000u, nb = (b & 0x7fffffffu) > 0x7f800000u;
    if (!(na | nb)) return 0;
    int sa = na && !(a & 0x00400000u), sb = nb && !(b & 0x00400000u);
    *r = (sa ? a : sb ? b : na ? a : b) | 0x00400000u;
    return 1;
}
static inline int fp_nan2_64(uint64_t a, uint64_t b, uint64_t *r) {
    int na = (a & 0x7fffffffffffffffull) > 0x7ff0000000000000ull, nb = (b & 0x7fffffffffffffffull) > 0x7ff0000000000000ull;
    if (!(na | nb)) return 0;
    int sa = na && !(a & 0x0008000000000000ull), sb = nb && !(b & 0x0008000000000000ull);
    *r = (sa ? a : sb ? b : na ? a : b) | 0x0008000000000000ull;
    return 1;
}
/* step = 0: FRECPS (2 + n*b), step = 1: FRSQRTS ((3 + n*b) / 2), with n = -a. */
static inline float fp_step32_soft(float a, float b, int step) {
    uint32_t ua, ub, r;
    memcpy(&ua, &a, 4); memcpy(&ub, &b, 4);
    ua ^= 0x80000000u;
    if (fp_nan2_32(ua, ub, &r)) { float f; memcpy(&f, &r, 4); return f; }
    int ia = (ua & 0x7fffffffu) == 0x7f800000u, ib = (ub & 0x7fffffffu) == 0x7f800000u;
    int za = (ua & 0x7fffffffu) == 0, zb = (ub & 0x7fffffffu) == 0;
    if ((ia && zb) || (za && ib)) return step ? 1.5f : 2.0f;
    if (ia || ib) { r = ((ua ^ ub) & 0x80000000u) | 0x7f800000u; float f; memcpy(&f, &r, 4); return f; }
    float n;
    memcpy(&n, &ua, 4);
    if (!step) return fmaf(n, b, 2.0f);
    /* (3 + n*b) / 2 rounded once: halving the rounded sum is exact (a non-zero sum is far above the subnormal range)
     * unless the sum overflowed; then halve the larger operand (exact: it is at least 2^63) and add 1.5 instead. */
    float t = fmaf(n, b, 3.0f);
    if (fabsf(t) == INFINITY) return fabsf(n) >= fabsf(b) ? fmaf(n * 0.5f, b, 1.5f) : fmaf(n, b * 0.5f, 1.5f);
    return t * 0.5f;
}
static inline double fp_step64_soft(double a, double b, int step) {
    uint64_t ua, ub, r;
    memcpy(&ua, &a, 8); memcpy(&ub, &b, 8);
    ua ^= 0x8000000000000000ull;
    if (fp_nan2_64(ua, ub, &r)) { double f; memcpy(&f, &r, 8); return f; }
    int ia = (ua & 0x7fffffffffffffffull) == 0x7ff0000000000000ull, ib = (ub & 0x7fffffffffffffffull) == 0x7ff0000000000000ull;
    int za = (ua & 0x7fffffffffffffffull) == 0, zb = (ub & 0x7fffffffffffffffull) == 0;
    if ((ia && zb) || (za && ib)) return step ? 1.5 : 2.0;
    if (ia || ib) { r = ((ua ^ ub) & 0x8000000000000000ull) | 0x7ff0000000000000ull; double f; memcpy(&f, &r, 8); return f; }
    double n;
    memcpy(&n, &ua, 8);
    if (!step) return fma(n, b, 2.0);
    double t = fma(n, b, 3.0);
    if (fabs(t) == INFINITY) return fabs(n) >= fabs(b) ? fma(n * 0.5, b, 1.5) : fma(n, b * 0.5, 1.5);
    return t * 0.5;
}

#if RECOMP_A64_NATIVE
/* AArch64 host: the guest's own instructions (bit-exact with FPCR = 0; tests/arm_host/fp_native_test.c checks the
 * software versions against them). */
static inline float recpe_f(float x) { return vrecpes_f32(x); }
static inline double recpe_d(double x) { return vrecped_f64(x); }
static inline float rsqrte_f(float x) { return vrsqrtes_f32(x); }
static inline double rsqrte_d(double x) { return vrsqrted_f64(x); }
static inline float fp_recps32(float a, float b) { return vrecpss_f32(a, b); }
static inline double fp_recps64(double a, double b) { return vrecpsd_f64(a, b); }
static inline float fp_rsqrts32(float a, float b) { return vrsqrtss_f32(a, b); }
static inline double fp_rsqrts64(double a, double b) { return vrsqrtsd_f64(a, b); }
#endif

/* FCVT between half precision (IEEE binary16, FPCR.AHP = 0) and single / double: FPConvert of the Arm manual with
 * round-to-nearest-even and FPCR.DN = 0. A NaN keeps its sign and the top bits of its payload and becomes quiet
 * (FPConvertNaN). Done on the bit patterns, so there is one rounding and no dependence on the host's conversions. */
static inline uint16_t fp_to_half_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint16_t sign = (uint16_t)(((bits >> (ebits + fbits)) & 1) << 15);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1, bias = emax >> 1;
    if (exp == emax) {
        if (frac) return (uint16_t)(sign | 0x7e00 | ((frac >> (fbits - 10)) & 0x1ff));   /* NaN: quiet, payload bits below the quiet bit */
        return (uint16_t)(sign | 0x7c00);                                                 /* infinity */
    }
    if (exp == 0 && frac == 0) return sign;
    uint64_t m;                                    /* value = m * 2^(e - fbits), bit fbits of m set */
    int64_t e;
    if (exp) { m = frac | (1ull << fbits); e = exp - bias; }
    else { m = frac; e = 1 - bias; while (!(m >> fbits)) { m <<= 1; e--; } }
    if (e > 15) return (uint16_t)(sign | 0x7c00);                                         /* overflow: infinity (round to nearest) */
    int64_t sh = fbits - 10 + (e < -14 ? -14 - e : 0);                                    /* below 2^-14 the result is subnormal */
    if (sh > fbits + 1) return sign;                                                      /* less than half the smallest subnormal */
    uint64_t q = m >> sh, rem = m & ((1ull << sh) - 1), half = 1ull << (sh - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    uint32_t r = e < -14 ? (uint32_t)q : (uint32_t)(((uint64_t)(e + 15) << 10) + (q - 0x400));   /* a carry out of the fraction moves into the exponent */
    if (r >= 0x7c00) r = 0x7c00;
    return (uint16_t)(sign | r);
}
static inline uint64_t fp_from_half_bits(uint16_t h, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = (uint64_t)(h >> 15) << (ebits + fbits), frac = h & 0x3ff, emax = (1u << ebits) - 1;
    int64_t exp = (h >> 10) & 31, bias = (int64_t)(emax >> 1), e;
    if (exp == 31) {
        if (frac) return sign | (emax << fbits) | (1ull << (fbits - 1)) | ((frac & 0x1ff) << (fbits - 10));
        return sign | (emax << fbits);
    }
    if (exp == 0) {
        if (!frac) return sign;
        e = -14;
        while (!(frac & 0x400)) { frac <<= 1; e--; }
        frac &= 0x3ff;
    } else e = exp - 15;
    return sign | ((uint64_t)(e + bias) << fbits) | (frac << (fbits - 10));
}
static inline uint16_t RT_SOFT(f32_to_half)(float x) { uint32_t b; memcpy(&b, &x, 4); return fp_to_half_bits(b, 0); }
static inline uint16_t RT_SOFT(f64_to_half)(double x) { uint64_t b; memcpy(&b, &x, 8); return fp_to_half_bits(b, 1); }
static inline float RT_SOFT(half_to_f32)(uint16_t h) { uint32_t b = (uint32_t)fp_from_half_bits(h, 0); float x; memcpy(&x, &b, 4); return x; }
static inline double RT_SOFT(half_to_f64)(uint16_t h) { uint64_t b = fp_from_half_bits(h, 1); double x; memcpy(&x, &b, 8); return x; }
#if RECOMP_A64_NATIVE
/* FCVT Hd, Sn / Dn and back (base ARMv8.0 instructions; one rounding, FPCR = 0). The half value travels in the low 16
 * bits of an S register so no FP16 arithmetic type is needed. */
static inline uint16_t f32_to_half(float x) { float h; __asm__("fcvt %h0, %s1" : "=w"(h) : "w"(x)); uint32_t b; memcpy(&b, &h, 4); return (uint16_t)b; }
static inline uint16_t f64_to_half(double x) { float h; __asm__("fcvt %h0, %d1" : "=w"(h) : "w"(x)); uint32_t b; memcpy(&b, &h, 4); return (uint16_t)b; }
static inline float half_to_f32(uint16_t v) { uint32_t b = v; float h, r; memcpy(&h, &b, 4); __asm__("fcvt %s0, %h1" : "=w"(r) : "w"(h)); return r; }
static inline double half_to_f64(uint16_t v) { uint32_t b = v; float h; double r; memcpy(&h, &b, 4); __asm__("fcvt %d0, %h1" : "=w"(r) : "w"(h)); return r; }
#endif

/* FMAX / FMIN / FMAXNM / FMINNM (FPMax, FPMin, FPMaxNum, FPMinNum of the Arm manual, FPCR.DN = 0), on bit patterns.
 * A signalling NaN operand wins (first operand first) and is returned quiet; then a quiet NaN (first operand first),
 * except that the NM forms return the other operand when exactly one operand is a quiet NaN and none is signalling.
 * +0 is greater than -0. The C library's fmax / fmin do neither. */
static inline uint32_t fp_maxmin32_bits(uint32_t a, uint32_t b, int is_min, int num) {
    int na = (a & 0x7fffffffu) > 0x7f800000u, nb = (b & 0x7fffffffu) > 0x7f800000u;
    if (na | nb) {
        int sa = na && !(a & 0x00400000u), sb = nb && !(b & 0x00400000u);
        if (num && !sa && !sb && na != nb) return na ? b : a;
        return (sa ? a : sb ? b : na ? a : b) | 0x00400000u;
    }
    if (((a | b) & 0x7fffffffu) == 0) return is_min ? (a | b) : (a & b);
    float fa, fb;
    memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
    return (is_min ? fa < fb : fa > fb) ? a : b;
}
static inline uint64_t fp_maxmin64_bits(uint64_t a, uint64_t b, int is_min, int num) {
    int na = (a & 0x7fffffffffffffffull) > 0x7ff0000000000000ull, nb = (b & 0x7fffffffffffffffull) > 0x7ff0000000000000ull;
    if (na | nb) {
        int sa = na && !(a & 0x0008000000000000ull), sb = nb && !(b & 0x0008000000000000ull);
        if (num && !sa && !sb && na != nb) return na ? b : a;
        return (sa ? a : sb ? b : na ? a : b) | 0x0008000000000000ull;
    }
    if (((a | b) & 0x7fffffffffffffffull) == 0) return is_min ? (a | b) : (a & b);
    double fa, fb;
    memcpy(&fa, &a, 8); memcpy(&fb, &b, 8);
    return (is_min ? fa < fb : fa > fb) ? a : b;
}
static inline float RT_SOFT(fp_maxmin32)(float x, float y, int is_min, int num) {
    uint32_t a, b;
    memcpy(&a, &x, 4); memcpy(&b, &y, 4);
    a = fp_maxmin32_bits(a, b, is_min, num);
    memcpy(&x, &a, 4);
    return x;
}
static inline double RT_SOFT(fp_maxmin64)(double x, double y, int is_min, int num) {
    uint64_t a, b;
    memcpy(&a, &x, 8); memcpy(&b, &y, 8);
    a = fp_maxmin64_bits(a, b, is_min, num);
    memcpy(&x, &a, 8);
    return x;
}
#if RECOMP_A64_NATIVE
/* The guest instructions themselves (is_min / num are constants at every lifted call site, the switch folds away). */
static inline float fp_maxmin32(float x, float y, int is_min, int num) {
    float r;
    switch (is_min * 2 + num) {
    case 0: __asm__("fmax %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); break;
    case 1: __asm__("fmaxnm %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); break;
    case 2: __asm__("fmin %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); break;
    default: __asm__("fminnm %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); break;
    }
    return r;
}
static inline double fp_maxmin64(double x, double y, int is_min, int num) {
    double r;
    switch (is_min * 2 + num) {
    case 0: __asm__("fmax %d0, %d1, %d2" : "=w"(r) : "w"(x), "w"(y)); break;
    case 1: __asm__("fmaxnm %d0, %d1, %d2" : "=w"(r) : "w"(x), "w"(y)); break;
    case 2: __asm__("fmin %d0, %d1, %d2" : "=w"(r) : "w"(x), "w"(y)); break;
    default: __asm__("fminnm %d0, %d1, %d2" : "=w"(r) : "w"(x), "w"(y)); break;
    }
    return r;
}
#endif

#endif
