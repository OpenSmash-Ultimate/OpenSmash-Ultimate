"""A private room's code (runtime/room_code.h, net_relay.c), through tools/dev/roomcodetest.c: codes
made from addresses and read back, the forms a person may type, and what is not a code. No game
files; built with clang-cl on Windows, clang elsewhere, and skipped where there is neither."""
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

if os.name == 'nt':
    COMPILER = cc.CLANG if (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG)) else None
else:
    COMPILER = shutil.which('clang') or shutil.which('cc')


@unittest.skipUnless(COMPILER, 'needs a C compiler (clang-cl on Windows, clang or cc elsewhere)')
class RoomCodeTests(unittest.TestCase):
    def test_room_code(self):
        with tempfile.TemporaryDirectory(prefix='roomcodetest ') as d:
            src = str(ROOT / 'tools' / 'dev' / 'roomcodetest.c')
            if os.name == 'nt':
                exe = os.path.join(d, 'roomcodetest.exe')
                cmd = [COMPILER, '/nologo', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, src, '/Fe' + exe, '/Fo' + os.path.join(d, 'roomcodetest.obj'),
                       '/link', *cc.LINK_OPTIONS]
            else:
                exe = os.path.join(d, 'roomcodetest')
                cmd = [COMPILER, '-O1', '-w', src, '-o', exe]
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertRegex(r.stdout, r'\d+ checks, 0 failed\n$')


if __name__ == '__main__':
    unittest.main()
