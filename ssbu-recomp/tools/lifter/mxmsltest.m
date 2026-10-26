/* Check mx_translate's Metal Shading Language target (MxIn.target 2) on a Mac, with made-up programs only (no game files).
 * Ported from WindowSmash's tools/mac/msltest.m: a corpus of hand-assembled Maxwell programs is translated to MSL and
 * compiled by Metal as a renderer would (safe math, invariant positions; also as on macOS 14, MSL 3.1), made into
 * pipelines; some of it then runs on the GPU (integer, float, half, compare, flow and memory instructions) and the results
 * are checked against the same arithmetic done here in C. Random programs of every translated encoding are compiled too.
 * The corpus' GLSL (targets 0 and 1) can be written to a file and compared with what an older translator wrote: the MSL
 * target must leave the OpenGL and Vulkan text byte for byte as it was.
 *
 * Usage: mxmsltest [all]                 the corpus, the random programs, the GPU checks
 *        mxmsltest corpus [-v]           translate, compile, make pipelines (-v: the MSL of failures)
 *        mxmsltest fuzz [<n>] [-v]       random programs to MSL, compiled (<n>: print that one's MSL)
 *        mxmsltest gpu [-v]              the GPU checks (-v: every value)
 *        mxmsltest print <program>       one program's MSL (mxmsltest list: their names)
 *        mxmsltest glsl <file>           write the corpus' and the random programs' GLSL, and what MxOut says of them
 *        mxmsltest glsldiff <file>       compare them with a file written so
 *   Exits 1 when anything fails, 2 on a usage error.
 *
 * Build, from ssbu-recomp:
 *   clang -arch arm64 -mmacosx-version-min=14.0 -fobjc-arc -fno-strict-aliasing -ffp-contract=off -O1 -I runtime \
 *     tools/lifter/mxmsltest.m runtime/mx_translate.c -framework Metal -framework Foundation -o mxmsltest
 * With -DMXMSLTEST_GLSL_ONLY (and -x c; on Linux also -lm) only the glsl modes are built, needing neither Metal nor the MSL
 * target: that builds against an older mx_translate.c, to write the file glsldiff compares with. */
#define _CRT_SECURE_NO_WARNINGS
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx_translate.h"
#include "mx_ops.h"

#ifndef MXMSLTEST_GLSL_ONLY
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t i32;
typedef uint64_t u64;
typedef int64_t i64;

/* ---- an assembler ----------------------------------------------------------------------- */
#define B_(v, pos, n) (((u64)(v) & ((1ull << (n)) - 1)) << (pos))
#define BITS_(w, pos, n) ((u32)(((w) >> (pos)) & ((1ull << (n)) - 1)))
#define D_(r) B_(r, 0, 8)                     /* destination */
#define A_(r) B_(r, 8, 8)                     /* operand A */
#define BR_(r) B_(r, 20, 8)                   /* operand B, a register */
#define CR_(r) B_(r, 39, 8)                   /* operand C (or B of the rc forms), a register */
#define IMM_(v) (B_((u32)(v), 20, 19) | B_((u32)(v) >> 31, 56, 1))    /* 20-bit signed immediate */
#define FIMM_(bits) (B_((u32)(bits) >> 12, 20, 19) | B_((u32)(bits) >> 31, 56, 1))   /* a float's top 20 bits */
#define CB_(bank, off) (B_(bank, 34, 5) | B_((off) / 4, 20, 14))      /* c[bank][off] */
#define I32_(v) B_(v, 20, 32)
#define PPT_ B_(7, 39, 3)                     /* the predicate operand at 39: PT */
#define RZ_ 255u
#define SB_(k) (0x310 + 16 * (k))             /* compute: storage buffer k's descriptor in c[0] (mx_translate.h) */
#define SBG_(k) (0x110 + 16 * (k))            /* vertex */
#define TU_(u) B_(8 + 2 * (u), 36, 13)        /* the handle of texture unit u (c2 word 8 + 2u) */

typedef struct Prog {
    const char *name;
    int stage, pair;              /* fragment: the corpus index of its vertex program, or -1 */
    u32 local[3], smem, lmem;
    u8 hdr[0x50];
    u64 *w; int *op; u32 n, cap;
    struct { const char *name; u32 at; } lab[320], fix[320];
    u32 nlab, nfix;
    u8 *bytes; u32 size;
    u32 cb[4][128];               /* constant buffers 0..3 (1: also the jump tables) */
    int opts;                     /* the options of the variant to compile it with (variant()), -1: both */
} Prog;

static Prog *corpus[1024];
static u32 ncorpus;

static Prog *prog(const char *name, int stage) {
    Prog *p = calloc(1, sizeof *p);
    corpus[ncorpus++] = p;
    p->name = name;
    p->stage = stage;
    p->pair = -1;
    p->opts = -1;
    p->local[0] = p->local[1] = p->local[2] = 1;
    return p;
}
static u64 opcode(int op) {
    for (size_t k = 0; k < sizeof mx_op_table / sizeof mx_op_table[0]; k++) if (mx_op_table[k].op == op) return mx_op_table[k].value;
    fprintf(stderr, "no instruction %d\n", op);
    exit(2);
}
/* An instruction, guarded by predicate p (7: PT) or its negation. */
static void IG(Prog *p, u32 pred, int neg, int op, u64 fields) {
    if (p->n == p->cap) { p->cap = p->cap * 2 + 64; p->w = realloc(p->w, p->cap * sizeof *p->w); p->op = realloc(p->op, p->cap * sizeof *p->op); }
    p->op[p->n] = op;
    p->w[p->n++] = opcode(op) | B_(pred, 16, 3) | B_(neg, 19, 1) | fields;
}
static void I(Prog *p, int op, u64 fields) { IG(p, 7, 0, op, fields); }
static void label(Prog *p, const char *name) { p->lab[p->nlab].name = name; p->lab[p->nlab++].at = p->n; }
/* A branch-like instruction (BRA, SSY, PBK) to a label; test 15: always. */
static void jump(Prog *p, u32 pred, int neg, int op, const char *to, u32 test) {
    p->fix[p->nfix].name = to; p->fix[p->nfix++].at = p->n;
    IG(p, pred, neg, op, B_(test, 0, 5));
}
/* Every fourth word of the code is scheduling control; graphics programs start with a 0x50-byte header, compute ones not. */
static u32 hdr_size(const Prog *p) { return p->stage == MX_COMPUTE ? 0 : 0x50; }
static u32 addr_of(const Prog *p, u32 i) { return hdr_size(p) + 8 * (4 * (i / 3) + 1 + i % 3); }
static u32 label_addr(Prog *p, const char *name) {
    for (u32 i = 0; i < p->nlab; i++) if (!strcmp(p->lab[i].name, name)) return addr_of(p, p->lab[i].at);
    fprintf(stderr, "%s: no label %s\n", p->name, name);
    exit(2);
}
static int bad_decoding;
static void assemble(Prog *p) {
    for (u32 f = 0; f < p->nfix; f++) {
        u32 at = p->fix[f].at;
        i32 off = (i32)(label_addr(p, p->fix[f].name) - (addr_of(p, at) + 8));
        p->w[at] = (p->w[at] & ~B_(0xFFFFFF, 20, 24)) | B_((u32)off, 20, 24);
    }
    for (u32 i = 0; i < p->n; i++)   /* (did every instruction assemble as the one meant?) */
        if (p->op[i] >= 0 && strcmp(mx_opcode_name(p->w[i]), mx_op_name[p->op[i]])) {
            printf("  %s: instruction %u (%s) decodes as %s\n", p->name, i, mx_op_name[p->op[i]], mx_opcode_name(p->w[i]));
            bad_decoding = 1;
        }
    u32 groups = (p->n + 2) / 3 + 1, h = hdr_size(p);
    p->size = h + groups * 32;
    p->bytes = calloc(1, p->size);
    memcpy(p->bytes, p->hdr, h);
    u64 nop = opcode(MXOP_NOP) | B_(7, 16, 3);
    for (u32 i = 0; i < 3 * (groups - 1); i++) {
        u64 w = i < p->n ? p->w[i] : nop;
        memcpy(p->bytes + addr_of(p, i), &w, 8);
    }
    u64 pad = 0xE2400FFFFF87000Full;   /* the 'BRA to self' the compiler pads with */
    memcpy(p->bytes + h + 32 * (groups - 1) + 8, &pad, 8);
}

/* ---- translating ------------------------------------------------------------------------- */
/* Option sets: 0 the defaults (as mxtest); 1 every option that changes the text (integer attributes and textures, images
 * of known formats, written storage buffers tracked, forced outputs, the internal resolution scale, the debug colour). */
static void inputs(const Prog *p, int target, int opts, MxIn *in) {
    memset(in, 0, sizeof *in);
    in->stage = p->stage; in->code = p->bytes; in->code_len = (int)p->size;
    in->cdata = (const u8 *)p->cb[1]; in->cdata_size = sizeof p->cb[1];
    in->binding_base = p->stage == MX_FRAGMENT ? 20 : 0; in->ssbo_base = p->stage == MX_FRAGMENT ? 56 : 40;
    if (p->stage == MX_COMPUTE) { memcpy(in->local_size, p->local, 12); in->smem_bytes = p->smem; in->lmem_bytes = p->lmem; in->track_writes = 1; in->wmask_binding = 90; }
    in->target = target;
    if (target == 1) {   /* (mxtest -vk) */
        unsigned b = p->stage == MX_COMPUTE ? 0u : 40u * (unsigned)p->stage;
        in->binding_base = b; in->ssbo_base = b + 20; in->wmask_binding = b + 38; in->tex_base = b; in->image_base = b + 32;
    }
    if (target == 2) { in->tex_base = 0; in->image_base = 16; }
    if (opts) {
        static const u16 fmt[8] = {0x8058, 0x8D70, 0x822E, 0x8D82, 0x881A, 0x8236, 0x8229, 0x8235};   /* rgba8 rgba32ui r32f rgba32i rgba16f r32ui r8 r32i */
        in->int_attrs = 0x00030015u; in->sint_attrs = 0x4u;     /* attributes 0, 2, 4 integer (2 signed), 0 and 1 SNORM 10:10:10:2 */
        in->int_tex = 0x0000000Cu; in->sint_tex = 0x8u;         /* texture units 2, 3 integer (3 signed) */
        memcpy(in->image_format, fmt, sizeof fmt);
        in->track_writes = 1; in->wmask_binding = in->binding_base + 38;
        in->force_out = 0x00000F00u;
        in->screen_scale = 1;
        in->fragdebug = 1;
    }
}
static int translate(const Prog *p, int target, int opts, MxOut *out) {
    MxIn in;
    inputs(p, target, opts, &in);
    return mx_translate(&in, out);
}

/* ---- arithmetic as the translated code does it, in C ------------------------------------ */
static float F32(u32 x) { float f; memcpy(&f, &x, 4); return f; }
static u32 U32(float f) { u32 x; memcpy(&x, &f, 4); return x; }
static float h2f(u32 h) { _Float16 x; u16 v = (u16)h; memcpy(&x, &v, 2); return (float)x; }
static u32 f2h(float f) { _Float16 x = (_Float16)f; u16 v; memcpy(&v, &x, 2); return v; }
static float sat(float x) { return x > 1.0f ? 1.0f : x > 0.0f ? x : 0.0f; }   /* (NaN: 0, as clamp's on Apple GPUs) */
static u32 rev(u32 x) { u32 r = 0; for (int i = 0; i < 32; i++) r |= (x >> i & 1) << (31 - i); return r; }
static u32 lop3(u32 a, u32 b, u32 c, u32 lut) {
    u32 r = 0;
    for (u32 i = 0; i < 8; i++)
        if (lut >> i & 1) r |= (i & 4 ? a : ~a) & (i & 2 ? b : ~b) & (i & 1 ? c : ~c);
    return r;
}
static int fcmp(u32 op, float a, float b) {
    switch (op) {
    case 0: return 0;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b && !isnan(a) && !isnan(b);
    case 6: return a >= b;
    case 7: return !isnan(a) && !isnan(b);
    case 8: return isnan(a) || isnan(b);
    case 9: return !(a >= b);
    case 10: return !(a != b && !isnan(a) && !isnan(b));
    case 11: return !(a > b);
    case 12: return !(a <= b);
    case 13: return a != b;
    case 14: return !(a < b);
    }
    return 1;
}
static i32 f2i(float x) { return isnan(x) ? 0 : x >= 2147483648.0f ? 0x7FFFFFFF : x <= -2147483648.0f ? (i32)0x80000000 : (i32)x; }
static u32 f2u(float x) { return isnan(x) || x <= 0.0f ? 0 : x >= 4294967296.0f ? 0xFFFFFFFFu : (u32)x; }
static float fclamp(float x, float lo, float hi) { return fminf(fmaxf(x, lo), hi); }   /* (Metal's clamp: NaN gives lo) */
static u32 bfe(u32 base, u32 b, int sg) {   /* (mx_bfe) */
    u32 o = b & 0xFF, n = (b >> 8) & 0xFF;
    if (n == 0) return 0;
    if (o >= 32) return sg && (i32)base < 0 ? 0xFFFFFFFFu : 0;
    if (o + n >= 32) return sg ? (u32)((i32)base >> o) : base >> o;
    return sg ? (u32)((i32)(base << (32 - o - n)) >> (32 - n)) : (base >> o) & ((1u << n) - 1);
}
static u32 bfi(u32 ins, u32 src, u32 base) {
    u32 o = src & 0xFF, n = (src >> 8) & 0xFF;
    if (o >= 32 || n == 0) return base;
    if (n > 32 - o) n = 32 - o;
    u32 m = n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1;
    return (base & ~(m << o)) | ((ins & m) << o);
}
static u32 pack(float lo, float hi) { return f2h(lo) | f2h(hi) << 16; }

/* ---- programs checked on the GPU (their builders also note what to expect) --------------- */
static void hdr32(Prog *p, u32 off, u32 v) { memcpy(p->hdr + off, &v, 4); }
static void exit_(Prog *p) { I(p, MXOP_EXIT, B_(15, 0, 5)); }

/* Compute: r0 thread, r1 workgroup (of 32); r2..r5 the first 16 bytes of the thread's input in storage buffer 0
 * (1 << in_shift bytes for each thread of the grid), r34/r35 their address; r36/r37 the address of its output in
 * storage buffer 1 (1 << out_shift bytes each). */
static void prologue(Prog *p, u32 in_shift, u32 out_shift) {
    I(p, MXOP_S2R, D_(0) | B_(0x21, 20, 8));
    I(p, MXOP_S2R, D_(1) | B_(0x25, 20, 8));
    I(p, MXOP_MOV_cbuf, D_(30) | CB_(0, SB_(0)) | B_(15, 39, 4));
    I(p, MXOP_MOV_cbuf, D_(31) | CB_(0, SB_(0) + 4) | B_(15, 39, 4));
    I(p, MXOP_MOV_cbuf, D_(32) | CB_(0, SB_(1)) | B_(15, 39, 4));
    I(p, MXOP_MOV_cbuf, D_(33) | CB_(0, SB_(1) + 4) | B_(15, 39, 4));
    I(p, MXOP_ISCADD_reg, D_(21) | A_(1) | BR_(0) | B_(5, 39, 5));   /* workgroup * 32 + thread */
    I(p, MXOP_SHL_imm, D_(20) | A_(21) | IMM_(in_shift));
    I(p, MXOP_IADD_reg, D_(34) | A_(30) | BR_(20));
    I(p, MXOP_MOV_reg, D_(35) | BR_(31) | B_(15, 39, 4));
    I(p, MXOP_LDG, D_(2) | A_(34) | B_(1, 45, 1) | B_(6, 48, 3));
    I(p, MXOP_SHL_imm, D_(21) | A_(21) | IMM_(out_shift));
    I(p, MXOP_IADD_reg, D_(36) | A_(32) | BR_(21));
    I(p, MXOP_MOV_reg, D_(37) | BR_(33) | B_(15, 39, 4));
}
static void store(Prog *p, u32 r, u32 word) { I(p, MXOP_STG, D_(r) | A_(36) | B_(1, 45, 1) | B_(4, 48, 3) | B_(4 * word, 20, 24)); }
/* MOV with every component (SR's MOV moves only with mask 0xf or 1) */
#define MOVR_(d, r) (D_(d) | BR_(r) | B_(15, 39, 4))

typedef u32 Model(u32 a, u32 b, u32 c);
enum { SAME_BITS, SAME_FLOAT, NEAR_ULPS, NEAR_2E6, ANY_ZERO, SAME_HALVES };   /* how a result must match: bits; NaN for NaN; within 4 ulps; to 2e-6; any zero for zero; per half */
typedef struct AluCase { const char *what; Model *model; int match; } AluCase;
static AluCase alu[300];
static u32 nalu;
static int dontcare;   /* (a model: this input's result is not defined) */

static void acase(Prog *p, const char *what, Model *m, int match) {
    store(p, 10, nalu);
    alu[nalu].what = what; alu[nalu].model = m; alu[nalu++].match = match;
}

#define M(name, expr) static u32 name(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; return (expr); }
/* Apple's GPUs flush float denormals in arithmetic but not in moves, abs or negation: a float result is checked against
 * the model with operands and result flushed, and without (ftz_mode) */
