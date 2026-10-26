"""tools/dev/fight_hash_diff.py on made-up SSBU_FIGHT_HASH logs: the first differing line, its frame,
fighter and fields, and what it leaves out (a cut last line, the game's other output, a line the
other output cut in two)."""
from pathlib import Path
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools' / 'dev'))
import fight_hash_diff  # noqa: E402


def made_up(frames=3, fighters=2, change=None, order=None, entry=None):
    """A made-up log in the hash line's format; change(frame, entry, values) may alter a line's
    values, order(frame) the order of the fighters' lines in a frame, entry(e) the e written. H
    runs over the lines, as the real one does over the records."""
    lines, run = [], 0xcbf29ce484222325
    for f in range(1, frames + 1):
        for e in (order(f) if order else range(fighters)):
            v = {'st': f'{0x10 * e + f:x}', 'mot': f'{0x1000 + f:x}', 'mf': f'{0x3f000000 + f:08x}', 'x': f'{0x41000000 + f + e:08x}',
                 'y': f'{0x3f400000 * e:08x}', 'dmg': f'{0x40600000 + f:08x}', 'rngcalls': str(100 + f), 'rng': f'{0xabc00 + f:08x}',
                 'rh': 0}
            if change:
                change(f, e, v)
            for word in (f, e, v['st'], v['mot'], v['mf'], v['x'], v['y'], v['dmg'], v['rngcalls'], v['rng'], v['rh']):
                run = ((run ^ zlib.crc32(str(word).encode())) * 0x100000001b3) & (1 << 64) - 1
            lines.append(f'F {f} e {entry(e) if entry else e} st {v["st"]} mot {v["mot"]} mf {v["mf"]} x {v["x"]} y {v["y"]} dmg {v["dmg"]} '
                         f'rngcalls {v["rngcalls"]} rng {v["rng"]} H {run:016x}  (1.000 2.000 frame {f}.00 3.50%)')
    return lines


