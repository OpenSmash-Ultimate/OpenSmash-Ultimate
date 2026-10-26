"""runtime/game_addr.h: every 13.0.5 address in it is what ssbu-recomp's GA() gives for the 13.0.4
address beside it. GA()'s rule and its table of code addresses are read from
ssbu-recomp/runtime/hostrt.c (and GUEST_BASE from hostrt.h), so a row typed wrong, or a code address
that table does not have, fails here. No game files: the addresses are the only data."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / 'runtime' / 'game_addr.h'
HOSTRT = ROOT / 'ssbu-recomp' / 'runtime' / 'hostrt.c'
HOSTRT_H = ROOT / 'ssbu-recomp' / 'runtime' / 'hostrt.h'


def define(text, name):
    m = re.search(r'^#define %s (0x[0-9A-Fa-f]+)ull\b' % name, text, re.M | re.I)
    if not m:
        raise AssertionError('no #define %s' % name)
    return int(m.group(1), 16)


def header_rows(text):
    """(name, 13.0.4, 13.0.5, kind comment) of each X(...) row of GAME_ADDRESSES."""
    rows = re.findall(r'^\s*X\((\w+),\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+)\)\s*/\* ([^*]*) \*/', text, re.M)
    return [(name, int(a, 16), int(b, 16), kind) for name, a, b, kind in rows]


class Recomp:
    """ssbu-recomp's GA() for 13.0.5, from its source."""
    def __init__(self):
        c, h = HOSTRT.read_text(encoding='utf-8'), HOSTRT_H.read_text(encoding='utf-8')
        self.base = int(re.search(r'^#define GUEST_BASE (0x[0-9A-Fa-f]+)ULL', h, re.M).group(1), 16)
        self.size = int(re.search(r'^#define IMAGE_SIZE (0x[0-9A-Fa-f]+)ULL', c, re.M).group(1), 16)
        m = re.search(r'if \(o >= (0x[0-9a-f]+)\) return a \+ (0x[0-9a-f]+);', c)
        self.ro, self.page = int(m.group(1), 16), int(m.group(2), 16)
        body = re.search(r'g_code_13_0_5\[\]\[2\] = \{(.*?)\n\};', c, re.S).group(1)
        self.code = {int(a, 16): int(b, 16) for a, b in re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+)\}', body)}

    def is_code(self, a):
        return self.base <= a < self.base + self.size and a - self.base < self.ro

    def ga(self, a):
        """The 13.0.5 address, or None where GA() stops the program (a code address it doesn't know)."""
        if a < self.base or a >= self.base + self.size:
            return a
        o = a - self.base
        if o >= self.ro:
            return a + self.page
        return self.base + self.code[o] if o in self.code else None


@unittest.skipUnless(HOSTRT.exists() and HOSTRT_H.exists(), 'needs ssbu-recomp/runtime/hostrt.c and hostrt.h, where GA() is')
class GameAddressTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sr = Recomp()
        cls.text = HEADER.read_text(encoding='utf-8')
        cls.rows = header_rows(cls.text)

    def test_reading_both(self):
        """(What the rest stands on: both files were read as expected.)"""
        self.assertGreater(len(self.sr.code), 20)
        self.assertEqual(len(self.rows), self.text.count('    X('))
        self.assertGreaterEqual(len(self.rows), 7)

    def test_rule_is_ssbu_recomps(self):
        self.assertEqual(define(self.text, 'GAME_MAIN_BASE'), self.sr.base)
        self.assertEqual(define(self.text, 'GAME_IMAGE_SIZE'), self.sr.size)
        self.assertEqual(define(self.text, 'GAME_13_0_4_RO_DATA'), self.sr.ro)
        self.assertEqual(define(self.text, 'GAME_13_0_5_PAGE'), self.sr.page)

    def test_every_address(self):
        for name, at_13_0_4, at_13_0_5, kind in self.rows:
            with self.subTest(name):
                self.assertEqual(self.sr.ga(at_13_0_4), at_13_0_5,
                                 '%s: 13.0.4 %#x is %s in ssbu-recomp' % (name, at_13_0_4, 'not in its table' if self.sr.ga(at_13_0_4) is None
                                                                       else hex(self.sr.ga(at_13_0_4))))

    def test_code_and_data_said_right(self):
        """A row's comment says code for an address in main's code (then it comes from the table)
        and data for one after it, as the rule decides."""
        for name, at_13_0_4, _, kind in self.rows:
            with self.subTest(name):
                self.assertEqual(kind.split(':')[0], 'code' if self.sr.is_code(at_13_0_4) else 'data')
                if self.sr.is_code(at_13_0_4):
                    self.assertIn(at_13_0_4 - self.sr.base, self.sr.code)

    def test_names_and_addresses_once(self):
        names = [r[0] for r in self.rows]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(len({r[2] for r in self.rows}), len(self.rows))

    def test_the_ones_asked_for(self):
        """The addresses the fight hash, the deterministic mode and the debug match start from."""
        by_name = {r[0]: r[1] for r in self.rows}
        self.assertEqual(by_name['FRAME_END'], 0x710386fc30)
        self.assertEqual(by_name['PLAYER_RECORD_BUILDER'], 0x7101788260)
        self.assertEqual(by_name['SCRIPT_RNG_ROOT'], 0x71052c25b0)
        self.assertEqual(by_name['MATCH_BLOCK'], 0x71052c41b0)


class RuntimeUseTests(unittest.TestCase):
    """The runtime's own uses of the table (game_addr.h alone, no ssbu-recomp needed)."""
    @classmethod
    def setUpClass(cls):
        cls.at_13_0_5 = {r[0]: r[2] for r in header_rows(HEADER.read_text(encoding='utf-8'))}
        cls.sources = {p.name: p.read_text(encoding='utf-8', errors='replace') for p in (ROOT / 'runtime').rglob('*.[ch]')}

    def test_wraps_by_address_are_the_rows(self):
        """The HLE_WRAP lines of the test switches (a literal, as tools/recomp.py reads them) are the
        13.0.5 addresses of the rows named beside them: the frame end in hle_each_frame.c (the
        frame hooks), the record builder in hle_fight_test.c."""
        wraps = {name: [int(a, 16) for a in re.findall(r'^HLE_WRAP\(([0-9a-f]+)\)', self.sources[name], re.M)]
                 for name in ('hle_each_frame.c', 'hle_fight_test.c')}
        self.assertEqual(wraps, {'hle_each_frame.c': [self.at_13_0_5['FRAME_END']],
                                 'hle_fight_test.c': [self.at_13_0_5['PLAYER_RECORD_BUILDER']]})

    def test_every_name_used_is_a_row(self):
        used = {name for text in self.sources.values() for name in re.findall(r'\bGAME_ADDR\((\w+)\)', text)} - {'name'}
        self.assertGreaterEqual(len(used), 9)   # (hle_fight_test.c alone uses 9)
        self.assertEqual(used - set(self.at_13_0_5), set())


if __name__ == '__main__':
    unittest.main()