static int ftz_mode;
static float ftz(float x) { return ftz_mode && fpclassify(x) == FP_SUBNORMAL ? copysignf(0.0f, x) : x; }
static u32 UF(float x) { return U32(ftz(x)); }
#define FA ftz(F32(a))
#define FB ftz(F32(b))
#define FC ftz(F32(c))
static const u32 CB0[8] = {0x12345678u, 0xFFFF0001u, 0x3FC00000u, 0xC0490FDBu, 0x3C00BC00u, 13u, 0x0810u, 0x7FC00000u};
/* (where SR's semantics are not WindowSmash's, the model is SR's: IADD.PO is a + b + 1, as suyu's) */
M(m_iadd, a + b) M(m_iadd_na, 0u - a + b) M(m_iadd_nb, a - b) M(m_iadd_po, a + b + 1u) M(m_iadd_imm, a - 5u) M(m_iadd_cb, a + CB0[1])
M(m_iadd32i, a + 0x10000u) M(m_iadd_x, c + (a + b < a ? 1u : 0u)) M(m_iadd3, a + b + c)
M(m_iadd3_h, (((a & 0xFFFFu) + (b >> 16)) << 16) + c) M(m_iadd3_imm, a - 0x1234u + c) M(m_iadd3_cb, a + CB0[0] + c)
M(m_iscadd, (a << 5) + b) M(m_iscadd_na, ((0u - a) << 5) + b) M(m_iscadd_imm, (a << 3) + 100u) M(m_iscadd32i, (a << 2) + 0x1000u)
M(m_iscadd_cb, (a << 1) + CB0[0])
M(m_imin_s, (i32)a < (i32)b ? a : b) M(m_imax_u, a > b ? a : b) M(m_imin_imm, (i32)a < -100 ? a : (u32)-100) M(m_imin_cb, a < CB0[0] ? a : CB0[0])
M(m_iset_lt, (i32)a < (i32)b ? ~0u : 0u) M(m_iset_ge_bf, a >= b ? 0x3F800000u : 0u) M(m_iset_ne, a != 0 ? ~0u : 0u)
M(m_iset_cb, (i32)a <= (i32)CB0[1] ? ~0u : 0u)
M(m_isetp_sel, (i32)a > (i32)b ? a : b) M(m_isetp_xor, (a == c) != (a > b) ? ~0u : 0u) M(m_isetp_imm, (i32)a >= 32 ? a : c)
M(m_isetp_cb, a < CB0[0] ? 1u : 2u)
M(m_and, a & b) M(m_or_na, ~a | b) M(m_xor_nb, a ^ ~b) M(m_pass_nb, ~b) M(m_lop_z, (a & b) == 0 ? a : c) M(m_lop_imm, a & 0x0F0F0u)
M(m_lop32i, ~a ^ 0x55AA55AAu) M(m_lop_cb, a | CB0[0]) M(m_lop3_96, lop3(a, b, c, 0x96)) M(m_lop3_e8, lop3(a, b, c, 0xE8))
M(m_lop3_ca, lop3(a, b, c, 0xCA)) M(m_lop3_imm, lop3(a, 0x7FFFFu, c, 0x80)) M(m_lop3_cb, lop3(a, CB0[0], c, 0x6A))
M(m_shl, b < 32 ? a << b : 0u) M(m_shl_w, a << (b & 31)) M(m_shl_imm, a << 7) M(m_shl_cb, a << 13)
M(m_shr, b < 32 ? a >> b : 0u) M(m_shr_s, (u32)((i32)a >> (b < 31 ? b : 31))) M(m_shr_w, a >> (b & 31)) M(m_shr_sw, (u32)((i32)a >> (b & 31)))
M(m_shr_rev, b < 32 ? rev(a) >> b : 0u) M(m_shr_imm, (u32)((i32)a >> 4)) M(m_shr_cb, a >> 13)
M(m_bfe_u, bfe(a, b, 0)) M(m_bfe_s, bfe(a, b, 1)) M(m_bfe_imm, bfe(a, 0x0C04, 1)) M(m_bfe_rev, bfe(rev(a), b, 0)) M(m_bfe_cb, bfe(a, CB0[6], 0))
M(m_bfi, bfi(a, b, c)) M(m_bfi_imm, bfi(a, 0x0A06, c)) M(m_bfi_rc, bfi(a, b, CB0[1])) M(m_bfi_cr, bfi(a, CB0[6], c))
M(m_shf_l, (u32)(((u64)c << 32 | a) << (b & 31) >> 32)) M(m_shf_r, (u32)(((u64)c << 32 | a) >> (b < 32 ? b : 32)))
M(m_shf_r_s, (u32)((i64)((u64)c << 32 | a) >> (b & 63))) M(m_shf_l_imm, (u32)(((u64)c << 32 | a) << 12 >> 32))
M(m_xmad, (a & 0xFFFFu) * (b & 0xFFFFu) + c)
M(m_xmad_h1s, (u32)((i32)a >> 16) * (b & 0xFFFFu) + c)
M(m_xmad_chi_psl, (((a & 0xFFFFu) * (b & 0xFFFFu)) << 16) + (c >> 16))
M(m_xmad_cbcc_mrg, ((((a & 0xFFFFu) * (b & 0xFFFFu)) + (c + (b << 16))) & 0xFFFFu) | (b << 16))
M(m_xmad_clo_sbhb, (a & 0xFFFFu) * (u32)((i32)b >> 16) + (c & 0xFFFFu))
M(m_xmad_x, (a & 0xFFFFu) * (b & 0xFFFFu) + c + (a + b < a ? 1u : 0u))
M(m_xmad_imm, (a & 0xFFFFu) * 0xBEEFu + c) M(m_xmad_rc, (a & 0xFFFFu) * (b >> 16) + CB0[0])
M(m_xmad_cr, (((a & 0xFFFFu) * (CB0[1] & 0xFFFFu)) << 16) + c)
M(m_i2i_s8, (u32)((i32)(a << 16) >> 24)) M(m_i2i_u16, (a >> 16) & 0xFFFFu) M(m_i2i_neg, 0u - a) M(m_i2i_abs, (i32)a < 0 ? 0u - a : a)
M(m_i2i_sat8, (u32)((i32)a < -128 ? -128 : (i32)a > 127 ? 127 : (i32)a)) M(m_i2i_satu16, (u32)((i32)a < 0 ? 0 : (i32)a > 65535 ? 65535 : (i32)a))
M(m_i2i_imm, 0xFDu) M(m_i2i_cb, CB0[0] & 0xFFFFu)
M(m_i2f_s, UF((float)(i32)a)) M(m_i2f_u, UF((float)a)) M(m_i2f_u8, UF((float)((a >> 16) & 0xFF))) M(m_i2f_s16n, UF(-(float)((i32)(a << 16) >> 16)))
M(m_i2f_abs, UF((float)(i32)((i32)a < 0 ? 0u - a : a))) M(m_i2f_h, f2h((float)(i32)a)) M(m_i2f_imm, UF(-7.0f)) M(m_i2f_cb, UF((float)CB0[0]))
M(m_f2i_rn, (u32)f2i(rintf(FA))) M(m_f2i_fl, (u32)f2i(floorf(FA))) M(m_f2i_ce, (u32)f2i(ceilf(FA))) M(m_f2i_tr, (u32)f2i(truncf(FA)))
M(m_f2u_tr, f2u(truncf(FA))) M(m_f2i_s16, (u32)(isnan(FA) ? 0 : f2i(fclamp(rintf(FA), -32768.0f, 32767.0f))))
M(m_f2u_16a, isnan(FA) ? 0 : f2u(fclamp(rintf(fabsf(FA)), 0.0f, 65535.0f)))
M(m_f2i_h1, (u32)f2i(truncf(h2f(a >> 16)))) M(m_f2i_cb, f2u(floorf(F32(CB0[2]))))
M(m_f2i_max, 0x7FFFFFFFu) M(m_f2u_max, 0xFFFFFFFFu) M(m_f2i_min, 0x80000000u) M(m_f2i_zero, 0u)
M(m_f2f_rn, UF(rintf(FA))) M(m_f2f_fl, UF(floorf(FA))) M(m_f2f_ce, UF(ceilf(FA))) M(m_f2f_tr, UF(truncf(FA))) M(m_f2f_h, f2h(FA))
M(m_f2f_h1, UF(h2f(a >> 16))) M(m_f2f_satn, UF(sat(-FA))) M(m_f2f_abs, UF(fabsf(FA))) M(m_f2f_imm, UF(rintf(2.5f))) M(m_f2f_cb, UF(ceilf(F32(CB0[3]))))
M(m_fadd, UF(FA + FB)) M(m_fadd_nab, UF(-FA + fabsf(FB))) M(m_fadd_sat, UF(sat(FA + FB))) M(m_fadd_imm, UF(FA + 1.5f))
M(m_fadd32i, UF(fabsf(FA) + F32(0x40490FDBu))) M(m_fadd_cb, UF(FA + F32(CB0[3])))
M(m_fmul, UF(FA * FB)) M(m_fmul_d2, UF((FA * 0.5f) * FB)) M(m_fmul_fmz, UF((FA * 8.0f == 0.0f || FB == 0.0f) ? 0.0f : (FA * 8.0f) * FB))
M(m_fmul_satn, UF(sat(FA * -FB))) M(m_fmul32i, UF(FA * F32(0x3EAAAAABu))) M(m_fmul_imm, UF(FA * -2.0f)) M(m_fmul_cb, UF(FA * F32(CB0[2])))
M(m_ffma, UF(fmaf(FA, FB, FC))) M(m_ffma_nc, UF(fmaf(FA, FB, -FC))) M(m_ffma_fmz, (FA == 0.0f || FB == 0.0f) ? c : UF(fmaf(FA, FB, FC)))
M(m_ffma_satnb, UF(sat(fmaf(FA, -FB, FC)))) M(m_ffma_imm, UF(fmaf(FA, 2.0f, FC))) M(m_ffma_rc, UF(fmaf(FA, FB, 1.5f)))
M(m_ffma_cr, UF(fmaf(FA, 1.5f, FC))) M(m_ffma32i, UF(fmaf(FA, 0.5f, -FC)))
M(m_fmin, UF(fminf(FA, FB))) M(m_fmax_na, UF(fmaxf(-FA, FB))) M(m_fmin_imm, UF(fminf(FA, 1.0f))) M(m_fmax_cb, UF(fmaxf(FA, F32(CB0[2]))))
M(m_rcp, UF(1.0f / FA)) M(m_rsq, UF(1.0f / sqrtf(FA))) M(m_sqrt, UF(sqrtf(FA))) M(m_rcp_an, UF(1.0f / -fabsf(FA)))
M(m_ex2, UF((dontcare = fabsf(FA) > 100.0f, exp2f(FA)))) M(m_lg2, UF(log2f(FA)))
M(m_sin, UF((dontcare = !(fabsf(FA) < 100.0f), sinf(FA)))) M(m_cos, UF((dontcare = !(fabsf(FA) < 100.0f), cosf(FA))))
M(m_rro, a) M(m_rro_n, UF(-FB)) M(m_rro_imm, 0x3F800000u) M(m_rro_cb, CB0[2])
M(m_fsetp_psel, fcmp(10, FA, FB) ? ~0u : 0u) M(m_fsetp_imm, FA > 0.0f ? a : b) M(m_fset_bf, fcmp(3, fabsf(FA), -FB) ? 0x3F800000u : 0u)
M(m_fset_imm, fcmp(14, FA, 1.0f) ? ~0u : 0u) M(m_fset_cb, fcmp(5, FA, F32(CB0[7])) ? ~0u : 0u)
M(m_fsetp_cb, fcmp(9, FA, F32(CB0[2])) ? 1u : 0u)
M(m_fcmp, FB >= 0.0f ? a : b)
M(m_csetp_lo, a + b >= a ? a : b) M(m_cset_eq, a + b == 0 ? 0x3F800000u : 0u) M(m_cset_lt, (i32)(a - b) < 0 ? ~0u : 0u)
M(m_psetp, ((a < b) != (b < c)) ? 0x3F800000u : 0u) M(m_r2p, (a >> 11) & 1 ? ~0u : 0u) M(m_r2p_reg, (a >> 2) & 1 ? 5u : 6u)
M(m_r2p_cb, (CB0[0] >> 4) & 1 ? 7u : 8u)
M(m_icmp, (i32)c < 0 ? a : b) M(m_popc, (u32)__builtin_popcount(a)) M(m_popc_inv, (u32)__builtin_popcount(~b))
M(m_flo_u, a ? 31u - (u32)__builtin_clz(a) : ~0u) M(m_flo_s, (i32)a < 0 ? (~a ? 31u - (u32)__builtin_clz(~a) : ~0u) : a ? 31u - (u32)__builtin_clz(a) : ~0u)
M(m_flo_sh, a ? (u32)__builtin_clz(a) : ~0u)
M(m_lea, b + (a << 3)) M(m_lea_hi, b + (u32)((((u64)c << 32) | a) >> (32 - 5)))
M(m_hadd2, pack(h2f(a) + h2f(b), h2f(a >> 16) + h2f(b >> 16)))
M(m_hadd2_nbs, pack(sat(h2f(a) + -h2f(b)), sat(h2f(a >> 16) + -h2f(b))))
M(m_hadd2_f32, UF(h2f(a) + FB))
M(m_hadd2_mrg, (c & 0xFFFF0000u) | f2h(h2f(a) + h2f(b)))
M(m_hadd2_imm, pack(h2f(a) + 1.0f, h2f(a >> 16) + -2.0f))
M(m_hadd2_32i, pack(-h2f(a) + 2.0f, -h2f(a >> 16) + 1.0f))
M(m_hadd2_cb, pack(h2f(a) + 1.5f, h2f(a >> 16) + 1.5f))
M(m_hmul2, pack(fabsf(h2f(a)) * h2f(b), fabsf(h2f(a >> 16)) * h2f(b >> 16)))
M(m_hmul2_32i, pack(h2f(a) * 2.0f, h2f(a >> 16) * 1.0f))
M(m_hmul2_imm, pack(h2f(a) * 1.0f, h2f(a >> 16) * -2.0f))
M(m_hmul2_cb, pack(-h2f(a) * 1.5f, -h2f(a >> 16) * 1.5f))
M(m_hfma2, pack(fmaf(h2f(a), h2f(b), -h2f(c >> 16)), fmaf(h2f(a >> 16), h2f(b >> 16), -h2f(c >> 16))))
M(m_hfma2_32i, pack(fmaf(h2f(a), 1.0f, -h2f(c)), fmaf(h2f(a >> 16), 2.0f, -h2f(c >> 16))))
M(m_hfma2_rc, pack(fmaf(h2f(a), h2f(b), 1.5f), fmaf(h2f(a >> 16), h2f(b >> 16), 1.5f)))
M(m_hfma2_cr, pack(fmaf(h2f(a), 1.5f, h2f(c)), fmaf(h2f(a >> 16), 1.5f, h2f(c >> 16))))
M(m_hfma2_imm, pack(fmaf(h2f(a), 1.0f, h2f(c)), fmaf(h2f(a >> 16), -2.0f, h2f(c >> 16))))
M(m_hset2, (fcmp(1, h2f(a), h2f(b)) ? 0xFFFFu : 0u) | (fcmp(1, h2f(a >> 16), h2f(b >> 16)) ? 0xFFFFu : 0u) << 16)
M(m_hset2_bf, (fcmp(13, h2f(a), h2f(b)) ? 0x3C00u : 0u) | (fcmp(13, h2f(a >> 16), h2f(b >> 16)) ? 0x3C00u : 0u) << 16)
M(m_hset2_imm, (fcmp(4, h2f(a), 1.0f) ? 0xFFFFu : 0u) | (fcmp(4, h2f(a >> 16), -2.0f) ? 0xFFFFu : 0u) << 16)
M(m_hset2_cb, (fcmp(3, h2f(a), 1.5f) ? 0xFFFFu : 0u) | (fcmp(3, h2f(a >> 16), 1.5f) ? 0xFFFFu : 0u) << 16)
M(m_hsetp2, fcmp(6, h2f(a), h2f(b)) && fcmp(6, h2f(a >> 16), h2f(b >> 16)) ? ~0u : 0u)
M(m_hsetp2_and, !(fcmp(2, h2f(a), h2f(b)) && fcmp(2, h2f(a >> 16), h2f(b >> 16))) ? ~0u : 0u)
M(m_hsetp2_imm, fcmp(1, h2f(a), 1.0f) ? 3u : 4u) M(m_hsetp2_cb, fcmp(8, h2f(a >> 16), 1.5f) ? 3u : 4u)
M(m_mov32i, 0xCAFEF00Du) M(m_mov_imm, 0xFFFFFFF9u) M(m_mov_cb, CB0[3]) M(m_sel_imm, 77u) M(m_sel_cb, CB0[0])
M(m_icmp_imm, (i32)c < 0 ? a : 77u) M(m_icmp_rc, (i32)CB0[1] < 0 ? a : b) M(m_icmp_cr, c == 0 ? a : CB0[0])
M(m_fcmp_imm, FC >= 0.0f ? a : 0x3F800000u) M(m_fcmp_rc, F32(CB0[3]) < 0.0f ? a : b) M(m_fcmp_cr, fcmp(4, FC, 0.0f) ? a : CB0[2])
M(m_flo_imm, 8u) M(m_flo_cb, 28u) M(m_popc_imm, 19u) M(m_popc_cb, (u32)__builtin_popcount(CB0[0]))
M(m_lea_imm, 0x40u + (a << 4)) M(m_lea_cb, CB0[0] + ((0u - a) << 2)) M(m_lea_hi_cb, CB0[0] + (u32)((((u64)c << 32) | a) >> (32 - 7)))
M(m_shf_r_imm, (u32)(((u64)c << 32 | a) >> 8)) M(m_r2p_cbm, (a >> 4) & 1 ? 7u : 8u)
M(m_vote, ~0u) M(m_ffma_dbl, UF(fmaf(FA, FB, FA * FB))) M(m_p2r, (b & ~0x41u) | ((i32)a < (i32)b ? 1u : 0u) | 0x40u)

