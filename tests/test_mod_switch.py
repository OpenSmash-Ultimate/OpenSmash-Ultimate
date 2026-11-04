"""Every mod's switch and ARCropolis' code patches' (mods/mods.json), as the game's settings menu,
the launcher and the build see them:
  - runtime/mod_switch.h and runtime/mod_diff.h through tools/dev/modswitchtest.c (mods.json read in
    its order, with titles and escapes; the code patches' rule; what differs between two mod sets);
  - tools/os_mods.py: "switch" (the menu's: mods.json only, an apply asked for the next start), "--all",
    enable/disable of ":code_patches" (the launcher's, no apply), and the code patches' rule;
  - tools/lift.py: a code patch lifted both ways behind rt_code_patches_on, the original's form the
    game's own instruction as without patches.
Made-up mods and words only; the C part needs clang-cl on Windows, clang or cc elsewhere."""
from pathlib import Path
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402
import lift  # noqa: E402
import mods  # noqa: E402
import os_mods  # noqa: E402

if os.name == 'nt':
    COMPILER = cc.CLANG if (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG)) else None
else:
    COMPILER = shutil.which('clang') or shutil.which('cc')


@unittest.skipUnless(COMPILER, 'needs a C compiler (clang-cl on Windows, clang or cc elsewhere)')
class Headers(unittest.TestCase):
    def test_modswitchtest(self):
        with tempfile.TemporaryDirectory(prefix='modswitchtest ') as d:
            src = str(ROOT / 'tools' / 'dev' / 'modswitchtest.c')
            if os.name == 'nt':
                exe = os.path.join(d, 'modswitchtest.exe')
                cmd = [COMPILER, '/nologo', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, src, '/Fe' + exe, '/Fo' + os.path.join(d, 'modswitchtest.obj'),
                       '/link', *cc.LINK_OPTIONS]
            else:
                exe = os.path.join(d, 'modswitchtest')
                cmd = [COMPILER, '-O1', '-w', src, '-o', exe]
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertRegex(r.stdout, r'\d+ checks, 0 failed\n$')


