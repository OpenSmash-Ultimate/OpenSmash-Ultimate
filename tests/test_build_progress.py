"""The build's progress bar (tools/progress.py, recomp.py all): the estimate of what is left and the
time it takes, the bar drawn under what the build prints, the compiler's start and end reports. No
compiler run; made-up modules and a made-up clock."""
from pathlib import Path
import io
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402
import ladder  # noqa: E402
import progress as P  # noqa: E402
import recomp  # noqa: E402


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class Out(io.StringIO):
    def __init__(self, tty):
        super().__init__()
        self.tty = tty

    def isatty(self):
        return self.tty


def screen(text):
    """The lines a console shows for this output (\\r goes back to the line's start)."""
    lines, cur, col = [], [], 0
    for ch in text:
        if ch == '\n':
            lines.append(''.join(cur).rstrip())
            cur, col = [], 0
        elif ch == '\r':
            col = 0
        else:
            cur[col:col + 1] = [ch]
            col += 1
    return lines + ([''.join(cur).rstrip()] if ''.join(cur).strip() else [])


class EstimateTests(unittest.TestCase):
    def test_file_cost_grows_with_the_biggest_function(self):
        self.assertLess(P.file_cost(40000, 5000), 5)
        self.assertGreater(P.file_cost(210157, 210157), 400)   # lua2cpp_item's: ten minutes, measured
        self.assertGreater(P.file_cost(107744, 107744), 20 * P.file_cost(40000, 5000))

    def test_makespan(self):
        self.assertEqual(P.makespan([3, 3, 3], [0, 0]), 6)
        self.assertEqual(P.makespan([1, 1], [5, 0]), 5)          # (both on the free worker)
        self.assertEqual(P.makespan([10, 1, 1, 1], [0, 0]), 10)  # one big file holds the end
        self.assertEqual(P.makespan([], [0, 0]), 0)

    def plan(self, clock):
        b = P.Build(2, clock=clock)
        b.module('main', 400000)                               # C to write: a guess until then
        b.module('sdk', 80000, [('sdk/g0000.c', 4.0), ('sdk/g0001.c', 4.0)])   # C there: these two to compile
        return b

    def test_share_done_and_time_left(self):
        clock = Clock()
        b = self.plan(clock)
        self.assertEqual(b.status(), (0.0, None, ''))
        self.assertEqual(b.cost[('compile', 'sdk')], 4.0)       # two files on two compilers

        b.begin('gen', 'main')
        clock.now = 1
        _f, left, label = b.status()
        self.assertIsNone(left)                                  # (nothing compiled yet: not known)
        self.assertEqual(label, 'writing the C code: main, 1 of 2 modules')
        b.written('main', [(f'main/g{i:04d}.c', 20.0) for i in range(4)] + [('main/g_module.c', 1.0)])
        b.end('gen', 'main')
        b.begin('gen', 'sdk')
        b.end('gen', 'sdk')
        self.assertEqual(b.cost[('compile', 'main')], 41.0)     # 4 x 20 on two, then the table

        b.begin('compile', 'main', b.files['main'])
        for i in range(4):                                      # this PC takes twice the estimate
            b.started((f'main/g{i:04d}.c',))
        clock.now = 41
        for i in range(4):
            b.finished((f'main/g{i:04d}.c',), 40.0)
        frac, left, label = b.status()
        self.assertEqual(label, 'compiling main: 4 of 5 files')
        self.assertAlmostEqual(b.pace(clock.now), 2.0)
        rest = 1.0 + 4.0 + P.LINK                               # the table, sdk's compile, the link
        self.assertAlmostEqual(left, 2 * rest)
        self.assertAlmostEqual(frac, 1 - rest / sum(b.cost.values()))

    def test_a_long_file_counts_its_time_so_far(self):
        clock = Clock()
        b = P.Build(1, clock=clock)
        b.module('item', 300000, [('item/g0000.c', 80.0), ('item/g0001.c', 400.0)])
        b.begin('compile', 'item', b.files['item'])
        b.started(('item/g0000.c',))
        clock.now = 80
        b.finished(('item/g0000.c',), 80.0)                    # pace 1
        b.started(('item/g0001.c',))
        _f, left0, label = b.status()
        self.assertEqual((left0, label), (400 + P.LINK, 'compiling item: 1 of 2 files'))
        clock.now = 280
        frac1, left1, label = b.status()
        self.assertAlmostEqual(left0 - left1, 200)              # the bar moves while the big file compiles
        self.assertEqual(label, 'compiling item: 1 of 2 files - a big one takes minutes')
        clock.now = 480                                         # its estimate gone by: a quarter of its time again
        self.assertAlmostEqual(b.status()[1], 100 + P.LINK)
        clock.now = 880
        frac2, left2, _l = b.status()
        self.assertAlmostEqual(left2, 200 + P.LINK)             # (taking longer: so does the time left)
        self.assertGreaterEqual(frac2, frac1)

    def test_never_backwards(self):
        clock = Clock()
        b = P.Build(4, clock=clock)
        b.module('a', 40000, [('a/g0000.c', 1.0)])
        b.module('b', 40000)
        b.begin('compile', 'a', b.files['a'])
        b.started(('a/g0000.c',))
        b.finished(('a/g0000.c',), 1.0)
        b.end('compile', 'a')
        before = b.status()[0]
        b.written('b', [('b/g0000.c', 5000.0)])                # its C shows a lot more to compile
        self.assertEqual(b.status()[0], before)

    def test_a_file_reported_twice_is_counted_once(self):
        b = P.Build(4, clock=Clock())
        b.module('a', 40000, [('a/g0000.c', 1.0), ('a/g0001.c', 1.0)])
        b.begin('compile', 'a', b.files['a'])
        for _ in range(2):   # (a compile reported finished again: the counter once said 2835 of 2660)
            b.started(('a/g0000.c',))
            b.finished(('a/g0000.c',), 1.0)
        self.assertEqual(b.compiled, 1)
        self.assertIn('1 of 2 files', b.label)

    def test_pace_goes_by_the_last_ten_minutes(self):
        clock = Clock()
        b = P.Build(1, clock=clock)
        for t in range(0, 1200, 10):                            # a slow start, then 1:1
            clock.now = t
            b.samples.append((t, 30.0 if t < 300 else 10.0, 10.0))
        self.assertAlmostEqual(b.pace(clock.now), 1.0)
        self.assertIsNone(P.Build(1).pace(0))

    def test_generate_only_goes_by_the_pace_of_writing_c(self):
        clock = Clock()
        b = P.Build(4, compile=False, link=None, clock=clock)   # (recomp.py all --generate-only, the Linux build's)
        b.module('main', 30_000_000)
        b.module('sdk', 30_000_000, [('sdk/g0000.c', 4.0)])     # (C there: nothing to write, nothing to compile)
        b.module('item', 30_000_000)
        self.assertEqual(sum(b.cost.values()), 2 * (P.GEN[0] + P.GEN[1] * 30_000_000))
        b.begin('gen', 'main')
        clock.now = 31
        b.end('gen', 'main')                                    # twice its estimate
        b.begin('gen', 'sdk')
        b.end('gen', 'sdk')
        b.begin('gen', 'item')
        frac, left, label = b.status()
        self.assertEqual(label, 'writing the C code: item, 3 of 3 modules')
        self.assertAlmostEqual(frac, 0.5)
        self.assertAlmostEqual(left, 31)

    def test_linux_build_has_no_c_to_write(self):
        clock = Clock()
        b = P.Build(2, link=None, clock=clock)                  # (build_linux.py --runtime-only)
        b.module('runtime', 0, [('rt/host.c', 2.0), ('rt/gpu_d3d11.c', 6.0)])
        self.assertEqual(sum(b.cost.values()), 6.0)
        b.begin('compile', 'runtime', b.files['runtime'])
        self.assertEqual(b.status()[2], 'compiling runtime: 0 of 2 files')
        b.module('game', 0, [(f'g{i}.c', 1.0) for i in range(1500)])
        b.end('compile', 'runtime')
        b.begin('compile', 'game', b.files['game'])
        self.assertEqual(b.status()[2], 'compiling game: 0 of 1,500 files')


