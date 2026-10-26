"""ssbu-recomp's rollback netcode, the parts that do not touch the game (ssbu-recomp/runtime/rb.h: the
input queues, the wire format, the time sync; the handshake record of rb_hs.h), through its own test,
ssbu-recomp/tests/rb_test.c, with made-up pads over a simulated link: built with the platform's
compiler, warnings as errors, and run. Windows: clang-cl (as tests/test_clock.py finds it); Linux and
macOS: clang, and once more with the address and undefined-behaviour sanitizers where that clang has
them. Skipped without a compiler, unless RB_TEST_NEED_CLANG=1 (.github/workflows/rollback-netcode.yml
sets it, so that a runner without one fails instead). What rb_test printed is shown, for the CI logs."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SR = ROOT / 'ssbu-recomp'
RUNTIME = SR / 'runtime'
SOURCES = [SR / 'tests' / 'rb_test.c', RUNTIME / 'rb_input.c', RUNTIME / 'rb_wire.c', RUNTIME / 'rb_timesync.c']

if os.name == 'nt':
    sys.path.insert(0, str(ROOT / 'tools'))
    import cc  # noqa: E402
    CLANG = cc.CLANG if os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) else None
    WHY = 'needs clang-cl (deps\\llvm, or Visual Studio\'s C++ Clang tools)'
else:
    CLANG = shutil.which('clang')
    WHY = 'needs clang'
NEED = os.environ.get('RB_TEST_NEED_CLANG') == '1'


def build(out, sanitize=False, sources=None):
    """The command that builds rb_test (or `sources`, with the same flags) in the folder `out`, and the program it makes."""
    srcs = [str(s) for s in (sources or SOURCES)]
    if os.name == 'nt':
        exe = os.path.join(out, 'rb_test.exe')
        return [CLANG, '/nologo', '/O1', '/W4', '/WX', *cc.LINK_FLAGS, '/I', str(RUNTIME), *srcs, '/Fe' + exe,
                '/link', *cc.LINK_OPTIONS], exe
    exe = os.path.join(out, 'rb_test')
    san = ['-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'] if sanitize else []
    return [CLANG, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', *san, '-I', str(RUNTIME), *srcs, '-o', exe], exe


class RollbackNetcodeTests(unittest.TestCase):
    def setUp(self):
        if CLANG is None:
            if NEED:
                self.fail(WHY + ' (RB_TEST_NEED_CLANG=1)')
            self.skipTest(WHY)
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def compile(self, cmd):
        # (in the temporary folder: clang-cl leaves its object files where it runs)
        return subprocess.run(cmd, capture_output=True, text=True, cwd=self.dir, timeout=300)

    def run_rb_test(self, exe, what):
        r = subprocess.run([exe], capture_output=True, text=True, cwd=self.dir, timeout=120)
        print('\nrb_test (%s, %s):\n%s%s' % (what, CLANG, r.stdout, r.stderr), end='', flush=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('rb_test: all passed', r.stdout)
        self.assertEqual(r.stdout.count(' rollbacks, '), 2, 'both links ran')

    def test_rb_test(self):
        """Every check of rb_test passes: wire format, input queues, time sync, the two links."""
        cmd, exe = build(self.dir)
        r = self.compile(cmd)
        if r.returncode:
            raise AssertionError('rb_test did not build:\n%s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
        self.run_rb_test(exe, 'built as is')

    @unittest.skipIf(os.name == 'nt', 'the sanitizer build is for clang on Linux and macOS')
    def test_rb_test_sanitizers(self):
        """The same with the address and undefined-behaviour sanitizers: no read past a message's end, no
        overflow. Skipped where this clang has no sanitizer runtime (an empty program does not build either)."""
        cmd, exe = build(self.dir, sanitize=True)
        r = self.compile(cmd)
        if r.returncode:
            empty = os.path.join(self.dir, 'empty.c')
            with open(empty, 'w') as f:
                f.write('int main(void) { return 0; }\n')
            if self.compile(build(self.dir, sanitize=True, sources=[empty])[0]).returncode:
                self.skipTest('this clang has no sanitizer runtime:\n' + r.stderr[-500:])
            raise AssertionError('rb_test did not build with the sanitizers:\n%s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
        self.run_rb_test(exe, 'address and undefined-behaviour sanitizers')


if __name__ == '__main__':
    unittest.main()
