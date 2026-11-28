"""tools/hdr.py, HewDraw Remix as a switch: installed or not, turned on (its mods first in the order,
its stage plugins among the standard ones) and off (they go), removed; the download refused when
the disk hasn't room. On made-up trees, without the real package."""
from pathlib import Path
import io
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


def arc_config(root, shown=('My Skin', 'Moveset'), workspace='Default'):
    """ARCropolis' settings as it leaves them after a start (one user's folder), and the mods.txt it was shown."""
    import arc_workspace as aw
    d = root / 'dump' / 'skyline' / 'sd' / 'ultimate' / 'arcropolis' / 'config' / '1' / '2'
    d.mkdir(parents=True, exist_ok=True)
    (d / 'version').write_text('"4.1.1"')
    (d / 'workspace').write_text(workspace)
    (d / 'workspace_list').write_text('{"Default":"presets"}')
    (d / 'presets').write_text(json.dumps(sorted(aw.mod_hash(n) for n in shown)))
    (d / 'mod_cache').write_text(json.dumps(sorted(aw.mod_hash(n) for n in shown)))
    (root / 'dump' / 'skyline' / 'mods.txt').write_text(''.join(n + '\n' for n in shown))
    return d


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

    def test_off_turns_its_mods_off_and_keeps_the_rest(self):
        self.install()
        hdr.set_on(self.tree, True, lambda s: None)
        (self.root / 'skyline' / 'plugins' / 'libarcropolis.nro').write_bytes(b'NRO')
        st = hdr.set_on(self.tree, False, lambda s: None)
        self.assertFalse(st['on'])
        # (its stage plugins stay while it is installed: the plugins the game is built with don't change with it)
        self.assertEqual(self.plugins(), sorted(['libarcropolis.nro'] + list(hdr.PLUGINS)))
        state = self.tree.load()
        self.assertFalse(any(state['mods'][m]['enabled'] for m in hdr.MODS))
        self.assertTrue(state['mods']['My Skin']['enabled'])

    def test_switched_on_elsewhere_stays_off_in_play(self):
        """HDR's folders switched on by another launcher's resync (not hdr.py on): off again, so Play has no HDR and
        its select screen is not hidden in dump/skyline/hidden.txt."""
        self.install()
        state = self.tree.load()
        for m in hdr.MODS:
            state['mods'][m]['enabled'] = True
        self.tree.save(state)
        state = self.tree.load()
        self.assertFalse(any(state['mods'][m]['enabled'] for m in hdr.MODS))
        self.assertTrue(state['mods']['My Skin']['enabled'])
        with open(self.tree.state_path, encoding='utf-8') as f:   # (written back: a launcher reading the file sees it off)
            saved = json.load(f)
        self.assertFalse(any(saved['mods'][m]['enabled'] for m in hdr.MODS))
        hdr.set_on(self.tree, True, lambda s: None)   # (asked for: HDR with the user's mods)
        self.assertTrue(all(self.tree.load()['mods'][m]['enabled'] for m in hdr.MODS))
        hdr.set_on(self.tree, False, lambda s: None)
        self.assertNotIn('hdr_in_play', self.tree.load())

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
        with mock.patch.object(hdr.shutil, 'disk_usage', return_value=full), mock.patch.object(hdr, 'fetch') as fetch, \
                mock.patch.object(hdr, 'releases', side_effect=hdr.Error('offline')):
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
        import arc_workspace as aw
        import skyline
        self.package(self.root / 'pkg')
        hdr.install(self.tree, str(self.root / 'pkg'), lambda s: None)
        d = arc_config(self.root)
        before = self.enabled()
        self.assertTrue(hdr.set_only(self.tree, True, lambda s: None))   # (the first time: HDR not shown to ARCropolis yet)
        self.assertEqual(self.enabled(), before)   # (every switch stays)
        self.assertTrue(hdr.status(self.tree)['only'])
        # every plugin: HDR's stage plugins among them, and its nro_hook (OpenSmash had none); its ARCropolis the same as ours
        self.assertEqual(len(skyline.standard(str(self.root))), 8)
        with mock.patch.object(skyline, 'cache_mod_plugins'), mock.patch.object(skyline, 'generate'), \
                mock.patch.object(skyline, 'module_of', lambda p, src, r, pr: (os.path.basename(src),) * 2):
            skyline.sync(str(self.root), lambda s: None)   # (what the apply does: mods.txt, then the workspaces)
        shown = (self.root / 'dump' / 'skyline' / 'mods.txt').read_text().split()
        self.assertEqual([x for x in shown if x.startswith('hdr')], list(hdr.MODS))
        aw.activate(str(self.root), True)
        self.assertEqual((d / 'workspace').read_text(), 'HDR')
        self.assertEqual(json.loads((d / 'workspace_list').read_text()), {'Default': 'presets', 'HDR': 'hdr_preset'})
        self.assertEqual(set(json.loads((d / 'hdr_preset').read_text())), {aw.mod_hash(m) for m in hdr.MODS})
        self.assertEqual(set(json.loads((d / 'presets').read_text())), {aw.mod_hash(n) for n in ('My Skin', 'Moveset')})
        self.assertTrue({aw.mod_hash(m) for m in hdr.MODS} <= set(json.loads((d / 'mod_cache').read_text())))
        # a mod turned on meanwhile: its switch saved, in the Play workspace, not HDR's
        state = self.tree.load()
        state['mods']['Off Skin']['enabled'] = True
        self.tree.save(state)
        with mock.patch.object(skyline, 'cache_mod_plugins'), mock.patch.object(skyline, 'generate'), \
                mock.patch.object(skyline, 'module_of', lambda p, src, r, pr: (os.path.basename(src),) * 2):
            skyline.sync(str(self.root), lambda s: None)
        self.assertIn(aw.mod_hash('Off Skin'), json.loads((d / 'presets').read_text()))
        self.assertNotIn(aw.mod_hash('Off Skin'), json.loads((d / 'hdr_preset').read_text()))
        self.assertFalse(hdr.set_only(self.tree, True, lambda s: None))   # (already: nothing to apply)
        applied = json.loads((self.root / 'dump' / 'mods' / 'applied.json').read_text())
        self.assertEqual(applied['mods'], list(hdr.MODS))   # (online: HDR's set)
        self.assertFalse(hdr.set_only(self.tree, False, lambda s: None))   # (no apply: the workspace switched)
        self.assertEqual((d / 'workspace').read_text(), 'Default')
        self.assertFalse(hdr.status(self.tree)['only'])
        applied = json.loads((self.root / 'dump' / 'mods' / 'applied.json').read_text())
        self.assertEqual(set(applied['mods']), {'My Skin', 'Moveset', 'Off Skin'})

    def test_the_mode_from_before_the_workspaces_gives_the_switches_back(self):
        arc_config(self.root)
        self.package(self.root / 'pkg')
        hdr.install(self.tree, str(self.root / 'pkg'), lambda s: None)
        state = self.tree.load()
        state['hdr_only'] = {'enabled': {'My Skin': True, 'Moveset': True}, 'hdr_was_on': False, 'mods': list(hdr.MODS),
                             'plugins': ['libarcropolis.nro']}
        for n, m in state['mods'].items():
            m['enabled'] = n in hdr.MODS
        self.tree.save(state)
        self.assertTrue(hdr.set_only(self.tree, True, lambda s: None))
        self.assertEqual(self.enabled(), {'My Skin', 'Moveset'})
        self.assertTrue(self.tree.load()['hdr_only']['workspace'])

    def test_only_keeps_hdr_on_after_if_it_was(self):
        arc_config(self.root)
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

    def test_only_needs_arcropolis_settings(self):
        """No settings of ARCropolis' yet (the game never started with it): no workspace to switch, so not at all."""
        self.package(self.root / 'pkg')
        hdr.install(self.tree, str(self.root / 'pkg'), lambda s: None)
        with self.assertRaises(hdr.Error):
            hdr.set_only(self.tree, True, lambda s: None)
        self.assertFalse(hdr.status(self.tree)['only'])
        self.assertFalse(hdr.set_only(self.tree, False, lambda s: None))

    def test_hdr_mode_leaves_the_mods_own_plugins_out(self):
        """A mod's plugin with a name of its own runs whatever ARCropolis' workspace says (Mother Smash FX's: HDR's
        matches crashed in Smashline): in HDR mode the game's list has only the standard plugins and HDR's."""
        import skyline
        arc_config(self.root)
        self.package(self.root / 'pkg')
        hdr.install(self.tree, str(self.root / 'pkg'), lambda s: None)
        sky = self.root / 'dump' / 'skyline'
        (sky / 'plugins_all.txt').write_text('arcropolis\t(standard) libarcropolis.nro\nsmashline\t(standard) libsmashline_plugin.nro\n'
                                             'training_modpack\t(standard) libtraining_modpack.nro\n'
                                             'one_slot_eff\t(standard) libone_slot_eff.nro\nmother_smash_fx\tMother Smash FX\n')
        (sky / 'plugins.txt').write_text('arcropolis\nsmashline\ntraining_modpack\none_slot_eff\nmother_smash_fx\n')
        (self.root / 'dump' / 'hdr' / 'plugins_extra.txt').write_text('one_slot_eff  # picked for HDR mode\n')
        hdr.set_only(self.tree, True, lambda s: None)
        self.assertEqual((sky / 'plugins.txt').read_text().split(), ['arcropolis', 'smashline', 'one_slot_eff'])
        hdr.set_only(self.tree, False, lambda s: None)
        self.assertEqual((sky / 'plugins.txt').read_text().split(),
                         ['arcropolis', 'smashline', 'training_modpack', 'one_slot_eff', 'mother_smash_fx'])
        # Play: HDR's stage plugins out (online, the other player's game has none), unless HDR's switch is on
        (sky / 'plugins_all.txt').write_text('arcropolis\t(standard) libarcropolis.nro\nstage_alts\t(standard) libstage_alts.nro\n'
                                             'stage_config\t(standard) libstage_config.nro\n')
        skyline.write_list(str(self.root), lambda s: None)
        self.assertEqual((sky / 'plugins.txt').read_text().split(), ['arcropolis'])
        hdr.set_on(self.tree, True, lambda s: None)
        skyline.write_list(str(self.root), lambda s: None)
        self.assertEqual((sky / 'plugins.txt').read_text().split(), ['arcropolis', 'stage_alts', 'stage_config'])

    def test_os_mods_switches_play_mods_in_hdr_mode(self):
        import os_mods
        arc_config(self.root)
        self.package(self.root / 'pkg')
        hdr.install(self.tree, str(self.root / 'pkg'), lambda s: None)
        hdr.set_only(self.tree, True, lambda s: None)
        os_mods.set_switches(self.tree, ['Off Skin'], True)   # (Play's set: HDR's workspace doesn't have it)
        self.assertIn('Off Skin', self.enabled())

    def test_a_newer_standard_plugin_in_the_package_replaces_opensmash_s(self):
        self.package(self.root / 'pkg')
        d = self.root / 'pkg' / 'skyline' / 'plugins'
        (d / 'libarcropolis.nro').write_bytes(b'\0ARCropolis v4.2.0\0')
        (d / 'libsmashline_plugin.nro').write_bytes(b'\0smashline 1.9.0\0')
        std = self.root / 'skyline' / 'plugins'
        (std / 'libarcropolis.nro').write_bytes(b'\0ARCropolis v4.1.1\0')
        (std / 'libsmashline_plugin.nro').write_bytes(b'\0smashline 2.0.0\0')
        hdr.install(self.tree, str(self.root / 'pkg'), lambda s: None)
        self.assertEqual((std / 'libarcropolis.nro').read_bytes(), b'\0ARCropolis v4.2.0\0')   # (newer: HDR's)
        self.assertEqual((std / 'libsmashline_plugin.nro').read_bytes(), b'\0smashline 2.0.0\0')   # (older: ours)
        hdr.remove(self.tree, lambda s: None)
        self.assertEqual((std / 'libarcropolis.nro').read_bytes(), b'\0ARCropolis v4.1.1\0')   # (ours back)


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
        arc_config(self.root, ('My Skin',))
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
        self.assertEqual(self.tree.load()['order'][:4], list(hdr.MODS) + ['hdr-motion'])   # (after HDR's: it wins)
        self.assertIn('hdr-motion', mods.hdr_seen(self.tree))   # (shown to ARCropolis with HDR's, in its workspace)
        self.assertFalse(hdr.bake_motion(self.tree, lambda s: None))   # (as it is: not written again)
        n = len(self.asked)
        y = self.root / 'mods' / 'hdr' / 'fighter' / 'ridley' / 'motion' / 'body' / 'motion_patch.yaml'
        y.write_text(self.PATCH + "fall:\n  blend_frames: 3\n")
        self.assertTrue(hdr.bake_motion(self.tree, lambda s: None))   # (HDR updated: again)
        self.assertGreater(len(self.asked), n)
        self.assertFalse(hdr.set_only(self.tree, False, lambda s: None))
        self.assertNotIn('hdr-motion', self.enabled())   # (Play: off with HDR's mods)

    def test_already_in_hdr_mode_before_this(self):
        """HDR mode switched on by an OpenSmash without hdr_motion.py: hdr-only on writes the lists and allows them."""
        import os_mods
        state = self.tree.load()
        state['hdr_only'] = {'enabled': {}, 'hdr_was_on': False, 'mods': list(hdr.MODS), 'plugins': ['libarcropolis.nro']}
        for m in hdr.MODS:
            state['mods'][m]['enabled'] = True
        self.tree.save(state)
        with mock.patch.object(mods, 'apply') as applied, mock.patch.object(os_mods, 'ROOT', str(self.root)), \
                mock.patch.object(sys, 'stdout'):
            os_mods.main(['os_mods.py', 'hdr-only', 'on'])
        self.assertTrue(applied.called)
        self.assertIn('hdr-motion', mods.hdr_seen(self.tree))
        self.assertTrue(self.tree.load()['hdr_only']['workspace'])


