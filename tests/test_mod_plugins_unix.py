"""A mod's Skyline plugin on each system (tools/skyline.py): on Windows its own DLL (cache_mod_plugins,
dll_current); on macOS and Linux compiled into the program, so no DLL is made (cache_mod_plugins
runs no clang-cl there), "built" means the program has it (built_in), and a plugin it doesn't have is
a reason to link it again (game_link_reason). On made-up trees, with each system's answer mocked."""
from pathlib import Path
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import skyline  # noqa: E402

MOD = 'Gogeta Moveset'


class ModPlugins(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.tree = self.tmp.name
        self.nro = os.path.join(self.tree, 'mods', MOD, 'plugin.nro')
        os.makedirs(os.path.dirname(self.nro))
        open(self.nro, 'wb').write(b'not read')
        patches = [mock.patch.object(skyline, 'load_record', lambda p: {'BID': {'module': 'gogeta'}}),
                   mock.patch.object(skyline, 'nro_build_id', lambda path: 'BID'),
                   mock.patch.object(skyline, 'turned_on', lambda tree: [(MOD, self.nro)])]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def on(self, system):
        p = mock.patch.object(skyline.os, 'name', system)
        p.start()
        self.addCleanup(p.stop)

    def test_no_dll_is_made_off_windows(self):
        self.on('posix')
        with mock.patch('subprocess.run') as run:
            self.assertEqual(skyline.cache_mod_plugins(self.tree, 1, lambda s: None), [])
        run.assert_not_called()

    def test_built_is_the_programs_copy_off_windows(self):
        self.on('posix')
        with mock.patch.object(skyline, 'built_in', return_value=True) as b, \
                mock.patch.object(skyline, 'dll_current', side_effect=AssertionError('no DLL off Windows')):
            state = skyline.plugin_state(self.tree, MOD, ['plugin.nro'])
        self.assertEqual(state, [{'file': 'plugin.nro', 'module': 'gogeta', 'built': True}])
        b.assert_called_with(self.tree, 'gogeta')

    def test_built_is_the_dll_on_windows(self):
        self.on('nt')
        with mock.patch.object(skyline, 'dll_current', return_value=False), \
                mock.patch.object(skyline, 'built_in', side_effect=AssertionError('the DLL on Windows')):
            state = skyline.plugin_state(self.tree, MOD, ['plugin.nro'])
        self.assertFalse(state[0]['built'])

    def test_a_plugin_the_program_lacks_is_a_reason_to_link(self):
        with mock.patch.object(skyline, 'built_in', return_value=False):
            self.assertEqual(skyline.unbuilt_mod_plugin(self.tree), f"{MOD}'s plugin isn't in the game")
        with mock.patch.object(skyline, 'built_in', return_value=True):
            self.assertIsNone(skyline.unbuilt_mod_plugin(self.tree))

    def test_play_without_mods_links_only_for_the_game(self):
        # (30 mods turned on but not built in don't make the plain game wait for a link; an update does)
        self.on('posix')
        exe = os.path.join(self.tree, 'build-linux', 'ssbu')
        os.makedirs(os.path.dirname(exe))
        open(exe, 'wb').close()
        os.utime(exe, (1e9, 1e9))
        with mock.patch.object(skyline, 'program', return_value=exe), \
                mock.patch.object(skyline, 'unbuilt_standard', return_value=None), \
                mock.patch.object(skyline, 'built_in', return_value=False), \
                mock.patch.object(skyline, 'stale_hooks', return_value=['main']), \
                mock.patch.dict('std_plugins.PLUGINS', clear=True):
            self.assertEqual(skyline.game_link_reason(self.tree), f"{MOD}'s plugin isn't in the game")
            self.assertIsNone(skyline.game_link_reason(self.tree, mods=False))
            os.makedirs(os.path.join(self.tree, 'runtime'))
            open(os.path.join(self.tree, 'runtime', 'host.c'), 'w').close()
            self.assertEqual(skyline.game_link_reason(self.tree, mods=False), 'runtime/host.c is newer than the game')


if __name__ == '__main__':
    unittest.main()
