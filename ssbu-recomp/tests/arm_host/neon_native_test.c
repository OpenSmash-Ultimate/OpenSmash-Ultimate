/* neon.h on an AArch64 host: the native versions (v_f3v / v_f1v / v_i3v / v_tbl / v_ldn / v_stn, NEON intrinsics)
 * against the portable element loops (*_c, what x86-64 runs), bit for bit, over random registers with a bias to
 * special floating-point values, every op / element size / element count the lifter can emit.
 *   cc -O1 -fno-strict-aliasing -ffp-contract=off -I../../runtime neon_native_test.c -o neon_native_test -lm
 *   ./neon_native_test [rounds = 200000] */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

#if !RECOMP_A64_NATIVE
#error "RECOMP_A64_NATIVE only (aarch64 Linux): compares the native NEON paths with the portable ones"
#endif

static uint64_t rng = 0x2545f4914f6cdd1dull;
static inline uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static const uint32_t SPS[] = { 0, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7f800001u, 0x7fa00000u,
                                0x00000001u, 0x007fffffu, 0x00800000u, 0x3f800000u, 0x40000000u, 0x7f7fffffu, 0x4f000000u,
                                0xcf000000u, 0x4f800000u, 0x3f000000u, 0x5f000000u, 0xdf000000u };
static const uint64_t SPD[] = { 0, 0x8000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
                                0xfff8000000000000ull, 0x7ff0000000000001ull, 0x7ff4000000000000ull, 1, 0x000fffffffffffffull,
                                0x0010000000000000ull, 0x3ff0000000000000ull, 0x7fefffffffffffffull, 0x43e0000000000000ull,
                                0xc3e0000000000000ull, 0x43f0000000000000ull, 0x41e0000000000000ull, 0x3fe0000000000000ull };
static void rand_v(V128 *v, int fp)
{
    v->d[0] = rnd();
    v->d[1] = rnd();
    if (!fp) {
        uint64_t r = rnd();
        if ((r & 3) == 0) v->d[0] &= 0x0101010101010101ull * (r >> 8 & 0xff);    /* equal / zero lanes */
        if ((r & 12) == 0) v->d[1] = v->d[0];
        return;
    }
    for (int i = 0; i < 4; i++) {
        uint64_t r = rnd();
        if ((r & 3) == 0) v->w[i] = SPS[(r >> 8) % (sizeof SPS / 4)];
        else if ((r & 3) == 1) v->w[i] = (v->w[i] & 0x807fffffu) | (uint32_t)((0x7d + (r >> 16) % 6) << 23);
    }
    if ((rnd() & 3) == 0) for (int i = 0; i < 2; i++) { uint64_t r = rnd(); if (r & 1) v->d[i] = SPD[(r >> 8) % (sizeof SPD / 8)]; else v->d[i] = (v->d[i] & 0x800fffffffffffffull) | ((0x3fd + (r >> 16) % 6) << 52); }
}

