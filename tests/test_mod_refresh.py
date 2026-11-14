"""Compile the real mod refresh handlers; exercise opt-in, mode checks and controller edges."""
import os
import subprocess
import sys
import struct
from pathlib import Path
import tempfile
import unittest

from test_frame_hooks import build, HAVE, WHY, ROOT


@unittest.skipUnless(HAVE, WHY)
class ModRefreshTests(unittest.TestCase):
    def test_policy(self):
        with tempfile.TemporaryDirectory() as out:
            cmd, exe = build(out)
            cmd = [part.replace('framehooktest', 'modrefreshtest') for part in cmd]
            if os.name != 'nt':   # (its file and thread calls: the platform layer, as the game's own build has)
                layer = ['runtime/linux/win32.c'] + (['runtime/macos/memory.c', 'runtime/macos/platform.c']
                                                     if sys.platform == 'darwin' else ['-lpthread'])
                cmd = [part for part in cmd if part not in ('-Wall', '-Werror')]   # (the layer is built with -w elsewhere)
                cmd[-2:-2] = ['-w', *(str(ROOT / f) if f.endswith('.c') else f for f in layer), '-lm']
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
            sys.path.insert(0, str(ROOT / 'ssbu-recomp/fighter_sdk/tools/brawl'))
            import ssbh
            def material(vector, second):
                return ssbh.build(b'LTAM', {'entries': [{'label': 'goku_test', 'shader_label': 'SFX_PBS_010000080800826b_opaque',
                    'attributes': [{'param_id': 160, 'param': (5, struct.pack('<4f', *vector))},
                                   {'param_id': 161, 'param': (5, struct.pack('<4f', *second))},
                                   {'param_id': 162, 'param': (5, struct.pack('<4f', 1, 2, 3, 4))}]},
                    # Unused entries in the same file must not block visible materials.
                    {'label': 'unused_costume', 'shader_label': 'SFX_PBS_010000080800826b_opaque',
                     'attributes': [{'param_id': 160, 'param': (5, struct.pack('<4f', *vector))}]}]})
            old, new = Path(out) / 'old.numatb', Path(out) / 'new.numatb'
            old.write_bytes(material((1, 1, 1, 1), (0, 0, 0, 0)))
            new.write_bytes(material((.2, .3, .4, 1), (.4, .5, .6, 1)) + b'\0' * 4096)
            result = subprocess.run([exe, '--materials', str(old), str(new)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS live material updates', result.stdout)