if __name__ == '__main__':
    unittest.main()


class ArcWorkspace(unittest.TestCase):
    """tools/arc_workspace.py: the hashes as smash-arc's hash40, and the folder names as the runtime shows them."""
    def test_hash40(self):
        import arc_workspace as aw
        self.assertEqual(aw.hash40('motion'), (6 << 32) | __import__('zlib').crc32(b'motion'))
        self.assertEqual(aw.flat('Pack/(Moveset) X'), 'Pack - (Moveset) X')
        self.assertEqual(aw.flat('.hdr-shares'), 'WindowSmash hdr-shares')

    def test_hands_off_a_cache_it_cannot_read(self):
        import arc_workspace as aw
        with tempfile.TemporaryDirectory() as t:
            d = Path(t) / 'dump' / 'skyline' / 'sd' / 'ultimate' / 'arcropolis' / 'config' / '1' / '2'
            d.mkdir(parents=True)
            (d / 'version').write_text('x')
            (d / 'workspace_list').write_text('{"Default":"presets"}')
            (d / 'mod_cache').write_text('[1, 2, 3]')
            self.assertEqual(aw.prepare(t, ['hdr'], ['hdr', 'My Skin'], False, lambda s: None), [])
            self.assertFalse((d / 'hdr_preset').exists())

    def test_the_spelling_the_cache_holds_is_used(self):
        import arc_workspace as aw
        form = aw.FORMS[1]
        with tempfile.TemporaryDirectory() as t:
            d = Path(t) / 'dump' / 'skyline' / 'sd' / 'ultimate' / 'arcropolis' / 'config' / '1' / '2'
            d.mkdir(parents=True)
            (d / 'version').write_text('x')
            (d / 'workspace').write_text('Default')
            (d / 'workspace_list').write_text('{"Default":"presets"}')
            (d / 'presets').write_text(json.dumps([aw.mod_hash('My Skin', form), aw.mod_hash('hdr', form)]))
            (d / 'mod_cache').write_text(json.dumps([aw.mod_hash('My Skin', form)]))
            self.assertTrue(aw.ready(t, ['My Skin']))
            self.assertEqual(len(aw.prepare(t, ['hdr'], ['hdr', 'My Skin', 'New'], False, lambda s: None)), 1)
            self.assertEqual(json.loads((d / 'hdr_preset').read_text()), [aw.mod_hash('hdr', form)])
            self.assertEqual(set(json.loads((d / 'presets').read_text())), {aw.mod_hash(n, form) for n in ('My Skin', 'New')})
            self.assertTrue(aw.activate(t, True))
            self.assertEqual((d / 'workspace').read_text(), 'HDR')
            self.assertTrue(aw.activate(t, False))
            self.assertEqual((d / 'workspace').read_text(), 'Default')


