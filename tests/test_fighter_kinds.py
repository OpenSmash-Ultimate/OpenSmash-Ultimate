"""tools/fighter_kinds.py: the sites that make room for new fighter kinds (plan/phases/R9_fighter_registry.md,
step 1). The table and the instruction edits are checked here without the game; with the game's dump in this
checkout, that its main takes every patch."""
from pathlib import Path
import os
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import fighter_kinds as fk  # noqa: E402


class Table(unittest.TestCase):
    def test_sites(self):
        s = fk.sites()
        self.assertEqual(len(s), 175)
        self.assertEqual(len({off for off, *_ in s}), 175)
        kinds = {k for _, k, *_ in s}
        self.assertLessEqual(kinds, {'ls', 'imm12', 'movz', 'movn'})
        bounds = [x for x in s if x[4].startswith('bound')]
        self.assertEqual(len(bounds), 66)
        self.assertTrue(all((old, new) == (fk.KINDS, fk.NEW_KINDS) for _, _, old, new, _ in bounds))
        moved = [x for x in s if x[4].startswith('B ')]
        self.assertTrue(all(new - old == 0x19A8 - 0x14F0 for _, _, old, new, _ in moved))

    def test_edits(self):
        """Made-up instructions of each kind: only the immediate changes."""
        cmp_w8 = 0x7100001F | (0x5E << 10) | (8 << 5)         # cmp w8, #0x5e
        self.assertEqual(fk.decode(cmp_w8, 'imm12'), 0x5E)
        self.assertEqual(fk.encode(cmp_w8, 'imm12', 0x72) ^ cmp_w8, (0x5E ^ 0x72) << 10)
        ldr = 0xB9400000 | ((0x14F0 // 4) << 10) | (1 << 5) | 2  # ldr w2, [x1, #0x14f0]
        self.assertEqual(fk.decode(ldr, 'ls'), 0x14F0)
        self.assertEqual(fk.decode(fk.encode(ldr, 'ls', 0x19A8), 'ls'), 0x19A8)
        strx = 0xF9000000 | ((0x1500 // 8) << 10)               # str x0, [x0, #0x1500]
        self.assertEqual(fk.decode(fk.encode(strx, 'ls', 0x1700), 'ls'), 0x1700)
        with self.assertRaises(ValueError):
            fk.encode(strx, 'ls', 0x1704)                        # (not a multiple of 8)
        movz = 0x52800000 | (0x19A8 << 5) | 1                    # mov w1, #0x19a8
        self.assertEqual(fk.decode(fk.encode(movz, 'movz', 0x1F00), 'movz'), 0x1F00)
        self.assertIsNone(fk.decode(movz, 'imm12'))
        self.assertIsNone(fk.decode(cmp_w8, 'ls'))

    def test_wrong_main(self):
        """A main whose words are not the expected instructions: nothing is patched."""
        found, bad = fk.patches(lambda off: 0)
        self.assertEqual((len(found), len(bad)), (0, 175))


@unittest.skipUnless((ROOT / 'dump' / 'exefs' / 'main').exists(), 'no game dump in this checkout')
class Dump(unittest.TestCase):
    def test_main_takes_them(self):
        found, bad = fk.patches(fk.main_words(str(ROOT)))
        self.assertEqual(bad, [])
        self.assertEqual(len(found), 175)


if __name__ == '__main__':
    unittest.main()
