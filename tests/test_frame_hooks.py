"""The frame hooks (runtime/frame_hooks.h, runtime/hle_each_frame.c): the one wrap of each of the
game's two once-a-frame points, and the handlers registered on them, through tools/dev/framehooktest.c
with made-up handlers and a made-up game function behind each wrap: nothing but the game's function
with nothing registered, the order handlers nest in, what each one is given, the limits, a
registration made during a call, and registrations from several threads while the wraps run. Built
with clang-cl on Windows, clang elsewhere (the runtime's POSIX headers); skipped without one.

Also where the wraps are: once each, in hle_each_frame.c, which tools/recomp.py reads before
hle_fight_test.c, so the exported names it wraps keep their order in the tools fingerprint."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402
import recomp  # noqa: E402

POSIX_CLANG = shutil.which('clang')
if os.name == 'nt':
    HAVE = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
    WHY = 'needs clang-cl (deps\\llvm, or Visual Studio\'s C++ Clang tools)'
else:
    HAVE = POSIX_CLANG is not None
    WHY = 'needs clang'

FRAME_LINE = '_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv'


def build(out):
    src = str(ROOT / 'tools' / 'dev' / 'framehooktest.c')
    if os.name == 'nt':
        exe = os.path.join(out, 'framehooktest.exe')
        return [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, src, '/Fe' + exe, '/Fo' + out + os.sep,
                '/link', *cc.LINK_OPTIONS], exe
    exe = os.path.join(out, 'framehooktest')
    rt = ROOT / 'runtime'
    return [POSIX_CLANG, '-O1', '-Wall', '-Werror', '-pthread', '-I', str(rt / 'macos' / 'include'), '-I', str(rt / 'linux' / 'include'),
            '-I', str(rt), src, '-o', exe], exe


@unittest.skipUnless(HAVE, WHY)
class FrameHookTests(unittest.TestCase):
    def test_checks(self):
        with tempfile.TemporaryDirectory() as out:
            cmd, exe = build(out)
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, 'framehooktest.c did not build:\n' + r.stdout + r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertRegex(r.stdout, r'^\d+ checks, 0 failed\n$')


class PlacementTests(unittest.TestCase):
    def test_one_wrap_each_here(self):
        text = (ROOT / 'runtime' / 'hle_each_frame.c').read_text(encoding='utf-8')
        self.assertEqual(re.findall(r'^HLE_WRAP\((\w+)\)', text, re.M), ['71038701e0'])
        self.assertEqual(re.findall(r'^HLE_WRAP_EXPORT\((\w+)\)', text, re.M), [FRAME_LINE])
        by_address, by_name = recomp.runtime_wraps()   # (which stops at a function wrapped twice)
        self.assertIn(0x71038701e0, by_address)
        self.assertIn(FRAME_LINE, by_name)

    def test_the_frame_line_before_the_fight_tests_names(self):
        """As it was in hle_fight_test.c, before the names wrapped there: their order is part of the
        tools fingerprint (tools/recomp.py tools_fingerprint), and a new one would have every game's
        generated C made again."""
        names = list(recomp.runtime_wraps()[1])
        self.assertLess(names.index(FRAME_LINE), names.index('_ZN2nn2os13GetSystemTickEv'))


if __name__ == '__main__':
    unittest.main()
