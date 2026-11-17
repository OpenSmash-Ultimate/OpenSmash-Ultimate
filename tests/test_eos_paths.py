"""A pair's path over Epic Online Services (runtime/net_path.c): our own direct path first, Epic's P2P when the direct
one is dead or clearly slower (15 % and 5 ms, for 3 s), never a move mid-fight unless the path in use died; and the
lobby's /gsp/records answer as runtime/net_results.c reads it. tools/dev/pathtest.c holds the cases."""
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


@unittest.skipUnless(HAVE_CLANG, 'needs clang-cl')
class PathChoice(unittest.TestCase):
    def test_cases(self):
        d = tempfile.mkdtemp()
        try:
            exe = os.path.join(d, 'pathtest.exe')
            r = subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, str(ROOT / 'tools' / 'dev' / 'pathtest.c'),
                                str(ROOT / 'runtime' / 'net_path.c'), str(ROOT / 'runtime' / 'net_results.c'), str(ROOT / 'runtime' / 'net_discord_core.c'),
                                '/Fe' + exe, '/Fo' + d + os.sep, '/link', *cc.LINK_OPTIONS], capture_output=True, text=True, cwd=d)
            self.assertEqual(r.returncode, 0, 'pathtest.c did not build:\n' + r.stdout + r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=60, cwd=d)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn(' 0 failed', r.stdout)
        finally:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
