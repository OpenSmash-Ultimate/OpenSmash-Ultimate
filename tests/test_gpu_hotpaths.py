"""Unit equivalence tests for the Maxwell shader translator and GPU command-stream fast paths.

Uses only synthetic Maxwell shader words and GPFIFO command buffers (no game files).
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)


def compile_and_run(source):
    with tempfile.TemporaryDirectory(prefix='gpu_hotpaths_') as directory:
        path = Path(directory)
        c_file = path / 'harness.c'
        exe_file = path / 'harness.exe'
        c_file.write_text(source, encoding='utf-8')
        compile_cmd = [
            cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS,
            str(c_file), '/Fo' + directory + os.sep, '/Fe' + str(exe_file),
            '/link', *cc.LINK_OPTIONS,
        ]
        built = subprocess.run(compile_cmd, cwd=ROOT, capture_output=True, text=True)
        if built.returncode != 0:
            raise AssertionError('harness failed to compile:\n' + built.stdout + built.stderr)
        ran = subprocess.run([str(exe_file)], cwd=directory, capture_output=True, text=True, timeout=60)
        if ran.returncode != 0:
            raise AssertionError(
                f'harness exited with {ran.returncode}:\n{ran.stdout}{ran.stderr}'
            )


@unittest.skipUnless(HAVE_CLANG, "Windows with clang-cl only (deps\\llvm, or Visual Studio's C++ Clang tools)")
class GpuHotpathTests(unittest.TestCase):
    def test_shader_fast_decode_and_scanners_equivalence(self):
        src = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_shader.c"

_Noreturn void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

static u32 ref_decode_linear(u16 top) {
    for (u32 i = 0; i < table_size; i++)
        if ((top & table[i].mask) == table[i].value) return table[i].op;
    return OP_UNKNOWN;
}

static int ref_form_of(u32 op) {
    const char *n = OPS[op].name, *u = strrchr(n, '_');
    size_t len = strlen(n);
    if (len > 3 && !strcmp(n + len - 3, "32I")) return FORM_32I;
    if (!u) return FORM_REG;
    if (!strcmp(u, "_reg")) return FORM_REG;
    if (!strcmp(u, "_cbuf")) return FORM_CBUF;
    if (!strcmp(u, "_imm")) return FORM_IMM;
    if (!strcmp(u, "_rc")) return FORM_RC;
    if (!strcmp(u, "_cr")) return FORM_CR;
    if (!strcmp(u, "_32I")) return FORM_32I;
    return FORM_REG;
}

static int ref_find_linear(const Ctx *c, u32 addr) {
    for (u32 i = 0; i < c->n; i++)
        if (c->ins[i].addr == addr) return (int)i;
    return -1;
}

static void init_sph(u8 *buf, size_t len, int stage) {
    memset(buf, 0, len);
    u32 st = stage == SHADER_VERTEX ? 1u : stage == SHADER_PIXEL ? 5u : 0u;
    u32 sph0 = 1u | (3u << 5) | (st << 10) | (3u << 17);
    memcpy(buf, &sph0, 4);
}

int main(void) {
    init_table();
    for (u32 top = 0; top < 65536; top++) {
        u64 w = (u64)top << 48;
        if (decode(w) != ref_decode_linear((u16)top)) {
            fprintf(stderr, "FAIL decode_lut at top=%#x\n", top);
            return 1;
        }
    }
    for (u32 op = 0; op < OP_COUNT; op++) {
        if (form_of(op) != ref_form_of(op)) {
            fprintf(stderr, "FAIL form_of op=%u (%s)\n", op, OPS[op].name);
            return 2;
        }
    }

    const u64 NOP = 0x50B0000000000000ull;
    const u64 SSY_A0 = 0xE29000000407000Full;     /* SSY at 0x58 -> 0x58+8+0x40 = 0xA0 */
    const u64 IADD_IMM = 0x3820000000100001ull;   /* IADD_imm r1 = r0 + 1 */
    const u64 SHR_S_REG = 0x5C29000000100102ull;  /* SHR.S32 r2 = r1 >> r1: emits asuint(asint(...)) */
    const u64 BRA_A0 = 0xE24000000287000Full;     /* BRA at 0x78 -> 0x78+8+0x28 = 0xA8 */
    const u64 EXIT = 0xE30000000007000Full;
    const u64 SYNC = 0xF0F800000007000Full;
    const u64 IPA_POS = 0xE000000000000000ull | ((0x70ull / 4) << 30);

    u8 prog[0x100];
    init_sph(prog, sizeof prog, SHADER_PIXEL);
    /* Quad 1: 0x58, 0x60, 0x68; Quad 2: 0x78, 0x80, 0x88; Quad 3: 0x98, 0xA0, 0xA8; Quad 4: 0xB8 */
    u64 insns[] = {
        SSY_A0, IADD_IMM, SHR_S_REG,
        BRA_A0, EXIT, NOP,
        SYNC, IADD_IMM, EXIT,
        NOP,
    };
    u32 offsets[] = {0x58, 0x60, 0x68, 0x78, 0x80, 0x88, 0x98, 0xA0, 0xA8, 0xB8};
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++)
        memcpy(prog + offsets[i], &insns[i], 8);

    Ctx c = {0};
    decode_program(&c, prog, sizeof prog);
    /* EXIT at 0x80 is bypassed because BRA/SSY target > 0x80; terminates at EXIT at 0xA8 (furthest = 0xA8) */
    if (shader_size(prog, sizeof prog) != 0xB0 || c.ins[c.n - 1].addr + 8 != shader_size(prog, sizeof prog)) {
        fprintf(stderr, "FAIL shader_size got %#x expected 0xb0\n", shader_size(prog, sizeof prog));
        free(c.ins);
        return 3;
    }
    for (u32 a = 0; a < 0x100; a += 4) {
        if (find(&c, a) != ref_find_linear(&c, a)) {
            fprintf(stderr, "FAIL find addr=%#x got=%d ref=%d\n", a, find(&c, a), ref_find_linear(&c, a));
            free(c.ins);
            return 4;
        }
    }
    free(c.ins);

    if (!shader_uses_stacks(prog, sizeof prog)) return 5;
    if (shader_uses_pixels(prog, sizeof prog, SHADER_PIXEL) != 0) return 6;

    /* Inject IPA attr 0x70 at 0x60: only pixel stage reports uses_pixels */
    memcpy(prog + 0x60, &IPA_POS, 8);
    if (shader_uses_pixels(prog, sizeof prog, SHADER_PIXEL) != 1) return 7;
    if (shader_uses_pixels(prog, sizeof prog, SHADER_VERTEX) != 0) return 8;

    /* Short program <= 0x58 */
    u8 empty[0x58];
    init_sph(empty, sizeof empty, SHADER_VERTEX);
    if (shader_size(empty, sizeof empty) != 0x50) return 9;
    if (shader_uses_stacks(empty, sizeof empty) != 0) return 10;

    /* Translate a larger multi-quad glslang pixel shader (>4 KB output) and check cast rewrite */
    u8 big[0x800];
    init_sph(big, sizeof big, SHADER_PIXEL);
    u32 nbig = 0;
    for (u32 a = 0x50; a + 8 <= sizeof big - 0x40; a += 8) {
        if (((a - 0x50) / 8) % 4 == 0) continue;
        u64 op = nbig == 0 ? BRA_A0 : (nbig & 1) ? SHR_S_REG : IADD_IMM;
        memcpy(big + a, &op, 8);
        nbig++;
    }
    /* terminate cleanly with EXIT near end */
    for (u32 a = sizeof big - 0x40; a + 8 <= sizeof big; a += 8) {
        if (((a - 0x50) / 8) % 4 == 0) continue;
        memcpy(big + a, &EXIT, 8);
        break;
    }
    ShaderInfo info_glsl, info_fxc, info_msl;
    char *gtxt = shader_translate(SHADER_HLSL_GLSLANG, big, sizeof big, SHADER_PIXEL, 0, NULL, 0, &info_glsl, NULL, NULL);
    char *ftxt = shader_translate(SHADER_HLSL_FXC, big, sizeof big, SHADER_PIXEL, 0, NULL, 0, &info_fxc, NULL, NULL);
    char *mtxt = shader_translate(SHADER_MSL, big, sizeof big, SHADER_PIXEL, 0, NULL, 0, &info_msl, NULL, NULL);
    if (!gtxt || !ftxt || !mtxt) return 11;
    if (strlen(gtxt) < 4096 || strstr(gtxt, "asu_(asi_(r") == NULL || strstr(gtxt, "asint(") != NULL || strstr(gtxt, "asuint(") != NULL) {
        fprintf(stderr, "FAIL glslang casts rewrite\n");
        return 12;
    }
    if (strstr(ftxt, "asuint(asint(r") == NULL || info_glsl.stage != SHADER_PIXEL || info_msl.stage != SHADER_PIXEL) return 13;
    free(gtxt);
    free(ftxt);
    free(mtxt);
    return 0;
}
"""
        compile_and_run(src)

    def test_gpu_batched_cb_load_and_direct_macro_equivalence(self):
        src = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_backend.h"

