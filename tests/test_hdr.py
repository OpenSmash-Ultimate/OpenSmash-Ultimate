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


class HdrOnly(unittest.TestCase):
    """The launcher's HDR mode (hdr.set_only): HDR's mods the only ones on, the community plugins only HDR's,
    and every switch back as it was afterwards; installing from a package already downloaded."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        for n in ('My Skin', 'Moveset', 'Off Skin'):
            (self.root / 'mods' / n / 'fighter').mkdir(parents=True)
        self.tree = mods.Tree(str(self.root))
        state = self.tree.load()
        state['mods']['My Skin']['enabled'] = True
        state['mods']['Moveset']['enabled'] = True
        self.tree.save(state)
        plugins = self.root / 'skyline' / 'plugins'
        plugins.mkdir(parents=True)
        for p in ('libarcropolis.nro', 'libsmashline_plugin.nro', 'libthe_csk_collection.nro', 'libone_slot_eff.nro', 'libtraining_modpack.nro'):
            (plugins / p).write_bytes(b'NRO')

    def tearDown(self):
        self.tmp.cleanup()

    def package(self, top, plugin_dir='skyline/plugins'):
        """A made-up switch-package, unpacked at top (the layout of the user's extracted one)."""
        for m in hdr.MODS:
            (top / 'ultimate' / 'mods' / m / 'fighter').mkdir(parents=True)
            (top / 'ultimate' / 'mods' / m / 'fighter' / 'x.bin').write_bytes(m.encode())
        (top / 'ultimate' / 'mods' / 'hdr' / 'ui').mkdir()
        (top / 'ultimate' / 'mods' / 'hdr' / 'ui' / 'hdr_version.txt').write_text('v0.50.11-prerelease\n')
        (top / 'ultimate' / 'stage-alts').mkdir(parents=True)
        (top / 'ultimate' / 'stage-alts' / 'Hashes_all').write_bytes(b'H')
        d = top.joinpath(*plugin_dir.split('/'))
        d.mkdir(parents=True)
        for p in hdr.PLUGINS + ('hdr-launcher.nro', 'libarcropolis.nro', 'libnro_hook.nro'):
            (d / p).write_bytes(b'NRO')

    def enabled(self):
        return {n for n, m in self.tree.load()['mods'].items() if m.get('enabled')}

    def test_install_from_a_folder_one_level_down(self):
        self.package(self.root / 'dl' / 'switch-package')
        st = hdr.install(self.tree, str(self.root / 'dl'), lambda s: None)
        self.assertEqual(st['installed'], 'v0.50.11-prerelease')
        self.assertTrue((self.root / 'mods' / 'hdr-assets' / 'fighter' / 'x.bin').is_file())
        self.assertTrue((self.root / 'dump' / 'skyline' / 'sd' / 'ultimate' / 'stage-alts' / 'Hashes_all').is_file())
        self.assertEqual(sorted(os.listdir(self.root / 'dump' / 'hdr' / 'plugins')), sorted(hdr.PLUGINS))   # (not its launcher, ARCropolis, nro_hook)
        self.assertTrue((self.root / 'dl' / 'switch-package' / 'ultimate' / 'mods' / 'hdr').is_dir())   # (copied, the source kept)
        self.assertFalse(st['on'])

    def test_install_from_the_zip_in_the_console_layout(self):
        self.package(self.root / 'pkg', hdr.PLUGIN_DIR.strip('/'))
        z = self.root / 'switch-package.zip'
        shutil.make_archive(str(z.with_suffix('')), 'zip', self.root / 'pkg')
        st = hdr.install(self.tree, str(z), lambda s: None)
        self.assertEqual(st['installed'], 'v0.50.11-prerelease')
        self.assertEqual(sorted(os.listdir(self.root / 'dump' / 'hdr' / 'plugins')), sorted(hdr.PLUGINS))

    def test_not_a_package(self):
        (self.root / 'empty').mkdir()
        with self.assertRaises(hdr.Error):
            hdr.install(self.tree, str(self.root / 'empty'), lambda s: None)

    def test_only_and_back(self):
        import skyline
        self.package(self.root / 'pkg')
        hdr.install(self.tree, str(self.root / 'pkg'), lambda s: None)
        before = self.enabled()
        self.assertTrue(hdr.set_only(self.tree, True, lambda s: None))
        self.assertEqual(self.enabled(), set(hdr.MODS))
        self.assertTrue(hdr.status(self.tree)['only'])
        # the community plugins that load: ARCropolis, Smashline and HDR's stage plugins, not the CSK Collection, One Slot
        # Effects or the Training Modpack (still in the folder: built into the game, not listed)
        loaded = sorted(os.path.basename(f) for _n, f in skyline.standard(str(self.root)))
        self.assertEqual(loaded, sorted(['libarcropolis.nro', 'libsmashline_plugin.nro'] + list(hdr.PLUGINS)))
        self.assertTrue((self.root / 'skyline' / 'plugins' / 'libtraining_modpack.nro').exists())
        # nothing turns another mod on meanwhile (the Mods app, the game's menu: they save the switch, load keeps it off)
        state = self.tree.load()
        state['mods']['Moveset']['enabled'] = True
        self.tree.save(state)
        self.assertNotIn('Moveset', self.enabled())
        self.assertFalse(hdr.set_only(self.tree, True, lambda s: None))   # (already: nothing to apply)
        self.assertTrue(hdr.set_only(self.tree, False, lambda s: None))
        self.assertEqual(self.enabled(), before)
        self.assertFalse(hdr.status(self.tree)['on'])
        self.assertEqual(len(skyline.standard(str(self.root))), 5)

    def test_only_keeps_hdr_on_after_if_it_was(self):
        self.package(self.root / 'pkg')
        hdr.install(self.tree, str(self.root / 'pkg'), lambda s: None)
        hdr.set_on(self.tree, True, lambda s: None)   # (HDR with the user's mods, from the Mods app)
        hdr.set_only(self.tree, True, lambda s: None)
        hdr.set_only(self.tree, False, lambda s: None)
        self.assertEqual(self.enabled(), {'My Skin', 'Moveset'} | set(hdr.MODS))

    def test_only_needs_hdr(self):
        with self.assertRaises(hdr.Error):
            hdr.set_only(self.tree, True, lambda s: None)
        self.assertFalse(hdr.set_only(self.tree, False, lambda s: None))

    def test_os_mods_refuses_another_mod_in_hdr_mode(self):
        import os_mods
        self.package(self.root / 'pkg')
        hdr.install(self.tree, str(self.root / 'pkg'), lambda s: None)
        hdr.set_only(self.tree, True, lambda s: None)
        with self.assertRaises(mods.Error):
            os_mods.set_switches(self.tree, ['Moveset'], True)
        os_mods.set_switches(self.tree, ['hdr'], True)   # (HDR's own: fine)


if __name__ == '__main__':
    unittest.main()
