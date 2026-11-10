"""Prepare Shaders' work queue (tools/shader_aot.py): the keys shaderaot --dry lists, which of
them the cache has, the order they are compiled in, the compile times kept between runs, and
the workers (shaderaot --serve) pulling from one queue. No compiler run: a fake worker, a
small Python script, answers the keys it is sent."""
from pathlib import Path
import io
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import shader_aot as S  # noqa: E402

VS = S.Key('vs', '0123456789abcdef', None, 224)
PS = S.Key('ps', 'fedcba9876543210', '00000007', 712)

# A worker as shaderaot --serve answers: "done <key> <outcome> <ms>" per key read, with the
# worker's number (FAKE_ID) as the ms, so that a test sees which worker took which key. A hash
# starting with "s1" takes a second, "s0" a twentieth of one (an instant answer lets the first
# worker's thread drain the whole queue before a later worker's process is up); "dead" ends the
# worker without an answer.
FAKE_WORKER = r'''
import os, sys, time
me = os.environ.get("FAKE_ID", "0")
sys.stderr.write("vkd3d:1:fixme:some chatter\n" * 3)
for line in sys.stdin:
    p = line.split()
    if p[1].startswith("dead"):
        sys.exit(3)
    if p[1].startswith("s1"):
        time.sleep(1.0)
    if p[1].startswith("s0"):
        time.sleep(0.2)   # (long enough that the last worker is up before the first two finish: a busy
                          # Windows runner took over 1.5 s to start it, and it got no key)
    outcome = "failed" if p[1].startswith("bad") else "compiled"
    sys.stderr.write("ps_x: shader compiler failed: no\n" if outcome == "failed" else "noise\n")
    sys.stdout.write("done " + " ".join(p) + " %s %s\n" % (outcome, me))
    sys.stdout.flush()
'''


