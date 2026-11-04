"""A mod that ships a file whole and as ARCropolis' text source beside it (Knuckles'
fighter/sonic/motion/body/c120/motion_list.bin and .yml): tools/mods.py hide_shadowed_sources adds
the .yml to dump/skyline/hidden.txt, so ARCropolis loads the whole file instead of building one from
an older-format .yml. A .yml alone (a patch with nothing beside it) is left to ARCropolis. On a
made-up tree."""
from pathlib import Path
import os
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import mods  # noqa: E402

BODY = 'fighter/sonic/motion/body'


class ShadowedSources(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.tree = mods.Tree(self.tmp.name)
        self.hidden = os.path.join(self.tree.root, 'dump', 'skyline', 'hidden.txt')
        os.makedirs(os.path.dirname(self.hidden))
        self.log = []

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, mod, rel):
        p = Path(self.tree.mods, mod, rel)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b'x')

    def hidden_lines(self):
        try:
            return Path(self.hidden).read_text(encoding='utf-8').splitlines()
        except FileNotFoundError:
            return []

    def test_the_yml_beside_its_bin_is_hidden(self):
        self.put('Knuckles', f'{BODY}/c120/motion_list.bin')
        self.put('Knuckles', f'{BODY}/c120/motion_list.yml')
        Path(self.hidden).write_text('Other/ui/kept.bntx\n', encoding='utf-8')   # (what's there stays)
        mods.hide_shadowed_sources(self.tree, ['Knuckles'], self.log.append)
        self.assertEqual(self.hidden_lines(), ['Other/ui/kept.bntx', f'Knuckles/{BODY}/c120/motion_list.yml'])
        self.assertIn('1 text source(s)', self.log[0])

    def test_a_yml_alone_is_left_to_arcropolis(self):
        self.put('Patch', f'{BODY}/c00/motion_list.yml')
        mods.hide_shadowed_sources(self.tree, ['Patch'], self.log.append)
        self.assertEqual(self.hidden_lines(), [])
        self.assertEqual(self.log, [])

    def test_only_turned_on_mods(self):
        self.put('Off', f'{BODY}/c120/motion_list.bin')
        self.put('Off', f'{BODY}/c120/motion_list.yml')
        mods.hide_shadowed_sources(self.tree, [], self.log.append)
        self.assertEqual(self.hidden_lines(), [])


if __name__ == '__main__':
    unittest.main()
