"""The native kernel's wait queues (runtime/kernel.c) through tools/dev/waittest.c, with made-up guest
threads and no game: WaitSynchronization (events, threads ending), userspace mutexes, condition
variables and the address arbiter - a signal before the wait, timeouts, cancels, many waiters and
one signal (first come first served, only the waiters of what was signalled woken, each once), a
wait on several objects, an event set and cleared before its waiters run, waiters that exit, the
result codes, the freeze report's states - and a stress run of each for lost wake-ups; thread
priorities and core masks (as before by default, kept and checked as on the console with
SSBU_CORE_MASKS=1), the queues' order with SSBU_PRIORITY_QUEUES=1 and without, and with
SSBU_HOST_PRIORITIES=1 the host priority calls the kernel makes and what the host thread then has;
with SSBU_THREAD_RECYCLING=1 and without, and both again with the three priority switches set in
the environment. Built with the platform's compiler: clang-cl on Windows (as
tests/test_clock.py builds clocktest.c), clang with the platform layer elsewhere
(runtime/linux/win32.c, with runtime/macos/'s memory and platform parts on macOS; the Windows types
from runtime/macos/include on both). Skipped where there is no such compiler, and on hosts the
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
    src = str(ROOT / 'tools' / 'dev' / 'waittest.c')
    if os.name == 'nt':
        exe = os.path.join(folder, 'waittest.exe')
        # (as cc.link links the game: kernel_init reserves the guest's areas at fixed addresses, and with
        # high-entropy ASLR Windows can put a heap or a thread stack there first)
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, src, '/Fe' + exe, '/Fo' + os.path.join(folder, 'waittest.obj'),
               '/link', *cc.LINK_OPTIONS, '/HIGHENTROPYVA:NO']
    else:
        exe = os.path.join(folder, 'waittest')
        layer = ['runtime/linux/win32.c']   # (the platform layer)
        if sys.platform == 'darwin':
            layer += ['runtime/macos/memory.c', 'runtime/macos/platform.c']
        else:
            layer += ['-lpthread']
        cmd = ['clang', '-O1', '-w', '-I', 'runtime/macos/include', '-I', 'runtime/linux/include', '-I', 'runtime', src, *layer, '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError('waittest.c did not build:\n' + r.stdout + r.stderr)
    return exe


@unittest.skipUnless(HAVE_CLANG, 'needs clang on x86-64 Linux or an Apple silicon Mac, or clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class KernelWaitTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='waittest ')
        cls.exe = build(cls.dir)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    SWITCHES = ('SSBU_THREAD_RECYCLING', 'SSBU_PRIORITY_QUEUES', 'SSBU_HOST_PRIORITIES', 'SSBU_CORE_MASKS')

    def waittest(self, switch=None, **more):
        env = {k: v for k, v in os.environ.items() if k not in self.SWITCHES}
        if switch is not None:
            env['SSBU_THREAD_RECYCLING'] = switch
        env.update(more)
        r = subprocess.run([self.exe], capture_output=True, text=True, env=env, timeout=600)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'\d+ checks, 0 failed')
        return r

    def test_with_recycling(self):
        out = self.waittest('1').stdout
        self.assertIn('thread recycling on', out)
        self.assertIn('queues by priority off, host priorities off, core masks as before', out)

    def test_without_recycling(self):
        self.assertIn('thread recycling off', self.waittest().stdout)

    def test_with_the_priority_switches(self):
        for recycling in ('1', None):
            with self.subTest(recycling=recycling):
                r = self.waittest(recycling, SSBU_PRIORITY_QUEUES='1', SSBU_HOST_PRIORITIES='1', SSBU_CORE_MASKS='1')
                self.assertIn('queues by priority on', r.stdout)
                self.assertIn("core masks the console's", r.stdout)
                self.assertIn('(SSBU_PRIORITY_QUEUES=1)', r.stderr)
                self.assertIn('(SSBU_CORE_MASKS=1)', r.stderr)
                if 'SSBU_HOST_PRIORITIES=1 left off' in r.stderr:   # (Linux without RLIMIT_NICE of 20 or root)
                    self.assertTrue(sys.platform.startswith('linux'), r.stderr)
                    self.assertIn('host priorities off', r.stdout)
                else:
                    self.assertIn('host priorities on', r.stdout)
                    self.assertIn('(SSBU_HOST_PRIORITIES=1)', r.stderr)

    def test_other_values_keep_the_old_answers(self):
        self.assertIn('core masks as before', self.waittest(SSBU_CORE_MASKS='0').stdout)
        self.assertIn('core masks as before', self.waittest(SSBU_CORE_MASKS='yes').stdout)


if __name__ == '__main__':
    unittest.main()