class OsMods(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix='os_mods ')
        self.addCleanup(shutil.rmtree, self.dir, True)
        for name in ('Alpha', 'Beta'):
            os.makedirs(os.path.join(self.dir, 'mods', name))
        self.tree = mods.Tree(self.dir)
        state = self.tree.load()
        state['mods']['Alpha'].update(enabled=True, title='The Alpha')
        self.tree.save(state)

    def run_cmd(self, *argv):
        lines = []
        with unittest.mock.patch.object(os_mods.mods, 'Tree', lambda root: self.tree), \
                unittest.mock.patch.object(os_mods.mods, 'apply', lambda tree, log: lines.append('applied')), \
                unittest.mock.patch('builtins.print', lambda *a, **k: lines.append(' '.join(map(str, a)))):
            code = os_mods.main(['os_mods.py', *argv])
        return code, lines

    def state(self):
        with open(os.path.join(self.dir, 'mods', 'mods.json'), encoding='utf-8') as f:
            return json.load(f)

    def pending(self):
        return os.path.exists(os.path.join(self.dir, 'dump', 'mods', 'apply_pending'))

    def test_switch_writes_and_asks_for_an_apply(self):
        code, lines = self.run_cmd('switch', 'Beta', 'on')
        self.assertEqual(code, 0)
        self.assertNotIn('applied', lines)   # (not now: at the game's next start)
        self.assertTrue(self.state()['mods']['Beta']['enabled'])
        self.assertEqual(self.state()['mods']['Alpha']['title'], 'The Alpha')   # (the rest kept)
        self.assertTrue(self.pending())

    def test_switch_to_the_same_asks_nothing(self):
        self.run_cmd('switch', 'Alpha', 'on')
        self.assertFalse(self.pending())

    def test_all_off(self):
        self.run_cmd('switch', 'Beta', 'on')
        self.run_cmd('switch', '--all', 'off')
        self.assertEqual({n: m['enabled'] for n, m in self.state()['mods'].items()}, {'Alpha': False, 'Beta': False})

    def test_code_patches(self):
        self.assertFalse(os_mods.code_patches_on(self.tree))   # (auto: no mod runs - no dump/skyline/mods.txt)
        os.makedirs(os.path.join(self.dir, 'dump', 'skyline'))
        with open(os.path.join(self.dir, 'dump', 'skyline', 'mods.txt'), 'w') as f:
            f.write('Alpha\n')
        self.assertTrue(os_mods.code_patches_on(self.tree))   # (auto: a mod runs)
        code, lines = self.run_cmd('disable', os_mods.CODE)   # (the launcher's switch)
        self.assertEqual(code, 0)
        self.assertNotIn('applied', lines)
        self.assertIs(self.state()['code_patches'], False)
        self.assertFalse(os_mods.code_patches_on(self.tree))
        self.run_cmd('switch', os_mods.CODE, 'on')   # (the menu's)
        self.assertIs(self.state()['code_patches'], True)
        self.assertFalse(self.pending())   # (read at start: no apply)

    def test_code_patches_not_removed(self):
        code, lines = self.run_cmd('remove', os_mods.CODE)
        self.assertEqual(code, 1)
        self.assertTrue(any('turn them off instead' in line for line in lines))

    def result(self, lines):
        line = [x for x in lines if x.startswith('RESULT ')][-1]
        return json.loads(line[len('RESULT '):])

    def test_conflicts(self):
        """The launcher's Check card: files two turned-on mods both change, the later one's kept."""
        for name in ('Alpha', 'Beta'):
            path = os.path.join(self.dir, 'mods', name, 'fighter', 'mario', 'model', 'body', 'c00')
            os.makedirs(path)
            with open(os.path.join(path, 'def_mario_001_col.nutexb'), 'wb') as f:
                f.write(name.encode())
        self.run_cmd('switch', 'Beta', 'on')
        code, lines = self.run_cmd('conflicts')
        self.assertEqual(code, 0)
        got = self.result(lines)
        self.assertTrue(got['ok'])
        self.assertEqual([c['path'] for c in got['conflicts']], ['fighter/mario/model/body/c00/def_mario_001_col.nutexb'])
        self.assertEqual(got['conflicts'][0]['mods'][-1], got['conflicts'][0]['wins'])

    def test_updates_name_their_page(self):
        found = [{'name': 'Alpha', 'title': 'The Alpha', 'installed': 'a.zip', 'newest': 'b.zip', 'file_id': '9'}]
        state = self.tree.load()
        state['mods']['Alpha']['gamebanana'] = 1234
        self.tree.save(state)
        with unittest.mock.patch.object(os_mods.mods, 'updates', lambda tree: [dict(u) for u in found]):
            code, lines = self.run_cmd('updates')
        self.assertEqual(code, 0)
        self.assertEqual(self.result(lines)['updates'][0]['gamebanana'], '1234')   # (install 1234 9 updates it)

    def test_export_then_import_a_list(self):
        listed = os.path.join(self.dir, 'mine.json')
        code, lines = self.run_cmd('export-list', listed)
        self.assertEqual((code, self.result(lines)['count']), (0, 2))
        code, lines = self.run_cmd('import-list', listed)   # (both here already: nothing to download)
        got = self.result(lines)
        self.assertEqual((code, sorted(got['already']), got['installed'], got['missing']), (0, ['Alpha', 'Beta'], [], []))
        self.assertIn('applied', lines)
        code, lines = self.run_cmd('import-list')
        self.assertEqual(code, 1)

    def test_no_such_mod(self):
        code, _ = self.run_cmd('switch', 'Gamma', 'on')
        self.assertEqual(code, 1)


class LiftSwitch(unittest.TestCase):
    """cmp x19, #8 at 0, patched to cmp x19, #256 (Aegis' first, recomp.CODE_PATCHES), then ret."""
    WORDS = (0xF100227F, 0xD65F03C0)

    def lifted(self, patches=None):
        img = struct.pack('<2I', *self.WORDS) + bytes(8)
        text, _reasons, _callees = lift.Lifter(img, {0}, patches=patches).lift_function(0, 8)
        return text

    def test_both_ways(self):
        plain = self.lifted()
        switched = self.lifted({0: ('switch', [0xF104027F])})
        patched = self.lifted({0: [0xF104027F]})   # (a plugin's patch: always)
        self.assertIn('if (rt_code_patches_on()) {', switched)
        # the original's form: the game's own instruction, as lifted without patches; the other the patch's
        own = lift.Lifter(struct.pack('<2I', *self.WORDS) + bytes(8), {0}, patches=None)
        own.fstart, own.fend, own.words, own.i = 0, 8, self.WORDS, 0
        stmt_own = own.insn(self.WORDS[0], 0)[0]
        own.words = (0xF104027F, self.WORDS[1])
        stmt_patch = own.insn(0xF104027F, 0)[0]
        self.assertIn(f'if (rt_code_patches_on()) {{ {stmt_patch} }} else {{ {stmt_own} }}', switched)
        self.assertNotIn('rt_code_patches_on', plain + patched)
        self.assertNotEqual(stmt_own, stmt_patch)


if __name__ == '__main__':
    unittest.main()