class ChainTests(unittest.TestCase):
    """Programs run one after another (the Mods app's plugin build) as one bar: it never starts over,
    and its share done and time left say the same thing."""

    def test_seconds_left_reads_time_left_back(self):
        for s in (5 * 60, 59 * 60, 61 * 60, 2 * 3600 + 7 * 60):
            self.assertEqual(P.seconds_left(P.time_left(s)), s)
        self.assertEqual(P.seconds_left('under a minute left'), 30.0)
        self.assertIsNone(P.seconds_left('estimating the time left'))
        self.assertIsNone(P.seconds_left('done'))

    def test_one_bar_across_programs(self):
        clock = Clock()
        c = P.Chain(later=180, clock=clock)
        shown = []
        # a plugin's C: 20 s, its own bar 0 -> 100 %
        c.start()
        for t, frac, left in ((0, 0.0, None), (10, 0.5, 10), (20, 1.0, None)):
            clock.now = t
            shown.append(c.update(frac, left))
        # the relink: its own bar starts at 0 again, and has no time left at first
        clock.now = 20
        c.start(later=0)
        for t, frac, left in ((21, 0.0, None), (60, 0.04, None), (120, 0.5, 100), (219, 0.99, 1), (220, 1.0, None)):
            clock.now = t
            shown.append(c.update(frac, left))
        fracs = [f for f, _l in shown]
        self.assertEqual(fracs, sorted(fracs))                     # never backwards
        self.assertLess(fracs[2], 0.2)                             # the plugin's 100 % is not the whole's
        self.assertLess(max(fracs), 1.0)                           # done only when the caller says so
        self.assertAlmostEqual(shown[5][0], 120 / 220)             # spent / (spent + left)
        self.assertEqual(shown[5][1], 100)

    def test_run_logged_keeps_one_bar_across_programs(self):
        import skyline
        prog = ("import progress as P\n"
                "for f in (0.0, 0.5, 1.0): print(P.line_text(f, 30 if f < 1 else 0, 'writing x C'), flush=True)\n")
        cmd = [sys.executable, '-c', f'import sys; sys.path.insert(0, {str(ROOT / "tools")!r})\n' + prog]
        got = []
        with mock.patch.object(skyline, '_CHAIN', None):
            skyline.chain().later = 600   # (a relink still to come)
            skyline.run_logged(cmd, got.append)
            skyline.run_logged(cmd, got.append, later=0)
        pcts = [int(line.split(' — ')[1].split('%')[0]) for line in got]
        self.assertEqual(len(pcts), 6)
        self.assertEqual(pcts, sorted(pcts))
        self.assertLess(pcts[2], 50)                               # the first program's 100 % is not the whole's
        self.assertTrue(all(line.startswith('writing x C — ') for line in got))   # (its last line names nothing)

    def test_unknown_time_left_holds_the_share(self):
        clock = Clock()
        c = P.Chain(clock=clock)
        clock.now = 5
        self.assertEqual(c.update(0.5, None), (0.0, None))         # too early to go by its pace
        clock.now = 20
        f, left = c.update(0.5, None)                              # its share at its pace so far
        self.assertAlmostEqual(left, 20)
        self.assertAlmostEqual(f, 0.5)