class HdrUpdate(unittest.TestCase):
    """hdr.check / hdr.update: a newer prerelease on HDR's releases page, its upgrade package (to-beta.zip) over the
    release before it, the whole package otherwise. The page and the downloads are made up."""
    def setUp(self):
        import zipfile
        self.zipfile = zipfile
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.tree = mods.Tree(str(self.root))
        for m in hdr.MODS:
            (self.root / 'mods' / m / 'fighter').mkdir(parents=True)
            (self.root / 'mods' / m / 'fighter' / 'x.bin').write_bytes(b'old')
        (self.root / 'mods' / 'hdr' / 'fighter' / 'gone.bin').write_bytes(b'old')
        keep = self.root / 'dump' / 'hdr' / 'plugins'
        keep.mkdir(parents=True)
        for p in hdr.PLUGINS:
            (keep / p).write_bytes(b'NRO')
        (self.root / 'dump' / 'hdr' / 'installed.json').write_text(json.dumps({'version': 'v0.50.10-prerelease'}))
        self.zips = {}

    def tearDown(self):
        self.tmp.cleanup()

    def delta(self, name='to-beta.zip'):
        path = self.root / name
        with self.zipfile.ZipFile(path, 'w') as z:
            z.writestr('ultimate/mods/hdr/fighter/x.bin', b'new')
            z.writestr('ultimate/mods/hdr/ui/hdr_version.txt', 'v0.50.11-prerelease\n')
            z.writestr('ultimate/mods/hdr-stages/fighter/y.bin', b'added')
            z.writestr(hdr.PLUGIN_DIR + 'libstage_alts.nro', b'NRO2')
            z.writestr(hdr.PLUGIN_DIR + 'hdr-launcher.nro', b'NOT US')
            z.writestr('deletions.json', json.dumps(['sd:/ultimate/mods/hdr/fighter/gone.bin', 'sd:/atmosphere/../../evil']))
        return path

    def rels(self, assets=('to-beta.zip', 'switch-package.zip')):
        mk = lambda n: {'url': 'https://example/' + n, 'size': 0, 'sha256': None}
        return [{'tag': 'v0.50.11-prerelease', 'assets': {n: mk(n) for n in assets}},
                {'tag': 'v0.50.10-prerelease', 'assets': {}}]

    def test_check(self):
        c = hdr.check(self.tree, self.rels())
        self.assertTrue(c['newer'])
        self.assertTrue(c['delta'])
        self.assertEqual(c['asset'], 'to-beta.zip')
        (self.root / 'dump' / 'hdr' / 'installed.json').write_text(json.dumps({'version': 'v0.50.9-prerelease'}))
        c = hdr.check(self.tree, self.rels())
        self.assertFalse(c['delta'])   # (two releases behind: the whole package)
        self.assertEqual(c['asset'], 'switch-package.zip')
        (self.root / 'dump' / 'hdr' / 'installed.json').write_text(json.dumps({'version': 'v0.50.11-prerelease'}))
        self.assertFalse(hdr.check(self.tree, self.rels())['newer'])

    def test_update_with_the_upgrade_package(self):
        src = self.delta()

        def fetch(info, target, progress=None, log=None):
            shutil.copyfile(src, target)
        with mock.patch.object(hdr, 'fetch_asset', fetch), mock.patch.object(hdr, 'bake_motion'):
            self.assertTrue(hdr.update(self.tree, None, lambda s: None, self.rels()))
        self.assertEqual((self.root / 'mods' / 'hdr' / 'fighter' / 'x.bin').read_bytes(), b'new')
        self.assertEqual((self.root / 'mods' / 'hdr-assets' / 'fighter' / 'x.bin').read_bytes(), b'old')   # (unchanged)
        self.assertTrue((self.root / 'mods' / 'hdr-stages' / 'fighter' / 'y.bin').is_file())
        self.assertFalse((self.root / 'mods' / 'hdr' / 'fighter' / 'gone.bin').exists())
        self.assertEqual((self.root / 'dump' / 'hdr' / 'plugins' / 'libstage_alts.nro').read_bytes(), b'NRO2')
        self.assertEqual((self.root / 'skyline' / 'plugins' / 'libstage_alts.nro').read_bytes(), b'NRO2')
        self.assertFalse((self.root / 'skyline' / 'plugins' / 'hdr-launcher.nro').exists())
        self.assertEqual(hdr.installed(self.tree), 'v0.50.11-prerelease')
        self.assertFalse(list((self.root / 'dump' / 'hdr').glob('update-*.zip')))

    def test_the_release_s_deleted_files_list(self):
        """The release's deleted-files JSON (an asset beside to-beta.zip): those files removed from HDR's folders."""
        src = self.delta()
        (self.root / 'mods' / 'hdr-assets' / 'fighter' / 'old.nuanmb').write_bytes(b'old')
        listed = json.dumps({'deleted': ['sd:/ultimate/mods/hdr-assets/fighter/old.nuanmb', 'mods/../../escape.txt']})
        rels = self.rels(('to-beta.zip', 'switch-package.zip', 'deleted_files.json'))

        class Answer(io.BytesIO):
            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

        def fetch(info, target, progress=None, log=None):
            shutil.copyfile(src, target)
        with mock.patch.object(hdr, 'fetch_asset', fetch), mock.patch.object(hdr, 'bake_motion'), \
                mock.patch.object(hdr.urllib.request, 'urlopen', lambda *a, **k: Answer(listed.encode())):
            self.assertTrue(hdr.update(self.tree, None, lambda s: None, rels))
        self.assertFalse((self.root / 'mods' / 'hdr-assets' / 'fighter' / 'old.nuanmb').exists())
        self.assertFalse((self.root / 'mods' / 'hdr' / 'fighter' / 'gone.bin').exists())   # (the zip's own list too)
        self.assertTrue((self.root / 'mods' / 'hdr-assets' / 'fighter' / 'x.bin').exists())

    def test_a_zip_without_hdr_s_files_changes_nothing(self):
        bad = self.root / 'bad.zip'
        with self.zipfile.ZipFile(bad, 'w') as z:
            z.writestr('readme.txt', 'hi')
        with self.assertRaises(hdr.Error):
            hdr.apply_delta(self.tree, str(bad), 'v0.50.11-prerelease', lambda s: None)
        self.assertEqual(hdr.installed(self.tree), 'v0.50.10-prerelease')