/* r10 from the thread's inputs a, b, c (r2, r3, r4), one check each */
static Prog *build_alu(void) {
    Prog *p = prog("cs_alu", MX_COMPUTE);
    p->local[0] = 32;
    prologue(p, 4, 10);
    nalu = 0;
    /* integer */
    I(p, MXOP_IADD_reg, D_(10) | A_(2) | BR_(3)); acase(p, "IADD", m_iadd, SAME_BITS);
    I(p, MXOP_IADD_reg, D_(10) | A_(2) | BR_(3) | B_(1, 49, 1)); acase(p, "IADD -a", m_iadd_na, SAME_BITS);
    I(p, MXOP_IADD_reg, D_(10) | A_(2) | BR_(3) | B_(1, 48, 1)); acase(p, "IADD -b", m_iadd_nb, SAME_BITS);
    I(p, MXOP_IADD_reg, D_(10) | A_(2) | BR_(3) | B_(3, 48, 2)); acase(p, "IADD.PO (a + b + 1)", m_iadd_po, SAME_BITS);
    I(p, MXOP_IADD_imm, D_(10) | A_(2) | IMM_(-5)); acase(p, "IADD imm", m_iadd_imm, SAME_BITS);
    I(p, MXOP_IADD_cbuf, D_(10) | A_(2) | CB_(0, 4)); acase(p, "IADD cbuf", m_iadd_cb, SAME_BITS);
    I(p, MXOP_IADD32I, D_(10) | A_(2) | I32_(0x10000)); acase(p, "IADD32I", m_iadd32i, SAME_BITS);
    I(p, MXOP_IADD_reg, D_(11) | A_(2) | BR_(3) | B_(1, 47, 1));
    I(p, MXOP_IADD_reg, D_(10) | A_(4) | BR_(RZ_) | B_(1, 43, 1)); acase(p, "IADD.CC, IADD.X", m_iadd_x, SAME_BITS);
    I(p, MXOP_IADD3_reg, D_(10) | A_(2) | BR_(3) | CR_(4)); acase(p, "IADD3", m_iadd3, SAME_BITS);
    I(p, MXOP_IADD3_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 35, 2) | B_(2, 33, 2) | B_(2, 37, 2)); acase(p, "IADD3 halves, shift", m_iadd3_h, SAME_BITS);
    I(p, MXOP_IADD3_imm, D_(10) | A_(2) | IMM_(0x1234) | CR_(4) | B_(1, 50, 1)); acase(p, "IADD3 imm -b", m_iadd3_imm, SAME_BITS);
    I(p, MXOP_IADD3_cbuf, D_(10) | A_(2) | CB_(0, 0) | CR_(4)); acase(p, "IADD3 cbuf", m_iadd3_cb, SAME_BITS);
    I(p, MXOP_ISCADD_reg, D_(10) | A_(2) | BR_(3) | B_(5, 39, 5)); acase(p, "ISCADD", m_iscadd, SAME_BITS);
    I(p, MXOP_ISCADD_reg, D_(10) | A_(2) | BR_(3) | B_(5, 39, 5) | B_(1, 49, 1)); acase(p, "ISCADD -a", m_iscadd_na, SAME_BITS);
    I(p, MXOP_ISCADD_imm, D_(10) | A_(2) | IMM_(100) | B_(3, 39, 5)); acase(p, "ISCADD imm", m_iscadd_imm, SAME_BITS);
    I(p, MXOP_ISCADD32I, D_(10) | A_(2) | I32_(0x1000) | B_(2, 53, 5)); acase(p, "ISCADD32I", m_iscadd32i, SAME_BITS);
    I(p, MXOP_ISCADD_cbuf, D_(10) | A_(2) | CB_(0, 0) | B_(1, 39, 5)); acase(p, "ISCADD cbuf", m_iscadd_cb, SAME_BITS);
    I(p, MXOP_IMNMX_reg, D_(10) | A_(2) | BR_(3) | B_(1, 48, 1) | PPT_); acase(p, "IMNMX.S32 min", m_imin_s, SAME_BITS);
    I(p, MXOP_IMNMX_reg, D_(10) | A_(2) | BR_(3) | PPT_ | B_(1, 42, 1)); acase(p, "IMNMX.U32 max", m_imax_u, SAME_BITS);
    I(p, MXOP_IMNMX_imm, D_(10) | A_(2) | IMM_(-100) | B_(1, 48, 1) | PPT_); acase(p, "IMNMX imm", m_imin_imm, SAME_BITS);
    I(p, MXOP_IMNMX_cbuf, D_(10) | A_(2) | CB_(0, 0) | PPT_); acase(p, "IMNMX cbuf", m_imin_cb, SAME_BITS);
    I(p, MXOP_ISET_reg, D_(10) | A_(2) | BR_(3) | B_(1, 49, 3) | B_(1, 48, 1) | PPT_); acase(p, "ISET.LT.S32", m_iset_lt, SAME_BITS);
    I(p, MXOP_ISET_reg, D_(10) | A_(2) | BR_(3) | B_(6, 49, 3) | B_(1, 44, 1) | PPT_); acase(p, "ISET.GE.U32.BF", m_iset_ge_bf, SAME_BITS);
    I(p, MXOP_ISET_imm, D_(10) | A_(2) | IMM_(0) | B_(5, 49, 3) | PPT_); acase(p, "ISET imm NE", m_iset_ne, SAME_BITS);
    I(p, MXOP_ISET_cbuf, D_(10) | A_(2) | CB_(0, 4) | B_(3, 49, 3) | B_(1, 48, 1) | PPT_); acase(p, "ISET cbuf LE", m_iset_cb, SAME_BITS);
    I(p, MXOP_ISETP_reg, A_(2) | BR_(3) | B_(4, 49, 3) | B_(1, 48, 1) | PPT_ | B_(0, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_SEL_reg, D_(10) | A_(2) | BR_(3) | B_(0, 39, 3)); acase(p, "ISETP.GT.S32, SEL", m_isetp_sel, SAME_BITS);
    I(p, MXOP_ISETP_reg, A_(2) | BR_(3) | B_(4, 49, 3) | PPT_ | B_(0, 3, 3) | B_(1, 0, 3));
    I(p, MXOP_ISETP_reg, A_(2) | BR_(4) | B_(2, 49, 3) | B_(0, 39, 3) | B_(2, 45, 2) | B_(2, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_PSET, D_(10) | B_(2, 12, 3) | B_(7, 29, 3) | PPT_); acase(p, "ISETP.XOR, PSET", m_isetp_xor, SAME_BITS);
    I(p, MXOP_ISETP_imm, A_(2) | IMM_(32) | B_(6, 49, 3) | B_(1, 48, 1) | PPT_ | B_(3, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_SEL_reg, D_(10) | A_(2) | BR_(4) | B_(3, 39, 3)); acase(p, "ISETP imm", m_isetp_imm, SAME_BITS);
    I(p, MXOP_ISETP_cbuf, A_(2) | CB_(0, 0) | B_(1, 49, 3) | PPT_ | B_(7, 3, 3) | B_(4, 0, 3));
    I(p, MXOP_MOV32I, D_(11) | I32_(1) | B_(15, 12, 4)); I(p, MXOP_MOV32I, D_(12) | I32_(2) | B_(15, 12, 4));
    I(p, MXOP_SEL_reg, D_(10) | A_(12) | BR_(11) | B_(4, 39, 3)); acase(p, "ISETP cbuf (second predicate)", m_isetp_cb, SAME_BITS);
    I(p, MXOP_ICMP_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 49, 3) | B_(1, 48, 1)); acase(p, "ICMP.LT.S32 c", m_icmp, SAME_BITS);
    I(p, MXOP_ICMP_imm, D_(10) | A_(2) | IMM_(77) | CR_(4) | B_(1, 49, 3) | B_(1, 48, 1)); acase(p, "ICMP imm", m_icmp_imm, SAME_BITS);
    I(p, MXOP_ICMP_rc, D_(10) | A_(2) | CR_(3) | CB_(0, 4) | B_(1, 49, 3) | B_(1, 48, 1)); acase(p, "ICMP rc", m_icmp_rc, SAME_BITS);
    I(p, MXOP_ICMP_cr, D_(10) | A_(2) | CB_(0, 0) | CR_(4) | B_(2, 49, 3)); acase(p, "ICMP cr EQ", m_icmp_cr, SAME_BITS);
    I(p, MXOP_LOP_reg, D_(10) | A_(2) | BR_(3) | B_(0, 41, 2)); acase(p, "LOP.AND", m_and, SAME_BITS);
    I(p, MXOP_LOP_reg, D_(10) | A_(2) | BR_(3) | B_(1, 41, 2) | B_(1, 39, 1)); acase(p, "LOP.OR ~a", m_or_na, SAME_BITS);
    I(p, MXOP_LOP_reg, D_(10) | A_(2) | BR_(3) | B_(2, 41, 2) | B_(1, 40, 1)); acase(p, "LOP.XOR ~b", m_xor_nb, SAME_BITS);
    I(p, MXOP_LOP_reg, D_(10) | A_(2) | BR_(3) | B_(3, 41, 2) | B_(1, 40, 1)); acase(p, "LOP.PASS_B ~b", m_pass_nb, SAME_BITS);
    I(p, MXOP_LOP_reg, D_(11) | A_(2) | BR_(3) | B_(2, 44, 2) | B_(1, 48, 3));
    I(p, MXOP_SEL_reg, D_(10) | A_(2) | BR_(4) | B_(1, 39, 3)); acase(p, "LOP.Z, SEL", m_lop_z, SAME_BITS);
    I(p, MXOP_LOP_imm, D_(10) | A_(2) | IMM_(0x0F0F0)); acase(p, "LOP imm", m_lop_imm, SAME_BITS);
    I(p, MXOP_LOP32I, D_(10) | A_(2) | I32_(0x55AA55AA) | B_(2, 53, 2) | B_(1, 55, 1)); acase(p, "LOP32I.XOR ~a", m_lop32i, SAME_BITS);
    I(p, MXOP_LOP_cbuf, D_(10) | A_(2) | CB_(0, 0) | B_(1, 41, 2)); acase(p, "LOP cbuf OR", m_lop_cb, SAME_BITS);
    I(p, MXOP_LOP3_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(0x96, 28, 8)); acase(p, "LOP3 0x96", m_lop3_96, SAME_BITS);
    I(p, MXOP_LOP3_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(0xE8, 28, 8)); acase(p, "LOP3 0xE8", m_lop3_e8, SAME_BITS);
    I(p, MXOP_LOP3_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(0xCA, 28, 8)); acase(p, "LOP3 0xCA", m_lop3_ca, SAME_BITS);
    I(p, MXOP_LOP3_imm, D_(10) | A_(2) | IMM_(0x7FFFF) | CR_(4) | B_(0x80, 48, 8)); acase(p, "LOP3 imm", m_lop3_imm, SAME_BITS);
    I(p, MXOP_LOP3_cbuf, D_(10) | A_(2) | CB_(0, 0) | CR_(4) | B_(0x6A, 48, 8)); acase(p, "LOP3 cbuf", m_lop3_cb, SAME_BITS);
    I(p, MXOP_POPC_reg, D_(10) | BR_(2)); acase(p, "POPC", m_popc, SAME_BITS);
    I(p, MXOP_POPC_reg, D_(10) | BR_(3) | B_(1, 40, 1)); acase(p, "POPC ~b", m_popc_inv, SAME_BITS);
    I(p, MXOP_FLO_reg, D_(10) | BR_(2)); acase(p, "FLO.U32", m_flo_u, SAME_BITS);
    I(p, MXOP_FLO_reg, D_(10) | BR_(2) | B_(1, 48, 1)); acase(p, "FLO.S32", m_flo_s, SAME_BITS);
    I(p, MXOP_FLO_reg, D_(10) | BR_(2) | B_(1, 41, 1)); acase(p, "FLO.SH", m_flo_sh, SAME_BITS);
    I(p, MXOP_FLO_imm, D_(10) | IMM_(0x100)); acase(p, "FLO imm", m_flo_imm, SAME_BITS);
    I(p, MXOP_FLO_cbuf, D_(10) | CB_(0, 0)); acase(p, "FLO cbuf", m_flo_cb, SAME_BITS);
    I(p, MXOP_POPC_imm, D_(10) | IMM_(0x7FFFF)); acase(p, "POPC imm", m_popc_imm, SAME_BITS);
    I(p, MXOP_POPC_cbuf, D_(10) | CB_(0, 0)); acase(p, "POPC cbuf", m_popc_cb, SAME_BITS);
    I(p, MXOP_SHL_reg, D_(10) | A_(2) | BR_(3)); acase(p, "SHL", m_shl, SAME_BITS);
    I(p, MXOP_SHL_reg, D_(10) | A_(2) | BR_(3) | B_(1, 39, 1)); acase(p, "SHL.W", m_shl_w, SAME_BITS);
    I(p, MXOP_SHL_imm, D_(10) | A_(2) | IMM_(7)); acase(p, "SHL imm", m_shl_imm, SAME_BITS);
    I(p, MXOP_SHL_cbuf, D_(10) | A_(2) | CB_(0, 20)); acase(p, "SHL cbuf", m_shl_cb, SAME_BITS);
    I(p, MXOP_SHR_reg, D_(10) | A_(2) | BR_(3)); acase(p, "SHR.U32", m_shr, SAME_BITS);
    I(p, MXOP_SHR_reg, D_(10) | A_(2) | BR_(3) | B_(1, 48, 1)); acase(p, "SHR.S32", m_shr_s, SAME_BITS);
    I(p, MXOP_SHR_reg, D_(10) | A_(2) | BR_(3) | B_(1, 39, 1)); acase(p, "SHR.W.U32", m_shr_w, SAME_BITS);
    I(p, MXOP_SHR_reg, D_(10) | A_(2) | BR_(3) | B_(1, 39, 1) | B_(1, 48, 1)); acase(p, "SHR.W.S32", m_shr_sw, SAME_BITS);
    I(p, MXOP_SHR_reg, D_(10) | A_(2) | BR_(3) | B_(1, 40, 1)); acase(p, "SHR.BREV", m_shr_rev, SAME_BITS);
    I(p, MXOP_SHR_imm, D_(10) | A_(2) | IMM_(4) | B_(1, 48, 1)); acase(p, "SHR imm S32", m_shr_imm, SAME_BITS);
    I(p, MXOP_SHR_cbuf, D_(10) | A_(2) | CB_(0, 20)); acase(p, "SHR cbuf", m_shr_cb, SAME_BITS);
    I(p, MXOP_SHF_l_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 50, 1)); acase(p, "SHF.L.W", m_shf_l, SAME_BITS);
    I(p, MXOP_SHF_r_reg, D_(10) | A_(2) | BR_(3) | CR_(4)); acase(p, "SHF.R (clamped to 32)", m_shf_r, SAME_BITS);
    I(p, MXOP_SHF_r_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(3, 37, 2) | B_(1, 50, 1)); acase(p, "SHF.R.S64.W", m_shf_r_s, SAME_BITS);
    I(p, MXOP_SHF_l_imm, D_(10) | A_(2) | IMM_(12) | CR_(4)); acase(p, "SHF.L imm", m_shf_l_imm, SAME_BITS);
    I(p, MXOP_SHF_r_imm, D_(10) | A_(2) | IMM_(8) | CR_(4)); acase(p, "SHF.R imm", m_shf_r_imm, SAME_BITS);
    I(p, MXOP_BFE_reg, D_(10) | A_(2) | BR_(3)); acase(p, "BFE.U32", m_bfe_u, SAME_BITS);
    I(p, MXOP_BFE_reg, D_(10) | A_(2) | BR_(3) | B_(1, 48, 1)); acase(p, "BFE.S32", m_bfe_s, SAME_BITS);
    I(p, MXOP_BFE_imm, D_(10) | A_(2) | IMM_(0x0C04) | B_(1, 48, 1)); acase(p, "BFE imm S32", m_bfe_imm, SAME_BITS);
    I(p, MXOP_BFE_reg, D_(10) | A_(2) | BR_(3) | B_(1, 40, 1)); acase(p, "BFE.BREV", m_bfe_rev, SAME_BITS);
    I(p, MXOP_BFE_cbuf, D_(10) | A_(2) | CB_(0, 24)); acase(p, "BFE cbuf", m_bfe_cb, SAME_BITS);
    I(p, MXOP_BFI_reg, D_(10) | A_(2) | BR_(3) | CR_(4)); acase(p, "BFI", m_bfi, SAME_BITS);
    I(p, MXOP_BFI_imm, D_(10) | A_(2) | IMM_(0x0A06) | CR_(4)); acase(p, "BFI imm", m_bfi_imm, SAME_BITS);
    I(p, MXOP_BFI_rc, D_(10) | A_(2) | CR_(3) | CB_(0, 4)); acase(p, "BFI rc", m_bfi_rc, SAME_BITS);
    I(p, MXOP_BFI_cr, D_(10) | A_(2) | CB_(0, 24) | CR_(4)); acase(p, "BFI cr", m_bfi_cr, SAME_BITS);
    I(p, MXOP_LEA_lo_reg, D_(10) | A_(2) | BR_(3) | B_(3, 39, 5) | B_(7, 48, 3)); acase(p, "LEA.LO", m_lea, SAME_BITS);
    I(p, MXOP_LEA_hi_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(5, 28, 5) | B_(7, 48, 3)); acase(p, "LEA.HI", m_lea_hi, SAME_BITS);
    I(p, MXOP_LEA_lo_imm, D_(10) | A_(2) | IMM_(0x40) | B_(4, 39, 5) | B_(7, 48, 3)); acase(p, "LEA.LO imm", m_lea_imm, SAME_BITS);
    I(p, MXOP_LEA_lo_cbuf, D_(10) | A_(2) | CB_(0, 0) | B_(2, 39, 5) | B_(7, 48, 3) | B_(1, 45, 1)); acase(p, "LEA.LO cbuf -a", m_lea_cb, SAME_BITS);
    I(p, MXOP_LEA_hi_cbuf, D_(10) | A_(2) | CB_(0, 0) | CR_(4) | B_(7, 51, 5) | B_(7, 48, 3)); acase(p, "LEA.HI cbuf", m_lea_hi_cb, SAME_BITS);
    I(p, MXOP_XMAD_reg, D_(10) | A_(2) | BR_(3) | CR_(4)); acase(p, "XMAD", m_xmad, SAME_BITS);
    I(p, MXOP_XMAD_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 53, 1) | B_(1, 48, 1)); acase(p, "XMAD.S16 H1(a)", m_xmad_h1s, SAME_BITS);
    I(p, MXOP_XMAD_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(2, 50, 3) | B_(1, 36, 1)); acase(p, "XMAD.CHI.PSL", m_xmad_chi_psl, SAME_BITS);
    I(p, MXOP_XMAD_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(4, 50, 3) | B_(1, 37, 1)); acase(p, "XMAD.CBCC.MRG", m_xmad_cbcc_mrg, SAME_BITS);
    I(p, MXOP_XMAD_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 50, 3) | B_(1, 49, 1) | B_(1, 35, 1)); acase(p, "XMAD.CLO S16 H1(b)", m_xmad_clo_sbhb, SAME_BITS);
    I(p, MXOP_IADD_reg, D_(11) | A_(2) | BR_(3) | B_(1, 47, 1));
    I(p, MXOP_XMAD_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 38, 1)); acase(p, "IADD.CC, XMAD.X", m_xmad_x, SAME_BITS);
    I(p, MXOP_XMAD_imm, D_(10) | A_(2) | B_(0xBEEF, 20, 16) | CR_(4)); acase(p, "XMAD imm", m_xmad_imm, SAME_BITS);
    I(p, MXOP_XMAD_rc, D_(10) | A_(2) | CR_(3) | CB_(0, 0) | B_(1, 52, 1)); acase(p, "XMAD rc H1(b)", m_xmad_rc, SAME_BITS);
    I(p, MXOP_XMAD_cr, D_(10) | A_(2) | CB_(0, 4) | CR_(4) | B_(1, 55, 1)); acase(p, "XMAD cr PSL", m_xmad_cr, SAME_BITS);
    I(p, MXOP_I2I_reg, D_(10) | BR_(2) | B_(0, 10, 2) | B_(1, 13, 1) | B_(1, 41, 3) | B_(2, 8, 2) | B_(1, 12, 1)); acase(p, "I2I.S32.S8 b1", m_i2i_s8, SAME_BITS);
    I(p, MXOP_I2I_reg, D_(10) | BR_(2) | B_(1, 10, 2) | B_(2, 41, 3) | B_(2, 8, 2)); acase(p, "I2I.U32.U16 h1", m_i2i_u16, SAME_BITS);
    I(p, MXOP_I2I_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(2, 8, 2) | B_(1, 12, 1) | B_(1, 45, 1)); acase(p, "I2I -x", m_i2i_neg, SAME_BITS);
    I(p, MXOP_I2I_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(2, 8, 2) | B_(1, 12, 1) | B_(1, 49, 1)); acase(p, "I2I |x|", m_i2i_abs, SAME_BITS);
    I(p, MXOP_I2I_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(0, 8, 2) | B_(1, 12, 1) | B_(1, 50, 1)); acase(p, "I2I.S8.SAT", m_i2i_sat8, SAME_BITS);
    I(p, MXOP_I2I_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(1, 8, 2) | B_(1, 50, 1)); acase(p, "I2I.U16.SAT", m_i2i_satu16, SAME_BITS);
    I(p, MXOP_I2I_imm, D_(10) | IMM_(-3) | B_(2, 10, 2) | B_(1, 13, 1) | B_(0, 8, 2)); acase(p, "I2I imm", m_i2i_imm, SAME_BITS);
    I(p, MXOP_I2I_cbuf, D_(10) | CB_(0, 0) | B_(1, 10, 2) | B_(2, 8, 2)); acase(p, "I2I cbuf", m_i2i_cb, SAME_BITS);
    I(p, MXOP_I2F_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(2, 8, 2)); acase(p, "I2F.S32", m_i2f_s, SAME_FLOAT);
    I(p, MXOP_I2F_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2)); acase(p, "I2F.U32", m_i2f_u, SAME_FLOAT);
    I(p, MXOP_I2F_reg, D_(10) | BR_(2) | B_(0, 10, 2) | B_(2, 41, 2) | B_(2, 8, 2)); acase(p, "I2F.U8 b2", m_i2f_u8, SAME_FLOAT);
    I(p, MXOP_I2F_reg, D_(10) | BR_(2) | B_(1, 10, 2) | B_(1, 13, 1) | B_(1, 45, 1) | B_(2, 8, 2)); acase(p, "I2F.S16 -x", m_i2f_s16n, SAME_FLOAT);
    I(p, MXOP_I2F_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(1, 49, 1) | B_(2, 8, 2)); acase(p, "I2F.S32 |x|", m_i2f_abs, SAME_FLOAT);
    I(p, MXOP_I2F_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(1, 8, 2)); acase(p, "I2F.F16", m_i2f_h, SAME_BITS);
    I(p, MXOP_I2F_imm, D_(10) | IMM_(-7) | B_(2, 10, 2) | B_(1, 13, 1) | B_(2, 8, 2)); acase(p, "I2F imm", m_i2f_imm, SAME_FLOAT);
    I(p, MXOP_I2F_cbuf, D_(10) | CB_(0, 0) | B_(2, 10, 2) | B_(2, 8, 2)); acase(p, "I2F cbuf", m_i2f_cb, SAME_FLOAT);
    /* conversions from float */
    I(p, MXOP_F2I_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(0, 39, 2)); acase(p, "F2I.S32 RN", m_f2i_rn, SAME_BITS);
    I(p, MXOP_F2I_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(1, 39, 2)); acase(p, "F2I.S32 floor", m_f2i_fl, SAME_BITS);
    I(p, MXOP_F2I_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(2, 39, 2)); acase(p, "F2I.S32 ceil", m_f2i_ce, SAME_BITS);
    I(p, MXOP_F2I_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "F2I.S32 trunc", m_f2i_tr, SAME_BITS);
    I(p, MXOP_F2I_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(3, 39, 2)); acase(p, "F2I.U32 trunc", m_f2u_tr, SAME_BITS);
    I(p, MXOP_F2I_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 8, 2) | B_(1, 12, 1)); acase(p, "F2I.S16", m_f2i_s16, SAME_BITS);
    I(p, MXOP_F2I_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 8, 2) | B_(1, 45, 1)); acase(p, "F2I.U16 |x|", m_f2u_16a, SAME_BITS);
    I(p, MXOP_F2I_reg, D_(10) | BR_(2) | B_(1, 10, 2) | B_(1, 41, 1) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "F2I.F16 h1 trunc", m_f2i_h1, SAME_BITS);
    I(p, MXOP_F2I_cbuf, D_(10) | CB_(0, 8) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 39, 2)); acase(p, "F2I cbuf floor", m_f2i_cb, SAME_BITS);
    /* out of range and NaN operands the compiler knows: saturated as at run time */
    I(p, MXOP_MOV32I, D_(11) | I32_(0x7F800000) | B_(15, 12, 4));
    I(p, MXOP_F2I_reg, D_(10) | BR_(11) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "MOV32I +inf, F2I.S32", m_f2i_max, SAME_BITS);
    I(p, MXOP_MOV32I, D_(11) | I32_(0xE0AD78EC) | B_(15, 12, 4));
    I(p, MXOP_F2I_reg, D_(10) | BR_(11) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "MOV32I -1e20, F2I.S32", m_f2i_min, SAME_BITS);
    I(p, MXOP_MOV32I, D_(11) | I32_(0x7FC00000) | B_(15, 12, 4));
    I(p, MXOP_F2I_reg, D_(10) | BR_(11) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "MOV32I NaN, F2I.S32", m_f2i_zero, SAME_BITS);
    I(p, MXOP_MOV32I, D_(11) | I32_(0x60AD78EC) | B_(15, 12, 4));
    I(p, MXOP_F2I_reg, D_(10) | BR_(11) | B_(2, 10, 2) | B_(2, 8, 2) | B_(3, 39, 2)); acase(p, "MOV32I 1e20, F2I.U32", m_f2u_max, SAME_BITS);
    I(p, MXOP_MUFU, D_(11) | A_(RZ_) | B_(4, 20, 4));   /* (1 / RZ: +inf) */
    I(p, MXOP_F2I_reg, D_(10) | BR_(11) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "MUFU.RCP RZ, F2I.S32", m_f2i_max, SAME_BITS);
    I(p, MXOP_F2F_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(8, 39, 4)); acase(p, "F2F RN (rint)", m_f2f_rn, SAME_FLOAT);
    I(p, MXOP_F2F_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(9, 39, 4)); acase(p, "F2F floor", m_f2f_fl, SAME_FLOAT);
    I(p, MXOP_F2F_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(10, 39, 4)); acase(p, "F2F ceil", m_f2f_ce, SAME_FLOAT);
    I(p, MXOP_F2F_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(11, 39, 4)); acase(p, "F2F trunc", m_f2f_tr, SAME_FLOAT);
    I(p, MXOP_F2F_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 8, 2)); acase(p, "F2F.F16.F32", m_f2f_h, SAME_HALVES);
    I(p, MXOP_F2F_reg, D_(10) | BR_(2) | B_(1, 10, 2) | B_(1, 41, 1) | B_(2, 8, 2)); acase(p, "F2F.F32.F16 h1", m_f2f_h1, SAME_FLOAT);
    I(p, MXOP_F2F_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 50, 1) | B_(1, 45, 1)); acase(p, "F2F.SAT -x", m_f2f_satn, SAME_FLOAT);
    I(p, MXOP_F2F_reg, D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 49, 1)); acase(p, "F2F |x|", m_f2f_abs, SAME_FLOAT);
    I(p, MXOP_F2F_imm, D_(10) | FIMM_(0x40200000) | B_(2, 10, 2) | B_(2, 8, 2) | B_(8, 39, 4)); acase(p, "F2F imm 2.5 RN", m_f2f_imm, SAME_FLOAT);
    I(p, MXOP_F2F_cbuf, D_(10) | CB_(0, 12) | B_(2, 10, 2) | B_(2, 8, 2) | B_(10, 39, 4)); acase(p, "F2F cbuf ceil", m_f2f_cb, SAME_FLOAT);
    /* float */
    I(p, MXOP_FADD_reg, D_(10) | A_(2) | BR_(3)); acase(p, "FADD", m_fadd, SAME_FLOAT);
    I(p, MXOP_FADD_reg, D_(10) | A_(2) | BR_(3) | B_(1, 48, 1) | B_(1, 49, 1)); acase(p, "FADD -a |b|", m_fadd_nab, SAME_FLOAT);
    I(p, MXOP_FADD_reg, D_(10) | A_(2) | BR_(3) | B_(1, 50, 1)); acase(p, "FADD.SAT", m_fadd_sat, SAME_FLOAT);
    I(p, MXOP_FADD_imm, D_(10) | A_(2) | FIMM_(0x3FC00000)); acase(p, "FADD imm", m_fadd_imm, SAME_FLOAT);
    I(p, MXOP_FADD32I, D_(10) | A_(2) | I32_(0x40490FDB) | B_(1, 54, 1)); acase(p, "FADD32I |a|", m_fadd32i, SAME_FLOAT);
    I(p, MXOP_FADD_cbuf, D_(10) | A_(2) | CB_(0, 12)); acase(p, "FADD cbuf", m_fadd_cb, SAME_FLOAT);
    I(p, MXOP_FMUL_reg, D_(10) | A_(2) | BR_(3)); acase(p, "FMUL", m_fmul, SAME_FLOAT);
    I(p, MXOP_FMUL_reg, D_(10) | A_(2) | BR_(3) | B_(1, 41, 3)); acase(p, "FMUL.D2", m_fmul_d2, SAME_FLOAT);
    I(p, MXOP_FMUL_reg, D_(10) | A_(2) | BR_(3) | B_(4, 41, 3) | B_(2, 44, 2)); acase(p, "FMUL.M8.FMZ", m_fmul_fmz, SAME_FLOAT);
    I(p, MXOP_FMUL_reg, D_(10) | A_(2) | BR_(3) | B_(1, 50, 1) | B_(1, 48, 1)); acase(p, "FMUL.SAT -b", m_fmul_satn, SAME_FLOAT);
    I(p, MXOP_FMUL32I, D_(10) | A_(2) | I32_(0x3EAAAAAB)); acase(p, "FMUL32I", m_fmul32i, SAME_FLOAT);
    I(p, MXOP_FMUL_imm, D_(10) | A_(2) | FIMM_(0xC0000000)); acase(p, "FMUL imm", m_fmul_imm, SAME_FLOAT);
    I(p, MXOP_FMUL_cbuf, D_(10) | A_(2) | CB_(0, 8)); acase(p, "FMUL cbuf", m_fmul_cb, SAME_FLOAT);
    I(p, MXOP_FFMA_reg, D_(10) | A_(2) | BR_(3) | CR_(4)); acase(p, "FFMA (fused)", m_ffma, SAME_FLOAT);
    I(p, MXOP_FFMA_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 49, 1)); acase(p, "FFMA -c", m_ffma_nc, SAME_FLOAT);
    I(p, MXOP_FFMA_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(2, 53, 2)); acase(p, "FFMA.FMZ", m_ffma_fmz, SAME_FLOAT);
    I(p, MXOP_FFMA_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 50, 1) | B_(1, 48, 1)); acase(p, "FFMA.SAT -b", m_ffma_satnb, SAME_FLOAT);
    I(p, MXOP_FFMA_imm, D_(10) | A_(2) | FIMM_(0x40000000) | CR_(4)); acase(p, "FFMA imm", m_ffma_imm, SAME_FLOAT);
    I(p, MXOP_FFMA_rc, D_(10) | A_(2) | CR_(3) | CB_(0, 8)); acase(p, "FFMA rc", m_ffma_rc, SAME_FLOAT);
    I(p, MXOP_FFMA_cr, D_(10) | A_(2) | CB_(0, 8) | CR_(4)); acase(p, "FFMA cr", m_ffma_cr, SAME_FLOAT);
    I(p, MXOP_MOV_reg, MOVR_(10, 4));
    I(p, MXOP_FFMA32I, D_(10) | A_(2) | I32_(0x3F000000) | B_(1, 57, 1)); acase(p, "FFMA32I -c(d)", m_ffma32i, SAME_FLOAT);
    I(p, MXOP_FMUL_reg, D_(11) | A_(2) | BR_(3));
    I(p, MXOP_FFMA_reg, D_(10) | A_(2) | BR_(3) | CR_(11)); acase(p, "FMUL, FFMA", m_ffma_dbl, SAME_FLOAT);
    I(p, MXOP_FMNMX_reg, D_(10) | A_(2) | BR_(3) | PPT_); acase(p, "FMNMX min", m_fmin, ANY_ZERO);
    I(p, MXOP_FMNMX_reg, D_(10) | A_(2) | BR_(3) | PPT_ | B_(1, 42, 1) | B_(1, 48, 1)); acase(p, "FMNMX max -a", m_fmax_na, ANY_ZERO);
    I(p, MXOP_FMNMX_imm, D_(10) | A_(2) | FIMM_(0x3F800000) | PPT_); acase(p, "FMNMX imm", m_fmin_imm, ANY_ZERO);
    I(p, MXOP_FMNMX_cbuf, D_(10) | A_(2) | CB_(0, 8) | PPT_ | B_(1, 42, 1)); acase(p, "FMNMX cbuf max", m_fmax_cb, ANY_ZERO);
    I(p, MXOP_FCMP_reg, D_(10) | A_(2) | BR_(3) | CR_(3) | B_(6, 48, 4)); acase(p, "FCMP.GE (b)", m_fcmp, SAME_BITS);
    I(p, MXOP_MUFU, D_(10) | A_(2) | B_(4, 20, 4)); acase(p, "MUFU.RCP", m_rcp, NEAR_ULPS);
    I(p, MXOP_MUFU, D_(10) | A_(2) | B_(5, 20, 4)); acase(p, "MUFU.RSQ", m_rsq, NEAR_ULPS);
    I(p, MXOP_MUFU, D_(10) | A_(2) | B_(8, 20, 4)); acase(p, "MUFU.SQRT", m_sqrt, NEAR_ULPS);
    I(p, MXOP_MUFU, D_(10) | A_(2) | B_(4, 20, 4) | B_(1, 46, 1) | B_(1, 48, 1)); acase(p, "MUFU.RCP -|a|", m_rcp_an, NEAR_ULPS);
    I(p, MXOP_MUFU, D_(10) | A_(2) | B_(2, 20, 4)); acase(p, "MUFU.EX2", m_ex2, NEAR_2E6);
    I(p, MXOP_MUFU, D_(10) | A_(2) | B_(3, 20, 4)); acase(p, "MUFU.LG2", m_lg2, NEAR_2E6);
    I(p, MXOP_MUFU, D_(10) | A_(2) | B_(1, 20, 4)); acase(p, "MUFU.SIN", m_sin, NEAR_2E6);
    I(p, MXOP_MUFU, D_(10) | A_(2) | B_(0, 20, 4)); acase(p, "MUFU.COS", m_cos, NEAR_2E6);
    I(p, MXOP_FCMP_imm, D_(10) | A_(2) | FIMM_(0x3F800000) | CR_(4) | B_(6, 48, 4)); acase(p, "FCMP imm", m_fcmp_imm, SAME_BITS);
    I(p, MXOP_FCMP_rc, D_(10) | A_(2) | CR_(3) | CB_(0, 12) | B_(1, 48, 4)); acase(p, "FCMP rc", m_fcmp_rc, SAME_BITS);
    I(p, MXOP_FCMP_cr, D_(10) | A_(2) | CB_(0, 8) | CR_(4) | B_(4, 48, 4)); acase(p, "FCMP cr", m_fcmp_cr, SAME_BITS);
    I(p, MXOP_RRO_reg, D_(10) | BR_(2)); acase(p, "RRO", m_rro, SAME_FLOAT);
    I(p, MXOP_RRO_reg, D_(10) | BR_(3) | B_(1, 45, 1)); acase(p, "RRO -b", m_rro_n, SAME_FLOAT);
    I(p, MXOP_RRO_imm, D_(10) | FIMM_(0x3F800000)); acase(p, "RRO imm", m_rro_imm, SAME_FLOAT);
    I(p, MXOP_RRO_cbuf, D_(10) | CB_(0, 8)); acase(p, "RRO cbuf", m_rro_cb, SAME_FLOAT);
    /* compares: every FSET condition, then the predicate forms */
    for (u32 k = 0; k < 16; k++) {
        static char names[16][24];
        snprintf(names[k], sizeof names[k], "FSET cond %u", k);
        I(p, MXOP_FSET_reg, D_(10) | A_(2) | BR_(3) | B_(k, 48, 4) | PPT_);
        acase(p, names[k], NULL, SAME_BITS);
        alu[nalu - 1].match = 100 + (int)k;   /* (checked by fcmp itself) */
    }
    I(p, MXOP_FSET_reg, D_(10) | A_(2) | BR_(3) | B_(3, 48, 4) | PPT_ | B_(1, 52, 1) | B_(1, 54, 1) | B_(1, 53, 1)); acase(p, "FSET.LE.BF |a| -b", m_fset_bf, SAME_BITS);
    I(p, MXOP_FSET_imm, D_(10) | A_(2) | FIMM_(0x3F800000) | B_(14, 48, 4) | PPT_); acase(p, "FSET imm", m_fset_imm, SAME_BITS);
    I(p, MXOP_FSET_cbuf, D_(10) | A_(2) | CB_(0, 28) | B_(5, 48, 4) | PPT_); acase(p, "FSET cbuf NaN", m_fset_cb, SAME_BITS);
    I(p, MXOP_FSETP_reg, A_(2) | BR_(3) | B_(10, 48, 4) | PPT_ | B_(2, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_PSET, D_(10) | B_(2, 12, 3) | B_(7, 29, 3) | PPT_); acase(p, "FSETP, PSET", m_fsetp_psel, SAME_BITS);
    I(p, MXOP_FSETP_imm, A_(2) | FIMM_(0) | B_(4, 48, 4) | PPT_ | B_(5, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_SEL_reg, D_(10) | A_(2) | BR_(3) | B_(5, 39, 3)); acase(p, "FSETP imm, SEL", m_fsetp_imm, SAME_BITS);
    I(p, MXOP_FSETP_cbuf, A_(2) | CB_(0, 8) | B_(9, 48, 4) | PPT_ | B_(7, 3, 3) | B_(6, 0, 3));
    I(p, MXOP_MOV32I, D_(11) | I32_(0) | B_(15, 12, 4)); I(p, MXOP_MOV32I, D_(12) | I32_(1) | B_(15, 12, 4));
    I(p, MXOP_SEL_reg, D_(10) | A_(11) | BR_(12) | B_(6, 39, 3)); acase(p, "FSETP cbuf (second)", m_fsetp_cb, SAME_BITS);
    I(p, MXOP_IADD_reg, D_(11) | A_(2) | BR_(3) | B_(1, 47, 1));
    I(p, MXOP_CSETP, B_(17, 8, 5) | PPT_ | B_(3, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_SEL_reg, D_(10) | A_(2) | BR_(3) | B_(3, 39, 3)); acase(p, "IADD.CC, CSETP.LO", m_csetp_lo, SAME_BITS);
    I(p, MXOP_IADD_reg, D_(11) | A_(2) | BR_(3) | B_(1, 47, 1));
    I(p, MXOP_CSET, D_(10) | B_(10, 8, 5) | PPT_ | B_(1, 44, 1)); acase(p, "IADD.CC, CSET.EQ.BF", m_cset_eq, SAME_BITS);
    I(p, MXOP_IADD_reg, D_(11) | A_(2) | BR_(3) | B_(1, 48, 1) | B_(1, 47, 1));
    I(p, MXOP_CSET, D_(10) | B_(21, 8, 5) | PPT_); acase(p, "IADD.CC -b, CSET.LT", m_cset_lt, SAME_BITS);
    I(p, MXOP_ISETP_reg, A_(2) | BR_(3) | B_(1, 49, 3) | PPT_ | B_(0, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_ISETP_reg, A_(3) | BR_(4) | B_(1, 49, 3) | PPT_ | B_(1, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_PSETP, B_(0, 12, 3) | B_(1, 29, 3) | B_(2, 24, 2) | PPT_ | B_(2, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_PSET, D_(10) | B_(2, 12, 3) | B_(7, 29, 3) | PPT_ | B_(1, 44, 1)); acase(p, "PSETP.XOR, PSET.BF", m_psetp, SAME_BITS);
    I(p, MXOP_R2P_imm, A_(2) | IMM_(0x7F) | B_(1, 41, 2));   /* (the mask selects predicates, as suyu's; not shifted to the byte) */
    I(p, MXOP_PSET, D_(10) | B_(3, 12, 3) | B_(7, 29, 3) | PPT_); acase(p, "R2P b1, PSET", m_r2p, SAME_BITS);
    I(p, MXOP_MOV32I, D_(11) | I32_(0x7F) | B_(15, 12, 4));
    I(p, MXOP_R2P_reg, A_(2) | BR_(11));
    I(p, MXOP_MOV32I, D_(12) | I32_(5) | B_(15, 12, 4)); I(p, MXOP_MOV32I, D_(13) | I32_(6) | B_(15, 12, 4));
    I(p, MXOP_SEL_reg, D_(10) | A_(12) | BR_(13) | B_(2, 39, 3)); acase(p, "R2P reg, SEL", m_r2p_reg, SAME_BITS);
    I(p, MXOP_R2P_imm, A_(RZ_) | IMM_(0x7F) | B_(0, 41, 2));
    I(p, MXOP_MOV_cbuf, D_(14) | CB_(0, 0) | B_(15, 39, 4));
    I(p, MXOP_R2P_reg, A_(14) | BR_(14));
    I(p, MXOP_MOV32I, D_(12) | I32_(7) | B_(15, 12, 4)); I(p, MXOP_MOV32I, D_(13) | I32_(8) | B_(15, 12, 4));
    I(p, MXOP_SEL_reg, D_(10) | A_(12) | BR_(13) | B_(4, 39, 3)); acase(p, "R2P cbuf mask, SEL", m_r2p_cb, SAME_BITS);
    I(p, MXOP_R2P_cbuf, A_(2) | CB_(0, 24));
    I(p, MXOP_SEL_reg, D_(10) | A_(12) | BR_(13) | B_(4, 39, 3)); acase(p, "R2P cbuf, SEL", m_r2p_cbm, SAME_BITS);
    I(p, MXOP_ISETP_reg, A_(2) | BR_(3) | B_(1, 49, 3) | B_(1, 48, 1) | PPT_ | B_(0, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_PSETP, B_(7, 12, 3) | B_(7, 29, 3) | PPT_ | B_(6, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_P2R_imm, D_(10) | A_(3) | IMM_(0x41)); acase(p, "ISETP, P2R (P0, P6)", m_p2r, SAME_BITS);
    /* half pairs */
    I(p, MXOP_HADD2_reg, D_(10) | A_(2) | BR_(3)); acase(p, "HADD2", m_hadd2, SAME_HALVES);
    I(p, MXOP_HADD2_reg, D_(10) | A_(2) | BR_(3) | B_(1, 31, 1) | B_(1, 32, 1) | B_(2, 28, 2)); acase(p, "HADD2 -b.H0_H0 SAT", m_hadd2_nbs, SAME_HALVES);
    I(p, MXOP_HADD2_reg, D_(10) | A_(2) | BR_(3) | B_(1, 28, 2) | B_(1, 49, 2)); acase(p, "HADD2 b F32, merge F32", m_hadd2_f32, NEAR_ULPS);
    I(p, MXOP_MOV_reg, MOVR_(10, 4));
    I(p, MXOP_HADD2_reg, D_(10) | A_(2) | BR_(3) | B_(2, 49, 2)); acase(p, "HADD2 merge H0", m_hadd2_mrg, SAME_HALVES);
    I(p, MXOP_HADD2_imm, D_(10) | A_(2) | B_(0xF0, 20, 9) | B_(0x100, 30, 9) | B_(1, 56, 1)); acase(p, "HADD2 imm", m_hadd2_imm, SAME_HALVES);
    I(p, MXOP_HADD2_32I, D_(10) | A_(2) | I32_(0x3C004000) | B_(1, 56, 1)); acase(p, "HADD2_32I -a", m_hadd2_32i, SAME_HALVES);
    I(p, MXOP_HADD2_cbuf, D_(10) | A_(2) | CB_(0, 8)); acase(p, "HADD2 cbuf", m_hadd2_cb, SAME_HALVES);
    I(p, MXOP_HMUL2_reg, D_(10) | A_(2) | BR_(3) | B_(1, 44, 1)); acase(p, "HMUL2 |a|", m_hmul2, SAME_HALVES);
    I(p, MXOP_HMUL2_32I, D_(10) | A_(2) | I32_(0x3C004000)); acase(p, "HMUL2_32I", m_hmul2_32i, SAME_HALVES);
    I(p, MXOP_HMUL2_imm, D_(10) | A_(2) | B_(0xF0, 20, 9) | B_(0x100, 30, 9) | B_(1, 56, 1)); acase(p, "HMUL2 imm", m_hmul2_imm, SAME_HALVES);
    I(p, MXOP_HMUL2_cbuf, D_(10) | A_(2) | CB_(0, 8) | B_(1, 43, 1)); acase(p, "HMUL2 cbuf -a", m_hmul2_cb, SAME_HALVES);
    I(p, MXOP_HFMA2_reg, D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 30, 1) | B_(3, 35, 2)); acase(p, "HFMA2 -c.H1_H1", m_hfma2, SAME_HALVES);
    I(p, MXOP_MOV_reg, MOVR_(10, 4));
    I(p, MXOP_HFMA2_32I, D_(10) | A_(2) | I32_(0x40003C00) | B_(1, 52, 1)); acase(p, "HFMA2_32I -c(d)", m_hfma2_32i, SAME_HALVES);
    I(p, MXOP_HFMA2_rc, D_(10) | A_(2) | CR_(3) | CB_(0, 8)); acase(p, "HFMA2 rc", m_hfma2_rc, SAME_HALVES);
    I(p, MXOP_HFMA2_cr, D_(10) | A_(2) | CB_(0, 8) | CR_(4)); acase(p, "HFMA2 cr", m_hfma2_cr, SAME_HALVES);
    I(p, MXOP_HFMA2_imm, D_(10) | A_(2) | B_(0xF0, 20, 9) | B_(0x100, 30, 9) | B_(1, 56, 1) | CR_(4)); acase(p, "HFMA2 imm", m_hfma2_imm, SAME_HALVES);
    I(p, MXOP_HSET2_reg, D_(10) | A_(2) | BR_(3) | B_(1, 35, 4) | PPT_); acase(p, "HSET2.LT", m_hset2, SAME_BITS);
    I(p, MXOP_HSET2_reg, D_(10) | A_(2) | BR_(3) | B_(13, 35, 4) | PPT_ | B_(1, 49, 1)); acase(p, "HSET2.NEU.BF", m_hset2_bf, SAME_BITS);
    I(p, MXOP_HSET2_imm, D_(10) | A_(2) | B_(0xF0, 20, 9) | B_(0x100, 30, 9) | B_(1, 56, 1) | B_(4, 49, 4) | PPT_); acase(p, "HSET2 imm", m_hset2_imm, SAME_BITS);
    I(p, MXOP_HSET2_cbuf, D_(10) | A_(2) | CB_(0, 8) | B_(3, 49, 4) | PPT_); acase(p, "HSET2 cbuf", m_hset2_cb, SAME_BITS);
    I(p, MXOP_HSETP2_reg, A_(2) | BR_(3) | B_(6, 35, 4) | PPT_ | B_(1, 3, 3) | B_(2, 0, 3));
    I(p, MXOP_PSET, D_(10) | B_(1, 12, 3) | B_(2, 29, 3) | PPT_); acase(p, "HSETP2.GE, PSET.AND", m_hsetp2, SAME_BITS);
    I(p, MXOP_HSETP2_reg, A_(2) | BR_(3) | B_(2, 35, 4) | PPT_ | B_(1, 49, 1) | B_(7, 3, 3) | B_(3, 0, 3));
    I(p, MXOP_PSET, D_(10) | B_(3, 12, 3) | B_(7, 29, 3) | PPT_); acase(p, "HSETP2.EQ.H_AND (second)", m_hsetp2_and, SAME_BITS);
    I(p, MXOP_HSETP2_imm, A_(2) | B_(0xF0, 20, 9) | B_(0x100, 30, 9) | B_(1, 56, 1) | B_(1, 49, 4) | PPT_ | B_(4, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_MOV32I, D_(12) | I32_(3) | B_(15, 12, 4)); I(p, MXOP_MOV32I, D_(13) | I32_(4) | B_(15, 12, 4));
    I(p, MXOP_SEL_reg, D_(10) | A_(12) | BR_(13) | B_(4, 39, 3)); acase(p, "HSETP2 imm", m_hsetp2_imm, SAME_BITS);
    I(p, MXOP_HSETP2_cbuf, A_(2) | CB_(0, 8) | B_(8, 49, 4) | PPT_ | B_(7, 3, 3) | B_(4, 0, 3));
    I(p, MXOP_SEL_reg, D_(10) | A_(12) | BR_(13) | B_(4, 39, 3)); acase(p, "HSETP2 cbuf (high half)", m_hsetp2_cb, SAME_BITS);
    /* moves, selects, votes */
    I(p, MXOP_MOV32I, D_(10) | I32_(0xCAFEF00D) | B_(15, 12, 4)); acase(p, "MOV32I", m_mov32i, SAME_BITS);
    I(p, MXOP_MOV_imm, D_(10) | IMM_(-7) | B_(15, 39, 4)); acase(p, "MOV imm", m_mov_imm, SAME_BITS);
    I(p, MXOP_MOV_cbuf, D_(10) | CB_(0, 12) | B_(15, 39, 4)); acase(p, "MOV cbuf", m_mov_cb, SAME_BITS);
    I(p, MXOP_SEL_imm, D_(10) | A_(2) | IMM_(77) | PPT_ | B_(1, 42, 1)); acase(p, "SEL imm", m_sel_imm, SAME_BITS);
    I(p, MXOP_SEL_cbuf, D_(10) | A_(2) | CB_(0, 0) | PPT_ | B_(1, 42, 1)); acase(p, "SEL cbuf", m_sel_cb, SAME_BITS);
    I(p, MXOP_VOTE, D_(10) | PPT_); acase(p, "VOTE ballot (all lanes)", m_vote, SAME_BITS);
    I(p, MXOP_ISETP_imm, A_(2) | IMM_(0) | B_(5, 49, 3) | PPT_ | B_(1, 3, 3) | B_(7, 0, 3));
    I(p, MXOP_VOTE, D_(RZ_) | B_(1, 39, 3) | B_(0, 48, 2) | B_(2, 45, 3));
    I(p, MXOP_MOV32I, D_(12) | I32_(0) | B_(15, 12, 4)); I(p, MXOP_MOV32I, D_(13) | I32_(~0u) | B_(15, 12, 4));
    I(p, MXOP_SEL_reg, D_(10) | A_(13) | BR_(12) | B_(2, 39, 3) | B_(1, 42, 1)); acase(p, "VOTE.ALL (a lane's a != 0)", NULL, SAME_BITS);
    alu[nalu - 1].match = 200;   /* (for the whole SIMD group) */
    I(p, MXOP_VOTE_vtg, 0); I(p, MXOP_NOP, 0); I(p, MXOP_DEPBAR, 0); I(p, MXOP_MEMBAR, 0); I(p, MXOP_CCTL, 0); I(p, MXOP_CCTLL, 0);
    exit_(p);
    for (u32 i = 0; i < 8; i++) p->cb[0][i] = CB0[i];
    return p;
}

/* Branches: a loop, SSY/SYNC, PBK/BRK, an indirect branch through a jump table, a condition-code branch; the last thread
 * leaves at once (a guarded EXIT inside the dispatch loop). */
static Prog *build_flow(void) {
    Prog *p = prog("cs_flow", MX_COMPUTE);
    p->local[0] = 32;
    prologue(p, 4, 6);
    I(p, MXOP_ISETP_imm, A_(0) | IMM_(31) | B_(2, 49, 3) | PPT_ | B_(6, 3, 3) | B_(7, 0, 3));
    IG(p, 6, 0, MXOP_EXIT, B_(15, 0, 5));
    I(p, MXOP_MOV32I, D_(10) | I32_(0) | B_(15, 12, 4));                       /* r10 = 1 + 2 + ... + (a & 15) */
    I(p, MXOP_LOP_imm, D_(12) | A_(2) | IMM_(15));
    label(p, "loop");
    I(p, MXOP_ISETP_imm, A_(12) | IMM_(0) | B_(2, 49, 3) | PPT_ | B_(0, 3, 3) | B_(7, 0, 3));
    jump(p, 0, 0, MXOP_BRA, "done", 15);
    I(p, MXOP_IADD_reg, D_(10) | A_(10) | BR_(12));
    I(p, MXOP_IADD_imm, D_(12) | A_(12) | IMM_(-1));
    jump(p, 7, 0, MXOP_BRA, "loop", 15);
    label(p, "done");
    store(p, 10, 0);
    jump(p, 7, 0, MXOP_SSY, "join", 15);                                       /* r11 = a odd ? 1 : 2 */
    I(p, MXOP_LOP_imm, D_(13) | A_(2) | IMM_(1));
    I(p, MXOP_ISETP_imm, A_(13) | IMM_(0) | B_(5, 49, 3) | PPT_ | B_(1, 3, 3) | B_(7, 0, 3));
    jump(p, 1, 0, MXOP_BRA, "odd", 15);
    I(p, MXOP_MOV32I, D_(11) | I32_(2) | B_(15, 12, 4));
    I(p, MXOP_SYNC, B_(15, 0, 5));
    label(p, "odd");
    I(p, MXOP_MOV32I, D_(11) | I32_(1) | B_(15, 12, 4));
    I(p, MXOP_SYNC, B_(15, 0, 5));
    label(p, "join");
    store(p, 11, 1);
    I(p, MXOP_MOV32I, D_(14) | I32_(0) | B_(15, 12, 4));                       /* r14 counts to b & 7, BRK leaving */
    I(p, MXOP_LOP_imm, D_(15) | A_(3) | IMM_(7));
    jump(p, 7, 0, MXOP_PBK, "after", 15);
    label(p, "loop2");
    I(p, MXOP_ISETP_reg, A_(14) | BR_(15) | B_(6, 49, 3) | PPT_ | B_(2, 3, 3) | B_(7, 0, 3));
    IG(p, 2, 0, MXOP_BRK, B_(15, 0, 5));
    I(p, MXOP_IADD_imm, D_(14) | A_(14) | IMM_(1));
    jump(p, 7, 0, MXOP_BRA, "loop2", 15);
    label(p, "after");
    store(p, 14, 2);
    I(p, MXOP_LOP_imm, D_(16) | A_(4) | IMM_(3));                              /* r18 = 100 + (c & 3), by BRX */
    I(p, MXOP_IMNMX_imm, D_(16) | A_(16) | IMM_(3) | PPT_);
    I(p, MXOP_SHL_imm, D_(16) | A_(16) | IMM_(2));
    I(p, MXOP_LDC, D_(17) | A_(16) | B_(0x40, 20, 16) | B_(1, 36, 5) | B_(4, 48, 3));   /* (the table in c1, as NVN's) */
    u32 brx = p->n;
    I(p, MXOP_BRX, A_(17) | B_(15, 0, 5));
    static const char *const cases[4] = {"case0", "case1", "case2", "case3"};
    for (u32 k = 0; k < 4; k++) {
        label(p, cases[k]);
        I(p, MXOP_MOV32I, D_(18) | I32_(100 + k) | B_(15, 12, 4));
        if (k < 3) jump(p, 7, 0, MXOP_BRA, "joinx", 15);
    }
    label(p, "joinx");
    store(p, 18, 3);
    I(p, MXOP_IADD_reg, D_(20) | A_(2) | BR_(3) | B_(1, 47, 1));               /* r19 = a + b carries ? 1 : 2 */
    I(p, MXOP_MOV32I, D_(19) | I32_(1) | B_(15, 12, 4));
    jump(p, 7, 0, MXOP_BRA, "carried", 22);
    I(p, MXOP_MOV32I, D_(19) | I32_(2) | B_(15, 12, 4));
    label(p, "carried");
    store(p, 19, 4);
    exit_(p);
    for (u32 k = 0; k < 4; k++) p->cb[1][0x40 / 4 + k] = label_addr(p, cases[k]) - (addr_of(p, brx) + 8);
    return p;
}

/* Memory: constant buffer 3 at every size and out of range, global memory loads and stores of every size (past the
 * storage buffer: 0), shared and local memory, special registers. Two workgroups of 32; the grid's last thread leaves
 * first (a guarded EXIT, straight code). */
static const u8 *cb3_bytes(void) {
    static u8 b[64];
    for (u32 i = 0; i < 64; i++) b[i] = (u8)(i * 37 + 11);
    return b;
}
static Prog *build_mem(void) {
    Prog *p = prog("cs_mem", MX_COMPUTE);
    p->local[0] = 32;
    p->smem = 512;
    p->lmem = 256;
    prologue(p, 4, 8);
    I(p, MXOP_ISCADD_reg, D_(22) | A_(1) | BR_(0) | B_(5, 39, 5));
    I(p, MXOP_ISETP_imm, A_(22) | IMM_(63) | B_(2, 49, 3) | PPT_ | B_(6, 3, 3) | B_(7, 0, 3));
    IG(p, 6, 0, MXOP_EXIT, B_(15, 0, 5));
    I(p, MXOP_LOP_imm, D_(12) | A_(2) | IMM_(0x7F));
    for (u32 s = 0; s < 5; s++) { I(p, MXOP_LDC, D_(10) | A_(12) | B_(3, 36, 5) | B_(s, 48, 3)); store(p, 10, s); }
    I(p, MXOP_LDC, D_(10) | A_(12) | B_(3, 36, 5) | B_(5, 48, 3)); store(p, 10, 5); store(p, 11, 6);
    I(p, MXOP_LDC, D_(10) | A_(RZ_) | B_(0x10, 20, 16) | B_(3, 36, 5) | B_(4, 48, 3)); store(p, 10, 7);
    I(p, MXOP_LDC, D_(10) | A_(12) | B_(0xFFFC, 20, 16) | B_(3, 36, 5) | B_(4, 48, 3)); store(p, 10, 8);
    I(p, MXOP_LOP_imm, D_(13) | A_(3) | IMM_(0x3F));
    I(p, MXOP_IADD_reg, D_(40) | A_(30) | BR_(13));
    I(p, MXOP_MOV_reg, MOVR_(41, 31));
    for (u32 s = 0; s < 5; s++) { I(p, MXOP_LDG, D_(10) | A_(40) | B_(1, 45, 1) | B_(s, 48, 3)); store(p, 10, 9 + s); }
    I(p, MXOP_LDG, D_(10) | A_(40) | B_(1, 45, 1) | B_(5, 48, 3)); store(p, 10, 14); store(p, 11, 15);
    I(p, MXOP_IADD32I, D_(42) | A_(30) | I32_(0x100000));
    I(p, MXOP_MOV_reg, MOVR_(43, 31));
    I(p, MXOP_LDG, D_(10) | A_(42) | B_(1, 45, 1) | B_(4, 48, 3)); store(p, 10, 16);   /* past the buffer */
    I(p, MXOP_MOV_reg, MOVR_(44, 40));
    I(p, MXOP_IADD32I, D_(45) | A_(31) | I32_(1));
    I(p, MXOP_LDG, D_(10) | A_(44) | B_(1, 45, 1) | B_(4, 48, 3)); store(p, 10, 17);   /* another high word (not read: the same word) */
    I(p, MXOP_LDG, D_(10) | A_(34) | B_(1, 45, 1) | B_(6, 48, 3) | B_(16, 20, 24));
    store(p, 10, 18); store(p, 11, 19); store(p, 12, 20); store(p, 13, 21);
    I(p, MXOP_MOV32I, D_(14) | I32_(0xAAAAAAAA) | B_(15, 12, 4));
    store(p, 14, 22); store(p, 14, 23);
    I(p, MXOP_LOP_imm, D_(15) | A_(0) | IMM_(3));
    I(p, MXOP_IADD_reg, D_(46) | A_(36) | BR_(15));
    I(p, MXOP_MOV_reg, MOVR_(47, 37));
    I(p, MXOP_STG, D_(3) | A_(46) | B_(1, 45, 1) | B_(0, 48, 3) | B_(4 * 22, 20, 24));
    I(p, MXOP_STG, D_(4) | A_(36) | B_(1, 45, 1) | B_(2, 48, 3) | B_(4 * 23 + 2, 20, 24));
    I(p, MXOP_STG, D_(2) | A_(36) | B_(1, 45, 1) | B_(5, 48, 3) | B_(4 * 24, 20, 24));
    I(p, MXOP_STG, D_(2) | A_(36) | B_(1, 45, 1) | B_(6, 48, 3) | B_(4 * 28, 20, 24));
    I(p, MXOP_SHL_imm, D_(50) | A_(0) | IMM_(4));                             /* shared memory: the thread's 16 bytes */
    I(p, MXOP_STS, D_(2) | A_(50) | B_(4, 48, 3));
    I(p, MXOP_STS, D_(3) | A_(50) | B_(0, 48, 3) | B_(1, 20, 24));
    I(p, MXOP_LDS, D_(10) | A_(50) | B_(4, 48, 3)); store(p, 10, 32);
    I(p, MXOP_STS, D_(5) | A_(50) | B_(4, 48, 3) | B_(4, 20, 24));
    I(p, MXOP_STS, D_(4) | A_(50) | B_(2, 48, 3) | B_(6, 20, 24));
    I(p, MXOP_LDS, D_(10) | A_(50) | B_(3, 48, 3) | B_(6, 20, 24)); store(p, 10, 33);
    I(p, MXOP_LDS, D_(10) | A_(50) | B_(0, 48, 3) | B_(3, 20, 24)); store(p, 10, 34);
    I(p, MXOP_LDS, D_(10) | A_(50) | B_(1, 48, 3) | B_(1, 20, 24)); store(p, 10, 35);
    I(p, MXOP_LDS, D_(10) | A_(50) | B_(2, 48, 3) | B_(5, 20, 24)); store(p, 10, 36);
    I(p, MXOP_STS, D_(2) | A_(50) | B_(5, 48, 3) | B_(8, 20, 24));
    I(p, MXOP_LDS, D_(10) | A_(50) | B_(5, 48, 3) | B_(8, 20, 24)); store(p, 10, 37); store(p, 11, 38);
    I(p, MXOP_STS, D_(2) | A_(50) | B_(6, 48, 3));
    I(p, MXOP_LDS, D_(10) | A_(50) | B_(6, 48, 3)); store(p, 10, 39); store(p, 11, 40); store(p, 12, 41); store(p, 13, 42);
    I(p, MXOP_STL, D_(4) | A_(RZ_) | B_(4, 48, 3) | B_(0x20, 20, 24));        /* local memory */
    I(p, MXOP_STL, D_(2) | A_(RZ_) | B_(0, 48, 3) | B_(0x21, 20, 24));
    I(p, MXOP_LDL, D_(10) | A_(RZ_) | B_(4, 48, 3) | B_(0x20, 20, 24)); store(p, 10, 43);
    I(p, MXOP_LDL, D_(10) | A_(RZ_) | B_(1, 48, 3) | B_(0x21, 20, 24)); store(p, 10, 44);
    I(p, MXOP_STL, D_(2) | A_(RZ_) | B_(5, 48, 3) | B_(0x40, 20, 24));
    I(p, MXOP_LDL, D_(10) | A_(RZ_) | B_(5, 48, 3) | B_(0x40, 20, 24)); store(p, 10, 45); store(p, 11, 46);
    static const u32 srs[7] = {0x25, 0x1D, 0x22, 0x12, 0x20, 0, 0x50};
    for (u32 i = 0; i < 7; i++) { I(p, MXOP_S2R, D_(10) | B_(srs[i], 20, 8)); store(p, 10, 47 + i); }
    exit_(p);
    memcpy(p->cb[3], cb3_bytes(), 64);
    return p;
}

/* ---- programs that are only compiled ------------------------------------------------------ */
/* Texture forms, one family a program (SR translates a program whole or not at all): one texture unit a type (shadow
 * forms on unit 8 + type), so that each family stays within Metal's 16 samplers. Registers: coordinates from r2, the
 * operand register r8 (r4 for TEXS / TLDS / TLD4S), results r10.. */
enum { T1D, T1DA, T2D, T2DA, T3D, TBUF_, TCUBE, TCUBEA };
static u64 TEX_(u32 d, u32 a, u32 m, u32 type, u32 mask, u32 unit, u32 dc, u32 aoffi, u32 blod) {
    return D_(d) | A_(a) | BR_(m) | B_(type, 28, 3) | B_(mask, 31, 4) | TU_(unit) | B_(dc, 50, 1) | B_(aoffi, 54, 1) | B_(blod, 55, 3) | B_(7, 51, 3);
}
static u64 TEXS_(u32 da, u32 a, u32 b, u32 db, u32 unit, u32 swz, u32 enc) {
    return D_(da) | A_(a) | BR_(b) | B_(db, 28, 8) | TU_(unit) | B_(swz, 50, 3) | B_(enc, 53, 4) | B_(1, 59, 1);
}
static u64 TLD4_(u32 d, u32 a, u32 m, u32 type, u32 unit, u32 dc, u32 offmode, u32 comp) {
    return D_(d) | A_(a) | BR_(m) | B_(type, 28, 3) | B_(15, 31, 4) | TU_(unit) | B_(dc, 50, 1) | B_(offmode, 54, 2) | B_(comp, 56, 2) | B_(7, 51, 3);
}
static u64 TLD4S_(u32 da, u32 a, u32 b, u32 db, u32 unit, u32 dc, u32 aoffi, u32 comp) {
    return D_(da) | A_(a) | BR_(b) | B_(db, 28, 8) | TU_(unit) | B_(dc, 50, 1) | B_(aoffi, 51, 1) | B_(comp, 52, 2);
}
static u64 TLD_(u32 d, u32 a, u32 m, u32 type, u32 unit, u32 lod, u32 aoffi, u32 ms) {
    return D_(d) | A_(a) | BR_(m) | B_(type, 28, 3) | B_(15, 31, 4) | TU_(unit) | B_(lod, 55, 1) | B_(aoffi, 35, 1) | B_(ms, 50, 1) | B_(7, 51, 3);
}
static void texture_family(int stage, const char *name, int family) {
    Prog *p = prog(name, stage);
    int fs = stage == MX_FRAGMENT;
    if (fs) hdr32(p, 0x48, 0xF);
    switch (family) {
    case 0: {   /* TEXS, every encoding (1D unit 0, 2D 2, 2D shadow 10, 2D array 3, its shadow 11, 3D 4, cube 6) */
        static const u8 unit[14] = {0, 2, 2, 2, 10, 10, 10, 3, 3, 11, 4, 4, 6, 6};
        for (u32 i = 0; i < 14; i++) I(p, MXOP_TEXS, TEXS_(10, 2, 4, i & 1 ? RZ_ : 12, unit[i], i & 7 & (i & 1 ? 7 : 3) , i) | (i & 2 ? 0 : B_(1, 59, 1)) );
        break;
    }
    case 1: case 2: {   /* TEX on each type at each LOD mode, with and without offsets (1: compares) */
        static const u8 types[] = {T1D, T1DA, T2D, T2DA, T3D, TCUBE, TCUBEA};
        for (u32 t = 0; t < sizeof types; t++)
            for (u32 blod = 0; blod < 8; blod++) {
                if (blod == 4 || blod == 5) continue;
                for (u32 aoffi = 0; aoffi < 2; aoffi++) {
                    u32 dc = family == 2, ty = types[t];
                    if (aoffi && (ty == TCUBE || ty == TCUBEA)) continue;              /* (no offsets on cube maps) */
                    if (dc && ty == T3D) continue;                                      /* (no 3D depth textures) */
                    if (dc && ty == TCUBEA && (blod || aoffi || !fs)) continue;         /* (a cube array compare takes no LOD) */
                    I(p, MXOP_TEX, TEX_(10, 2, 8, ty, 15, dc ? 8 + ty : ty, dc, aoffi, blod));
                }
            }
        break;
    }
    case 3: {   /* TLDS, every encoding SR translates */
        static const u8 enc[] = {0, 1, 2, 4, 5, 6, 7, 8, 12}, unit[] = {0, 0, 2, 2, 2, 12, 4, 3, 2};
        for (u32 i = 0; i < sizeof enc; i++) I(p, MXOP_TLDS, TEXS_(10, 2, 4, i & 1 ? RZ_ : 12, unit[i], i & 1 ? i & 7 : i & 3, enc[i]));
        break;
    }
    case 4: {   /* TLD: each type, with an LOD, an offset, multisampled */
        for (u32 t = 0; t <= T3D; t++)
            for (u32 v = 0; v < 4; v++) I(p, MXOP_TLD, TLD_(10, 2, 8, t, t, v & 1, v >> 1, 0));
        I(p, MXOP_TLD, TLD_(10, 2, 8, T2D, 12, 0, 0, 1));
        I(p, MXOP_TLD, TLD_(10, 2, 8, T2DA, 13, 0, 0, 1));
        break;
    }
    case 5: {   /* TLD4 and TLD4S: gathers of each type, offset mode, compare */
        static const u8 gtypes[] = {T2D, T2DA, TCUBE, TCUBEA};
        for (u32 t = 0; t < sizeof gtypes; t++)
            for (u32 off = 0; off < 3; off++)
                for (u32 dc = 0; dc < 2; dc++) {
                    if (off && gtypes[t] >= TCUBE) continue;   /* (cube maps take no offsets) */
                    I(p, MXOP_TLD4, TLD4_(10, 2, 8, gtypes[t], dc ? 8 + gtypes[t] : gtypes[t], dc, off, (off + dc) & 3));
                }
        for (u32 v = 0; v < 4; v++) I(p, MXOP_TLD4S, TLD4S_(10, 2, 4, 12, v >> 1 ? 10 : 2, v >> 1, v & 1, v));
        break;
    }
    case 6: {   /* TXD (with offsets), TXQ, and TMML in fragment programs */
        for (u32 t = 0; t <= T2DA; t++)
            for (u32 off = 0; off < 2; off++) I(p, MXOP_TXD, D_(10) | A_(2) | BR_(20) | B_(t, 28, 3) | B_(15, 31, 4) | TU_(t) | B_(off, 35, 1) | B_(7, 51, 3));
        I(p, MXOP_TXQ, D_(10) | A_(2) | B_(1, 22, 3) | B_(15, 31, 4) | TU_(2));
        if (fs) {
            static const u8 ltypes[] = {T1D, T1DA, T2D, T2DA, T3D, TCUBE, TCUBEA};
            for (u32 t = 0; t < sizeof ltypes; t++) I(p, MXOP_TMML, D_(10) | A_(2) | B_(ltypes[t], 28, 3) | B_(3, 31, 4) | TU_(ltypes[t]));
        }
        break;
    }
    }
    if (fs) { I(p, MXOP_MOV_reg, MOVR_(0, 10)); I(p, MXOP_MOV_reg, MOVR_(1, 11)); I(p, MXOP_MOV_reg, MOVR_(2, 12)); I(p, MXOP_MOV_reg, MOVR_(3, 13)); }
    else if (stage == MX_VERTEX) I(p, MXOP_AST, D_(10) | A_(RZ_) | B_(0x70, 20, 10) | B_(3, 47, 2));
    exit_(p);
}
/* image loads and stores: every type, typed sizes, typeless with the known formats (opts 1) */
static void images(int stage, const char *name, int typeless) {
    Prog *p = prog(name, stage);
    if (stage == MX_FRAGMENT) hdr32(p, 0x48, 0xF);
    p->local[0] = 8;
    for (u32 ty = 0; ty < 6; ty++) {
        u32 k = ty;
        if (typeless) {
            p->opts = 1;
            I(p, MXOP_SULD, D_(10) | A_(2) | B_(ty, 33, 3) | B_(k == 1 ? 15 : 3, 20, 4) | B_(72 + 2 * k, 36, 13) | B_(1, 51, 1));
            I(p, MXOP_SUST, D_(10) | A_(2) | B_(ty, 33, 3) | B_(15, 20, 4) | B_(72 + 2 * k, 36, 13) | B_(1, 51, 1));
        } else {
            u32 sz = ty == 5 ? 6 : ty;   /* (an image's type and format must agree in a program) */
            I(p, MXOP_SULD, D_(10) | A_(2) | B_(ty, 33, 3) | B_(sz, 20, 3) | B_(72 + 2 * k, 36, 13) | B_(1, 51, 1) | B_(1, 52, 1));
            I(p, MXOP_SUST, D_(14) | A_(2) | B_(ty, 33, 3) | B_(sz, 20, 3) | B_(72 + 2 * k, 36, 13) | B_(1, 51, 1) | B_(1, 52, 1));
        }
    }
    I(p, MXOP_SUST, D_(14) | A_(2) | B_(3, 33, 3) | B_(4, 20, 3) | B_(72 + 2 * 6, 36, 13) | B_(1, 51, 1) | B_(1, 52, 1));   /* (write only) */
    if (stage == MX_VERTEX) I(p, MXOP_AST, D_(10) | A_(RZ_) | B_(0x70, 20, 10) | B_(3, 47, 2));
    exit_(p);
}

static void build_compiled(void) {
    /* a vertex and fragment pair */
    Prog *v = prog("vs_basic", MX_VERTEX);
    I(v, MXOP_ALD, D_(0) | A_(RZ_) | B_(0x80, 20, 10) | B_(3, 47, 2));
    I(v, MXOP_ALD, D_(4) | A_(RZ_) | B_(0x90, 20, 10) | B_(3, 47, 2));
    I(v, MXOP_LDC, D_(8) | A_(4) | B_(0x10, 20, 16) | B_(3, 36, 5) | B_(4, 48, 3));
    I(v, MXOP_AST, D_(0) | A_(RZ_) | B_(0x70, 20, 10) | B_(3, 47, 2));
    I(v, MXOP_AST, D_(4) | A_(RZ_) | B_(0x80, 20, 10) | B_(3, 47, 2));
    jump(v, 7, 0, MXOP_BRA, "end", 15);
    label(v, "end");
    exit_(v);
    Prog *f = prog("fs_basic", MX_FRAGMENT);
    f->pair = (int)ncorpus - 2;
    f->hdr[0x18] = 0xAA;
    hdr32(f, 0x48, 0xF);
    I(f, MXOP_IPA, D_(0) | A_(RZ_) | BR_(RZ_) | B_(0x80 / 4, 30, 8));
    I(f, MXOP_IPA, D_(1) | A_(RZ_) | BR_(RZ_) | B_(0x84 / 4, 30, 8));
    I(f, MXOP_TEXS, TEXS_(0, 0, 1, 2, 2, 4, 1));
    I(f, MXOP_TLD4, TLD4_(4, 0, 8, T2D, 2, 0, 2, 1));
    IG(f, 0, 0, MXOP_KIL, B_(15, 0, 5));
    exit_(f);

    /* texture forms in fragment (straight: derivatives), vertex and compute programs */
    static const char *const fam[7] = {"texs", "tex", "tex_shadow", "tlds", "tld", "tld4", "txd_txq_tmml"};
    static char names[3][7][32];
    for (int s = 0; s < 3; s++)
        for (int k = 0; k < 7; k++) {
            int stage = s == 0 ? MX_FRAGMENT : s == 1 ? MX_VERTEX : MX_COMPUTE;
            snprintf(names[s][k], sizeof names[s][k], "%s_%s", s == 0 ? "fs" : s == 1 ? "vs" : "cs", fam[k]);
            texture_family(stage, names[s][k], k);
        }
    /* in a dispatch loop too (no derivatives) */
    Prog *tl = prog("fs_tex_loop", MX_FRAGMENT);
    hdr32(tl, 0x48, 0xF);
    I(tl, MXOP_ISETP_imm, A_(1) | IMM_(0) | B_(2, 49, 3) | PPT_ | B_(6, 3, 3) | B_(7, 0, 3));
    jump(tl, 6, 0, MXOP_BRA, "skip", 15);
    I(tl, MXOP_TEX, TEX_(10, 2, 8, T2D, 15, T2D, 0, 1, 0));
    I(tl, MXOP_TEXS, TEXS_(10, 2, 4, 12, T2D, 4, 1));
    I(tl, MXOP_TLD4S, TLD4S_(10, 2, 4, 12, T2D, 0, 1, 1));
    label(tl, "skip");
    exit_(tl);
    images(MX_COMPUTE, "cs_images", 0);
    images(MX_COMPUTE, "cs_images_typeless", 1);
    images(MX_FRAGMENT, "fs_images", 0);
    images(MX_VERTEX, "vs_images", 0);

    /* attributes in and out, special inputs and outputs, global memory, local memory */
    Prog *va = prog("vs_attributes", MX_VERTEX);
    u32 vo = (u32)ncorpus - 1;
    hdr32(va, 4, 64);   /* (64 bytes of local memory) */
    for (u32 n = 0; n < 16; n++) {
        I(va, MXOP_ALD, D_(100) | A_(RZ_) | B_(0x80 + 16 * n, 20, 10) | B_(3, 47, 2));
        I(va, MXOP_AST, D_(100) | A_(RZ_) | B_(0x80 + 16 * n, 20, 10) | B_(3, 47, 2));
        I(va, MXOP_AST, D_(100) | A_(RZ_) | B_(0x80 + 16 * (n + 12), 20, 10) | B_(3, 47, 2));
    }
    I(va, MXOP_ALD, D_(104) | A_(RZ_) | B_(0x80, 20, 10) | B_(3, 47, 2) | B_(1, 32, 1));   /* (an output read back) */
    I(va, MXOP_ALD, D_(2) | A_(RZ_) | B_(0x2F8, 20, 10) | B_(1, 47, 2));     /* instance, vertex id */
    I(va, MXOP_AST, D_(2) | A_(RZ_) | B_(0x6C, 20, 10) | B_(0, 47, 2));      /* point size */
    I(va, MXOP_AST, D_(2) | A_(RZ_) | B_(0x2C0, 20, 10) | B_(3, 47, 2));     /* clip distances 0..3 */
    I(va, MXOP_AST, D_(2) | A_(RZ_) | B_(0x64, 20, 10) | B_(1, 47, 2));      /* layer, viewport */
    I(va, MXOP_AST, D_(2) | A_(RZ_) | B_(0x300, 20, 10) | B_(3, 47, 2));     /* (a legacy output: ignored) */
    I(va, MXOP_MOV_cbuf, D_(30) | CB_(0, SBG_(0)) | B_(15, 39, 4));
    I(va, MXOP_LDG, D_(4) | A_(30) | B_(1, 45, 1) | B_(4, 48, 3));
    I(va, MXOP_STL, D_(4) | A_(RZ_) | B_(4, 48, 3) | B_(0x20, 20, 24));
    I(va, MXOP_LDL, D_(5) | A_(RZ_) | B_(1, 48, 3) | B_(0x21, 20, 24));
    I(va, MXOP_S2R, D_(7) | B_(0, 20, 8));                                   /* lane */
    I(va, MXOP_S2R, D_(8) | B_(0x39, 20, 8));
    I(va, MXOP_AST, D_(4) | A_(RZ_) | B_(0x70, 20, 10) | B_(3, 47, 2));
    exit_(va);
    /* eight targets, depth and the sample mask; inputs of every interpolation; special inputs; global memory */
    Prog *o8 = prog("fs_outputs", MX_FRAGMENT);
    o8->pair = (int)vo;
    hdr32(o8, 0x48, 0xFFFFFFFF);
    hdr32(o8, 0x4C, 3);
    for (u32 n = 0; n < 8; n++) o8->hdr[0x18 + n] = (u8)(n % 3 == 0 ? 0x55 : n % 3 == 1 ? 0xAA : 0xFF);
    I(o8, MXOP_IPA, D_(9) | A_(RZ_) | BR_(RZ_) | B_(0x7C / 4, 30, 8));
    for (u32 n = 0; n < 8; n++) {
        I(o8, MXOP_IPA, D_(4 * n) | A_(RZ_) | BR_(9) | B_((0x80 + 16 * n) / 4, 30, 8) | B_(n & 1, 54, 2) | B_(n >> 2 & 1, 51, 1));
        I(o8, MXOP_IPA, D_(4 * n + 1) | A_(RZ_) | BR_(RZ_) | B_((0x84 + 16 * n) / 4, 30, 8));
    }
    static const u32 at[9] = {0x70, 0x74, 0x78, 0x3FC, 0x2E0, 0x2E4, 0x60, 0x64, 0x68};
    for (u32 s = 0; s < 9; s++) I(o8, MXOP_IPA, D_(40 + s) | A_(RZ_) | BR_(RZ_) | B_(at[s] / 4, 30, 8));
    I(o8, MXOP_MOV_cbuf, D_(50) | CB_(0, 0x510) | B_(15, 39, 4));
    I(o8, MXOP_LDG, D_(52) | A_(50) | B_(1, 45, 1) | B_(6, 48, 3));
    I(o8, MXOP_STG, D_(52) | A_(50) | B_(1, 45, 1) | B_(1, 48, 3) | B_(0x11, 20, 24));
    I(o8, MXOP_S2R, D_(56) | B_(0x13, 20, 8));                               /* helper invocation */
    I(o8, MXOP_S2R, D_(57) | B_(0x3C, 20, 8));
    I(o8, MXOP_VOTE, D_(58) | PPT_ | B_(1, 48, 2) | B_(1, 45, 3));
    exit_(o8);
    Prog *nd = prog("fs_no_outputs", MX_FRAGMENT);
    IG(nd, 0, 1, MXOP_KIL, B_(15, 0, 5));
    exit_(nd);
    Prog *dz = prog("fs_depth_only", MX_FRAGMENT);
    hdr32(dz, 0x4C, 2);
    I(dz, MXOP_IPA, D_(1) | A_(RZ_) | BR_(RZ_) | B_(0x78 / 4, 30, 8));
    I(dz, MXOP_FADD_reg, D_(3) | A_(1) | BR_(1) | B_(1, 47, 1));
    I(dz, MXOP_KIL, B_(1, 0, 5));   /* (a condition-code test) */
    exit_(dz);
    Prog *rs = prog("fs_scaled", MX_FRAGMENT);   /* (MxIn.screen_scale: opts 1) */
    rs->opts = 1;
    hdr32(rs, 0x48, 0xF);
    I(rs, MXOP_IPA, D_(4) | A_(RZ_) | BR_(RZ_) | B_(0x70 / 4, 30, 8));
    I(rs, MXOP_TLDS, TEXS_(0, 2, 4, 2, T2D, 4, 2));
    I(rs, MXOP_TLDS, TEXS_(0, 2, 4, 2, T2D, 4, 4));
    I(rs, MXOP_TLD, TLD_(0, 2, 8, T2D, T2D, 1, 1, 0));
    exit_(rs);
    /* every flow test of a branch, nested SSY and PBK, in a vertex program */
    Prog *cf = prog("vs_flow", MX_VERTEX);
    I(cf, MXOP_IADD_reg, D_(1) | A_(2) | BR_(3) | B_(1, 47, 1));
    for (u32 test = 0; test < 32; test++) {
        static char tn[32][8];
        if (test >= 24 && test < 28) continue;   /* (no such tests) */
        if (test == 29) continue;
        snprintf(tn[test], sizeof tn[test], "t%u", test);
        jump(cf, 7, 0, MXOP_BRA, tn[test], test);
        I(cf, MXOP_IADD32I, D_(4) | A_(4) | I32_(test));
        label(cf, tn[test]);
    }
    jump(cf, 7, 0, MXOP_SSY, "s_out", 15);
    jump(cf, 7, 0, MXOP_PBK, "p_out", 15);
    label(cf, "in");
    I(cf, MXOP_ISETP_imm, A_(4) | IMM_(100) | B_(1, 49, 3) | PPT_ | B_(0, 3, 3) | B_(1, 0, 3));
    IG(cf, 0, 0, MXOP_SYNC, B_(15, 0, 5));
    IG(cf, 1, 0, MXOP_BRK, B_(15, 0, 5));
    I(cf, MXOP_IADD32I, D_(4) | A_(4) | I32_(7));
    IG(cf, 0, 1, MXOP_EXIT, B_(15, 0, 5));
    jump(cf, 7, 0, MXOP_BRA, "in", 15);
    label(cf, "p_out");
    I(cf, MXOP_MOV32I, D_(5) | I32_(1) | B_(15, 12, 4));
    label(cf, "s_out");
    I(cf, MXOP_AST, D_(4) | A_(RZ_) | B_(0x70, 20, 10) | B_(3, 47, 2));
    exit_(cf);
    /* PCNT / CONT */
    Prog *pc = prog("cs_cont", MX_COMPUTE);
    I(pc, MXOP_S2R, D_(0) | B_(0x21, 20, 8));
    jump(pc, 7, 0, MXOP_PCNT, "next", 15);
    I(pc, MXOP_ISETP_imm, A_(0) | IMM_(3) | B_(1, 49, 3) | PPT_ | B_(0, 3, 3) | B_(7, 0, 3));
    IG(pc, 0, 0, MXOP_CONT, B_(15, 0, 5));
    I(pc, MXOP_IADD32I, D_(2) | A_(2) | I32_(5));
    label(pc, "next");
    I(pc, MXOP_IADD32I, D_(2) | A_(2) | I32_(1));
    exit_(pc);
    /* a long dispatch loop (the compile time of a big program), shared memory, subgroup masks */
    Prog *big = prog("cs_big", MX_COMPUTE);
    big->local[0] = 64; big->local[1] = 2;
    big->smem = 4096;
    for (u32 k = 0; k < 250; k++) {
        static char bn[250][8];
        snprintf(bn[k], sizeof bn[k], "b%u", k);
        I(big, MXOP_XMAD_reg, D_(2 + k % 8) | A_(2 + (k + 1) % 8) | BR_(2 + (k + 3) % 8) | CR_(2 + (k + 5) % 8));
        I(big, MXOP_ISETP_imm, A_(2 + k % 8) | IMM_(k) | B_(1, 49, 3) | PPT_ | B_(0, 3, 3) | B_(7, 0, 3));
        jump(big, 0, 0, MXOP_BRA, bn[k], 15);
        I(big, MXOP_STS, D_(2 + k % 8) | A_(2) | B_(4, 48, 3) | B_(4 * k, 20, 24));
        label(big, bn[k]);
    }
    for (u32 s = 0x38; s <= 0x3C; s++) I(big, MXOP_S2R, D_(20 + s - 0x38) | B_(s, 20, 8));
    I(big, MXOP_S2R, D_(30) | B_(0x23, 20, 8));
    I(big, MXOP_S2R, D_(31) | B_(0x27, 20, 8));
    exit_(big);
    /* storage buffers written with the write mask (opts 1: track_writes), uniform buffers through global memory */
    Prog *gm = prog("cs_gmem", MX_COMPUTE);
    gm->opts = 1;
    I(gm, MXOP_MOV_cbuf, D_(30) | CB_(0, SB_(2)) | B_(15, 39, 4));
    I(gm, MXOP_MOV_cbuf, D_(31) | CB_(0, 0x210 + 16 * 5) | B_(15, 39, 4));
    I(gm, MXOP_LDG, D_(4) | A_(31) | B_(1, 45, 1) | B_(5, 48, 3) | B_(8, 20, 24));
    I(gm, MXOP_STG, D_(4) | A_(30) | B_(1, 45, 1) | B_(1, 48, 3) | B_(3, 20, 24));
    I(gm, MXOP_STG, D_(4) | A_(30) | B_(1, 45, 1) | B_(6, 48, 3) | B_(16, 20, 24));
    exit_(gm);
}

static void build_corpus(void) {
    if (ncorpus) return;
    build_alu();
    build_flow();
    build_mem();
    build_compiled();
    for (u32 i = 0; i < ncorpus; i++) assemble(corpus[i]);
}
static const char *stage_name(int stage) { return stage == MX_VERTEX ? "vertex" : stage == MX_FRAGMENT ? "fragment" : "compute"; }

/* ---- random programs ---------------------------------------------------------------------- */
/* The same every run: 1..6 instructions, each one of the encodings mx_translate translates with random fields (cleaned
 * up now and then, so that more of them translate: texture and image handles, attribute addresses, special registers),
 * random headers, constant buffers (jump tables), workgroups, local and shared memory. */
#define NFUZZ 3000
static const int k_handled[] = {
    MXOP_ALD, MXOP_AST, MXOP_BFE_cbuf, MXOP_BFE_imm, MXOP_BFE_reg, MXOP_BFI_cr, MXOP_BFI_imm, MXOP_BFI_rc, MXOP_BFI_reg, MXOP_BRA, MXOP_BRK, MXOP_BRX,
    MXOP_CCTL, MXOP_CCTLL, MXOP_CONT, MXOP_CSET, MXOP_CSETP, MXOP_DEPBAR, MXOP_EXIT, MXOP_F2F_cbuf, MXOP_F2F_imm, MXOP_F2F_reg, MXOP_F2I_cbuf,
    MXOP_F2I_reg, MXOP_FADD32I, MXOP_FADD_cbuf, MXOP_FADD_imm, MXOP_FADD_reg, MXOP_FCMP_cr, MXOP_FCMP_imm, MXOP_FCMP_rc, MXOP_FCMP_reg, MXOP_FFMA32I,
    MXOP_FFMA_cr, MXOP_FFMA_imm, MXOP_FFMA_rc, MXOP_FFMA_reg, MXOP_FLO_cbuf, MXOP_FLO_imm, MXOP_FLO_reg, MXOP_FMNMX_cbuf, MXOP_FMNMX_imm, MXOP_FMNMX_reg,
    MXOP_FMUL32I, MXOP_FMUL_cbuf, MXOP_FMUL_imm, MXOP_FMUL_reg, MXOP_FSETP_cbuf, MXOP_FSETP_imm, MXOP_FSETP_reg, MXOP_FSET_cbuf, MXOP_FSET_imm,
    MXOP_FSET_reg, MXOP_HADD2_32I, MXOP_HADD2_cbuf, MXOP_HADD2_imm, MXOP_HADD2_reg, MXOP_HFMA2_32I, MXOP_HFMA2_cr, MXOP_HFMA2_imm, MXOP_HFMA2_rc,
    MXOP_HFMA2_reg, MXOP_HMUL2_32I, MXOP_HMUL2_cbuf, MXOP_HMUL2_imm, MXOP_HMUL2_reg, MXOP_HSET2_cbuf, MXOP_HSET2_imm, MXOP_HSET2_reg, MXOP_HSETP2_cbuf,
    MXOP_HSETP2_imm, MXOP_HSETP2_reg, MXOP_I2F_cbuf, MXOP_I2F_imm, MXOP_I2F_reg, MXOP_I2I_cbuf, MXOP_I2I_imm, MXOP_I2I_reg, MXOP_IADD32I, MXOP_IADD3_cbuf,
    MXOP_IADD3_imm, MXOP_IADD3_reg, MXOP_IADD_cbuf, MXOP_IADD_imm, MXOP_IADD_reg, MXOP_ICMP_cr, MXOP_ICMP_imm, MXOP_ICMP_rc, MXOP_ICMP_reg,
    MXOP_IMNMX_cbuf, MXOP_IMNMX_imm, MXOP_IMNMX_reg, MXOP_IPA, MXOP_ISCADD32I, MXOP_ISCADD_cbuf, MXOP_ISCADD_imm, MXOP_ISCADD_reg, MXOP_ISETP_cbuf,
    MXOP_ISETP_imm, MXOP_ISETP_reg, MXOP_ISET_cbuf, MXOP_ISET_imm, MXOP_ISET_reg, MXOP_KIL, MXOP_LDC, MXOP_LDG, MXOP_LDL, MXOP_LDS, MXOP_LEA_hi_cbuf,
    MXOP_LEA_hi_reg, MXOP_LEA_lo_cbuf, MXOP_LEA_lo_imm, MXOP_LEA_lo_reg, MXOP_LOP32I, MXOP_LOP3_cbuf, MXOP_LOP3_imm, MXOP_LOP3_reg, MXOP_LOP_cbuf,
    MXOP_LOP_imm, MXOP_LOP_reg, MXOP_MEMBAR, MXOP_MOV32I, MXOP_MOV_cbuf, MXOP_MOV_imm, MXOP_MOV_reg, MXOP_MUFU, MXOP_NOP, MXOP_P2R_imm, MXOP_PBK,
    MXOP_PCNT, MXOP_POPC_cbuf, MXOP_POPC_imm, MXOP_POPC_reg, MXOP_PSET, MXOP_PSETP, MXOP_R2P_cbuf, MXOP_R2P_imm, MXOP_R2P_reg, MXOP_RRO_cbuf,
    MXOP_RRO_imm, MXOP_RRO_reg, MXOP_S2R, MXOP_SEL_cbuf, MXOP_SEL_imm, MXOP_SEL_reg, MXOP_SHF_l_imm, MXOP_SHF_l_reg, MXOP_SHF_r_imm, MXOP_SHF_r_reg,
    MXOP_SHL_cbuf, MXOP_SHL_imm, MXOP_SHL_reg, MXOP_SHR_cbuf, MXOP_SHR_imm, MXOP_SHR_reg, MXOP_SSY, MXOP_STG, MXOP_STL, MXOP_STS, MXOP_SULD, MXOP_SUST,
    MXOP_SYNC, MXOP_TEX, MXOP_TEXS, MXOP_TLD, MXOP_TLD4, MXOP_TLD4S, MXOP_TLDS, MXOP_TMML, MXOP_TXD, MXOP_TXQ, MXOP_VOTE, MXOP_VOTE_vtg, MXOP_XMAD_cr,
    MXOP_XMAD_imm, MXOP_XMAD_rc, MXOP_XMAD_reg};
#define NHANDLED (sizeof k_handled / sizeof k_handled[0])
static u64 fuzz_state = 0x9E3779B97F4A7C15ull;
static u64 fuzz_next(void) {   /* xorshift64* */
    fuzz_state ^= fuzz_state >> 12; fuzz_state ^= fuzz_state << 25; fuzz_state ^= fuzz_state >> 27;
    return fuzz_state * 0x2545F4914F6CDD1Dull;
}
static void fuzz_program(Prog *p, u32 i) {
    free(p->w); free(p->op); free(p->bytes);
    memset(p, 0, sizeof *p);
    p->name = "fuzz";
    p->stage = (int)(i % 3) == 2 ? MX_COMPUTE : (int)(i % 3);
    p->pair = -1;
    p->opts = (int)(fuzz_next() & 1);
    for (u32 k = 0; k < 0x50; k += 8) { u64 r = fuzz_next(); memcpy(p->hdr + k, &r, 8); }
    if (fuzz_next() % 2) { u32 small = (u32)(fuzz_next() % 256); memcpy(p->hdr + 4, &small, 4); memset(p->hdr + 8, 0, 4); }   /* (local memory) */
    for (u32 b = 0; b < 4; b++) for (u32 k = 0; k < 128; k++) p->cb[b][k] = (u32)fuzz_next() & (k & 1 ? 0x3F8u : 0xFFFFFFFFu);
    p->local[0] = 1 + (u32)(fuzz_next() % 64); p->local[1] = 1 + (u32)(fuzz_next() % 4); p->local[2] = 1;
    p->smem = (u32)(fuzz_next() % 3) * 1024; p->lmem = (u32)(fuzz_next() % 3) * 64;
    u32 n = 1 + (u32)(fuzz_next() % 6);
    for (u32 k = 0; k < n; k++) {
        int op = k_handled[fuzz_next() % NHANDLED];
        u64 fixed = opcode(op), mask = 0, w;
        for (size_t t = 0; t < sizeof mx_op_table / sizeof mx_op_table[0]; t++) if (mx_op_table[t].op == op) mask = mx_op_table[t].mask;
        w = (fuzz_next() & ~mask) | fixed;
        if (fuzz_next() % 3) w = (w & ~B_(15, 16, 4)) | B_(7, 16, 3);   /* (mostly unguarded) */
        if (fuzz_next() % 4 == 0) w = (w & ~B_(0xFFFFFF, 20, 24)) | B_((u32)(fuzz_next() % 64) * 8 - 128, 20, 24);   /* (near branches) */
        if (fuzz_next() % 2) w = (w & ~B_(31, 34, 5)) | B_(BITS_(w, 34, 5) % 18, 34, 5);   /* (constant buffers 0..17) */
        if (op == MXOP_LDC) w = (w & ~B_(31, 36, 5)) | B_(BITS_(w, 36, 5) % 18, 36, 5);
        if (fuzz_next() % 2) {   /* (fields that make most random words fail, now and then set to what translates) */
            switch (op) {
            case MXOP_TEX: case MXOP_TEXS: case MXOP_TLD: case MXOP_TLDS: case MXOP_TLD4: case MXOP_TLD4S: case MXOP_TMML: case MXOP_TXD: case MXOP_TXQ:
                w = (w & ~B_(0x1FFF, 36, 13)) | TU_(fuzz_next() % 16);
                if (op == MXOP_TXQ) w = (w & ~B_(7, 22, 3)) | B_(1, 22, 3);
                if (op == MXOP_TEX) w &= ~B_(1, 58, 1);
                break;
            case MXOP_SULD: case MXOP_SUST:
                w = (w & ~B_(0x1FFF, 36, 13) & ~B_(3, 49, 2)) | B_(72 + 2 * (fuzz_next() % 8), 36, 13) | B_(1, 51, 1);
                break;
            case MXOP_ALD: case MXOP_AST: case MXOP_IPA: {
                static const u32 ad[] = {0x70, 0x74, 0x7C, 0x80, 0x90, 0xA4, 0x1F0, 0x60, 0x64, 0x68, 0x6C, 0x2C0, 0x2C8, 0x2E0, 0x2E4, 0x2F8, 0x2FC, 0x3FC};
                u32 a = ad[fuzz_next() % (sizeof ad / sizeof ad[0])];
                if (op == MXOP_IPA) w = (w & ~B_(0xFF, 30, 8) & ~B_(1, 38, 1)) | B_(a / 4, 30, 8);
                else w = (w & ~B_(0x3FF, 20, 10) & ~B_(1, 31, 1) & ~B_(0xFF, 8, 8)) | B_(a, 20, 10) | A_(RZ_);
                break;
            }
            case MXOP_S2R: {
                static const u8 sr[] = {0x00, 0x11, 0x12, 0x13, 0x1C, 0x1D, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x50};
                w = (w & ~B_(0xFF, 20, 8)) | B_(sr[fuzz_next() % sizeof sr], 20, 8);
                break;
            }
            case MXOP_LDG: case MXOP_STG:
                if (BITS_(w, 8, 8) == RZ_) w = (w & ~B_(0xFF, 8, 8)) | A_(5);
                break;
            case MXOP_MOV_reg: case MXOP_MOV_cbuf: case MXOP_MOV_imm: w |= B_(15, 39, 4); break;
            }
        }
        IG(p, 7, 0, op, 0);
        p->w[p->n - 1] = w;
        p->op[p->n - 1] = -1;   /* (not checked: the fields are random) */
    }
    IG(p, 7, 0, MXOP_EXIT, B_(15, 0, 5));
    assemble(p);
}

/* ---- GLSL: written, and compared with what an older translator wrote ----------------------- */
static void put_out(FILE *f, int ok, const MxOut *o) {
    if (!ok) { fprintf(f, "FAILED: %s: %s\n", o->op, o->why); return; }
    fprintf(f, "out: cbufs %08x in %08x out %08x tex %08x ssbos %08x/%08x images %08x/%08x/%08x int_dep %08x insns %d blocks %d alpha %d formats",
            o->cbufs, o->in_attr, o->out_attr, o->tex_units, o->ssbos, o->ssbo_written, o->images, o->images_read, o->images_written, o->int_dep,
            o->insns, o->blocks, o->uses_alpha);
    for (int k = 0; k < 8; k++) fprintf(f, " %x", o->image_gl_format[k]);
    fprintf(f, "\nout: %d samplers", o->nsmp);
    for (int k = 0; k < o->nsmp; k++) fprintf(f, " %u/%u/%u/%s", o->smp[k].unit, o->smp[k].shadow, o->smp[k].binding, o->smp[k].type);
    fprintf(f, "\n%s", o->glsl);
}
static void glsl_one(FILE *f, const char *name, u32 index, const Prog *p) {
    for (int target = 0; target < 2; target++)
        for (int opts = 0; opts < 2; opts++) {
            MxOut out;
            int ok = translate(p, target, opts, &out);
            fprintf(f, "==== %s %u (%s, target %d, options %d)\n", name, index, stage_name(p->stage), target, opts);
            put_out(f, ok, &out);
            free(out.glsl);
        }
}
static int glsl_write(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    for (u32 i = 0; i < ncorpus; i++) glsl_one(f, corpus[i]->name, i, corpus[i]);
    static Prog fz;
    fuzz_state = 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < NFUZZ; i++) {
        fuzz_program(&fz, i);
        glsl_one(f, "fuzz", i, &fz);
    }
    fclose(f);
    return 0;
}
static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(size + 1);
    size_t got = fread(s, 1, size, f);
    s[got] = 0;
    fclose(f);
    if (n) *n = got;
    return s;
}
static int glsl_diff(const char *baseline) {
    char now[1024];
    snprintf(now, sizeof now, "%s.now", baseline);
    if (glsl_write(now)) return 1;
    size_t na, nb;
    char *a = slurp(baseline, &na), *b = slurp(now, &nb);
    if (!a || !b) { fprintf(stderr, "cannot read %s\n", a ? now : baseline); return 1; }
    if (na == nb && !memcmp(a, b, na)) {
        printf("GLSL: %u programs and %u random ones, targets 0 and 1, two option sets, %zu bytes: identical to %s\n", ncorpus, NFUZZ, na, baseline);
        remove(now);
        return 0;
    }
    size_t at = 0, line = 1, start = 0;
    while (at < na && at < nb && a[at] == b[at]) { if (a[at] == '\n') { line++; start = at + 1; } at++; }
    const char *prog_name = "?";
    for (size_t k = start; k > 0; k--)
        if (!strncmp(a + k, "==== ", 5) && a[k - 1] == '\n') { prog_name = a + k; break; }
    printf("GLSL DIFFERS from %s (now in %s) at line %zu, in %.60s\n  was: %.200s\n  now: %.200s\n", baseline, now, line, prog_name, a + start, b + start);
    return 1;
}

#ifndef MXMSLTEST_GLSL_ONLY
/* ---- Metal ------------------------------------------------------------------------------------ */
static Prog *find_prog(const char *name) {
    for (u32 i = 0; i < ncorpus; i++) if (!strcmp(corpus[i]->name, name)) return corpus[i];
    return NULL;
}
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static int verbose;

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}
/* As a renderer must compile: safe math (fast math breaks NaN compares), invariant positions. floor14: also as on
 * macOS 14 (MSL 3.1, which has no mathMode). */
static MTLCompileOptions *options(int floor14) {
    MTLCompileOptions *o = [MTLCompileOptions new];
    o.preserveInvariance = YES;
    if (floor14) {
        o.languageVersion = MTLLanguageVersion3_1;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        o.fastMathEnabled = NO;
#pragma clang diagnostic pop
    } else if (@available(macOS 15.0, *)) o.mathMode = MTLMathModeSafe;
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        o.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    return o;
}
static id<MTLLibrary> compile(const char *src, int floor14, double *ms, NSString **error) {
    NSError *err = nil;
    double t0 = now_ms();
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:options(floor14) error:&err];
    if (ms) *ms = now_ms() - t0;
    if (error) *error = lib ? nil : err.localizedDescription;
    else if (!lib) printf("  compile: %s\n", err.localizedDescription.UTF8String);
    return lib;
}
/* mx_main, specialised for the render targets' kinds (MX_MSL_RT_TYPES: 2 bits each, 0 float, 1 uint, 2 int) and, for a vertex
 * function, for points or not (MX_MSL_POINTS; -1: left undefined) */
static id<MTLFunction> function(id<MTLLibrary> lib, u32 rt_types, int points) {
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    BOOL pts = points != 0;
    [cv setConstantValue:&rt_types type:MTLDataTypeUInt atIndex:MX_MSL_RT_TYPES];
    if (points >= 0) [cv setConstantValue:&pts type:MTLDataTypeBool atIndex:MX_MSL_POINTS];
    NSError *err = nil;
    id<MTLFunction> f = [lib newFunctionWithName:@"mx_main" constantValues:cv error:&err];
    if (!f) printf("  specialise: %s\n", err.localizedDescription.UTF8String);
    return f;
}
static const char k_frag_any[] = "#include <metal_stdlib>\nusing namespace metal;\nfragment float4 mx_main() { return float4(0.0); }\n";

/* a render pipeline of a vertex and a fragment function: the vertex inputs as MxIn says (attribute n from buffer 30) */
static id<MTLRenderPipelineState> pipeline(id<MTLFunction> vf, const MxIn *vin, const MxOut *vout, id<MTLFunction> ff, const MxOut *fout,
                                           const u8 *fhdr, u32 rt_types, int points, NSString **error) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vf;
    d.fragmentFunction = ff;
    d.inputPrimitiveTopology = points ? MTLPrimitiveTopologyClassPoint : MTLPrimitiveTopologyClassTriangle;   /* (needed by a layer output) */
    if (vout->in_attr) {
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        for (u32 n = 0; n < 32; n++) {
            if (!(vout->in_attr >> n & 1)) continue;
            int integer = n < 16 && ((vin->int_attrs >> n & 1) || (vin->int_attrs >> (16 + n) & 1)), sgn = n < 16 && (vin->sint_attrs >> n & 1) && !(vin->int_attrs >> (16 + n) & 1);
            vd.attributes[n].format = !integer ? MTLVertexFormatFloat4 : sgn ? MTLVertexFormatInt4 : MTLVertexFormatUInt4;
            vd.attributes[n].offset = 16 * n;
            vd.attributes[n].bufferIndex = 30;
        }
        vd.layouts[30].stride = 512;
        d.vertexDescriptor = vd;
    }
    u32 omap = 0, flags = 0;
    if (fhdr && fout) { memcpy(&omap, fhdr + 0x48, 4); memcpy(&flags, fhdr + 0x4C, 4); }
    else omap = 0xF;
    for (u32 rt = 0; rt < 8; rt++)
        if ((omap >> (rt * 4)) & 15) {
            u32 k = rt_types >> (2 * rt) & 3;
            d.colorAttachments[rt].pixelFormat = k == 1 ? MTLPixelFormatRGBA32Uint : k == 2 ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA16Float;
        }
    if (flags & 2) d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError *e = nil;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (error) *error = ps ? nil : e.localizedDescription;
    return ps;
}

/* One program: translated with both option sets (or its own), compiled (and as for macOS 14), specialised, made into a
 * pipeline (a fragment program with its vertex program, or with one written to match it: an EXIT that forces the outputs
 * it reads; a vertex program with a fragment function that reads nothing, for triangles and for points). *refused: the
 * translator refused it; quiet (the random programs): refusing is fine when the Vulkan text refuses too, and only
 * failures are printed. */
static int metal_only;   /* (refused for want of a Metal form, where the Vulkan text is written) */
static int check_program(const Prog *p, int *refused, double *ms_total, int quiet) {
    int any = 0;
    for (int opts = 0; opts < 2; opts++) {
        if (p->opts >= 0 && opts != p->opts) continue;
        MxOut out, gl;
        MxIn in;
        inputs(p, 2, opts, &in);
        int ok = mx_translate(&in, &out), glok = translate(p, 1, opts, &gl), bad = 0;
        if (glok) free(gl.glsl);
        if (!ok) {   /* (what has no Metal form is refused saying so: fine for a random program) */
            int metal = strstr(out.why, "Metal") != NULL;
            if (refused) (*refused)++;
            if (glok && (!(quiet && metal) || verbose)) printf("  %s (options %d): MSL refused what Vulkan GLSL takes: %s: %s\n", p->name, opts, out.op, out.why);
            else if (!quiet) printf("  %s (options %d): refused: %s: %s\n", p->name, opts, out.op, out.why);
            if (glok && metal) metal_only++;
            any |= (glok && !(quiet && metal)) || !quiet;
            continue;
        }
        double ms = 0, ms14 = 0;
        NSString *err = nil, *err14 = nil;
        id<MTLLibrary> lib = compile(out.glsl, 0, &ms, &err), lib14 = compile(out.glsl, 1, &ms14, &err14);
        if (ms_total) *ms_total += ms;
        const char *what = "";
        NSString *perr = nil;
        if (!lib || !lib14) { bad = 1; what = "compile"; }
        else if (p->stage == MX_COMPUTE) {
            NSError *e = nil;
            id<MTLFunction> fn = function(lib, 0, -1);
            id<MTLComputePipelineState> cps = fn ? [dev newComputePipelineStateWithFunction:fn error:&e] : nil;
            u32 want = p->local[0] * p->local[1] * p->local[2];
            if (!cps || cps.maxTotalThreadsPerThreadgroup < want) { bad = 1; what = "pipeline"; perr = cps ? @"takes fewer threads than its workgroup" : e.localizedDescription; }
        } else if (p->stage == MX_VERTEX) {
            id<MTLLibrary> fl = compile(k_frag_any, 0, NULL, NULL);
            for (int points = 0; points < 2 && !bad; points++) {
                id<MTLFunction> fn = function(lib, 0, points);
                if (!fn || !pipeline(fn, &in, &out, [fl newFunctionWithName:@"mx_main"], NULL, NULL, 0, points, &perr)) { bad = 1; what = points ? "pipeline (points)" : "pipeline"; }
            }
        } else {
            /* the vertex program it is drawn with: its pair's, else one that only exits, writing what this reads */
            Prog vp;
            const Prog *v = p->pair >= 0 ? corpus[p->pair] : NULL;
            if (!v) {
                memset(&vp, 0, sizeof vp);
                vp.name = "vs_exit"; vp.stage = MX_VERTEX; vp.pair = -1;
                exit_(&vp);
                assemble(&vp);
                v = &vp;
            }
            MxIn vin;
            MxOut vout;
            inputs(v, 2, opts, &vin);
            vin.force_out |= out.in_attr;
            if (!mx_translate(&vin, &vout)) { bad = 1; what = "vertex"; perr = @(vout.why); }
            else {
                id<MTLLibrary> vl = compile(vout.glsl, 0, NULL, NULL);
                for (u32 k = 0; k < 2 && !bad; k++) {   /* (float targets, then uint ones) */
                    u32 rt = k ? 0x5555u : 0;
                    id<MTLFunction> vf = function(vl, 0, 0), ff = function(lib, rt, -1);
                    if (!vf || !ff || !pipeline(vf, &vin, &vout, ff, &out, p->bytes, rt, 0, &perr)) { bad = 1; what = "pipeline"; }
                }
                free(vout.glsl);
            }
            if (v == &vp) { free(vp.w); free(vp.op); free(vp.bytes); }
        }
        if (!quiet || bad)
            printf("%-24s %-8s options %d: %4d instructions, %6zu bytes of MSL, %6.1f ms to compile%s%s\n", p->name, stage_name(p->stage), opts,
                   out.insns, strlen(out.glsl), ms, bad ? " FAILED " : "", what);
        if (err || err14) printf("  %s\n", (err ? err : err14).UTF8String);
        if (perr) printf("  %s\n", perr.UTF8String);
        if (bad && verbose) printf("%s\n", out.glsl);
        free(out.glsl);
        any |= bad;
    }
    return any;
}

static int run_corpus(void) {
    int failed = bad_decoding, refused = 0;
    double total = 0;
    u32 used[MXOP_COUNT] = {0};
    for (u32 i = 0; i < ncorpus; i++) {
        for (u32 k = 0; k < corpus[i]->n; k++) used[corpus[i]->op[k]]++;
        failed |= check_program(corpus[i], &refused, &total, 0);
    }
    int missing = 0;
    for (size_t s = 0; s < NHANDLED; s++)
        if (!used[k_handled[s]]) { if (!missing++) printf("not in the corpus:"); printf(" %s", mx_op_name[k_handled[s]]); }
    if (missing) printf("\n");
    printf("corpus: %u programs, %zu of the %zu translated encodings, %d refused, %.0f ms compiling: %s\n", ncorpus, NHANDLED - (size_t)missing,
           NHANDLED, refused, total, failed ? "FAILED" : "ok");
    return failed;
}

static int run_fuzz(int only) {
    static Prog fz;
    fuzz_state = 0x9E3779B97F4A7C15ull;
    int failed = 0, refused = 0;
    double total = 0;
    for (u32 i = 0; i < NFUZZ; i++) {
        fuzz_program(&fz, i);
        if (only >= 0) {
            if ((int)i != only) continue;
            MxOut out;
            if (translate(&fz, 2, fz.opts, &out)) { fputs(out.glsl, stdout); free(out.glsl); }
            else printf("refused: %s: %s\n", out.op, out.why);
            return 0;
        }
        static char nm[32];
        snprintf(nm, sizeof nm, "fuzz %u", i);
        fz.name = nm;
        if (check_program(&fz, &refused, &total, 1)) failed++;
    }
    printf("fuzz: %u random programs, %u to MSL (%d refused: as by the Vulkan text, or %d for want of a Metal form), %.0f ms compiling, %d FAILED\n",
           NFUZZ, NFUZZ - (u32)refused, refused, metal_only, total, failed);
    return failed != 0;
}

/* ---- running programs ---------------------------------------------------------------------- */
static id<MTLBuffer> buffer(const void *data, u32 size) {
    id<MTLBuffer> b = [dev newBufferWithLength:size ? size : 16 options:MTLResourceStorageModeShared];
    if (data) memcpy(b.contents, data, size);
    return b;
}
static int checks, failures;
static void check(const char *what, int index, u32 got, u32 want, int ok) {
    checks++;
    if (ok && !verbose) return;
    if (!ok) failures++;
    if (!ok && failures > 60 && !verbose) return;
    printf("  %s %-32s [%2d] got %08x want %08x%s\n", ok ? "  " : "!!", what, index, got, want, ok ? "" : "  MISMATCH");
}
static int same_float(u32 got, u32 want, int match) {
    float g = F32(got), w = F32(want);
    if (got == want || (isnan(g) && isnan(w))) return 1;
    if (match == ANY_ZERO) return fabsf(g) < FLT_MIN && fabsf(w) < FLT_MIN;   /* (zeros and denormals: either) */
    if (match == NEAR_ULPS) return !isnan(g) && !isnan(w) && (i64)got - (i64)want <= 4 && (i64)want - (i64)got <= 4 && (got ^ want) >> 31 == 0;
    if (match == NEAR_2E6) return fabsf(g - w) <= 2e-6f * fmaxf(1.0f, fabsf(w)) || (isinf(g) && g == w);
    if (match == SAME_HALVES) {
        for (int h = 0; h < 2; h++) {
            u32 a = got >> (16 * h) & 0xFFFF, b = want >> (16 * h) & 0xFFFF;
            if (a != b && !(isnan(h2f(a)) && isnan(h2f(b)))) return 0;
        }
        return 1;
    }
    return 0;
}

/* A compute program on the GPU, as a renderer would run it (mx_translate.h, target 2): constant buffers 0..3 from the
 * program, storage buffer 0 the input, 1 the output; the write mask. */
typedef struct Kernel {
    Prog *p;
    u32 groups;
    const void *in; u32 in_size;
    void *out; u32 out_size;
    u32 cb_size[4];
    u32 wmask;   /* (out: the write mask's word) */
} Kernel;
static int run_kernel(Kernel *k) {
    Prog *p = k->p;
    MxIn in;
    MxOut out;
    inputs(p, 2, 0, &in);
    in.track_writes = 1;
    if (!mx_translate(&in, &out)) { printf("  %s: %s: %s\n", p->name, out.op, out.why); return 1; }
    id<MTLLibrary> lib = compile(out.glsl, 0, NULL, NULL);
    if (!lib) { printf("%s\n", out.glsl); free(out.glsl); return 1; }
    if (verbose > 1) printf("%s\n", out.glsl);
    free(out.glsl);
    NSError *e = nil;
    id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithFunction:function(lib, 0, -1) error:&e];
    if (!cps) { printf("  pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
    MxMslBinds binds;
    memset(&binds, 0, sizeof binds);
    NSMutableArray *keep = [NSMutableArray array];
    for (u32 b = 0; b < 4; b++) {
        if (!(out.cbufs >> b & 1)) continue;
        id<MTLBuffer> buf = buffer(p->cb[b], sizeof p->cb[b]);
        [keep addObject:buf];
        binds.cb[b] = (MxMslBind){buf.gpuAddress, k->cb_size[b] ? k->cb_size[b] : (u32)sizeof p->cb[b], 0};
    }
    id<MTLBuffer> bin = buffer(k->in, k->in_size), bout = buffer(k->out, k->out_size), wm = buffer(NULL, 16);
    memset(wm.contents, 0, 16);
    binds.sb[0] = (MxMslBind){bin.gpuAddress, k->in_size, 0};
    binds.sb[1] = (MxMslBind){bout.gpuAddress, k->out_size, 0};
    binds.wm = (MxMslBind){wm.gpuAddress, 4, 0};
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:cps];
    [enc setBytes:&binds length:sizeof binds atIndex:MX_MSL_BINDS];
    for (id<MTLBuffer> b in keep) [enc useResource:b usage:MTLResourceUsageRead];
    [enc useResource:bin usage:MTLResourceUsageRead];
    [enc useResource:bout usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [enc useResource:wm usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [enc dispatchThreadgroups:MTLSizeMake(k->groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(p->local[0], p->local[1], p->local[2])];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { printf("  %s: %s\n", p->name, cb.error.localizedDescription.UTF8String); return 1; }
    memcpy(k->out, bout.contents, k->out_size);
    memcpy(&k->wmask, wm.contents, 4);
    return 0;
}

/* thread inputs: integers' edges, floats' (NaN, infinities, denormals, halves), mixed */
static const u32 IN_A[32] = {0x00000000, 0x00000001, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x80000001, 0x0000001F, 0x00000020,
                             0x12345678, 0xDEADBEEF, 0x0000FFFF, 0xFFFF0000, 0x00FF00FF, 0x3F800000, 0xBF800000, 0x40200000,
                             0xC0200000, 0x40600000, 0x7FC00000, 0x7F800000, 0xFF800000, 0x00000003, 0x4F000000, 0x4F800000,
                             0xCF000001, 0x3EAAAAAB, 0x447A0000, 0x3C003C00, 0xC000BC00, 0x7BFF3555, 0x3F000000, 0xBF000000};
static const u32 IN_B[32] = {0x00000000, 0x00000005, 0x0000001F, 0x00000020, 0x00000021, 0xFFFFFFFF, 0x80000000, 0x00000010,
                             0x00000808, 0x00002000, 0x0000081C, 0x00000820, 0x00000C04, 0x3F800000, 0x40000000, 0x7FC00000,
                             0xBF800000, 0x40200000, 0x3F800000, 0x7F800000, 0x7F800000, 0x00000004, 0x4F000000, 0x00000001,
                             0x00001810, 0x3EAAAAAB, 0xC47A0000, 0x40003C00, 0x3C00C000, 0x7C00FC00, 0x00000000, 0x3F000000};
static const u32 IN_C[32] = {0x00000000, 0xFFFFFFFF, 0x00010000, 0x12345678, 0x80000000, 0x7FFFFFFF, 0x00000001, 0xFFFF8000,
                             0x55555555, 0x0F0F0F0F, 0x3F800000, 0xBF800000, 0x40490FDB, 0x7FC00000, 0x00800000, 0x00000007,
                             0x3C003C00, 0x00000002, 0xC1200000, 0x41200000, 0x7F7FFFFF, 0x00000003, 0x80000000, 0x00000100,
                             0x0000FFFF, 0x3F000000, 0xDEADBEEF, 0x3C00BC00, 0x40004000, 0x00000000, 0x12345678, 0x87654321};

static int gpu_alu(void) {
    Prog *p = find_prog("cs_alu");
    static u32 in[64 * 4], out[32 * 256];
    for (u32 t = 0; t < 32; t++) { in[4 * t] = IN_A[t]; in[4 * t + 1] = IN_B[t]; in[4 * t + 2] = IN_C[t]; in[4 * t + 3] = t; }
    memset(out, 0xCD, sizeof out);
    Kernel k = {.p = p, .groups = 1, .in = in, .in_size = 32 * 16, .out = out, .out_size = sizeof out};
    if (run_kernel(&k)) return 1;
    int before = failures;
    u32 all_nonzero = 1;
    for (u32 t = 0; t < 32; t++) all_nonzero &= IN_A[t] != 0;
    for (u32 c = 0; c < nalu; c++)
        for (u32 t = 0; t < 32; t++) {
            u32 a = IN_A[t], b = IN_B[t], cc = IN_C[t], got = out[t * 256 + c], want = 0;
            int ok = 0;
            for (ftz_mode = 0; ftz_mode < 2 && !ok; ftz_mode++) {   /* (denormals kept, then flushed) */
                dontcare = 0;
                if (alu[c].match == 200) { want = all_nonzero ? 0u : ~0u; ok = got == want; }   /* (the SEL tests !P2) */
                else if (alu[c].match >= 100) {
                    want = fcmp((u32)alu[c].match - 100, FA, FB) ? ~0u : 0u;
                    ok = got == want;
                } else {
                    u32 w = alu[c].model(a, b, cc);
                    if (!ftz_mode) want = w;
                    ok = alu[c].match == SAME_BITS ? got == w : same_float(got, w, alu[c].match);
                    if (dontcare) ok = 1;
                }
            }
            ftz_mode = 0;
            check(alu[c].what, (int)t, got, want, ok);
        }
    check("write mask (storage buffer 1)", 0, k.wmask, 2u, k.wmask == 2u);
    printf("gpu cs_alu: %u checks of 32 threads each: %s\n", nalu, failures == before ? "ok" : "FAILED");
    return failures != before;
}

static int gpu_flow(void) {
    Prog *p = find_prog("cs_flow");
    static u32 in[32 * 4], out[32 * 16];
    for (u32 t = 0; t < 32; t++) { in[4 * t] = IN_A[t]; in[4 * t + 1] = IN_B[t]; in[4 * t + 2] = IN_C[t] ^ t; in[4 * t + 3] = 0; }
    memset(out, 0xCD, sizeof out);
    Kernel k = {.p = p, .groups = 1, .in = in, .in_size = sizeof in, .out = out, .out_size = sizeof out};
    if (run_kernel(&k)) return 1;
    int before = failures;
    for (u32 t = 0; t < 32; t++) {
        u32 a = in[4 * t], b = in[4 * t + 1], c = in[4 * t + 2], n = a & 15, cx = c & 3;
        u32 want[5] = {n * (n + 1) / 2, a & 1 ? 1u : 2u, b & 7, 100 + cx, a + b < a ? 1u : 2u};
        static const char *const what[5] = {"loop", "SSY/SYNC", "PBK/BRK", "BRX jump table", "branch on carry"};
        for (u32 i = 0; i < 5; i++) {
            u32 w = t == 31 ? 0xCDCDCDCDu : want[i];
            check(t == 31 ? "(left first: nothing written)" : what[i], (int)t, out[t * 16 + i], w, out[t * 16 + i] == w);
        }
    }
    printf("gpu cs_flow: branches, 32 threads: %s\n", failures == before ? "ok" : "FAILED");
    return failures != before;
}

static u32 rd(const u8 *m, u32 size, u32 off, u32 bytes) {   /* little-endian, 0 past the end */
    u32 v = 0;
    for (u32 i = 0; i < bytes; i++) v |= (off + i < size ? m[off + i] : 0u) << (8 * i);
    return v;
}
static u32 sext(u32 v, u32 bits) { return (u32)((i32)(v << (32 - bits)) >> (32 - bits)); }

static int gpu_mem(void) {
    Prog *p = find_prog("cs_mem");
    static u32 in[64 * 4], out[64 * 64];
    for (u32 t = 0; t < 64; t++) {
        in[4 * t] = IN_A[t & 31] ^ (t >> 5) * 0x5A5A5A5Au;
        in[4 * t + 1] = IN_B[(t * 7) & 31] + t;
        in[4 * t + 2] = IN_C[t & 31] ^ (t * 0x01010101u);
        in[4 * t + 3] = t * 0x11111111u;
    }
    memset(out, 0xCD, sizeof out);
    Kernel k = {.p = p, .groups = 2, .in = in, .in_size = sizeof in, .out = out, .out_size = sizeof out, .cb_size = {[3] = 64}};
    if (run_kernel(&k)) return 1;
    int before = failures;
    const u8 *cb3 = cb3_bytes(), *m = (const u8 *)in;
    for (u32 t = 0; t < 64; t++) {
        u32 a = in[4 * t], b = in[4 * t + 1], c = in[4 * t + 2], d = in[4 * t + 3], want[54];
        const u32 *got = out + t * 64;
        u32 idx = a & 0x7F, w = idx < 64 ? rd(cb3, 64, idx & ~3u, 4) : 0, sh = (idx & 3) * 8;
        want[0] = (w >> sh) & 0xFF;
        want[1] = sext((w >> sh) & 0xFF, 8);
        want[2] = (w >> (sh & 16)) & 0xFFFF;
        want[3] = sext((w >> (sh & 16)) & 0xFFFF, 16);
        want[4] = w;
        want[5] = idx < 64 ? rd(cb3, 64, idx & ~3u, 4) : 0;            /* (LDC.64: the word, and the next) */
        want[6] = (idx & ~3u) + 4 < 64 ? rd(cb3, 64, (idx & ~3u) + 4, 4) : 0;
        want[7] = rd(cb3, 64, 0x10, 4);
        u32 back = idx - 4;
        want[8] = back < 64 ? rd(cb3, 64, back & ~3u, 4) : 0;
        u32 off = b & 0x3F, gw = rd(m, sizeof in, off & ~3u, 4), gsh = (off & 3) * 8;
        want[9] = (gw >> gsh) & 0xFF;
        want[10] = sext((gw >> gsh) & 0xFF, 8);
        want[11] = (gw >> (gsh & 16)) & 0xFFFF;
        want[12] = sext((gw >> (gsh & 16)) & 0xFFFF, 16);
        want[13] = rd(m, sizeof in, off & ~3u, 4);
        want[14] = want[13];
        want[15] = rd(m, sizeof in, (off & ~3u) + 4, 4);
        want[16] = 0;          /* past storage buffer 0 */
        want[17] = want[13];   /* another high word: SR reads 32-bit tagged addresses */
        for (u32 i = 0; i < 4; i++) want[18 + i] = t < 63 ? in[4 * (t + 1) + i] : 0;
        u32 bsh = (t & 3) * 8;
        want[22] = (0xAAAAAAAAu & ~(0xFFu << bsh)) | ((b & 0xFF) << bsh);
        want[23] = (0xAAAAAAAAu & 0xFFFF) | (c & 0xFFFF) << 16;
        want[24] = a; want[25] = b;
        for (u32 i = 26; i < 28; i++) want[i] = 0xCDCDCDCDu;
        want[28] = a; want[29] = b; want[30] = c; want[31] = d;
        u32 s0 = (a & ~0xFF00u) | (b & 0xFF) << 8;
        want[32] = s0;
        want[33] = sext(c & 0xFFFF, 16);
        want[34] = s0 >> 24;
        want[35] = sext(b & 0xFF, 8);
        want[36] = (d >> 0 & 0xFFFF);   /* (LDS.U16 at byte 5: the half word at 4, d's low half) */
        want[37] = a; want[38] = b;
        want[39] = a; want[40] = b; want[41] = c; want[42] = d;
        want[43] = (c & ~0xFF00u) | (a & 0xFF) << 8;
        want[44] = sext(a & 0xFF, 8);
        want[45] = a; want[46] = b;
        want[47] = t >> 5;
        want[48] = 0x00FF0000u;
        want[49] = 0;
        want[50] = 0x3F800000;
        want[51] = t & 31;
        want[52] = t & 31;
        want[53] = 0;
        static const char *const what[54] = {"LDC.U8", "LDC.S8", "LDC.U16", "LDC.S16", "LDC.32", "LDC.64 lo", "LDC.64 hi", "LDC c[3][0x10]",
            "LDC [r - 4]", "LDG.U8", "LDG.S8", "LDG.U16", "LDG.S16", "LDG.32", "LDG.64 lo", "LDG.64 hi", "LDG past the buffer", "LDG other high word",
            "LDG.128 x", "LDG.128 y", "LDG.128 z", "LDG.128 w", "STG.U8", "STG.U16", "STG.64 lo", "STG.64 hi", "(unwritten)", "(unwritten)",
            "STG.128 x", "STG.128 y", "STG.128 z", "STG.128 w", "STS, STS.U8, LDS", "STS.U16, LDS.S16", "LDS.U8", "LDS.S8", "LDS.U16",
            "STS.64, LDS.64 lo", "LDS.64 hi", "STS.128, LDS.128 x", "LDS.128 y", "LDS.128 z", "LDS.128 w", "STL, STL.U8, LDL", "LDL.S8",
            "STL.64, LDL.64 lo", "LDL.64 hi", "S2R CTAID.X", "S2R invocation info", "S2R TID.Y", "S2R SR18", "S2R TID", "S2R lane", "S2R clock"};
        for (u32 i = 0; i < 54; i++) {
            u32 wv = t == 63 ? 0xCDCDCDCDu : want[i];
            check(t == 63 ? "(left first: nothing written)" : what[i], (int)t, got[i], wv, got[i] == wv);
        }
    }
    printf("gpu cs_mem: memory, 2 workgroups of 32: %s\n", failures == before ? "ok" : "FAILED");
    return failures != before;
}

static int run_gpu(void) {
    int bad = 0;
    printf("device: %s\n", dev.name.UTF8String);
    bad |= gpu_alu();
    bad |= gpu_flow();
    bad |= gpu_mem();
    printf("gpu: %d checks, %d failed%s\n", checks, failures, failures > 60 ? " (the first 60 shown)" : "");
    return bad;
}
#endif

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "all";
    build_corpus();
    if (!strcmp(mode, "glsl") && argc > 2) return glsl_write(argv[2]);
    if (!strcmp(mode, "glsldiff") && argc > 2) return glsl_diff(argv[2]);
    if (!strcmp(mode, "list")) { for (u32 i = 0; i < ncorpus; i++) printf("%s (%s)\n", corpus[i]->name, stage_name(corpus[i]->stage)); return 0; }
#ifdef MXMSLTEST_GLSL_ONLY
    fprintf(stderr, "built with MXMSLTEST_GLSL_ONLY: glsl <file>, glsldiff <file> or list\n");
    return 2;
#else
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose++;
    if (!strcmp(mode, "print") && argc > 2) {
        Prog *p = find_prog(argv[2]);
        if (!p) { fprintf(stderr, "no program %s\n", argv[2]); return 2; }
        MxOut out;
        if (!translate(p, 2, p->opts > 0, &out)) { printf("refused: %s: %s\n", out.op, out.why); return 1; }
        fputs(out.glsl, stdout);
        free(out.glsl);
        return 0;
    }
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 0; }
        queue = [dev newCommandQueue];
        int bad = 0;
        if (!strcmp(mode, "corpus")) bad = run_corpus();
        else if (!strcmp(mode, "fuzz")) bad = run_fuzz(argc > 2 && argv[2][0] != '-' ? atoi(argv[2]) : -1);
        else if (!strcmp(mode, "gpu")) bad = run_gpu();
        else if (!strcmp(mode, "all")) { bad |= run_corpus(); bad |= run_fuzz(-1); bad |= run_gpu(); }
        else { fprintf(stderr, "usage: mxmsltest [all | corpus | fuzz [<n>] | gpu | print <program> | list | glsl <file> | glsldiff <file>] [-v]\n"); return 2; }
        printf("%s\n", bad ? "FAILED" : "all ok");
        return bad;
    }
#endif
}
