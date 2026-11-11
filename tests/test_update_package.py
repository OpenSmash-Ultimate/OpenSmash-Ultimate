"""An update inside a package that holds the game too: update.py's choice of the update's program,
and the build passing that choice on. Made-up NCAs; no game files."""
from pathlib import Path
import runpy
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import update  # noqa: E402

PROGRAM, CONTROL = 0, 2
CTR, BKTR = 3, 4   # a section's encryption: a whole RomFS, or an update's patch over the base


def nca(content_type, romfs_enc=None):
    section = types.SimpleNamespace(enc_type=romfs_enc) if romfs_enc is not None else None
    return types.SimpleNamespace(content_type=content_type, section=lambda index, s=section: s if index == 0 else None)


class PickPatchProgramTests(unittest.TestCase):
    def test_update_package(self):
        prog = nca(PROGRAM, BKTR)
        self.assertIs(update.pick_patch_program([('a.nca', prog), ('b.nca', nca(CONTROL, CTR))]), prog)

    def test_game_and_update_in_one_package(self):
        base, patch = nca(PROGRAM, CTR), nca(PROGRAM, BKTR)
        ncas = [('base.nca', base), ('control.nca', nca(CONTROL, CTR)), ('update.nca', patch)]
        self.assertIs(update.pick_patch_program(ncas), patch)                            # the one with a patch RomFS
        self.assertIs(update.pick_patch_program(ncas, inner='UPDATE.nca'), patch)        # or by name
        with self.assertRaises(SystemExit):
            update.pick_patch_program(ncas, inner='missing.nca')

    def test_no_choice(self):
        with self.assertRaises(SystemExit):
            update.pick_patch_program([('c.nca', nca(CONTROL, CTR))])
        with self.assertRaises(SystemExit):
            update.pick_patch_program([('a.nca', nca(PROGRAM, BKTR)), ('b.nca', nca(PROGRAM, BKTR))])
        lone = nca(PROGRAM, CTR)   # (a lone program is taken as given: Patch then says it is no patch)
        self.assertIs(update.pick_patch_program([('a.nca', lone)]), lone)


class BuildPassesTheChoiceTests(unittest.TestCase):
    """build_game's data step on a package holding the game and its update: the base is decrypted
    out of it, and update.py is told which program in it is the update's."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='update-package-test-')
        self.addCleanup(self.temp.cleanup)
        top = Path(self.temp.name)
        self.game, self.modules = top / 'game', top / 'port' / 'dump' / 'modules'
        self.game.mkdir()
        self.modules.mkdir(parents=True)
        self.calls = []
        build = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_package')['write_romfs_cfg']
        self.g = build.__globals__   # (the module's own globals: run_path returns a copy)
        self.g.update(ROOT=str(top / 'port'), MODULES=str(self.modules), log=lambda msg='': None,   # (as test_build_game.py)
                      run=lambda args, failure, failures=None: self.calls.append([str(a) for a in args]))
        self.write_romfs_cfg = build

    def record(self, name, kind, romfs, container):
        path = self.game / (container or name)
        return {'path': str(path), 'name': name, 'container': container, 'type': 'program', 'kind': kind,
                'romfs': romfs, 'app': '01006A800016E000', 'title_id': '01006A800016E000', 'size': 10, 'version': None}

    def apply_call(self, program, base):
        result = {'contents': [base, program]}
        self.write_romfs_cfg(str(self.game), 'keys', lambda: (program['path'], 0x01006A800016E000, program, result))
        return next(c for c in self.calls if 'update.py' in ' '.join(c))

    def test_one_package(self):
        program = self.record('update.nca', 'update', 'patch', 'game+update.nsp')
        base = self.record('base.nca', 'base', 'encrypted', 'game+update.nsp')
        call = self.apply_call(program, base)
        self.assertEqual(call[call.index('apply') + 1], str(self.game / 'game+update.nsp'))
        self.assertEqual(call[call.index('--inner') + 1], 'update.nca')
        decrypt = next(c for c in self.calls if 'decrypt' in c)
        self.assertEqual(decrypt[decrypt.index('--inner') + 1], 'base.nca')     # the base out of the same package

    def test_loose_update_nca(self):
        program = self.record('update.nca', 'update', 'patch', None)
        base = self.record('base.nca', 'base', 'plain', None)
        call = self.apply_call(program, base)
        self.assertNotIn('--inner', call)

    def test_unconfirmed_files_say_why(self):
        """update.py stopping on unconfirmed bytes inside files the game loads (its exit code
        UNSAFE_EXIT): the build's error is that, not the generic "a file is incomplete" - the
        installers show the build's ERROR line to the player."""
        Failed = self.g['Failed']

        def run(args, failure, failures=None):
            self.calls.append([str(a) for a in args])
            if 'update.py' in ' '.join(map(str, args)):
                raise Failed((failures or {}).get(update.UNSAFE_EXIT, failure))
        self.g['run'] = run
        program = self.record('update.nca', 'update', 'patch', None)
        base = self.record('base.nca', 'base', 'plain', None)
        with self.assertRaises(Failed) as e:
            self.apply_call(program, base)
        self.assertIn('romfs.unverified.txt lists them', str(e.exception))
        self.assertIn(update.UNSAFE_BASE, str(e.exception))


class RunExitCodeTests(unittest.TestCase):
    """build_game.run: a tool's exit code picks its own message."""

    def test_codes(self):
        g = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_run')
        run, Failed = g['run'], g['Failed']
        run.__globals__['log'] = lambda msg='': None
        for code, want in ((3, 'its own message'), (1, 'the usual message')):
            with self.assertRaises(Failed) as e:
                run([sys.executable, '-c', f'import sys; sys.exit({code})'], 'the usual message', {3: 'its own message'})
            self.assertEqual(str(e.exception), want)
        run([sys.executable, '-c', 'pass'], 'the usual message', {3: 'its own message'})   # (no exit code: no error)


if __name__ == '__main__':
    unittest.main()
