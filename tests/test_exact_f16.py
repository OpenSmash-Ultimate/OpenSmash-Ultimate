"""The exact f32tof16 written into glslang's HLSL on AMD cards (runtime/gpu_shader.c F32TOF16_HLSL, #353): every
float gives the half the processor's own conversion gives (F16C, round to nearest even; out of range: infinity), and a
NaN stays a NaN of its sign. The HLSL is compiled as C, as it is written in gpu_shader.c. Windows with clang-cl only."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)


def hlsl_function():
    text = (ROOT / 'runtime' / 'gpu_shader.c').read_text(encoding='utf-8')
    block = re.search(r'static const char F32TOF16_HLSL\[\] =\n(.*?);\n', text, re.S).group(1)
    return ''.join(eval(line.strip()) for line in block.splitlines() if line.strip())   # (C string literals)


HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include <immintrin.h>
typedef unsigned uint;
static uint asuint(float f) { uint u; memcpy(&u, &f, 4); return u; }
%s
__attribute__((target("f16c"))) static unsigned short ref(float f) { return (unsigned short)_cvtss_sh(f, _MM_FROUND_TO_NEAREST_INT); }
int main(void) {
    unsigned long long bad = 0, nans = 0;
    for (unsigned long long i = 0; i <= 0xFFFFFFFFull; i++) {
        uint x = (uint)i;
        float f;
        memcpy(&f, &x, 4);
        uint got = f32tof16(f);
        if ((x & 0x7FFFFFFFu) > 0x7F800000u) {   /* NaN: a NaN of its sign */
            nans++;
            if (got != (((x >> 16) & 0x8000u) | 0x7E00u)) { if (bad++ < 5) printf("nan %%08x -> %%04x\n", x, got); }
            continue;
        }
        uint want = ref(f);
        if (got != want && bad++ < 5) printf("%%08x -> %%04x, want %%04x\n", x, got, want);
    }
    printf("bad %%llu nans %%llu\n", bad, nans);
    return bad != 0;
}
'''


@unittest.skipUnless(HAVE_CLANG, "Windows with clang-cl only (deps\\llvm, or Visual Studio's C++ Clang tools)")
class ExactF16Tests(unittest.TestCase):
    def test_every_float(self):
        src = HARNESS % hlsl_function()
        with tempfile.TemporaryDirectory(prefix='exact_f16_') as directory:
            c_file = Path(directory) / 'harness.c'
            exe = Path(directory) / 'harness.exe'
            c_file.write_text(src, encoding='utf-8')
            built = subprocess.run([cc.CLANG, '/nologo', '/O2', *cc.LINK_FLAGS, *cc.CFLAGS, str(c_file),
                                    '/Fo' + directory + os.sep, '/Fe' + str(exe), '/link', *cc.LINK_OPTIONS],
                                   cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('bad 0 ', ran.stdout)


if __name__ == '__main__':
    unittest.main()