#define VA_CONTIG   0x10000000ull
#define VA_SPLIT    0x20000000ull
#define VA_CMD      0x30000000ull

static u8 *contig_mem;
static u8 *split_p0;
static u8 *split_p1;
static u32 *cmd_mem;

static u64 mock_rb_bytes;
static u64 mock_stream_bytes;
static u32 mock_stream_calls;

u64 gpu_va_to_guest(u64 va) {
    if (va >= VA_CONTIG && va < VA_CONTIG + 0x4000)
        return (u64)(uintptr_t)(contig_mem + (va - VA_CONTIG));
    if (va >= VA_SPLIT && va < VA_SPLIT + 0x1000)
        return (u64)(uintptr_t)(split_p0 + (va - VA_SPLIT));
    if (va >= VA_SPLIT + 0x1000 && va < VA_SPLIT + 0x2000)
        return (u64)(uintptr_t)(split_p1 + (va - (VA_SPLIT + 0x1000)));
    if (va >= VA_CMD && va < VA_CMD + 0x8000)
        return (u64)(uintptr_t)(cmd_mem + (va - VA_CMD) / 4);
    return 0;
}

void gpu_rb_wrote(u64 guest, u64 bytes) { (void)guest; mock_rb_bytes += bytes; }
void gpu_rb_semaphore(u64 guest) { (void)guest; }
int kernel_rb_gpu_clock(u64 *fixed_ns, i64 *off) { *fixed_ns = 0; *off = 0; return 1; }
u64 rt_now_ns(void) { return 0; }
u32 gpu_syncpt_min[SYNCPT_MAX], gpu_syncpt_max[SYNCPT_MAX];