def fake_popen_factory():
    """popen() for serve(): each worker a fresh FAKE_WORKER with its number in FAKE_ID."""
    count = [0]

    def popen():
        count[0] += 1
        env = {**os.environ, 'FAKE_ID': str(count[0])}
        return subprocess.Popen([sys.executable, '-c', FAKE_WORKER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, bufsize=1, env=env)
    return popen


def keys(n, kind='vs', prefix='a'):
    return [S.Key(kind, f'{prefix}{i:0{16 - len(prefix)}x}', None if kind == 'vs' else '00000001', 100 + i) for i in range(n)]


class LineTests(unittest.TestCase):
    def test_dry_lines(self):
        self.assertEqual(S.parse_key_line('vs 0123456789ABCDEF 224\n'), VS)
        self.assertEqual(S.parse_key_line('ps fedcba9876543210 00000007 712'), PS)
        self.assertEqual(S.parse_key_line('vs 0123456789abcdef'), VS._replace(size=0))   # (an older tool: no size)
        for bad in ('progress 1 2', 'vs 123', 'ps fedcba9876543210', '', 'cs 0123456789abcdef 1'):
            self.assertIsNone(S.parse_key_line(bad), bad)

    def test_key_and_request_line(self):
        self.assertEqual(VS.key, ('vs', '0123456789abcdef'))
        self.assertEqual(PS.key, ('ps', 'fedcba9876543210', '00000007'))
        self.assertEqual(VS.line, 'vs 0123456789abcdef')
        self.assertEqual(PS.line, 'ps fedcba9876543210 00000007')

    def test_done_lines(self):
        self.assertEqual(S.parse_done_line('done vs 0123456789abcdef compiled 22\n'), (VS.key, 'compiled', 22))
        self.assertEqual(S.parse_done_line('done ps fedcba9876543210 00000007 existing 0'), (PS.key, 'existing', 0))
        for bad in ('serving 26626 programs', 'done vs 0123456789abcdef', 'vkd3d:1:fixme: x', ''):
            self.assertIsNone(S.parse_done_line(bad), bad)


class CacheTests(unittest.TestCase):
    def test_file_names(self):
        self.assertEqual(S.cache_file_name(VS, 10), 'v10_vs_0123456789abcdef_00000000_0000000000000000.dxbc')
        self.assertEqual(S.cache_file_name(PS, 10), 'v10_ps_fedcba9876543210_00000007_0000000000000000.dxbc')
        self.assertEqual(S.parse_cache_file_name(S.cache_file_name(PS, 10), 10), PS.key)
        self.assertIsNone(S.parse_cache_file_name(S.cache_file_name(PS, 10), 9))          # another version
        self.assertIsNone(S.parse_cache_file_name('v10_ps_fedcba9876543210_00000007_00000000000000ab.dxbc', 10))   # jump tables
        self.assertIsNone(S.parse_cache_file_name('compile_times.txt', 10))

    def test_cached_keys(self):
        with tempfile.TemporaryDirectory() as d:
            for name in (S.cache_file_name(VS, 10), S.cache_file_name(PS, 10), 'v9_vs_0123456789abcdef_00000000_0000000000000000.dxbc',
                         'compile_times.txt', 'v10_ps_fedcba9876543210_00000007_0000000000000000.dxbc.123.tmp'):
                open(os.path.join(d, name), 'w').close()
            self.assertEqual(S.cached_keys(d, 10), {VS.key, PS.key})

    def test_spirv_file_names(self):
        g = 'g1001.01100600'
        self.assertEqual(S.spirv_file_name(VS, g), 'g1001.01100600_vs_0123456789abcdef_00000000_0000000000000000.spv')
        self.assertEqual(S.spirv_file_name(PS, g), 'g1001.01100600_ps_fedcba9876543210_00000007_0000000000000000.spv')
        self.assertEqual(S.parse_spirv_file_name(S.spirv_file_name(PS, g), g), PS.key)
        self.assertEqual(S.parse_spirv_file_name(S.spirv_file_name(VS, g), g), VS.key)
        self.assertIsNone(S.parse_spirv_file_name(S.spirv_file_name(PS, g), 'g1001.01100601'))   # another library
        self.assertIsNone(S.parse_spirv_file_name(S.spirv_file_name(PS, g), 'g1002.01100600'))   # another version
        self.assertIsNone(S.parse_spirv_file_name('g1001.01100600_ps_fedcba9876543210_00000007_00000000000000ab.spv', g))   # jump tables
        self.assertIsNone(S.parse_spirv_file_name('g1001.01100600_cs_fedcba9876543210_00000000_0000000000000000.spv', g))   # compute
        self.assertIsNone(S.parse_spirv_file_name(S.cache_file_name(PS, 10), g))

    def test_with_spirv_a_key_is_cached_when_both_its_files_are_there(self):
        g = 'g1001.01100600'
        with tempfile.TemporaryDirectory() as d:
            for name in (S.cache_file_name(VS, 10), S.cache_file_name(PS, 10), S.spirv_file_name(PS, g),
                         S.spirv_file_name(VS, 'g1001.01100500'),                     # (an older library's)
                         S.spirv_file_name(S.Key('ps', 'a' * 16, '00000001', 1), g)):  # (no DXBC beside it)
                open(os.path.join(d, name), 'w').close()
            self.assertEqual(S.cached_keys(d, 10, spirv=g), {PS.key})
            self.assertEqual(S.cached_keys(d, 10), {VS.key, PS.key})

    def test_an_alpha_tested_key_is_cached_under_its_salted_hash(self):
        g = 'g1001.01100600'
        pa = S.Key('pa', PS.hash, PS.mask, 1)
        salted = S.Key('ps', S.alpha_file_hash(PS.hash), PS.mask, 1)
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, S.cache_file_name(salted, 10)), 'w').close()
            self.assertTrue(S.cached(pa, S.cached_keys(d, 10)))
            self.assertFalse(S.cached(pa, S.cached_keys(d, 10, spirv=g)))
            open(os.path.join(d, S.spirv_file_name(salted, g)), 'w').close()
            self.assertTrue(S.cached(pa, S.cached_keys(d, 10, spirv=g)))

    def test_the_tool_is_asked_for_spirv_only_when_it_can_make_it(self):
        was = S.SPIRV
        try:
            S.SPIRV = None
            self.assertEqual(S.tool_command('t.exe', 'pack', 'cache', '--serve'), ['t.exe', 'pack', 'cache', '--serve'])
            S.SPIRV = 'g1001.01100600'
            self.assertEqual(S.tool_command('t.exe', 'pack', 'cache', '0', '4'), ['t.exe', 'pack', 'cache', '0', '4', '--spirv'])
        finally:
            S.SPIRV = was

    def test_no_spirv_when_it_is_turned_off(self):
        was = os.environ.get('SSBU_AOT_SPIRV')
        os.environ['SSBU_AOT_SPIRV'] = '0'
        try:
            self.assertIsNone(S.spirv_names('no such tool.exe', log=lambda *_: None))
        finally:
            if was is None:
                del os.environ['SSBU_AOT_SPIRV']
            else:
                os.environ['SSBU_AOT_SPIRV'] = was


class TimesTests(unittest.TestCase):
    def test_missing_file_is_empty(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(S.load_times(d), {})

    def test_round_trip_and_merge(self):
        with tempfile.TemporaryDirectory() as d:
            S.save_times(d, {VS.key: 22, PS.key: 7})
            self.assertEqual(S.load_times(d), {VS.key: 22, PS.key: 7})
            merged = S.save_times(d, {PS.key: 9, ('vs', 'b' * 16): 1})   # (a later run: its records added, one replaced)
            self.assertEqual(merged, {VS.key: 22, PS.key: 9, ('vs', 'b' * 16): 1})
            self.assertEqual(S.load_times(d), merged)

    def test_damaged_lines_are_skipped(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, S.TIMES_FILE), 'w') as f:
                f.write('vs 0123456789abcdef 22\nps fedcba9876543210 00000007 x\nnonsense\nps fedcba9876543210 00000007 7\nvs aaaa')   # (cut short)
            self.assertEqual(S.load_times(d), {VS.key: 22, PS.key: 7})


