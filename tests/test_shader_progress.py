"""Prepare Shaders' progress (tools/shader_aot.py): shaderaot's "progress <done> <of>" lines from
every part, shown as one bar with the time left. No compiler run; made-up lines."""
from pathlib import Path
import io
import sys
import types
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import shader_aot as S  # noqa: E402


class Out(io.StringIO):
    def __init__(self, tty):
        super().__init__()
        self.tty = tty

    def isatty(self):
        return self.tty


class ProgressTests(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(S.parse_progress('progress 16 11601\n'), (16, 11601))
        self.assertIsNone(S.parse_progress('part 0/4: 11596 keys; ...'))
        self.assertIsNone(S.parse_progress('vs_0123: fxc failed: progress 1 2'))

    def test_time_left(self):
        self.assertEqual(S.time_left(30), 'under a minute left')
        self.assertEqual(S.time_left(25 * 60), 'about 25 min left')
        self.assertEqual(S.time_left(100 * 60), 'about 1 h 40 min left')

    def test_bar(self):
        self.assertEqual(S.progress_text(0, 3081), '[------------------------------]   0%  0 of 3,081 shaders')
        self.assertEqual(S.progress_text(1540, 3081, rate=2.0),
                         '[##############----------------]  50%  1,540 of 3,081 shaders, about 13 min left')
        self.assertEqual(S.progress_text(3081, 3081, rate=2.0), '[##############################] 100%  3,081 of 3,081 shaders')

    def test_log_lines_every_percent(self):
        out = Out(tty=False)
        show = S.Progress(out)
        show([(0, None), (0, None)], now=0)          # (the parts translating)
        show([(0, None), (0, None)], now=1)
        for t in range(0, 101):                       # 1 % a second, of 2 x 500
            show([(5 * t, 500), (5 * t, 500)], now=10 + t)
        show([(500, 500), (500, 500)], now=111, final=True)
        lines = out.getvalue().splitlines()
        self.assertEqual(lines[0], 'getting ready: translating the vertex programs...')
        self.assertEqual(len([l for l in lines if 'getting ready' in l]), 1)
        self.assertEqual(len(lines), 1 + 101)         # (0 % with the total, then every 1 %; the last not twice)
        self.assertEqual(lines[1], '[------------------------------]   0%  0 of 1,000 shaders')
        self.assertIn('  50%  500 of 1,000 shaders, about 1 min left', lines[51])
        self.assertTrue(lines[-1].endswith('100%  1,000 of 1,000 shaders'))

    def test_console_draws_over_one_line(self):
        out = Out(tty=True)
        show = S.Progress(out)
        show([(0, 100)], now=0)
        show([(50, 100)], now=20)
        show([(100, 100)], now=40, final=True)
        text = out.getvalue()
        self.assertEqual(text.count('\n'), 1)        # (one line, ended at the last)
        self.assertEqual(text.count('\r'), 3)

    def test_time_left_goes_by_the_recent_pace(self):
        show = S.Progress(Out(tty=False))
        for t in range(0, 301):                       # 1 shader a second for 300 s, then 10 a second
            show.rate(t, t)
        for t in range(301, 401):
            rate = show.rate(t, 300 + 10 * (t - 300))
        self.assertGreater(rate, 8)                   # (the last 2 minutes', not the average since the start)

    def test_summary(self):
        lines = ['part 0/2: 100 keys; 90 compiled, 8 already cached, 1 with jump tables (left out), 1 fxc failures, 0 not translated',
                 'part 1/2: 200 keys; 150 compiled, 50 already cached, 0 with jump tables (left out), 0 fxc failures, 2 not translated']
        self.assertEqual(S.summary(lines), '300 shaders: 240 compiled, 58 already there, 1 left out (jump tables), '
                                           '1 that fxc could not compile, 2 not translated')
        self.assertIsNone(S.summary(['something else']))

    def test_follow(self):
        def proc(lines):
            return types.SimpleNamespace(stderr=io.StringIO(''.join(l + '\n' for l in lines)), wait=lambda: 0)
        procs = [proc(['progress 0 20', 'progress 16 20', 'vs_1: fxc failed: x', 'progress 20 20', 'part 0/2: done']),
                 proc(['progress 0 10', 'progress 10 10', 'part 1/2: done'])]
        seen = []
        last = S.follow(procs, lambda state, final=False: seen.append((list(state), final)), every=0.01)
        self.assertEqual(sorted(last), ['part 0/2: done', 'part 1/2: done'])
        self.assertEqual(seen[-1], ([(20, 20), (10, 10)], True))


if __name__ == '__main__':
    unittest.main()