static void mock_clear(const u32 *r, u32 a) { (void)r; (void)a; }
static void mock_draw(const u32 *r, const CbBinding cb[STAGES][18], int ix) { (void)r; (void)cb; (void)ix; }
static void mock_present(u64 g, u32 w, u32 h) { (void)g; (void)w; (void)h; }
static void mock_blit2d(const u32 *r) { (void)r; }
static void mock_dispatch(const u32 *r, const u32 *q) { (void)r; (void)q; }
static int mock_copy(u64 d, u64 s, u64 b) { (void)d; (void)s; (void)b; return 0; }
static void mock_rflush(u64 v, u64 b) { (void)v; (void)b; }
static void mock_rwrote(u64 v, u64 b) { (void)v; (void)b; }
static void mock_stream_wrote(u64 va, u64 bytes) { (void)va; mock_stream_bytes += bytes; mock_stream_calls++; }
static void mock_guest_wrote(u64 va, u64 bytes) { (void)va; (void)bytes; }
static void mock_stream_end(void) {}
static void mock_unmap(u64 a, u64 s) { (void)a; (void)s; }
static int mock_dlog(void) { return 0; }
static void mock_void(void) {}
static DWORD mock_tid(void) { return 0; }
static void mock_rep(void (*f)(const char *)) { (void)f; }
static void *mock_disp(void) { return NULL; }

GfxBackend gfx_backend = {
    .clear = mock_clear, .draw = mock_draw, .present = mock_present, .blit2d = mock_blit2d,
    .dispatch = mock_dispatch, .copy_resident = mock_copy, .resident_flush = mock_rflush,
    .resident_written = mock_rwrote, .stream_wrote = mock_stream_wrote, .guest_wrote = mock_guest_wrote,
    .stream_end = mock_stream_end, .gpu_unmapped = mock_unmap, .guest_unmapped = mock_unmap,
    .draw_logging = mock_dlog, .status_lines = mock_void, .closing = mock_void,
    .lock_owner = mock_tid, .last_enter_thread = mock_tid, .report_compiles = mock_rep,
    .display_output = mock_disp,
};

#include "gpu.c"

static u32 hdr(u32 mode, u32 sub, u32 method, u32 count) {
    return (mode << 29) | ((count & 0x1FFF) << 16) | ((sub & 7) << 13) | (method & 0x1FFF);
}

static u64 gpfifo_entry(u64 va, u32 words) {
    return (va & 0xFFFFFFFFFCull) | ((u64)(words & 0x1FFFFF) << 42);
}

/* MME instruction encoder helpers */
static u32 mme_add_imm(u32 op, u32 dst, u32 src, i32 imm18) {
    return 1u | (op << 4) | (dst << 8) | (src << 11) | ((u32)(imm18 & 0x3FFFF) << 14);
}

