"""A match's report for a lobby's count of matches played, as runtime/net_results.c writes it (its id, the data
tools/nex/matches.py parses, the "mastery" object of OpenSmash mastery points and its switch). tools/dev/resultstest.c holds the cases. (Before OpenSmash 0.5.0 this was
test_eos_paths.py, with Epic Online Services' path choice: gone with EOS.)"""
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
class MatchReport(unittest.TestCase):
    def test_cases(self):
        d = tempfile.mkdtemp()
        try:
            exe = os.path.join(d, 'resultstest.exe')
            r = subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, str(ROOT / 'tools' / 'dev' / 'resultstest.c'),
                                str(ROOT / 'runtime' / 'net_results.c'), str(ROOT / 'runtime' / 'net_discord_core.c'),
                                '/Fe' + exe, '/Fo' + d + os.sep, '/link', *cc.LINK_OPTIONS], capture_output=True, text=True, cwd=d)
            self.assertEqual(r.returncode, 0, 'resultstest.c did not build:\n' + r.stdout + r.stderr)
            env = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
            r = subprocess.run([exe], capture_output=True, text=True, timeout=60, cwd=d, env=env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn(' 0 failed', r.stdout)
            # SSBU_MASTERY_REPORT=0: the report as before (OpenSmash mastery points off)
            r = subprocess.run([exe, 'off'], capture_output=True, text=True, timeout=60, cwd=d, env={**env, 'SSBU_MASTERY_REPORT': '0'})
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn(' 0 failed', r.stdout)
        finally:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
