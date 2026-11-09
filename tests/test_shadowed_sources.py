"""A mod that ships motion_list.bin and its text source motion_list.yml side by side:
tools/mods.py hide_shadowed_sources adds the .yml to dump/skyline/hidden.txt only when ARCropolis
can't read it - an older format whose flags lack unk_800, unk_1000 and unk_2000 (Knuckles') - so the
whole file is loaded. A .yml in the current format (Electivire's, Zeraora's) is the motion list the
mod uses, often newer than the .bin, and stays. On a made-up tree."""
from pathlib import Path
import os
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import mods  # noqa: E402

BODY = 'fighter/sonic/motion/body'
FLAGS_OLD = ''.join(f'      {k}: false\n' for k in ('turn', 'loop', 'move', 'fix_trans', 'fix_rot', 'fix_scale',
                                                     'unk_40', 'unk_80', 'unk_100', 'unk_200', 'unk_400'))
FLAGS_NEW = FLAGS_OLD + ''.join(f'      {k}: false\n' for k in ('unk_800', 'unk_1000', 'unk_2000'))


def motion_list(flags):
    return ('---\nmotion_path: fighter/sonic/motion/body/c00\nlist:\n'
            + ''.join(f'  {m}:\n    game_script: game_{m}\n    flags:\n{flags}    blend_frames: 6\n'
                      for m in ('wait', 'attack11')))


class ShadowedSources(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.tree = mods.Tree(self.tmp.name)
        self.hidden = os.path.join(self.tree.root, 'dump', 'skyline', 'hidden.txt')
        os.makedirs(os.path.dirname(self.hidden))
        self.log = []

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, mod, rel, text='x'):
        p = Path(self.tree.mods, mod, rel)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding='utf-8')

    def hidden_lines(self):
        try:
            return Path(self.hidden).read_text(encoding='utf-8').splitlines()
        except FileNotFoundError:
            return []

    def test_an_old_format_yml_beside_its_bin_is_hidden(self):
        self.put('Knuckles', f'{BODY}/c120/motion_list.bin')
        self.put('Knuckles', f'{BODY}/c120/motion_list.yml', motion_list(FLAGS_OLD))
        Path(self.hidden).write_text('Other/ui/kept.bntx\n', encoding='utf-8')   # (what's there stays)
        mods.hide_shadowed_sources(self.tree, ['Knuckles'], self.log.append)
        self.assertEqual(self.hidden_lines(), ['Other/ui/kept.bntx', f'Knuckles/{BODY}/c120/motion_list.yml'])
        self.assertIn('1 text source(s)', self.log[0])

    def test_a_current_format_yml_beside_its_bin_stays(self):
        self.put('Electivire', f'{BODY}/c16/motion_list.bin')
        self.put('Electivire', f'{BODY}/c16/motion_list.yml', motion_list(FLAGS_NEW))
        mods.hide_shadowed_sources(self.tree, ['Electivire'], self.log.append)
        self.assertEqual(self.hidden_lines(), [])
        self.assertEqual(self.log, [])

    def test_one_old_motion_is_enough_to_hide_it(self):
        self.put('Mixed', f'{BODY}/c08/motion_list.bin')
        self.put('Mixed', f'{BODY}/c08/motion_list.yml', motion_list(FLAGS_NEW) + motion_list(FLAGS_OLD)[60:])
        mods.hide_shadowed_sources(self.tree, ['Mixed'], self.log.append)
        self.assertEqual(self.hidden_lines(), [f'Mixed/{BODY}/c08/motion_list.yml'])

    def test_a_yml_alone_is_left_to_arcropolis(self):
        self.put('Patch', f'{BODY}/c00/motion_list.yml', motion_list(FLAGS_OLD))
        mods.hide_shadowed_sources(self.tree, ['Patch'], self.log.append)
        self.assertEqual(self.hidden_lines(), [])
        self.assertEqual(self.log, [])

    def test_only_turned_on_mods(self):
        self.put('Off', f'{BODY}/c120/motion_list.bin')
        self.put('Off', f'{BODY}/c120/motion_list.yml', motion_list(FLAGS_OLD))
        mods.hide_shadowed_sources(self.tree, [], self.log.append)
        self.assertEqual(self.hidden_lines(), [])


if __name__ == '__main__':
    unittest.main()