class OrderTests(unittest.TestCase):
    def test_untimed_first_then_longest(self):
        a, b, c, d = keys(4)
        order = S.order([a, b, c, d], {b.key: 5, c.key: 50})
        self.assertEqual(order, [d, a, c, b])   # untimed by size (d bigger than a), then the timed ones longest first

    def test_untimed_vertex_before_pixel_larger_first(self):
        v1, v2 = keys(2, 'vs', 'a')
        p1, p2 = keys(2, 'ps', 'b')
        self.assertEqual(S.order([p2, v1, p1, v2], {}), [v2, v1, p2, p1])

    def test_ties_keep_the_order_given(self):
        same = [S.Key('vs', f'{i:016x}', None, 100) for i in range(5)]
        self.assertEqual(S.order(same, {}), same)
        self.assertEqual(S.order(same, {k.key: 3 for k in same}), same)


class WeightTests(unittest.TestCase):
    def test_timed_keys_weigh_their_time(self):
        a, b = keys(2)
        self.assertEqual(S.weights([a, b], {a.key: 500, b.key: 20}), {a.key: 500.0, b.key: 20.0})

    def test_untimed_keys_weigh_their_size_at_the_timed_ones_rate(self):
        a, b, c = keys(3)                                   # sizes 100, 101, 102
        w = S.weights([a, b, c], {a.key: 1000})             # a: 10 ms a byte
        self.assertEqual(w[a.key], 1000.0)
        self.assertEqual(w[b.key], 101 * 10 + 1)
        self.assertEqual(w[c.key], 102 * 10 + 1)

    def test_an_alpha_tested_key_weighs_as_a_pixel_program(self):
        pa = S.Key('pa', 'c' * 16, '00000001', 100)   # (drawn_keys.txt's kind: it stopped the queue with a KeyError)
        self.assertAlmostEqual(S.weights([pa], {})[pa.key], 100 * S.MS_PER_BYTE['ps'] + 1)

    def test_without_any_time_the_kinds_have_their_own_rate(self):
        v, = keys(1, 'vs', 'a')
        p, = keys(1, 'ps', 'b')
        w = S.weights([v, p], {})
        self.assertAlmostEqual(w[v.key], 100 * S.MS_PER_BYTE['vs'] + 1)
        self.assertAlmostEqual(w[p.key], 100 * S.MS_PER_BYTE['ps'] + 1)
        self.assertGreater(w[v.key], w[p.key])
        self.assertEqual(S.weights([VS._replace(size=0)], {}), {VS.key: 1.0})   # (an older tool, no size: never nothing)


