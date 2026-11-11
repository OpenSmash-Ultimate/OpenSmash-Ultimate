"""NVIDIA's shader caches (runtime/gpu_nvcache.c) through tools/dev/nvcachetest.c: the game's own folder for the
Vulkan driver's cache, trimmed oldest set first (a .bin and its .toc go together, a set written in the last
minutes stays), the variable the driver reads set for the process and its C runtime (a player's own setting
kept), and the words for the driver's Shader Cache Size. Windows only; skipped where there is no clang-cl."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)
MB = 1 << 20


@unittest.skipUnless(HAVE_CLANG, 'Windows with clang-cl only (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class NvCache(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.mkdtemp(prefix='nvcachetest ')
        cls.exe = os.path.join(cls.folder, 'nvcachetest.exe')
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, 'tools/dev/nvcachetest.c', '/Fe' + cls.exe,
               '/Fo' + cls.folder + os.sep, '/link', *cc.LINK_OPTIONS]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('nvcachetest.c did not build:\n' + r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.folder, ignore_errors=True)

    def run_test(self, *args, **env_more):
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('SSBU_', '__GL_'))}
        env.update(env_more)
        r = subprocess.run([self.exe, *args], env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout.splitlines(), r.stderr

    def make_set(self, d, name, mb, age_s, toc=True):
        """A driver set like GLCache's: <name>.bin of mb megabytes (and a small .toc), written age_s seconds ago."""
        sub = Path(d) / 'GLCache' / '38af177a636d64eb6428bd306291f7a8' / '6654e9322393e267'
        sub.mkdir(parents=True, exist_ok=True)
        when = time.time() - age_s
        for ext, size in (('.bin', mb * MB), ('.toc', 4096)) if toc else (('.bin', mb * MB),):
            p = sub / (name + ext)
            with open(p, 'wb') as f:
                f.truncate(size)
            os.utime(p, (when, when))
        return sub

    def test_trim_oldest_sets_first(self):
        d = tempfile.mkdtemp(dir=self.folder)
        sub = self.make_set(d, 'aaaa', 3, 3 * 86400)
        self.make_set(d, 'bbbb', 3, 2 * 86400)
        self.make_set(d, 'cccc', 3, 86400)
        out, _ = self.run_test('trim', d, '7', '600000')
        left, removed, gone = out[0].split()[1:]
        self.assertEqual(int(removed), 1)
        self.assertEqual(int(gone), 3 * MB + 4096)
        self.assertEqual(int(left), 6 * MB + 2 * 4096)
        self.assertEqual(sorted(p.name for p in sub.iterdir()), ['bbbb.bin', 'bbbb.toc', 'cccc.bin', 'cccc.toc'])

    def test_a_set_in_use_stays(self):
        """A set written in the last minutes is a game running now: kept even over the cap."""
        d = tempfile.mkdtemp(dir=self.folder)
        sub = self.make_set(d, 'old0', 2, 86400)
        self.make_set(d, 'new0', 4, 5)
        out, _ = self.run_test('trim', d, '1', '600000')
        self.assertEqual(out[0].split()[2], '1')
        self.assertEqual(sorted(p.name for p in sub.iterdir()), ['new0.bin', 'new0.toc'])

    def test_a_set_is_as_new_as_its_newest_file(self):
        """The .toc rewritten at a later start keeps its .bin's set from looking old."""
        d = tempfile.mkdtemp(dir=self.folder)
        sub = self.make_set(d, 'used', 3, 5 * 86400)
        now = time.time() - 3600
        os.utime(sub / 'used.toc', (now, now))
        self.make_set(d, 'idle', 3, 2 * 86400)
        out, _ = self.run_test('trim', d, '4', '600000')
        self.assertEqual(sorted(p.name for p in sub.iterdir()), ['used.bin', 'used.toc'])

    def test_under_the_cap_nothing_goes(self):
        d = tempfile.mkdtemp(dir=self.folder)
        sub = self.make_set(d, 'only', 2, 86400)
        out, _ = self.run_test('trim', d, '3072', '600000')
        self.assertEqual(out[0].split()[2:], ['0', '0'])
        self.assertEqual(len(list(sub.iterdir())), 2)

    def test_setup_points_the_driver_at_the_game_folder(self):
        d = tempfile.mkdtemp(dir=self.folder)
        out, err = self.run_test('setup', d)
        want = os.path.join(d, 'nvidia')
        self.assertTrue(out[0].startswith('setup '))
        process, crt = out[0][len('setup '):].split('|')
        self.assertTrue(os.path.isdir(want))
        self.assertTrue(os.path.samefile(process, want), process)
        self.assertEqual(crt, process)
        self.assertIn("NVIDIA's shader cache: the game's own folder", err)

    def test_setup_trims_with_the_cap(self):
        d = tempfile.mkdtemp(dir=self.folder)
        sub = self.make_set(os.path.join(d, 'nvidia'), 'gone', 2, 86400)
        self.make_set(os.path.join(d, 'nvidia'), 'kept', 2, 3600)
        _, err = self.run_test('setup', d, SSBU_NV_CACHE_MB='3')
        self.assertEqual(sorted(p.name for p in sub.iterdir()), ['kept.bin', 'kept.toc'])
        self.assertIn('1 old sets removed', err)

    def test_the_players_own_path_is_kept(self):
        d = tempfile.mkdtemp(dir=self.folder)
        out, _ = self.run_test('setup', d, __GL_SHADER_DISK_CACHE_PATH='D:\\mine')
        self.assertTrue(out[0].startswith('setup D:\\mine|'))
        self.assertFalse(os.path.exists(os.path.join(d, 'nvidia')))

    def test_off(self):
        d = tempfile.mkdtemp(dir=self.folder)
        out, err = self.run_test('setup', d, SSBU_NV_CACHE='0')
        self.assertEqual(out[0], 'setup -|-')
        self.assertIn('SSBU_NV_CACHE=0', err)

    def test_setting_words(self):
        out, _ = self.run_test('words', '0', '0', 'words', '1', '0', 'words', '1', '10240', 'words', '1', '0xFFFFFFFF',
                               'words', '1', '512', 'words', '1', '102400')
        self.assertEqual(out, ['words Driver Default', 'words Driver Default', 'words 10 GB', 'words Unlimited', 'words 512 MB',
                               'words 100 GB'])


if __name__ == '__main__':
    unittest.main()
