"""The sleep behind the guest's svcSleepThread (runtime/sleep.h) through tools/dev/sleeptest.c, built with
the platform's compiler and run: never shorter than asked, a sleep of 200 us well under the
millisecond it used to be rounded up to, and SSBU_SLEEP=ms bringing the whole milliseconds back.
Windows: clang-cl (skipped without it); Linux: clang; macOS (arm64, as the port): clang, with
runtime/macos/platform.c, whose timer it sleeps on. What it measured is printed, for the CI logs."""
from pathlib import Path
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

SRC = ROOT / 'tools' / 'dev' / 'sleeptest.c'
POSIX_CLANG = shutil.which('clang')
if os.name == 'nt':
    HAVE = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
    WHY = 'needs clang-cl (deps\\llvm, or Visual Studio\'s C++ Clang tools)'
elif sys.platform == 'darwin':
    HAVE = POSIX_CLANG is not None and platform.machine() == 'arm64'
    WHY = 'needs clang on an arm64 Mac (the macOS port is arm64 only: runtime/macos/platform.c)'
else:
    HAVE = POSIX_CLANG is not None
    WHY = 'needs clang'


def build(out):
    """The command that builds sleeptest into the folder `out`, and the program it makes."""
    if os.name == 'nt':
        exe = os.path.join(out, 'sleeptest.exe')
        return [cc.CLANG, '/nologo', '/O2', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, str(SRC), '/Fe' + exe,
                '/Fo' + os.path.join(out, 'sleeptest.obj'), '/link', *cc.LINK_OPTIONS], exe
    exe = os.path.join(out, 'sleeptest')
    if sys.platform == 'darwin':
        rt = ROOT / 'runtime'
        return [POSIX_CLANG, '-O2', '-I', str(rt / 'macos' / 'include'), '-I', str(rt / 'linux' / 'include'), str(SRC),
                str(rt / 'macos' / 'platform.c'), '-o', exe], exe
    return [POSIX_CLANG, '-O2', str(SRC), '-o', exe, '-pthread'], exe


@unittest.skipUnless(HAVE, WHY)
class SleepTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.addClassCleanup(shutil.rmtree, cls.dir, ignore_errors=True)   # (also when the build fails)
        cmd, cls.exe = build(cls.dir)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('sleeptest.c did not build:\n' + r.stdout + r.stderr)

    def run_test(self, mode, whole_ms=False):
        env = {k: v for k, v in os.environ.items() if k != 'SSBU_SLEEP'}
        if whole_ms:
            env['SSBU_SLEEP'] = 'ms'
        r = subprocess.run([self.exe, mode], capture_output=True, text=True, timeout=120, env=env)
        print('\nsleeptest %s:\n%s' % (mode, r.stdout), end='', flush=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout

    def test_never_early(self):
        """Every sleep at least as long as asked, from 1 us to 2.5 ms, also on four threads at once
        (each with its timer on Windows)."""
        self.assertIn(' 0 failed', self.run_test('early'))

    def test_well_under_a_millisecond(self):
        """The median of 401 sleeps of 200 us under 900 us (the old one took a whole millisecond or
        more). CI's machines are virtual and their timers noisy: hence the median and the margin."""
        out = self.run_test('fine')
        skipped = re.search(r'^skipped: (.*)$', out, re.M)
        if skipped:
            self.skipTest(skipped.group(1))
        self.assertIn(' 0 failed', out)

    def test_whole_ms_switch(self):
        """SSBU_SLEEP=ms: whole milliseconds, rounded up, as before (Linux, macOS: each sleep at least
        that; Windows: by Sleep(), with no high-resolution timer made)."""
        self.assertIn(' 0 failed', self.run_test('ms', whole_ms=True))


if __name__ == '__main__':
    unittest.main()
