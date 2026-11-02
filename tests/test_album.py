"""The album (runtime/hle_album.c): the pictures the game saves (the Vault's snapshots) kept as
files in the port's album folder, listed and loaded back for the Vault, through tools/dev/albumtest.c
in a made-up album folder. Built with the game's compiler; skipped where there is no clang-cl."""
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


@unittest.skipUnless(HAVE_CLANG, 'needs Windows and clang-cl (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class AlbumTests(unittest.TestCase):
    def test_album(self):
        d = tempfile.mkdtemp()
        try:
            exe = os.path.join(d, 'albumtest.exe')
            r = subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, str(ROOT / 'tools' / 'dev' / 'albumtest.c'),
                                '/Fe' + exe, '/Fo' + os.path.join(d, 'albumtest.obj'), '/link', *cc.LINK_OPTIONS],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, 'albumtest.c did not build:\n' + r.stdout + r.stderr)
            data = os.path.join(d, 'native port')
            os.mkdir(data)
            env = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
            env['SSBU_DATA_ROOT'] = data
            r = subprocess.run([exe], capture_output=True, text=True, timeout=60, cwd=d, env=env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('checks passed', r.stdout)
            names = sorted(os.listdir(os.path.join(data, 'album')))
            self.assertTrue(any(n.endswith('_00.userdata.bin') for n in names), names)
            self.assertFalse(any(n.endswith('.part') for n in names), names)   # (written whole)
        finally:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
