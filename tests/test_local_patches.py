"""tools/recomp.py's local_patches: dump/patches/<module>.txt read as (offset, old, new), main addresses made
offsets, comments and other lines passed over, and the result in plugin_patches (so a module's C is generated
again when they change: module_fingerprint)."""
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import recomp  # noqa: E402


class LocalPatches(unittest.TestCase):
    def test_read(self):
        with tempfile.TemporaryDirectory() as tree:
            os.makedirs(os.path.join(tree, 'dump', 'patches'))
            with open(os.path.join(tree, 'dump', 'patches', 'main.txt'), 'w') as f:
                f.write('# made up\n710034b0c0 71017a9f 7101ca9f   # bound\n\n1234 aaaaaaaa bbbbbbbb\nnot a patch\n')
            with mock.patch.object(recomp, 'TREE', tree):
                self.assertEqual(recomp.local_patches('main'), [(0x34b0c0, 0x71017a9f, 0x7101ca9f), (0x1234, 0xaaaaaaaa, 0xbbbbbbbb)])
                self.assertEqual(recomp.local_patches('sdk'), [])
                self.assertEqual(recomp.plugin_patches('main')[-1], (0x1234, 0xaaaaaaaa, 0xbbbbbbbb))


if __name__ == '__main__':
    unittest.main()