class Diff(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def log(self, name, lines, tail=''):
        path = os.path.join(self.dir, name)
        with open(path, 'w', newline='') as f:
            f.write(''.join(line + '\n' for line in lines) + tail)
        return path

    def run_diff(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = fight_hash_diff.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def test_the_same(self):
        code, out, _ = self.run_diff(self.log('a', made_up()), self.log('b', made_up()))
        self.assertEqual(code, 0)
        self.assertTrue(out.endswith(': the same over 6 lines (6 / 6 lines; last frame 3)\n'), out)

    def test_a_cut_last_line_and_other_output_are_left_out(self):
        a = self.log('a', made_up(), tail=made_up(4)[6][:40])   # (a stopped run: its last line half written)
        b = self.log('b', ['[fight] first battle frame', *made_up()[:3], 'guest process exited', *made_up()[3:]])
        code, out, _ = self.run_diff(a, b)
        self.assertEqual(code, 0)
        self.assertIn('the same over 6 lines (6 / 6 lines; last frame 3)', out)

    def test_a_line_lost_from_a_log_moves_nothing_after_it(self):
        """(The standard output with the game's other output in it: a line another one cut in two
        is left out, and the lines after it are still paired by frame and fighter.)"""
        lines = made_up(4)
        b = self.log('b', [*lines[:2], lines[2][:50] + '[display] speed: 60 frames shown', '... ' + lines[2][50:], *lines[3:]])
        code, out, _ = self.run_diff(self.log('a', lines), b)
        self.assertEqual(code, 0, out)
        self.assertIn('the same over 7 lines (8 / 7 lines; last frame 4; 1 line in one log only, left out)', out)
        code, out, _ = self.run_diff(b, self.log('c', [lines[0], *lines[2:5], *lines[6:]]))   # (each log lost some)
        self.assertIn('the same over 5 lines (7 / 6 lines; last frame 4; 3 lines in one log only, left out)', out)
        # and a difference after it is still found where it is
        def moved(f, e, v):
            if (f, e) == (3, 1):
                v['x'] = '41200000'
        code, out, _ = self.run_diff(self.log('a', made_up(4, change=moved)), b)
        self.assertEqual(code, 1)
        self.assertIn(': first difference at line 6 / 5 of 8 / 7: frame 3 fighter e 1: fields x\n', out)

    def test_lines_in_another_order(self):
        """Two fighters' lines of one frame in another order: the same lines, but H (running over
        the lines in the order they were written) differs from there on."""
        code, out, _ = self.run_diff(self.log('a', made_up()), self.log('b', made_up(order=lambda f: (1, 0) if f == 2 else (0, 1))))
        self.assertEqual(code, 1)
        self.assertIn(': first difference at line 3 / 4 of 6 / 6: frame 2 fighter e 0: only H (the running hash', out)

    def test_no_entry_id(self):
        """(Every e -1: the fighters' lines of a frame are told apart by their order.)"""
        minus = lambda e: -1  # noqa: E731
        code, out, _ = self.run_diff(self.log('a', made_up(entry=minus)), self.log('b', made_up(2, entry=minus)))
        self.assertEqual(code, 0)
        self.assertIn('the same over 4 lines (6 / 4 lines; last frame 2)', out)
        def moved(f, e, v):
            if (f, e) == (2, 1):
                v['y'] = '0'
        code, out, _ = self.run_diff(self.log('a', made_up(entry=minus)), self.log('b', made_up(change=moved, entry=minus)))
        self.assertIn(': first difference at line 4 of 6 / 6: frame 2 fighter e -1: fields y\n', out)

    def test_no_line_in_both(self):
        code, out, _ = self.run_diff(self.log('a', made_up()), self.log('b', made_up(entry=lambda e: e + 5)))
        self.assertEqual(code, 1)
        self.assertIn(': no frame and fighter in both (6 / 6 lines)', out)

    def test_windows_line_ends(self):
        b = os.path.join(self.dir, 'b')
        with open(b, 'w', newline='') as f:
            f.write(''.join(line + '\r\n' for line in made_up()))
        self.assertEqual(self.run_diff(self.log('a', made_up()), b)[0], 0)

    def test_one_run_went_on(self):
        code, out, _ = self.run_diff(self.log('a', made_up(5)), self.log('b', made_up(3)))
        self.assertEqual(code, 0)
        self.assertIn('the same over 6 lines (10 / 6 lines; last frame 3)', out)

    def test_the_first_difference(self):
        def moved(f, e, v):
            if (f, e) >= (2, 1):
                v['x'] = '41200000'
        code, out, _ = self.run_diff(self.log('a', made_up()), self.log('b', made_up(change=moved)))
        self.assertEqual(code, 1)
        first = out.splitlines()[0]
        self.assertTrue(first.endswith(': first difference at line 4 of 6 / 6: frame 2 fighter e 1: fields x'), first)
        shown = out.splitlines()[1:]
        self.assertEqual(len(shown), 2 * 5)   # (2 lines either side)
        self.assertEqual([s[:4] for s in shown], ['   A', '   B'] * 2 + ['>  A', '>  B'] + ['   A', '   B'] * 2)
        self.assertIn(made_up()[3], shown[4])

    def test_several_fields_named_in_order(self):
        def turned(f, e, v):
            if f == 3 and e == 0:
                v['mot'], v['st'], v['rngcalls'], v['rng'] = '2000', '99', '7', '00000007'
        code, out, _ = self.run_diff(self.log('a', made_up()), self.log('b', made_up(change=turned)))
        self.assertEqual(code, 1)
        self.assertIn('at line 5 of 6 / 6: frame 3 fighter e 0: fields st mot rngcalls rng\n', out)

    def test_only_the_hash(self):
        def hidden(f, e, v):
            if (f, e) == (1, 1):
                v['rh'] = 1   # (in H, not on the line)
        code, out, _ = self.run_diff(self.log('a', made_up()), self.log('b', made_up(change=hidden)))
        self.assertEqual(code, 1)
        self.assertIn('at line 2 of 6 / 6: frame 1 fighter e 1: only H', out)

    def test_context(self):
        def moved(f, e, v):
            v['x'] = '0'
        code, out, _ = self.run_diff(self.log('a', made_up()), self.log('b', made_up(change=moved)), '--context', '0')
        self.assertEqual(code, 1)
        self.assertEqual(len(out.splitlines()), 3)

    def test_nothing_to_compare(self):
        a = self.log('a', made_up())
        code, _, err = self.run_diff(a, self.log('b', ['[fight] SSBU_FIGHT_HASH -> b'], tail='F 1 e 0 st'))
        self.assertEqual((code, err.strip()), (2, os.path.join(self.dir, 'b') + ': no hash lines'))
        code, _, err = self.run_diff(a, os.path.join(self.dir, 'missing'))
        self.assertEqual(code, 2)
        self.assertIn('missing', err)

    def test_usage(self):
        for args in ((), ('a',), ('a', 'b', 'c'), ('a', 'b', '--context')):
            with self.subTest(args=args):
                code, _, err = self.run_diff(*args)
                self.assertEqual(code, 2)
                self.assertIn('Usage: fight_hash_diff.py', err)

    def test_as_a_program(self):
        a, b = self.log('a', made_up()), self.log('b', made_up(2))
        r = subprocess.run([sys.executable, str(ROOT / 'tools' / 'dev' / 'fight_hash_diff.py'), a, b], capture_output=True, text=True)
        self.assertEqual((r.returncode, r.stderr), (0, ''))
        self.assertIn('the same over 4 lines (6 / 4 lines; last frame 2)', r.stdout)


if __name__ == '__main__':
    unittest.main()
