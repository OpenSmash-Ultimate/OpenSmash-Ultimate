"""tools/hdr.py, HewDraw Remix as a switch: installed or not, turned on (its mods first in the order,
its stage plugins among the standard ones) and off (they go), removed; the download refused when
the disk hasn't room. On made-up trees, without the real package."""
from pathlib import Path
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import hdr  # noqa: E402
import mods  # noqa: E402


class HdrSwitch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'mods' / 'My Skin' / 'fighter').mkdir(parents=True)
        self.tree = mods.Tree(str(self.root))
        state = self.tree.load()
        state['mods']['My Skin']['enabled'] = True   # (a folder just found starts off: the user turned it on)
        self.tree.save(state)

    def tearDown(self):
        self.tmp.cleanup()

    def install(self):
        """What download leaves: the three mods, the kept plugins and installed.json."""
        for m in hdr.MODS:
            (self.root / 'mods' / m / 'fighter').mkdir(parents=True)
        keep = self.root / 'dump' / 'hdr' / 'plugins'
        keep.mkdir(parents=True)
        for p in hdr.PLUGINS:
            (keep / p).write_bytes(b'NRO')
        (self.root / 'dump' / 'hdr' / 'installed.json').write_text(json.dumps({'version': hdr.VERSION}))

    def plugins(self):
        d = self.root / 'skyline' / 'plugins'
        return sorted(os.listdir(d)) if d.is_dir() else []

    def test_not_installed(self):
        st = hdr.status(self.tree)
        self.assertIsNone(st['installed'])
        self.assertFalse(st['on'])
        with self.assertRaises(hdr.Error):
            hdr.set_on(self.tree, True, lambda s: None)

    def test_on_puts_hdr_first_and_its_plugins_in(self):
        self.install()
        st = hdr.set_on(self.tree, True, lambda s: None)
        self.assertTrue(st['on'])
        state = self.tree.load()
        self.assertEqual(state['order'][:3], list(hdr.MODS))
        self.assertTrue(state['mods']['My Skin']['enabled'])   # (the user's own stay as they are)
        self.assertEqual(self.plugins(), sorted(hdr.PLUGINS))

    def test_off_takes_them_out_and_keeps_the_rest(self):
        self.install()
        hdr.set_on(self.tree, True, lambda s: None)
        (self.root / 'skyline' / 'plugins' / 'libarcropolis.nro').write_bytes(b'NRO')
        st = hdr.set_on(self.tree, False, lambda s: None)
        self.assertFalse(st['on'])
        self.assertEqual(self.plugins(), ['libarcropolis.nro'])
        state = self.tree.load()
        self.assertFalse(any(state['mods'][m]['enabled'] for m in hdr.MODS))
        self.assertTrue(state['mods']['My Skin']['enabled'])

    def test_remove(self):
        self.install()
        hdr.set_on(self.tree, True, lambda s: None)
        st = hdr.remove(self.tree, lambda s: None)
        self.assertIsNone(st['installed'])
        self.assertFalse(any((self.root / 'mods' / m).exists() for m in hdr.MODS))
        self.assertEqual(self.plugins(), [])
        self.assertEqual(list(self.tree.load()['mods']), ['My Skin'])

    def test_no_room_for_the_download(self):
        full = shutil._ntuple_diskusage(100, 100, 1 << 20)
        with mock.patch.object(hdr.shutil, 'disk_usage', return_value=full), mock.patch.object(hdr, 'fetch') as fetch:
            with self.assertRaises(hdr.Error) as e:
                hdr.download(self.tree, None, lambda s: None)
        fetch.assert_not_called()
        self.assertIn('disk space', str(e.exception))


if __name__ == '__main__':
    unittest.main()