static size_t emit_script(u32 *dst, int single_word_mode) {
    size_t n = 0;
    /* Bind CLASS_3D on subchannel 0 (class 0xB197 exceeds 13-bit inline count, so pass as argument word) */
    dst[n++] = hdr(0, 0, 0x000, 1);
    dst[n++] = CLASS_3D;
    /* Upload macro 0 at code[0] (r1 -> reg[0x500]=r1+7, reg[0x501]=r1+107) and
     * macro 1 at code[3] (fetches 2nd arg into r2 via op=5: reg[0x502]=r1, reg[0x503]=r2) */
    dst[n++] = hdr(4, 0, 0x045, 0);
    u32 mcode[] = {
        mme_add_imm(2, 0, 0, 0x1500),           /* set method 0x500, inc 1 */
        mme_add_imm(4, 2, 1, 7) | (1u << 7),    /* exit: send r1 + 7 to 0x500, r2 = r1+7 */
        mme_add_imm(4, 3, 2, 100),              /* delay slot: send r2 + 100 to 0x501 */
        mme_add_imm(5, 2, 0, 0x1502),           /* set method 0x502, inc 1; r2 = mme_fetch() */
        mme_add_imm(4, 0, 1, 0) | (1u << 7),    /* exit: send r1 to 0x502 */
        mme_add_imm(4, 0, 2, 0),                /* delay slot: send r2 to 0x503 */
    };
    if (single_word_mode) {
        for (size_t k = 0; k < 6; k++) { dst[n++] = hdr(0, 0, 0x046, 1); dst[n++] = mcode[k]; }
    } else {
        dst[n++] = hdr(2, 0, 0x046, 6);
        memcpy(dst + n, mcode, sizeof mcode); n += 6;
    }
    dst[n++] = hdr(4, 0, 0x047, 0);
    dst[n++] = hdr(4, 0, 0x048, 0);
    dst[n++] = hdr(4, 0, 0x048, 3);

    /* Configure CB_LOAD at VA_CONTIG + 0xFC0 (crosses 4 KB page boundary!) */
    dst[n++] = hdr(0, 0, 0x8E1, 3);
    dst[n++] = (u32)(VA_CONTIG >> 32);
    dst[n++] = (u32)VA_CONTIG;
    dst[n++] = 0xFC0;

    u32 payload[24];
    for (u32 i = 0; i < 24; i++) payload[i] = 0x11220000u + i;
    /* Mode 0 increasing across 0x8E4..0x8EB (8 words) -> crosses 0xFC0..0xFE0; then 12 words non-increasing across 0xFE0..0x1010! */
    if (single_word_mode) {
        for (u32 i = 0; i < 8; i++) { dst[n++] = hdr(0, 0, 0x8E4 + i, 1); dst[n++] = payload[i]; }
        for (u32 i = 8; i < 20; i++) { dst[n++] = hdr(2, 0, 0x8E4, 1); dst[n++] = payload[i]; }
        /* Mode 5 increase-once (4 words) */
        for (u32 i = 20; i < 24; i++) { dst[n++] = hdr(0, 0, i == 20 ? 0x8E4 : 0x8E5, 1); dst[n++] = payload[i]; }
    } else {
        dst[n++] = hdr(0, 0, 0x8E4, 8);
        memcpy(dst + n, payload, 32); n += 8;
        dst[n++] = hdr(2, 0, 0x8E4, 12);
        memcpy(dst + n, payload + 8, 48); n += 12;
        dst[n++] = hdr(5, 0, 0x8E4, 4);
        memcpy(dst + n, payload + 20, 16); n += 4;
    }

    /* Configure CB_LOAD at VA_SPLIT + 0xFE0 (non-contiguous host pages: MUST fall back safely!) */
    dst[n++] = hdr(0, 0, 0x8E1, 3);
    dst[n++] = (u32)(VA_SPLIT >> 32);
    dst[n++] = (u32)VA_SPLIT;
    dst[n++] = 0xFE0;
    if (single_word_mode) {
        for (u32 i = 0; i < 16; i++) { dst[n++] = hdr(2, 0, 0x8E4, 1); dst[n++] = 0x55AA0000u + i; }
    } else {
        dst[n++] = hdr(2, 0, 0x8E4, 16);
        for (u32 i = 0; i < 16; i++) dst[n++] = 0x55AA0000u + i;
    }

    /* Macro calls: mode 2 multi-word, mode 5 multi-word, mode 0 multi-word (< 0x1000), mode 4 inline */
    if (single_word_mode) {
        dst[n++] = hdr(0, 0, 0xE02, 1); dst[n++] = 555;   /* in single-word baseline, second fetched arg defaults to 0 */
    } else {
        dst[n++] = hdr(2, 0, 0xE02, 2); dst[n++] = 111; dst[n++] = 222;
        dst[n++] = hdr(5, 0, 0xE02, 2); dst[n++] = 333; dst[n++] = 444;
        dst[n++] = hdr(0, 0, 0xE02, 2); dst[n++] = 555; dst[n++] = 666;
    }
    /* Inline macro call (mode 4): r1 = 42 -> reg[0x500]=49, reg[0x501]=149 */
    dst[n++] = hdr(4, 0, 0xE00, 42);
    /* Mode 5 macro m = 0xFFF boundary case (must not invoke macro on k >= 1 where m+1 == 0x1000) */
    dst[n++] = hdr(5, 0, 0xFFF, 2); dst[n++] = 1; dst[n++] = 2;
    return n;
}

