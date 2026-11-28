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


class HdrMotion(unittest.TestCase):
    """tools/hdr_motion.py: HDR's motion_patch.yaml applied to the game's motion lists ahead of time (on the
    console its plugin does it as the lists load; here that never ran, and HDR's new moves T-posed)."""
    PATCH = """\
# Ridley
attack_air_lw:
  extra:
    cancel_frame: 40   # (a comment)
special_air_lw_pogo:
  game_script: game_specialairlwpogo
  flags:
    loop: false
    unk_80: true
  blend_frames: 0
  animations:
  - name: d03specialairlwpogo.nuanmb
  scripts:
  - expression_specialairlwpogo
  - sound_specialairlwpogo
  extra:
    intangible_start_frame: 0
    intangible_end_frame: 0
    cancel_frame: 70
    freeze_during_hitstop: false
wait:
  flags:
    loop: true
attack_air_lw:
  extra:
    cancel_frame: 41
"""

    def setUp(self):
        import hdr_motion
        self.hm = hdr_motion
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'mods' / 'My Skin' / 'fighter').mkdir(parents=True)
        self.tree = mods.Tree(str(self.root))
        for m in hdr.MODS:
            (self.root / 'mods' / m / 'fighter').mkdir(parents=True)
        body = self.root / 'mods' / 'hdr' / 'fighter' / 'ridley' / 'motion' / 'body'
        body.mkdir(parents=True)
        (body / 'motion_patch.yaml').write_text(self.PATCH)
        anims = self.root / 'mods' / 'hdr-assets' / 'fighter' / 'ridley' / 'motion' / 'body' / 'c00'
        anims.mkdir(parents=True)
        (anims / 'd03specialairlwpogo.nuanmb').write_bytes(b'ANIM')
        keep = self.root / 'dump' / 'hdr' / 'plugins'
        keep.mkdir(parents=True)
        for p in hdr.PLUGINS:
            (keep / p).write_bytes(b'NRO')
        (self.root / 'dump' / 'hdr' / 'installed.json').write_text(json.dumps({'version': hdr.VERSION}))
        h = hdr_motion.hash40
        m = hdr_motion.default_motion
        wait = dict(m(), game_script=h('game_wait'), animations=[(h('a00wait1.nuanmb'), 0)])
        air = dict(m(), game_script=h('game_attackairlw'), flags=0x80, animations=[(h('c05attackairlw.nuanmb'), 1)],
                   scripts=[h('effect_attackairlw'), h('sound_attackairlw')], extra=(0, 0, 54, False))
        self.vanilla = hdr_motion.write(h('fighter/ridley/motion/body'), {h('wait'): wait, h('attack_air_lw'): air})
        self.asked = []

        def get(path):
            self.asked.append(path)
            return self.vanilla if path == 'fighter/ridley/motion/body/c00/motion_list.bin' else None
        self.games = mock.patch.object(hdr_motion, 'game_files', lambda tree: get)
        self.games.start()

    def tearDown(self):
        self.games.stop()
        self.tmp.cleanup()

    def enabled(self):
        return {n for n, m in self.tree.load()['mods'].items() if m.get('enabled')}

    def test_the_file_format_round_trips(self):
        path, motions = self.hm.read(self.vanilla)
        self.assertEqual(self.hm.write(path, motions), self.vanilla)

    def test_the_patch_as_the_console_applies_it(self):
        h = self.hm.hash40
        _path, motions = self.hm.read(self.vanilla)
        new = self.hm.apply(motions, self.hm.load_patch(self.PATCH))
        self.assertEqual(list(new), sorted(new))   # (sorted by hash40, as motion-patch leaves it)
        pogo = new[h('special_air_lw_pogo')]   # (a new move: its animation, scripts and frame data)
        self.assertEqual(pogo['game_script'], h('game_specialairlwpogo'))
        self.assertEqual(pogo['animations'], [(h('d03specialairlwpogo.nuanmb'), 0)])
        self.assertEqual(pogo['scripts'], [h('expression_specialairlwpogo'), h('sound_specialairlwpogo')])
        self.assertEqual(pogo['flags'], 0x80)
        self.assertEqual(pogo['extra'], (0, 0, 70, False))
        air = new[h('attack_air_lw')]   # (a key twice: the last one; the rest of the motion as it was)
        self.assertEqual(air['extra'], (0, 0, 41, False))
        self.assertEqual(air['animations'], [(h('c05attackairlw.nuanmb'), 1)])
        self.assertEqual(new[h('wait')]['flags'], 0x2)   # (one flag changed, the others kept)

    def test_remove_leaves_a_default_motion_as_motion_patch_does(self):
        h = self.hm.hash40
        _path, motions = self.hm.read(self.vanilla)
        new = self.hm.apply(motions, self.hm.load_patch("wait:\n  remove: true\n"))
        self.assertEqual(new[h('wait')], self.hm.default_motion())

    def test_hdr_mode_writes_and_turns_on_the_motion_lists(self):
        self.assertTrue(hdr.set_only(self.tree, True, lambda s: None))
        lists = self.root / 'mods' / 'hdr-motion' / 'fighter' / 'ridley' / 'motion' / 'body'
        self.assertEqual(sorted(os.listdir(lists)), [f'c{i:02d}' for i in range(8)])
        _p, motions = self.hm.read((lists / 'c03' / 'motion_list.bin').read_bytes())
        self.assertIn(self.hm.hash40('special_air_lw_pogo'), motions)
        self.assertEqual(self.enabled(), set(hdr.MODS) | {'hdr-motion'})   # (HDR only allows it too)
        self.assertEqual(self.tree.load()['order'][:4], list(hdr.MODS) + ['hdr-motion'])   # (after HDR's: it wins)
        self.assertFalse(hdr.bake_motion(self.tree, lambda s: None))   # (as it is: not written again)
        n = len(self.asked)
        y = self.root / 'mods' / 'hdr' / 'fighter' / 'ridley' / 'motion' / 'body' / 'motion_patch.yaml'
        y.write_text(self.PATCH + "fall:\n  blend_frames: 3\n")
        self.assertTrue(hdr.bake_motion(self.tree, lambda s: None))   # (HDR updated: again)
        self.assertGreater(len(self.asked), n)
        self.assertTrue(hdr.set_only(self.tree, False, lambda s: None))
        self.assertNotIn('hdr-motion', self.enabled())   # (Play: off with HDR's mods)

    def test_already_in_hdr_mode_before_this(self):
        """HDR mode switched on by an OpenSmash without hdr_motion.py: hdr-only on writes the lists and allows them."""
        import os_mods
        state = self.tree.load()
        state['hdr_only'] = {'enabled': {}, 'hdr_was_on': False, 'mods': list(hdr.MODS), 'plugins': list(hdr.ONLY_PLUGINS)}
        for m in hdr.MODS:
            state['mods'][m]['enabled'] = True
        self.tree.save(state)
        with mock.patch.object(mods, 'apply') as applied, mock.patch.object(os_mods, 'ROOT', str(self.root)), \
                mock.patch.object(sys, 'stdout'):
            os_mods.main(['os_mods.py', 'hdr-only', 'on'])
        self.assertTrue(applied.called)
        self.assertIn('hdr-motion', self.enabled())


if __name__ == '__main__':
    unittest.main()