class LineTests(unittest.TestCase):
    def test_a_bar_printed_under_the_line_is_left_out(self):
        for tty in (False, True):
            out = Out(tty=tty)
            with mock.patch('sys.stdout', out), mock.patch('sys.stderr', out):
                with P.Line(lambda: (0.04, None, 'writing the C code: main, 1 of 101 modules'), out=out):
                    print('main: 12 files, 25 MB of C')
                    print(P.line_text(0.5, 30, "writing main's C, 6 of 12 files"), flush=True)
                    print(P.line_text(1.0, 0), flush=True)
                    print('')
                    print('sdk: C up to date')
            text = out.getvalue()
            self.assertNotIn("writing main's C", text)
            self.assertNotIn('100%', text)
            for line in ('main: 12 files, 25 MB of C', 'sdk: C up to date'):
                self.assertIn(line, text)
            if not tty:   # (the program's own blank line stays; the left-out bars leave none)
                self.assertIn('main: 12 files, 25 MB of C\n\nsdk: C up to date\n', text)

    def test_text(self):
        self.assertEqual(P.line_text(0.5, 25 * 60, 'compiling main: 120 of 394 files'),
                         '[###############---------------]  50%  about 25 min left  (compiling main: 120 of 394 files)')
        self.assertEqual(P.line_text(0.999, None), '[#############################-]  99%  estimating the time left')
        self.assertEqual(P.line_text(1.0, 0, 'linking'), '[##############################] 100%  done')

    def test_console_prints_go_above_the_bar(self):
        out = Out(tty=True)
        state = [(0.0, None, 'writing the C code')]
        with mock.patch.object(P.shutil, 'get_terminal_size', return_value=os.terminal_size((200, 25))),                 mock.patch('sys.stdout', out), mock.patch('sys.stderr', out):
            with P.Line(lambda: state[0], out=out) as line:
                print('main: 393 files')
                state[0] = (0.5, 600, 'compiling main: 1 of 394 files')
                line.show(*state[0])
                print('main: compiled', end='')
                print(' 394 of 394 stale files')
                sys.stderr.write('a warning\n')
                state[0] = (1.0, 0, 'linking')
        self.assertEqual(screen(out.getvalue()), [
            'main: 393 files', 'main: compiled 394 of 394 stale files', 'a warning',
            P.line_text(1.0, 0, 'linking')])
        self.assertNotIsInstance(sys.stdout, P._Through)

    def test_console_bar_fits_the_window(self):
        out = Out(tty=True)
        with mock.patch.object(P.shutil, 'get_terminal_size', return_value=os.terminal_size((40, 25))):
            with P.Line(lambda: (0.25, 3600, 'compiling lua2cpp_item: 1 of 63 files'), out=out):
                pass
        self.assertEqual(screen(out.getvalue()), [P.line_text(0.25, 3600, 'compiling lua2cpp_item: 1 of 63 files')[:39].rstrip()])

    def test_log_a_line_a_percent(self):
        # (PR #54: a pipe gets every new percent, so the launcher's and the Mods window's bars move;
        # a change of the step alone waits 15 seconds: test_log_a_step_change_waits)
        out, clock = Out(tty=False), Clock()
        line = P.Line(None, out=out, clock=clock)
        for t in range(0, 600):                                 # 1 % every 6 seconds
            clock.now = t
            line.show(t / 600, 600 - t, 'compiling')
        line.show(1.0, 0, 'linking', final=True)
        lines = out.getvalue().splitlines()
        self.assertEqual(lines[0], P.line_text(0.0, 600, 'compiling'))
        self.assertEqual(len(lines), 1 + 99 + 1)               # (at 0 s, each percent from 6 s to 594 s, then the last)
        self.assertEqual(lines[-1], P.line_text(1.0, 0, 'linking'))
        out2 = Out(tty=False)
        line = P.Line(None, out=out2, clock=clock)
        for t in range(0, 600):                                 # nothing moves: one line
            clock.now = 1000 + t
            line.show(0.3, None, 'compiling lua2cpp_item: 62 of 63 files')
        self.assertEqual(len(out2.getvalue().splitlines()), 1)

    def test_log_a_step_change_waits(self):
        out, clock = Out(tty=False), Clock()
        line = P.Line(None, out=out, clock=clock)
        for t in range(0, 60):                                  # a new file count every second, the same percent
            clock.now = t
            line.show(0.3, None, f'compiling lua2cpp_item: {t} of 63 files')
        self.assertEqual(len(out.getvalue().splitlines()), 4)  # (at 0, 15, 30 and 45 s)

    def test_terminal_title_on_linux(self):
        class Tty(io.StringIO):
            def close(self):
                self.closed_ = True
        tty = Tty()
        with mock.patch.object(P.sys, 'platform', 'linux'), mock.patch.dict(os.environ, {'TERM': 'xterm-256color'}), \
                mock.patch.object(P, 'open', create=True, return_value=tty) as opened:
            saved = P.save_title()
            P.set_title(saved, '45% - about 12 min left - building ssbu')
            P.restore_title(saved)
        opened.assert_called_once_with('/dev/tty', 'w')         # (the terminal itself: the output may go through tee)
        self.assertEqual(tty.getvalue(), '\x1b[22;0t' '\x1b]0;45% - about 12 min left - building ssbu\x07' '\x1b[23;0t')
        self.assertTrue(tty.closed_)
        with mock.patch.object(P.sys, 'platform', 'linux'), mock.patch.dict(os.environ, {'TERM': 'dumb'}):
            self.assertIsNone(P.save_title())

    def test_a_broken_estimate_does_not_stop_the_build(self):
        out = Out(tty=False)
        with mock.patch('sys.stdout', out), P.Line(lambda: 1 / 0, out=out):
            print('still building')
        self.assertIn('still building', out.getvalue())
        self.assertIn('no progress shown', out.getvalue())


