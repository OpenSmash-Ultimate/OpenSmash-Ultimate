"""The save data journal (runtime/hle_fs.c) through tools/dev/fstest.c, on made-up saves: its
checks in one process, then the crash matrix - a commit cut short at each of its steps by a
process that ends there, the save mounted again by a fresh process, which must find all of the
old save or all of the new one and nothing of the journal left; and the same once more with
that recovery cut short too. Built with the platform's compiler: clang-cl on Windows (as
tests/test_clock.py builds clocktest.c), clang with the platform layer elsewhere
(runtime/linux/win32.c, with runtime/macos/'s memory and platform parts on macOS; the
Windows types from runtime/macos/include on both, the Linux build's own coming with its
Direct3D headers). Skipped where there is no such compiler, and on hosts the platform layer
is not written for (Linux on x86-64 and macOS on Apple silicon only, as the game)."""
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
CRASHED = 99   # (fstest's exit code at the step SSBU_FS_CRASH_AT names)


def build(folder):
    src = str(ROOT / 'tools' / 'dev' / 'fstest.c')
    if os.name == 'nt':
        exe = os.path.join(folder, 'fstest.exe')
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, src, '/Fe' + exe, '/Fo' + os.path.join(folder, 'fstest.obj'),
               '/link', *cc.LINK_OPTIONS]
    else:
        exe = os.path.join(folder, 'fstest')
        layer = ['runtime/linux/win32.c']   # (the platform layer)
        if sys.platform == 'darwin':
            layer += ['runtime/macos/memory.c', 'runtime/macos/platform.c']
        else:
            layer += ['-lpthread']
        cmd = ['clang', '-O1', '-w', '-I', 'runtime/macos/include', '-I', 'runtime/linux/include', '-I', 'runtime', src, *layer, '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError('fstest.c did not build:\n' + r.stdout + r.stderr)
    return exe


@unittest.skipUnless(HAVE_CLANG, 'needs clang on x86-64 Linux or an Apple silicon Mac, or clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class SaveJournalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='fstest ')
        cls.exe = build(cls.dir)
        cls.steps = None

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def fstest(self, folder, *args, crash_at=None):
        env = {k: v for k, v in os.environ.items() if k not in ('SSBU_FS_CRASH_AT', 'SSBU_DATA_ROOT')}
        if crash_at:
            env['SSBU_FS_CRASH_AT'] = str(crash_at)
        return subprocess.run([self.exe, folder, *args], capture_output=True, text=True, env=env, timeout=120)

    def folder(self):
        return tempfile.mkdtemp(dir=self.dir)

    def verify(self, folder):
        """'old' or 'new': what a fresh process mounting the save finds, with nothing of the journal left."""
        r = self.fstest(folder, 'verify')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout.split()[0]

    def commit_steps(self):
        """How many steps a commit of the new save has (SSBU_FS_CRASH_AT=1..that)."""
        if self.steps is None:
            folder = self.folder()
            for n in range(1, 200):
                self.assertEqual(self.fstest(folder, 'old').returncode, 0)
                r = self.fstest(folder, 'new', crash_at=n)
                if r.returncode == 0:
                    type(self).steps = n - 1
                    break
                self.assertEqual(r.returncode, CRASHED, r.stdout + r.stderr)
            else:
                self.fail('the commit never ran to its end')
        return self.steps

    def test_checks(self):
        r = self.fstest(self.folder(), 'checks', str(os.getpid()))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'\d+ checks, 0 failed')

    def test_old_and_new(self):
        """Without a crash: the old save as made, then the new one after a commit."""
        folder = self.folder()
        self.assertEqual(self.fstest(folder, 'old').returncode, 0)
        self.assertEqual(self.verify(folder), 'old')
        r = self.fstest(folder, 'new')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.verify(folder), 'new')

    def test_crash_at_each_step(self):
        """Steps 1 (half the journal written) and 2 (before the rename that decides) leave the old
        save; from 3 on (the rename made, then each change applied, then the marker deleted) the
        next mount finishes the commit."""
        steps = self.commit_steps()
        self.assertGreaterEqual(steps, 3 + 13 + 2)   # (the new save is 13 deletions, folders and files)
        folder = self.folder()
        found = []
        for n in range(1, steps + 1):
            self.assertEqual(self.fstest(folder, 'old').returncode, 0)
            r = self.fstest(folder, 'new', crash_at=n)
            self.assertEqual(r.returncode, CRASHED, r.stdout + r.stderr)
            found.append(self.verify(folder))
        self.assertEqual(found, ['old'] * 2 + ['new'] * (steps - 2))

    def test_crash_while_recovering(self):
        """A commit cut short after it was decided (right after the rename, and before the marker
        is deleted), then the mount finishing it cut short at each of its steps too."""
        steps = self.commit_steps()
        folder = self.folder()
        for k in (3, steps - 1):
            for j in range(1, 200):
                self.assertEqual(self.fstest(folder, 'old').returncode, 0)
                self.assertEqual(self.fstest(folder, 'new', crash_at=k).returncode, CRASHED)
                r = self.fstest(folder, 'verify', crash_at=j)
                if r.returncode == 0:   # (the recovery has fewer steps than j)
                    self.assertEqual(r.stdout.split()[0], 'new', r.stdout)
                    break
                self.assertEqual(r.returncode, CRASHED, r.stdout + r.stderr)
                self.assertEqual(self.verify(folder), 'new', f'commit cut at step {k}, its recovery at step {j}')
            else:
                self.fail('the recovery never ran to its end')


if __name__ == '__main__':
    unittest.main()
