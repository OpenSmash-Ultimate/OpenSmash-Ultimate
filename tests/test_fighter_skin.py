"""tools/fighter_skin.py: a donor's skin mod put on a new fighter's folder. Made-up files; no game data."""
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import fighter_skin  # noqa: E402
import shader_aot  # noqa: E402


def put(path, data=b'x'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class SkinTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        self.mod, self.folder = t / 'download' / '[Fighter] skin', t / 'chars' / 'phantomg'
        put(self.mod / 'info.toml', b'display_name = "Some MODEL"\nauthors = "Someone"\n')
        put(self.mod / 'config.json', b'{}')
        put(self.mod / 'fighter' / 'ganon' / 'model' / 'body' / 'c03' / 'model.numdlb', b'model3')
        put(self.mod / 'fighter' / 'ganon' / 'model' / 'body' / 'c06' / 'alp_ganon_002_emi.nutexb', b'emi')
        put(self.mod / 'fighter' / 'ganon' / 'motion' / 'body' / 'c00' / 'j02win1.nuanmb', b'win')
        put(self.mod / 'sound' / 'bank' / 'fighter_voice' / 'vc_ganon_c05.nus3audio', b'voice5')
        put(self.mod / 'ui' / 'replace' / 'chara' / 'chara_0' / 'chara_0_ganon_07.bntx', b'pic7')
        for c in range(3):
            put(self.mod / 'effect' / 'fighter' / 'ganon' / ('ef_ganon_c%02d.eff' % c), b'effects')
        self.folder.mkdir(parents=True)
        (self.folder / 'fighter.json').write_text(json.dumps({'name': 'phantomg', 'donor': 'ganon'}))
        put(self.folder / 'effect' / 'fighter' / 'phantomg' / '0x0c82b78a10.eff', b'old effects')
        put(self.folder / 'fighter' / 'phantomg' / 'motion' / 'body' / 'c00' / fighter_skin.hash_name('j02win1.nuanmb'), b'old win')

    def tearDown(self):
        self.tmp.cleanup()

    def run_skin(self, *extra):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = fighter_skin.main([str(self.mod.parent), str(self.folder), *extra])
        return rc, out.getvalue()

    def test_paths_take_the_fighters_name(self):
        rc, out = self.run_skin()
        self.assertEqual(rc, 0, out)
        f = self.folder
        self.assertEqual((f / 'fighter' / 'phantomg' / 'model' / 'body' / 'c03' / 'model.numdlb').read_bytes(), b'model3')
        self.assertEqual((f / 'sound' / 'bank' / 'fighter_voice' / 'vc_phantomg_c05.nus3audio').read_bytes(), b'voice5')
        self.assertEqual((f / 'ui' / 'replace' / 'chara' / 'chara_0' / 'chara_0_phantomg_07.bntx').read_bytes(), b'pic7')
        self.assertFalse((f / 'config.json').exists())
        # a texture keeps its name: the material finds it by that name
        self.assertEqual((f / 'fighter' / 'phantomg' / 'model' / 'body' / 'c06' / 'alp_ganon_002_emi.nutexb').read_bytes(), b'emi')

    def test_one_effect_file_and_the_hashed_twin_goes(self):
        self.run_skin()
        eff = self.folder / 'effect' / 'fighter' / 'phantomg'
        self.assertEqual(sorted(os.listdir(eff)), ['0x0c82b78a10.eff'])
        self.assertEqual((eff / '0x0c82b78a10.eff').read_bytes(), b'effects')
        motion = self.folder / 'fighter' / 'phantomg' / 'motion' / 'body' / 'c00'
        self.assertEqual(sorted(os.listdir(motion)), ['j02win1.nuanmb'])

    def test_credit_and_dry(self):
        rc, out = self.run_skin('--dry')
        self.assertIn('would be', out)
        self.assertFalse((self.folder / 'ui').exists())
        self.run_skin()
        j = json.loads((self.folder / 'fighter.json').read_text())
        self.assertEqual(j['credits'], ['Some MODEL by Someone'])

    def test_hash_name(self):
        self.assertEqual(fighter_skin.hash_name('ef_ganon.eff'), '0x0c82b78a10.eff')


class LooseShaderFilesTests(unittest.TestCase):
    def test_loose_files_finds_containers(self):
        with tempfile.TemporaryDirectory() as t:
            put(Path(t) / 'effect' / 'a.eff')
            put(Path(t) / 'shader' / 'b.nushdb')
            put(Path(t) / 'model' / 'c.numdlb')
            got = [os.path.basename(p) for p in shader_aot.loose_files([t])]
        self.assertEqual(sorted(got), ['a.eff', 'b.nushdb'])


if __name__ == '__main__':
    unittest.main()