int main(void) {
    contig_mem = calloc(1, 0x4000);
    split_p0 = calloc(1, 0x1000);
    split_p1 = calloc(1, 0x1000);
    /* Ensure split_p1 is intentionally NOT split_p0 + 0x1000 */
    if (split_p1 == split_p0 + 0x1000) {
        void *alt = calloc(1, 0x1000);
        free(split_p1);
        split_p1 = alt;
    }
    cmd_mem = calloc(0x8000 / 4, sizeof(u32));

    /* Run 1: single-word reference on channel 0 */
    size_t n_ref = emit_script(cmd_mem, 1);
    u64 e_ref = gpfifo_entry(VA_CMD, (u32)n_ref);
    mock_rb_bytes = mock_stream_bytes = mock_stream_calls = gpu_stat_methods = 0;
    gpu_submit(0, &e_ref, 1);

    u8 ref_contig[0x4000], ref_p0[0x1000], ref_p1[0x1000];
    memcpy(ref_contig, contig_mem, sizeof ref_contig);
    memcpy(ref_p0, split_p0, sizeof ref_p0);
    memcpy(ref_p1, split_p1, sizeof ref_p1);
    u32 ref_regs[0x1000];
    memcpy(ref_regs, channels[0].sub[0]->regs, sizeof ref_regs);
    u64 ref_rb = mock_rb_bytes, ref_sb = mock_stream_bytes, ref_m = gpu_stat_methods;

    /* Reset buffers and run 2: batched fast paths on channel 1 */
    memset(contig_mem, 0, 0x4000);
    memset(split_p0, 0, 0x1000);
    memset(split_p1, 0, 0x1000);
    memset(cmd_mem, 0, 0x8000);
    size_t n_fast = emit_script(cmd_mem, 0);
    u64 e_fast = gpfifo_entry(VA_CMD, (u32)n_fast);
    mock_rb_bytes = mock_stream_bytes = mock_stream_calls = gpu_stat_methods = 0;
    gpu_submit(1, &e_fast, 1);

    if (memcmp(contig_mem, ref_contig, sizeof ref_contig) != 0) return 1;
    if (memcmp(split_p0, ref_p0, sizeof ref_p0) != 0) return 2;
    if (memcmp(split_p1, ref_p1, sizeof ref_p1) != 0) return 3;
    /* Compare all non-macro-2nd-arg registers; macro 1 in batched mode also set reg[0x503]=666 */
    if (channels[1].sub[0]->regs[0x502] != 555 || channels[1].sub[0]->regs[0x503] != 666) return 4;
    channels[1].sub[0]->regs[0x503] = ref_regs[0x503];
    if (memcmp(channels[1].sub[0]->regs, ref_regs, sizeof ref_regs) != 0) return 5;
    if (mock_rb_bytes != ref_rb || mock_stream_bytes != ref_sb) return 6;
    if (gpu_stat_methods != ref_m + 5) return 7;
    /* Verify CB_LOAD actually coalesced stream_wrote calls (8 + 12 + 4 = 24 words became 3 batched calls + 16 split) */
    if (mock_stream_calls != 19) {
        fprintf(stderr, "FAIL expected 19 stream_wrote calls, got %u\n", mock_stream_calls);
        return 8;
    }
    if (channels[1].sub[0]->regs[0x500] != 49 || channels[1].sub[0]->regs[0x501] != 149) return 9;
    return 0;
}
"""
        compile_and_run(src)


if __name__ == '__main__':
    unittest.main()
