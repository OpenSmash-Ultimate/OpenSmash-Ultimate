"""Targeted C/Python tests for the performance & stability migration on Windows."""
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


def compile_and_run(source, extra_sources=(), link_libs=(), cflags=()):
    with tempfile.TemporaryDirectory(prefix='perf_rt_') as d:
        c_path = Path(d) / 'test.c'
        exe_path = Path(d) / 'test.exe'
        c_path.write_text(source, encoding='utf-8')
        cmd = [
            cc.CLANG, '/nologo', '/O2', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, *cflags,
            '-I', str(ROOT / 'runtime'), str(c_path), *(str(s) for s in extra_sources),
            '/Fe' + str(exe_path), '/Fo' + str(Path(d)) + os.sep,
            '/link', *cc.LINK_OPTIONS, *link_libs,
        ]
        built = subprocess.run(cmd, cwd=d, capture_output=True, text=True)
        if built.returncode:
            raise AssertionError('compile failed:\n' + built.stdout + built.stderr)
        ran = subprocess.run([str(exe_path)], cwd=d, capture_output=True, text=True, timeout=60)
        if ran.returncode:
            raise AssertionError(f'test binary exited with {ran.returncode}:\n{ran.stdout}\n{ran.stderr}')
        return ran.stdout, ran.stderr


@unittest.skipUnless(HAVE_CLANG, "needs Windows clang-cl (deps\\llvm or VS Clang tools)")
class PerfRuntimeTests(unittest.TestCase):
    def test_texture_row_matches_scalar_gob_offset(self):
        src = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_texture.c"

static void ref_texture_read(const TextureDesc *d, const u8 *src, u8 *out) {
    u32 w = DIV_UP(d->width, d->bw), h = DIV_UP(d->height, d->bh);
    u32 row = w * d->bytes;
    u32 bh = level_block_height(h, d->block_height);
    u32 gobs_x = DIV_UP(row, 64), block_rows = 8u << bh;
    for (u32 y = 0; y < h; y++) {
        u32 by = y / block_rows, yin = y % block_rows;
        u64 line = (u64)by * gobs_x * (512ull << bh) + (yin / 8) * 512ull;
        for (u32 x = 0; x < row; x += 16) {
            u64 off = line + (u64)(x / 64) * (512ull << bh) + gob_offset(x, yin);
            u32 n = row - x < 16 ? row - x : 16;
            memcpy(out + (size_t)y * row + x, src + off, n);
        }
    }
}

static void ref_texture_read_3d(const TextureDesc *d, const u8 *src, u8 *out) {
    u32 w = DIV_UP(d->width, d->bw), h = DIV_UP(d->height, d->bh), rowb = w * d->bytes;
    u32 bh = d->block_height, bd = d->block_depth;
    u64 row = (u64)DIV_UP(rowb, 64) << (9 + bh + bd), slab = row * DIV_UP(h, 8u << bh);
    for (u32 z = 0; z < d->depth; z++) {
        for (u32 y = 0; y < h; y++) {
            u64 base = (z >> bd) * slab + ((u64)(z & ((1u << bd) - 1)) << (9 + bh)) +
                       (u64)((y / 8) >> bh) * row + ((u64)((y / 8) & ((1u << bh) - 1)) << 9);
            for (u32 x = 0; x < rowb; x += 16) {
                u32 n = rowb - x < 16 ? rowb - x : 16;
                memcpy(out + ((size_t)z * h + y) * rowb + x,
                       src + base + ((u64)(x / 64) << (9 + bh + bd)) + gob_offset(x, y % 8), n);
            }
        }
    }
}

int main(void) {
    static u8 src[256 * 1024], got[128 * 1024], ref[128 * 1024];
    for (size_t i = 0; i < sizeof src; i++) src[i] = (u8)((i * 131u + 17u) ^ (i >> 5));

    /* Widths in bytes covering <16, 16..63, exact 64, and non-multiples of 64. */
    const u32 widths[] = {3, 7, 15, 16, 20, 48, 63, 64, 65, 80, 127, 128, 192, 200};
    for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; wi++) {
        for (u32 bh = 0; bh <= 2; bh++) {
            TextureDesc d = {0};
            d.width = widths[wi]; d.height = 17; d.depth = 1; d.levels = 1;
            d.block_height = bh; d.bytes = 1; d.bw = 1; d.bh = 1;
            memset(got, 0xAA, sizeof got);
            memset(ref, 0xAA, sizeof ref);
            texture_read(&d, (u64)(uintptr_t)src, 0, 0, got);
            ref_texture_read(&d, src, ref);
            if (memcmp(got, ref, (size_t)d.width * d.height) != 0) {
                fprintf(stderr, "FAIL 2D w=%u bh=%u\n", d.width, bh);
                return 1;
            }
        }
    }

    const u32 widths3d[] = {12, 32, 64, 96, 140};
    for (size_t wi = 0; wi < sizeof widths3d / sizeof widths3d[0]; wi++) {
        TextureDesc d = {0};
        d.width = widths3d[wi]; d.height = 10; d.depth = 5; d.levels = 1;
        d.block_height = 1; d.block_depth = 1; d.bytes = 1; d.bw = 1; d.bh = 1;
        size_t out_bytes = (size_t)d.width * d.height * d.depth;
        memset(got, 0x55, sizeof got);
        memset(ref, 0x55, sizeof ref);
        texture_read_3d(&d, (u64)(uintptr_t)src, got);
        ref_texture_read_3d(&d, src, ref);
        if (memcmp(got, ref, out_bytes) != 0) {
            fprintf(stderr, "FAIL 3D w=%u\n", d.width);
            return 1;
        }
    }
    return 0;
}
"""
        compile_and_run(src)


if __name__ == '__main__':
    unittest.main()
