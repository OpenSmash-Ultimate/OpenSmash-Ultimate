"""ssbu-recomp's shader translator (runtime/mx_translate.c) and its Metal Shading Language target (MxIn.target 2):
tools/lifter/mxmsltest.m translates made-up Maxwell programs (no game files), compiles the MSL with Metal, makes
pipelines of it and runs some of it on the GPU, checked against the same arithmetic done in C. Built without Metal
(MXMSLTEST_GLSL_ONLY) it writes the OpenGL and Vulkan text of the same programs, which is how the MSL target was
shown to leave that text as it was (mxmsltest glsl / glsldiff, with an older mx_translate.c)."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SR = ROOT / 'ssbu-recomp'
SOURCES = ['tools/lifter/mxmsltest.m', 'runtime/mx_translate.c']
FLAGS = ['-fno-strict-aliasing', '-ffp-contract=off', '-O1', '-I', 'runtime']
LIBM = [] if sys.platform in ('darwin', 'win32') else ['-lm']   # (Linux: rintf, floorf... are in libm; macOS's are in libSystem)


class MxMslTests(unittest.TestCase):
    def build(self, cmd):
        r = subprocess.run(cmd, cwd=SR, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def run_tool(self, exe, *args, timeout=600):
        r = subprocess.run([exe, *args], capture_output=True, text=True, timeout=timeout)
        if 'no Metal device' in r.stderr:
            self.skipTest('no Metal device')
        self.assertEqual(r.returncode, 0, (r.stdout + r.stderr)[-6000:])
        return r.stdout

    @unittest.skipUnless(sys.platform == 'darwin', 'Metal')
    def test_msl(self):
        """Every encoding the translator takes, in programs of every stage; 3000 random programs; the GPU checks."""
        with tempfile.TemporaryDirectory(prefix='mxmsltest ') as tmp:
            exe = os.path.join(tmp, 'mxmsltest')
            self.build(['clang', '-arch', 'arm64', '-mmacosx-version-min=14.0', '-fobjc-arc', *FLAGS, *SOURCES,
                        '-framework', 'Metal', '-framework', 'Foundation', '-o', exe])
            out = self.run_tool(exe, 'all')
            m = re.search(r'corpus: \d+ programs, (\d+) of the (\d+) translated encodings, 0 refused, .*: ok', out)
            self.assertIsNotNone(m, out[-6000:])
            self.assertEqual(m.group(1), m.group(2))
            self.assertRegex(out, r'fuzz: 3000 random programs, .* 0 FAILED')
            self.assertRegex(out, r'gpu: \d+ checks, 0 failed')

    @unittest.skipIf(sys.platform == 'win32', 'not tried with clang on Windows (mxtest is the Windows tool)')
    def test_glsl(self):
        """The text of targets 0 and 1 for the same programs, built without Metal: it runs to the end (3000 random
        programs too) and every corpus program translates for Vulkan."""
        cc = shutil.which('clang') or shutil.which('cc')
        if not cc:
            self.skipTest('no C compiler')
        with tempfile.TemporaryDirectory(prefix='mxmsltest ') as tmp:
            exe, txt = os.path.join(tmp, 'mxglsl'), os.path.join(tmp, 'glsl.txt')
            self.build([cc, '-DMXMSLTEST_GLSL_ONLY', *FLAGS, '-x', 'c', *SOURCES, '-o', exe, *LIBM])
            self.run_tool(exe, 'glsl', txt)
            text = Path(txt).read_text()
            self.assertIn('==== fuzz 2999 (', text)
            corpus = re.findall(r'^==== (?!fuzz )(\S+) \d+ \(\w+, target 1, options (\d)\)\n(.*)$', text, re.M)
            self.assertGreater(len(corpus), 40)
            # (the two compiled only with options 1: typeless image loads need the formats, screen_scale is an option)
            failed = [(n, o) for n, o, line in corpus if line.startswith('FAILED') and (n, o) not in
                      {('cs_images_typeless', '0'), ('fs_scaled', '0')}]
            self.assertEqual(failed, [])


if __name__ == '__main__':
    unittest.main()
