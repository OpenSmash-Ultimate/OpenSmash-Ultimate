"""tools/recomp.py: the code signatures of the runtime's hooks (HLE_WRAP). A signature is a hash
of the hooked function's first instructions, never the instructions themselves: those are the
game's code (CONTRIBUTING.md, rule 1). The modules here are made of made-up words."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import recomp  # noqa: E402

# Made-up instructions, each picked for the masking rule it exercises (none comes from the game).
WORDS = [
    0xD10083FF,   # sub sp, sp, #0x20: kept whole
    0x94000123,   # bl: the target masked
    0x54000041,   # b.ne: the offset masked
    0xB4000062,   # cbz x2: the offset masked
    0x37080043,   # tbnz w3, #1: the offset masked
    0x90000004,   # adrp x4: the page masked, and x4 remembered
    0x91002084,   # add x4, x4, #8: an offset into x4's page, masked
    0xF9400485,   # ldr x5, [x4, #8]: the same
    0x58000046,   # ldr x6, literal: the offset masked
    0xAA0103E0,   # mov x0, x1: kept whole
    0xD65F03C0,   # ret
]


class FakeModule:
    """The one thing signature() reads from a module: its words."""

    def __init__(self, words):
        self.image = struct.pack(f'<{len(words)}I', *words)

    def u32(self, a):
        return struct.unpack_from('<I', self.image, a)[0]


class Signature(unittest.TestCase):
    def test_masks_what_depends_on_where_code_and_data_lie(self):
        self.assertEqual(recomp.signature(FakeModule(WORDS), 0, 4 * len(WORDS)), [
            'd10083ff', '9.......', '54.....1', 'b4.....2', '370....3', '.0.....4', '91....84', 'f9....85',
            '58.....6', 'aa0103e0', 'd65f03c0'])

    def test_takes_at_most_n_words_and_no_more_than_the_function_has(self):
        mod = FakeModule(WORDS)
        self.assertEqual(len(recomp.signature(mod, 0, 4 * len(WORDS), 4)), 4)
        self.assertEqual(len(recomp.signature(mod, 0, 12)), 3)

    def test_the_comment_holds_a_hash_and_a_count_not_the_words(self):
        words = recomp.signature(FakeModule(WORDS), 0, 4 * len(WORDS))
        comment = recomp.signature_comment(words)
        self.assertRegex(comment, r'^/\* signature: 11 words, sha256 [0-9a-f]{16} \*/$')
        for w in words:
            self.assertNotIn(w, comment)
        self.assertEqual(recomp.signature_comment(words[:1]), f'/* signature: 1 word, sha256 {recomp.signature_hash(words[:1])} */')
        self.assertNotEqual(recomp.signature_hash(words), recomp.signature_hash(words[:-1] + ['d65f03c1']))


def runtime_with(source):
    """A stand-in repository root whose runtime/ holds one file, wraps.c, with this text."""
    tmp = tempfile.TemporaryDirectory()
    (Path(tmp.name) / 'runtime').mkdir()
    (Path(tmp.name) / 'runtime' / 'wraps.c').write_text(source, encoding='utf-8')
    return tmp


class HleWraps(unittest.TestCase):
    def wraps(self, source):
        with runtime_with(source) as root, mock.patch.object(recomp, 'ROOT', root):
            return recomp.hle_wraps()

    def test_reads_the_count_and_hash(self):
        self.assertEqual(self.wraps('/* signature: 16 words, sha256 0123456789abcdef */\nHLE_WRAP(7100001000) {}\n'
                                    'HLE_WRAP(7100002000) {}\n'),
                         {0x7100001000: (16, '0123456789abcdef'), 0x7100002000: None})

    def test_refuses_a_signature_that_spells_out_instruction_words(self):
        for old in ('/* signature: 0123abcd 4567ef89 9....... */\n', '/* signature: 0123abcd */\n'):
            with self.subTest(old=old), self.assertRaises(SystemExit) as e:
                self.wraps(old + 'HLE_WRAP(7100001000) {}\n')
            self.assertIn('instruction words', str(e.exception))

    def test_refuses_a_signature_it_cannot_read(self):
        for bad in ('/* signature: 16 words, sha256 0123 */\n', '/* signature: sixteen words, sha256 0123456789abcdef */\n'):
            with self.subTest(bad=bad), self.assertRaises(SystemExit) as e:
                self.wraps(bad + 'HLE_WRAP(7100001000) {}\n')
            self.assertIn('not in the form', str(e.exception))

    # Hooks still waiting for theirs (tools/recomp.py wrap-sig prints it, from the game): the fight
    # test switches' frame end and player record builder, and their hooks by exported name: the
    # fighters' frame line, the system tick (both names) and the SDK's random bytes
    # (runtime/hle_fight_test.c; the frame end and the frame line in runtime/hle_each_frame.c).
    AWAITING = {0x71038701e0, 0x71017880c0}
    AWAITING_EXPORTS = {'_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv', '_ZN2nn2os13GetSystemTickEv',
                        'nnosGetSystemTick', '_ZN2nn2os19GenerateRandomBytesEPvm'}

    def test_the_runtimes_own_hooks_all_carry_a_hash(self):
        wraps, by_name = recomp.runtime_wraps()   # (raises on a signature that spells out words)
        self.assertGreater(len(wraps), 0)
        for key, sig in [*wraps.items(), *by_name.items()]:
            with self.subTest(hook=key if isinstance(key, str) else f'{key:x}'):
                if key in self.AWAITING | self.AWAITING_EXPORTS:
                    continue
                self.assertIsNotNone(sig, 'every hook says what the function it wraps begins with')
                self.assertTrue(1 <= sig[0] <= 16)

    def test_the_awaiting_ones_are_still_waiting(self):
        """(One given its signature comes off the lists above.)"""
        wraps, by_name = recomp.runtime_wraps()
        for address in self.AWAITING:
            with self.subTest(address=f'{address:x}'):
                self.assertIn(address, wraps)
                self.assertIsNone(wraps[address])
        for name in self.AWAITING_EXPORTS:
            with self.subTest(name=name):
                self.assertIn(name, by_name)
                self.assertIsNone(by_name[name])


class ResolveWraps(unittest.TestCase):
    BASE = recomp.MODULES['main']

    def resolve(self, signature_line, words=WORDS):
        with runtime_with(f'{signature_line}\nHLE_WRAP({self.BASE + 0x40:x}) {{}}\n') as root, \
                mock.patch.object(recomp, 'ROOT', root):
            mod = FakeModule([0] * 16 + words)
            return recomp.resolve_wraps(mod, self.BASE, {0x40: 4 * len(words)}, 'main')

    def comment(self, words=WORDS, n=16):
        return recomp.signature_comment(recomp.signature(FakeModule(words), 0, 4 * len(words), n))

    def test_a_matching_function_is_wrapped(self):
        self.assertEqual(self.resolve(self.comment()), {0x40: ('wrap', f'hle_wrap_{self.BASE + 0x40:x}')})
        self.assertEqual(self.resolve(self.comment(n=5)), {0x40: ('wrap', f'hle_wrap_{self.BASE + 0x40:x}')})

    def test_only_the_masked_parts_may_differ(self):
        moved = list(WORDS)
        moved[1] = 0x94000777   # the bl's target: where the code lies
        self.assertIn(0x40, self.resolve(self.comment(), moved))

    def test_a_different_function_stops_the_build(self):
        changed = list(WORDS)
        changed[9] = 0xAA0203E0   # mov x0, x2
        with self.assertRaises(SystemExit) as e:
            self.resolve(self.comment(), changed)
        self.assertIn('does not begin as its signature says', str(e.exception))

    def test_a_shorter_function_stops_the_build(self):
        with self.assertRaises(SystemExit):
            self.resolve(self.comment(), WORDS[:4])   # its first 4 words match, but the hash is of 11

    def test_no_function_there_stops_the_build(self):
        with runtime_with(f'HLE_WRAP({self.BASE + 0x44:x}) {{}}\n') as root, mock.patch.object(recomp, 'ROOT', root):
            with self.assertRaises(SystemExit) as e:
                recomp.resolve_wraps(FakeModule(WORDS), self.BASE, {0x40: 44}, 'main')
        self.assertIn('no function starts there', str(e.exception))


if __name__ == '__main__':
    unittest.main()