class HdrDownload(unittest.TestCase):
    """hdr.download: HDR-PreReleases' newest switch-package, not the release pinned here."""
    def test_the_newest_prerelease(self):
        import zipfile
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            tree = mods.Tree(str(root))
            pkg = root / 'pkg.zip'
            with zipfile.ZipFile(pkg, 'w') as z:
                for m in hdr.MODS:
                    z.writestr(f'ultimate/mods/{m}/fighter/x.bin', m)
                z.writestr('ultimate/stage-alts/Hashes_all', 'H')
                for p in hdr.PLUGINS:
                    z.writestr(hdr.PLUGIN_DIR + p, b'NRO')
            rels = [{'tag': 'v0.51.0-prerelease', 'assets': {'switch-package.zip': {'url': 'u', 'size': 10, 'sha256': None}}}]

            def fetch(info, target, progress=None, log=None):
                shutil.copyfile(pkg, target)
            with mock.patch.object(hdr, 'fetch_asset', fetch), mock.patch.object(hdr, 'fetch') as pinned:
                st = hdr.download(tree, None, lambda s: None, rels)
            pinned.assert_not_called()
            self.assertEqual(st['installed'], 'v0.51.0-prerelease')
            self.assertTrue((root / 'mods' / 'hdr-stages' / 'fighter' / 'x.bin').is_file())
