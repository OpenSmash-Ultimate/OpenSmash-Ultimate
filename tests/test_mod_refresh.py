"""Compile the real mod refresh handlers; exercise opt-in, mode checks and controller edges."""
import subprocess
import tempfile
import unittest

from test_frame_hooks import build, HAVE, WHY


@unittest.skipUnless(HAVE, WHY)
class ModRefreshTests(unittest.TestCase):
    def test_policy(self):
        with tempfile.TemporaryDirectory() as out:
            cmd, exe = build(out)
            cmd = [part.replace('framehooktest', 'modrefreshtest') for part in cmd]
            exe = exe.replace('framehooktest', 'modrefreshtest')
            result = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for preference in (None, '0', 'garbage', '1'):
                with self.subTest(preference=preference):
                    result = subprocess.run([exe] + ([] if preference is None else [preference]),
                                            capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn('PASS mod refresh policy', result.stdout)
                    self.assertEqual(result.stderr.count('[mods]'), 1 if preference == '1' else 0,
                                     result.stderr)