static long checks, fails, nan_order;
static int isnan_s(uint32_t v) { return (v & 0x7fffffffu) > 0x7f800000u; }
static int isnan_d(uint64_t v) { return (v & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }
/* Do two or more NaN operands meet in result lane i? Then the NaN chosen depends on operand order: the native path
 * keeps the guest's order (asm), the portable C path may not (the compiler may swap commutative operands). */
static int nan_order_lane(int op, int dbl, int n, const V128 *a, const V128 *b, const V128 *acc, int i)
{
    int k = 0;
    if (op == F_ADDP) {
        const V128 *src = i < n / 2 ? a : b;
        int j = 2 * (i % (n / 2 ? n / 2 : 1));
        if (n == 1) { src = a; j = 0; }
        return dbl ? isnan_d(src->d[j]) + isnan_d(src->d[j + 1]) >= 2 : isnan_s(src->w[j]) + isnan_s(src->w[j + 1]) >= 2;
    }
    if (dbl) k = isnan_d(a->d[i]) + isnan_d(b->d[i]) + ((op == F_MLA || op == F_MLS) && isnan_d(acc->d[i]));
    else k = isnan_s(a->w[i]) + isnan_s(b->w[i]) + ((op == F_MLA || op == F_MLS) && isnan_s(acc->w[i]));
    return k >= 2;
}
static void cmp_f3(int op, int dbl, int n, const V128 *a, const V128 *b, const V128 *acc, const V128 *got, const V128 *want)
{
    checks++;
    if (memcmp(got, want, 16) == 0) return;
    int lanes = dbl ? 2 : 4, only_order = 1;
    for (int i = 0; i < lanes; i++) {
        int differ = dbl ? got->d[i] != want->d[i] : got->w[i] != want->w[i];
        if (differ && !(i < n && nan_order_lane(op, dbl, n, a, b, acc, i))) only_order = 0;
    }
    if (only_order) { nan_order++; return; }
    if (++fails <= 30)
        printf("FAIL v_f3v op=%d dbl=%d n=%d a=%016llx%016llx b=%016llx%016llx acc=%016llx%016llx native=%016llx%016llx portable=%016llx%016llx\n", op, dbl, n,
               (unsigned long long)a->d[1], (unsigned long long)a->d[0], (unsigned long long)b->d[1], (unsigned long long)b->d[0],
               (unsigned long long)acc->d[1], (unsigned long long)acc->d[0],
               (unsigned long long)got->d[1], (unsigned long long)got->d[0], (unsigned long long)want->d[1], (unsigned long long)want->d[0]);
}
static void cmp(const char *what, int op, int sz, int n, const V128 *a, const V128 *b, const V128 *got, const V128 *want)
{
    checks++;
    if (memcmp(got, want, 16) == 0) return;
    if (++fails <= 30)
        printf("FAIL %s op=%d sz=%d n=%d a=%016llx%016llx b=%016llx%016llx native=%016llx%016llx portable=%016llx%016llx\n", what, op, sz, n,
               (unsigned long long)a->d[1], (unsigned long long)a->d[0], (unsigned long long)b->d[1], (unsigned long long)b->d[0],
               (unsigned long long)got->d[1], (unsigned long long)got->d[0], (unsigned long long)want->d[1], (unsigned long long)want->d[0]);
}

int main(int argc, char **argv)
{
    long rounds = argc > 1 ? atol(argv[1]) : 200000;
    static uint8_t mem[4096], mem2[4096];
    for (long it = 0; it < rounds; it++) {
        V128 a, b, acc, x, y;
        rand_v(&a, 1); rand_v(&b, 1); rand_v(&acc, 1);
        if ((rnd() & 7) == 0) b = a;
        /* v_f3v: single (n = 1, 2, 4) and double (n = 1, 2) */
        for (int op = F_ADD; op <= F_ADDP; op++)
            for (int dbl = 0; dbl < 2; dbl++)
                for (int n = 1; n <= (dbl ? 2 : 4); n *= 2) {
                    if (op == F_ADDP && n == 1) continue;                  /* scalar FADDP is lifted separately */
                    x = acc; y = acc;
                    v_f3v(&x, &a, &b, op, dbl, n);
                    v_f3v_c(&y, &a, &b, op, dbl, n);
                    cmp_f3(op, dbl, n, &a, &b, &acc, &x, &y);
                }
        for (int op = F1_ABS; op <= F1_CMLE0; op++)
            for (int dbl = 0; dbl < 2; dbl++)
                for (int n = 1; n <= (dbl ? 2 : 4); n *= 2) {
                    V128 src = a;
                    if ((op == F1_SCVTF || op == F1_UCVTF) && (rnd() & 1)) rand_v(&src, 0);
                    x = acc; y = acc;
                    v_f1v(&x, &src, op, dbl, n);
                    v_f1v_c(&y, &src, op, dbl, n);
                    cmp("v_f1v", op, dbl, n, &src, &src, &x, &y);
                }
        /* integer three-same */
        V128 ia, ib;
        rand_v(&ia, 0); rand_v(&ib, 0);
        for (int op = I_ADD; op <= I_MLS; op++)               /* MLA / MLS accumulate into the destination */
            for (int sz = 0; sz < 4; sz++)
                for (int bytes = 8; bytes <= 16; bytes += 8) {
                    int n = bytes >> sz;
                    x = acc; y = acc;
                    v_i3v(&x, &ia, &ib, op, sz, n);
                    v_i3v_c(&y, &ia, &ib, op, sz, n);
                    cmp("v_i3v", op, sz, n, &ia, &ib, &x, &y);
                    /* scalar form (one element) */
                    x = acc; y = acc;
                    v_i3v(&x, &ia, &ib, op, sz, 1);
                    v_i3v_c(&y, &ia, &ib, op, sz, 1);
                    cmp("v_i3v scalar", op, sz, 1, &ia, &ib, &x, &y);
                }
        /* TBL / TBX: 1..4 table registers, wrapping at v31, indexes partly out of range */
        {
            Ctx c1, c2;
            for (int i = 0; i < 32; i++) { c1.v[i].d[0] = rnd(); c1.v[i].d[1] = rnd(); }
            int rm = (int)(rnd() & 31), rd = (int)(rnd() & 31), rn = (int)(rnd() & 31);
            uint64_t lim = 1 + rnd() % 80;
            for (int i = 0; i < 16; i++) c1.v[rm].b[i] = (uint8_t)(rnd() % (lim + (rnd() & 1 ? 0 : 200)));
            for (int len = 1; len <= 4; len++)
                for (int q = 0; q < 2; q++)
                    for (int tbx = 0; tbx < 2; tbx++) {
                        memcpy(&c2, &c1, sizeof c1);
                        Ctx c3;
                        memcpy(&c3, &c1, sizeof c1);
                        v_tbl(&c2, rd, rn, len, rm, q, tbx);
                        v_tbl_c(&c3, rd, rn, len, rm, q, tbx);
                        checks++;
                        if (memcmp(c2.v, c3.v, sizeof c2.v)) { if (++fails <= 30) printf("FAIL v_tbl rd=%d rn=%d rm=%d len=%d q=%d tbx=%d\n", rd, rn, rm, len, q, tbx); }
                    }
        }
        /* LD2..LD4 / ST2..ST4 */
        if ((it & 3) == 0) {
            Ctx c1, c2;
            for (int i = 0; i < 4096; i++) mem[i] = (uint8_t)rnd();
            for (int i = 0; i < 32; i++) { c1.v[i].d[0] = rnd(); c1.v[i].d[1] = rnd(); }
            int rt = (int)(rnd() & 31);
            uint64_t off = rnd() % 3000;
            for (int nn = 2; nn <= 4; nn++)
                for (int sz = 0; sz < 4; sz++)
                    for (int q = 0; q < 2; q++) {
                        if (sz == 3 && !q) continue;                                   /* Unimpl in the lifter */
                        memcpy(&c2, &c1, sizeof c1);
                        Ctx c3;
                        memcpy(&c3, &c1, sizeof c1);
                        v_ldn(&c2, (uint64_t)(uintptr_t)(mem + off), rt, nn, sz, q);
                        v_ldn_c(&c3, (uint64_t)(uintptr_t)(mem + off), rt, nn, sz, q);
                        checks++;
                        if (memcmp(c2.v, c3.v, sizeof c2.v)) { if (++fails <= 30) printf("FAIL v_ldn n=%d sz=%d q=%d rt=%d\n", nn, sz, q, rt); }
                        memcpy(mem2, mem, sizeof mem);
                        v_stn(&c1, (uint64_t)(uintptr_t)(mem + off), rt, nn, sz, q);
                        v_stn_c(&c1, (uint64_t)(uintptr_t)(mem2 + off), rt, nn, sz, q);
                        checks++;
                        if (memcmp(mem, mem2, sizeof mem)) { if (++fails <= 30) printf("FAIL v_stn n=%d sz=%d q=%d rt=%d\n", nn, sz, q, rt); }
                    }
        }
    }
    printf("v_f3v: %ld results differ from the portable C only in lanes with two or more NaN operands (operand order; "
           "native = guest order)\n", nan_order);
    printf("%s: %ld checks, %ld failed\n", fails ? "NEON NATIVE FAILED" : "NEON NATIVE PASSED", checks, fails);
    return fails != 0;
}