class ServeTests(unittest.TestCase):
    def serve(self, todo, jobs, popen=None, times=None):
        times = {} if times is None else times
        shown = []
        self.out = io.StringIO()
        totals = S.serve('exe', 'pack', 'cache', todo, jobs, times,
                         lambda state, final=False, work=None: shown.append((list(state), final, work)),
                         every=0.01, popen=popen or fake_popen_factory(), out=self.out)
        return totals, times, shown

    def test_every_key_once_and_the_totals_add_up(self):
        todo = keys(30, prefix='s0')   # (not instant answers: the last worker started must get keys too)
        totals, times, shown = self.serve(todo, 3)
        self.assertEqual(totals, {'compiled': 30})
        self.assertEqual(set(times), {k.key for k in todo})
        self.assertEqual(shown[-1][:2], ([(30, 30)], True))
        self.assertEqual(set(times.values()), {1, 2, 3})   # (every worker took some)

    def test_the_first_keys_go_one_to_each_worker(self):
        # Longest first, the two longest must not wait for each other: a worker is given its
        # second key once it has answered its first, never ahead of it.
        slow = [S.Key('vs', f's1{i:014x}', None, 100) for i in range(2)]
        totals, times, _shown = self.serve(slow + keys(4), 2)
        self.assertEqual(totals, {'compiled': 6})
        self.assertNotEqual(times[slow[0].key], times[slow[1].key])

    def test_progress_carries_the_work_done_by_weight(self):
        todo = keys(6)
        given = {k.key: 1000 * (i + 1) for i, k in enumerate(todo)}   # (6 timed keys, 21,000 ms of work in all)
        _totals, _times, shown = self.serve(todo, 2, times=dict(given))
        state, final, work = shown[-1]
        self.assertEqual((state, final), ([(6, 6)], True))
        self.assertEqual(work, (21000.0, 21000.0))
        works = [w for _s, _f, w in shown]
        self.assertTrue(all(w[1] == 21000.0 for w in works))
        self.assertEqual(works, sorted(works))                           # (only ever up)

    def test_a_slow_worker_does_not_hold_the_others(self):
        todo = [S.Key('vs', 's1' + 'a' * 14, None, 100)] + keys(20)
        totals, times, _shown = self.serve(todo, 2)
        self.assertEqual(totals, {'compiled': 21})
        slow_worker = times[todo[0].key]
        taken_by_slow = sum(1 for k in todo[1:] if times[k.key] == slow_worker)
        self.assertEqual(taken_by_slow, 0)                        # (nothing waited behind it)

    def test_a_failed_compile_is_counted_and_said(self):
        todo = keys(5) + [S.Key('vs', 'bad' + 'a' * 13, None, 100)]
        totals, times, _shown = self.serve(todo, 2)
        self.assertEqual(totals, {'compiled': 5, 'failed': 1})
        self.assertNotIn(todo[-1].key, times)   # (no time recorded for a failure)
        self.assertIn('shader compiler failed', self.out.getvalue())

    def test_a_worker_that_stops_loses_only_its_pending_keys(self):
        todo = keys(12) + [S.Key('vs', 'dead' + 'a' * 12, None, 100)] + keys(12, 'vs', 'c')
        totals, _times, shown = self.serve(todo, 2)
        self.assertEqual(sum(totals.values()), len(todo))
        self.assertGreaterEqual(totals['died'], 1)
        self.assertLessEqual(totals['died'], S.Worker.IN_FLIGHT)
        self.assertEqual(totals['compiled'] + totals['died'], len(todo))
        self.assertEqual(shown[-1][:2], ([(len(todo), len(todo))], True))

    def test_times_are_saved_as_it_goes_and_at_the_end(self):
        todo = [S.Key('vs', 's1' + 'a' * 14, None, 100)] + keys(6)
        saves = []
        times = {}
        S.serve('exe', 'pack', 'cache', todo, 2, times, lambda state, final=False, work=None: None, every=0.01,
                popen=fake_popen_factory(), out=io.StringIO(), save=lambda t, last: saves.append((dict(t), last)), save_every=0.3)
        self.assertGreaterEqual(len(saves), 3)                       # (a second's run: two or three on the way, one at the end)
        self.assertEqual([last for _t, last in saves].count(True), 1)
        self.assertTrue(saves[-1][1])
        self.assertEqual(saves[-1][0], times)                        # (the last one has everything)
        self.assertTrue(all(set(t) <= set(times) for t, _l in saves))

    def test_no_more_workers_than_keys_is_the_callers_job_but_extra_workers_are_harmless(self):
        totals, _times, _shown = self.serve(keys(2), 4)
        self.assertEqual(totals, {'compiled': 2})


if __name__ == '__main__':
    unittest.main()


class DrawnKeysTests(unittest.TestCase):
    """drawn_keys.txt: programs the game compiled at a first draw, with their variant (2026-11-09)."""

    def test_parse_and_cache_names(self):
        k = S.parse_key_line('pa 0123456789abcdef 0000000f')
        self.assertEqual((k.kind, k.hash, k.mask), ('pa', '0123456789abcdef', '0000000f'))
        self.assertEqual(k.line, 'pa 0123456789abcdef 0000000f')
        salted = S.alpha_file_hash('0123456789abcdef')   # (gpu_d3d11.c: (h ^ salt) * FNV prime, 64 bits)
        self.assertEqual(salted, f"{((0x0123456789abcdef ^ 0x616C706861743032) * 1099511628211) & (2**64 - 1):016x}")
        have = {S.key_of('ps', salted, '0000000f')}
        self.assertTrue(S.cached(k, have))
        self.assertFalse(S.cached(S.parse_key_line('ps 0123456789abcdef 0000000f'), have))
        self.assertEqual(S.parse_done_line('done pa 0123456789abcdef 0000000f compiled 12'), (('pa', '0123456789abcdef', '0000000f'), 'compiled', 12))

    def test_drawn_keys_file(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, S.DRAWN_KEYS), 'w') as f:
                f.write('vs 00000000000000aa\nps 00000000000000bb 00000003\nps 00000000000000bb 00000003\npa 00000000000000bb 00000003\njunk\n')
            keys = S.drawn_keys(d)
            self.assertEqual(sorted(k.line for k in keys), ['pa 00000000000000bb 00000003', 'ps 00000000000000bb 00000003', 'vs 00000000000000aa'])
        self.assertEqual(S.drawn_keys(os.path.join(d, 'gone')), [])
