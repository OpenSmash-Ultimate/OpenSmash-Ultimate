"""The native kernel's threads and handles (runtime/kernel.c) through tools/dev/kerneltest.c, with
made-up guest threads and no game: with SSBU_THREAD_RECYCLING=1, thousands of threads made and
ended one after another and in batches without the tables filling up, exited threads, their
handles and thread-local regions used again, stale handles refused, waits, mutexes and condition
variables still right; without it (unset, or another value), the old behaviour. Built with the
platform's compiler: clang-cl on Windows (as tests/test_clock.py builds clocktest.c), clang with
the platform layer elsewhere (runtime/linux/win32.c, with runtime/macos/'s memory and platform
parts on macOS; the Windows types from runtime/macos/include on both, the Linux build's own
coming with its Direct3D headers). Skipped where there is no such compiler, and on hosts the
platform layer is not written for (Linux on x86-64 and macOS on Apple silicon only, as the game)."""
from pathlib import Path
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

if os.name == 'nt':
    HAVE_CLANG = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
else:
    HAVE_CLANG = shutil.which('clang') is not None and platform.machine() in (('arm64',) if sys.platform == 'darwin' else ('x86_64', 'AMD64'))


def build(folder):
    src = str(ROOT / 'tools' / 'dev' / 'kerneltest.c')
    if os.name == 'nt':
        exe = os.path.join(folder, 'kerneltest.exe')
        # (as cc.link links the game: kernel_init reserves the guest's areas at fixed addresses, and with
        # high-entropy ASLR Windows can put a heap or a thread stack there first)
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, src, '/Fe' + exe, '/Fo' + os.path.join(folder, 'kerneltest.obj'),
               '/link', *cc.LINK_OPTIONS, '/HIGHENTROPYVA:NO']
    else:
        exe = os.path.join(folder, 'kerneltest')
        layer = ['runtime/linux/win32.c']   # (the platform layer)
        if sys.platform == 'darwin':
            layer += ['runtime/macos/memory.c', 'runtime/macos/platform.c']
        else:
            layer += ['-lpthread']
        cmd = ['clang', '-O1', '-w', '-I', 'runtime/macos/include', '-I', 'runtime/linux/include', '-I', 'runtime', src, *layer, '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError('kerneltest.c did not build:\n' + r.stdout + r.stderr)
    return exe


@unittest.skipUnless(HAVE_CLANG, 'needs clang on x86-64 Linux or an Apple silicon Mac, or clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class KernelThreadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='kerneltest ')
        cls.exe = build(cls.dir)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def kerneltest(self, mode, switch=None):
        env = {k: v for k, v in os.environ.items() if k != 'SSBU_THREAD_RECYCLING'}
        if switch is not None:
            env['SSBU_THREAD_RECYCLING'] = switch
        r = subprocess.run([self.exe, mode], capture_output=True, text=True, env=env, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'\d+ checks, 0 failed')
        return r.stdout + r.stderr

    def test_recycling(self):
        out = self.kerneltest('recycling', '1')
        self.assertIn('8000 threads made', out)
        self.assertEqual(out.count('[kernel] half the handle table in use'), 1, out)   # (said once, with 2100 events open)

    def test_old_behaviour(self):
        self.assertNotIn('[kernel] half the handle table', self.kerneltest('old'))

    def test_other_values_keep_the_old_behaviour(self):
        self.kerneltest('old', '0')
        self.kerneltest('old', 'yes')


if __name__ == '__main__':
    unittest.main()