class CompileReportTests(unittest.TestCase):
    def test_compile_each_reports_each_file(self):
        seen = []
        ok, bad = ladder.Run(0, '', 0.5), ladder.Run(1, 'error: x', 0.5)
        with mock.patch.object(ladder, 'run', side_effect=lambda args, *a, **kw: bad if 'b.c' in args else ok):
            failures = cc.compile_each([('a.c', 'a.obj', '/O2'), ('b.c', 'b.obj', '/O2')], 2,
                                       lambda job: seen.append(('start', job[0])),
                                       lambda job, secs: seen.append(('end', job[0], secs >= 0)))
        self.assertEqual(failures, [('b.c', 'error: x')])
        self.assertEqual(sorted(seen), [('end', 'a.c', True), ('end', 'b.c', True), ('start', 'a.c'), ('start', 'b.c')])

    def test_compile_dir_follows_the_stale_files(self):
        def fake(jobs, n, started=None, finished=None, compiled=None, flags=None, **kw):
            for job in jobs:
                if started:
                    started(job)
                Path(job[1]).write_text('')
                if finished:
                    finished(job, 1.0)
                compiled(job, job[2])
            return []
        with tempfile.TemporaryDirectory() as d:
            for n in ('g0000.c', 'g0001.c', 'g_module.c'):
                Path(d, n).write_text('int x;\n')
            Path(d, 'manifest.tsv').write_text('g0000.c\t40000\t5000\ng0001.c\t210157\t210157\n')
            with mock.patch.object(cc, 'compile_each', side_effect=fake), mock.patch('sys.stdout', io.StringIO()):
                self.assertTrue(recomp.compile_dir(d, 4))   # (every file)
            for n in ('g0001.c', 'g_module.c'):   # g0000.obj up to date, these two not
                Path(d, n).write_text('int y;\n')
            todo = recomp.stale_files(d)
            self.assertEqual([(os.path.basename(t[0]), t[2]) for t in todo], [('g0001.c', '/O1'), ('g_module.c', '/O1')])
            self.assertAlmostEqual(todo[0][3], P.file_cost(210157, 210157))

            b = P.Build(4)
            b.module(os.path.basename(d), 250000, [(t[0], t[3]) for t in todo])
            with mock.patch.object(cc, 'compile_each', side_effect=fake), mock.patch('sys.stdout', io.StringIO()):
                self.assertTrue(recomp.compile_dir(d, 4, b))
            self.assertEqual(b.done, {('compile', os.path.basename(d))})
            self.assertEqual(len(b.samples), 2)
            self.assertEqual(recomp.stale_files(d), [])


if __name__ == '__main__':
    unittest.main()
