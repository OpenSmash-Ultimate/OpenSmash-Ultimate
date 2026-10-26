"""The compile ladder (tools/ladder.py) in the three builds: a file the compiler crashed on compiled
again on its own, then a level lower (on Linux and macOS as on Windows); a compile over its time
limit stopped with every process it started and nothing half-written left, then compiled again with
its longest functions unoptimised (optnone, from a header given before the file), then whole at -O0
or /Od; what the ladder compiled kept until what it is built from changes (objcache, macOS's
game_sources.tsv); a build stopped (Ctrl+Z) not counted against a time limit, a build too busy to
look counted where waitid tells the two apart (else, as on Windows, a late look taken for a pause such
as Modern Standby); a header that cannot be written; each file's seconds kept for the next build's time left (and its order, with
SSBU_LONGEST_FIRST=1); the report; SSBU_OPTNONE_ABOVE; and every compile command, and the order they
start in, as before when nothing goes wrong, on a first build and on the next. Made-up sources and
the made-up compiler of tests/test_objcache.py; the last tests run the real clang on made-up C, where
there is one (optnone through -include and /FI, a chain of musttail calls at -O0, a real timeout)."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import contextlib
import importlib.util
import io
import json
import os
import re
import runpy
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
import warnings
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
import cc  # noqa: E402
import ladder  # noqa: E402
import objcache  # noqa: E402
import progress as P  # noqa: E402
import recomp  # noqa: E402
import test_objcache as made_up  # noqa: E402  (the made-up compiler)

POSIX = os.name != 'nt'
LIMIT = {'SSBU_COMPILE_LIMIT': '2'}   # (seconds: the made-up slow compiles run a minute)
SMALL = (40, 10, 2)                   # ladder.THRESHOLDS for the made-up files' short functions
CLANG = shutil.which('clang')


def generated(*functions, note=''):
    """A generated file as tools/lift.py writes one: each (name, instructions) function a line an
    instruction (the first its flags' locals), its labels at the start of a line; a native forwarder."""
    text = '#include "recomp.h"\n' + (f'/* {note} */\n' if note else '') + '\n'
    text += ''.join(f'GuestFn {name};\n' for name, _n in functions) + 'GuestFn hle_x;\n\n'
    for name, n in functions:
        text += (f'void {name}(Cpu *c) {{\n    u32 nf = 0, zf = 0, cf = 0, vf = 0;\nL_10:;\n'
                 + ''.join(f'    X(0) = X(0) + {i};\n' for i in range(n - 1)) + '}\n\n')
    return text + 'void f_7100009000(Cpu *c) { hle_x(c); }\n'


def gone(pids, wait=10.0):
    """Whether these processes have all ended (a killed one goes once something reaps it)."""
    end = time.time() + wait
    while time.time() < end:
        alive = []
        for pid in pids:
            try:
                os.kill(pid, 0)
                alive.append(pid)
            except ProcessLookupError:
                pass
            except PermissionError:
                alive.append(pid)
        if not alive:
            return True
        time.sleep(0.1)
    return False


@contextlib.contextmanager
def started():
    """Each compiler started, as (its command, what else Popen was given) - not the --version runs."""
    starts, popen = [], subprocess.Popen

    def record(args, *a, **kw):
        if '--version' not in args and 'ps' not in args[:1]:
            starts.append((list(args), {k: v for k, v in kw.items() if k not in ('stdout', 'stderr', 'text', 'errors')}))
        return popen(args, *a, **kw)
    with mock.patch.object(subprocess, 'Popen', record):
        yield starts


class Pool(ThreadPoolExecutor):
    """The ladder's pool, its size kept, and the order the files are handed to it (by their names)."""
    sizes, handed = [], []

    def __init__(self, workers):
        Pool.sizes.append(workers)
        super().__init__(workers)

    def submit(self, fn, *args, **kw):
        Pool.handed.append(os.path.basename(args[0][0]))
        return super().submit(fn, *args, **kw)


@contextlib.contextmanager
def pool():
    """Pool as the ladder's, its record started over."""
    Pool.sizes, Pool.handed = [], []
    with mock.patch.object(ladder, 'ThreadPoolExecutor', Pool):
        yield Pool


class Clock:
    """ladder.clock jumping on by `by` seconds `after` seconds from now: time that went by while the build did
    not look. held: True, the compilers were stopped meanwhile (a build stopped with Ctrl+Z), which
    ladder.stopped tells each compile once, at its first ask after the jump; False, the build was only too busy
    to look (a PC paging hard), and nothing tells it so; None, what Windows sees (and macOS's Python before
    3.13): ladder.stopped cannot tell either. in_ask: the stop comes while a look asks ladder.stopped, which
    then tells it (asked after the going on), and the jump starts there."""
    def __init__(self, after, by, held=True, in_ask=False):
        self.t0, self.after, self.by, self.held, self.in_ask = time.monotonic(), after, by, held, in_ask
        self.local = threading.local()   # (each compile looks from a thread of its own)

    def __call__(self):
        now = time.monotonic()
        if self.in_ask:
            return now + self.by if getattr(self.local, 'jumped', False) else now
        if now - self.t0 <= self.after:
            return now
        if not getattr(self.local, 'told', False):
            self.local.told = self.local.pending = True
        return now + self.by

    def stopped(self, proc):
        if self.held is None:
            return None
        if self.in_ask:
            if getattr(self.local, 'jumped', False) or time.monotonic() - self.t0 <= self.after:
                return False
            self.local.jumped = True
            return True
        told, self.local.pending = getattr(self.local, 'pending', False), False
        return told and self.held

    @contextlib.contextmanager
    def patched(self):
        with mock.patch.object(ladder, 'clock', self), mock.patch.object(ladder, 'stopped', self.stopped):
            yield self


# ---- the parts ---------------------------------------------------------------------------------------------
class HeaderTests(unittest.TestCase):
    def test_function_sizes(self):
        with tempfile.TemporaryDirectory() as d:
            c = made_up.write(Path(d, 'g0000.c'), generated(('f_7100000000', 30), ('f_7100001000_orig', 3)))
            self.assertEqual(ladder.function_sizes(str(c)), [(30, 'f_7100000000'), (3, 'f_7100001000_orig')])

    def test_header(self):
        header, big = ladder.optnone_header([(30000, 'f_a'), (8000, 'f_b'), (100, 'f_c')], 5000)
        self.assertEqual(big, [(30000, 'f_a'), (8000, 'f_b')])
        self.assertIn('#include "recomp.h"\n#ifdef __cplusplus\nextern "C" {\n#endif\n', header)
        self.assertIn('void f_a(Cpu *c) __attribute__((optnone, noinline));\n'
                      'void f_b(Cpu *c) __attribute__((optnone, noinline));\n#ifdef __cplusplus\n}\n#endif\n', header)
        self.assertNotIn('f_c', header)
        self.assertEqual(ladder.optnone_header([(100, 'f_c')], 5000), (None, []))
        self.assertEqual(ladder.header_name(os.path.join('build', 'main', 'g0001.obj')),
                         os.path.join('build', 'main', 'g0001.optnone.h'))

    def test_steps_down_the_thresholds(self):
        sizes = [(30000, 'f_a'), (8000, 'f_b'), (2000, 'f_c'), (100, 'f_d')]
        steps = ladder.timeout_steps(ladder.Step('/O1'), sizes)
        self.assertEqual([(s.level, s.above, [n for _i, n in s.names], s.left) for s in steps],
                         [('/O1', 20000, ['f_a'], 8000), ('/O1', 5000, ['f_a', 'f_b'], 2000),
                          ('/O1', 1250, ['f_a', 'f_b', 'f_c'], 100), ('/Od', 0, [], 0)])
        # a threshold naming no more functions than the one before is left out
        steps = ladder.timeout_steps(ladder.Step('-O2'), [(30000, 'f_a'), (100, 'f_d')])
        self.assertEqual([(s.level, s.above) for s in steps], [('-O2', 20000), ('-O0', 0)])
        self.assertEqual([(s.level, s.above) for s in ladder.timeout_steps(ladder.Step('-O1'), [(100, 'f_d')])],
                         [('-O0', 0)])
        self.assertEqual(ladder.timeout_steps(ladder.Step('-O0'), sizes), [])   # (nothing lower)
        # from SSBU_OPTNONE_ABOVE's header: its threshold over 4, then over 16
        own = ladder.Step('/O2', 8000, *ladder.optnone_header(sizes, 8000))
        self.assertEqual([(s.level, s.above) for s in ladder.timeout_steps(own, sizes)], [('/O2', 2000), ('/O2', 500), ('/Od', 0)])

    def test_the_header_s_content_is_in_the_key(self):
        a, _ = ladder.optnone_header([(30000, 'f_a'), (8000, 'f_b')], 20000)
        b, _ = ladder.optnone_header([(30000, 'f_a'), (8000, 'f_b')], 5000)
        command = ['clang', '-c', '-O1', 'g.c']
        self.assertEqual(ladder.key_command(command, None), command)   # (the normal key: as before)
        self.assertNotEqual(ladder.key_command(command, a), ladder.key_command(command, b))
        self.assertEqual(ladder.key_command(command, a)[:-1], command)

    def test_records_and_tags(self):
        step = ladder.Step('/O1', 20000, *ladder.optnone_header([(30000, 'f_a')], 20000))
        self.assertEqual(step.record(), {'level': '/O1', 'above': 20000})
        self.assertEqual(ladder.recorded_step({'key': 'k', 'ladder': step.record()}), ladder.Step('/O1', 20000))
        self.assertEqual(ladder.recorded_step({'key': 'k', 'ladder': {'level': '/Od'}}), ladder.Step('/Od'))
        self.assertIsNone(ladder.recorded_step({'key': 'k'}))
        self.assertIsNone(ladder.recorded_step(None))
        self.assertRegex(step.tag(), r'^/O1 optnone>20000:[0-9a-f]{16}$')
        self.assertEqual(ladder.tagged_step(step.tag()), ladder.Step('/O1', 20000))
        self.assertEqual(ladder.tagged_step('-O0'), ladder.Step('-O0'))
        self.assertIsNone(ladder.tagged_step(None))


class CrashSignTests(unittest.TestCase):
    def test_each_platform_s_crash(self):
        llvm = 'PLEASE submit a bug report to https://github.com/llvm/llvm-project/issues/\nStack dump:\n'
        driver = ('clang: error: unable to execute command: Segmentation fault: 11\n'
                  'clang: error: clang frontend command failed due to signal (use -v to see invocation)\n')
        signals = [(-signal.SIGSEGV, '', 'SIGSEGV'),                                 # a signal (Linux, macOS)
                   (-signal.SIGKILL, '', 'SIGKILL: the system most likely ran out of memory')] if POSIX else []
        for code, out, cause in [(1, llvm + 'Exception Code: 0xC0000005\n', 'exception 0xC0000005'),   # clang-cl
                                 (0xC00000FD, '', 'exception 0xC00000FD'),        # clang-cl crashing outright
                                 (254, driver, 'Segmentation fault: 11'),           # clang's driver, its cc1 crashed
                                 (1, 'LLVM ERROR: out of memory\nAllocation failed\n', 'out of memory'),
                                 (70, 'fatal error: error in backend: x\nclang: error: clang frontend command failed with '
                                      'exit code 70\n', 'a crash')] + signals:
            with self.subTest(cause=cause):
                self.assertTrue(ladder.crashed(code, out))
                self.assertEqual(ladder.crash_cause(code, out), cause)
        self.assertFalse(ladder.crashed(1, "g.c:3:1: error: expected ';'\n1 error generated.\n"))
        self.assertIs(cc.crashed, ladder.crashed)   # (cc.py's names, as before)

    def test_a_crash_s_files_are_removed(self):
        with tempfile.NamedTemporaryFile(suffix='.c', delete=False) as f:
            temp = f.name
        with tempfile.TemporaryDirectory() as d:
            elsewhere = Path(d, 'g0000-abc.c')
            elsewhere.write_text('x')
            ladder.crash_files(f'clang: note: diagnostic msg: {temp}\nclang: note: diagnostic msg: {elsewhere}\n')
            self.assertFalse(os.path.exists(temp))
            self.assertTrue(elsewhere.exists())   # (only what clang wrote into the temporary folder)


def stopped_now(pid):
    """Whether a process is stopped now (ps's state T), on Linux and macOS."""
    r = subprocess.run(['ps', '-o', 'stat=', '-p', str(pid)], capture_output=True, text=True, timeout=30)
    return r.stdout.strip().startswith('T')


class RunTests(unittest.TestCase):
    """ladder.run: the time its compiler was stopped with the build (Ctrl+Z) is not counted against a time limit,
    nor in a compile's seconds; time the build was only too busy to look is; a compile that ends by itself just as
    its limit comes is kept, whatever its result."""
    def test_an_hour_stopped_is_not_over_the_limit(self):
        with Clock(0.5, 3600.0).patched():
            r = ladder.run([sys.executable, '-c', 'import time; time.sleep(2)'], 600.0, threading.Event())
        self.assertEqual((r.code, r.timed_out), (0, False))
        self.assertLess(r.seconds, 60)

    def test_over_the_limit_in_the_time_it_ran(self):
        t0 = time.monotonic()
        with Clock(0.5, 3600.0).patched():
            r = ladder.run([sys.executable, '-c', 'import time; time.sleep(60)'], 2.0, threading.Event())
        self.assertTrue(r.timed_out)
        self.assertLess(time.monotonic() - t0, 30)
        self.assertLess(r.seconds, 10)

    def test_stopped_in_the_middle_of_a_look(self):
        """A Ctrl+Z that comes as a look asks whether the compiler was stopped: the time is read after that ask, so
        the stop is inside this look's time, which the ask's answer leaves out (read before it, the stop fell in the
        next look's time, which nothing tells, and the file went over its limit)."""
        with Clock(0.5, 3600.0, in_ask=True).patched():
            r = ladder.run([sys.executable, '-c', 'import time; time.sleep(2)'], 600.0, threading.Event())
        self.assertEqual((r.code, r.timed_out), (0, False))
        self.assertLess(r.seconds, 60)

    def test_where_nothing_tells_a_late_look_is_a_pause(self):
        """Windows (and macOS's Python before 3.13): ladder.stopped cannot tell (None), so a look more than
        ladder.PAUSED seconds late is taken for a pause, such as Modern Standby, which suspends the build and its
        compilers while Windows' clock runs on. An hour of it is not over the limit, nor in the compile's seconds."""
        with Clock(0.5, 3600.0, held=None).patched():
            r = ladder.run([sys.executable, '-c', 'import time; time.sleep(2)'], 600.0, threading.Event())
        self.assertEqual((r.code, r.timed_out), (0, False))
        self.assertLess(r.seconds, 60)

    def test_what_stopped_can_tell(self):
        """ladder.stopped: None where nothing tells (no waitid: Windows, macOS's Python before 3.13), else whether
        the compiler was stopped or let go on since it was last asked; False once it has ended."""
        self.assertEqual(ladder.waitid is None, os.name == 'nt' or not hasattr(os, 'waitid'))
        p = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
        self.addCleanup(p.wait)
        self.addCleanup(p.kill)
        with mock.patch.object(ladder, 'waitid', None):
            self.assertIsNone(ladder.stopped(p))
        if ladder.waitid is None:
            self.assertIsNone(ladder.stopped(p))
            return
        self.assertIs(ladder.stopped(p), False)
        os.kill(p.pid, signal.SIGSTOP)
        # (let go on only once it has stopped: on Linux a SIGCONT that comes before the stop has taken effect
        # throws the stop away, and a process never stopped has no going on to report)
        end = time.monotonic() + 10
        while not stopped_now(p.pid) and time.monotonic() < end:
            time.sleep(0.01)
        self.assertTrue(stopped_now(p.pid))
        os.kill(p.pid, signal.SIGCONT)
        end = time.monotonic() + 10
        while not ladder.stopped(p) and time.monotonic() < end:
            time.sleep(0.05)
        self.assertLess(time.monotonic(), end)
        self.assertIs(ladder.stopped(p), False)   # (told once)
        p.kill()
        p.wait()
        self.assertIs(ladder.stopped(p), False)

    def test_a_late_look_counts_when_the_compiler_ran(self):
        """An hour in which the build did not look (a PC paging hard, as a compile running away with its memory makes
        it), its compiler not stopped: counted, so that compile is stopped as soon as the build looks again."""
        t0 = time.monotonic()
        with Clock(0.5, 3600.0, held=False).patched():
            r = ladder.run([sys.executable, '-c', 'import time; time.sleep(60)'], 600.0, threading.Event())
        self.assertTrue(r.timed_out)
        self.assertLess(time.monotonic() - t0, 30)
        self.assertGreater(r.seconds, 3600)

    @unittest.skipUnless(POSIX, 'Ctrl+Z')
    def test_a_build_stopped_with_ctrl_z_and_one_too_busy_to_look(self):
        """Real processes, a time limit of 4 s, a compile of 1.5 s of work, and a stop of 7 s (longer than
        ladder.PAUSED) half a second in. The build stopped with Ctrl+Z, its compiler with it (its process group:
        SIGSTOP stops it as Ctrl+Z's SIGTSTP does): the stop is not counted, and the file compiles. The build alone
        stopped (what a PC too busy to look does to it), its compiler working on: counted, and the compile stopped by
        its limit as soon as the build looks again. Each also with a Python without os.waitid (macOS's before 3.13),
        where nothing tells the two apart: both stops taken for a pause, the busy compile stopped by its limit only
        once it has had its 4 s of running."""
        script = ('import os, sys, threading, time\nsys.path.insert(0, sys.argv[1])\n'
                  'if sys.argv[3] == "untold": os.__dict__.pop("waitid", None)\nimport ladder\n'
                  'work = [sys.executable, "-c", "import sys, time\\nwhile time.process_time() < float(sys.argv[1]): pass", '
                  'sys.argv[2]]\nprint("ready", flush=True)\nt0 = time.monotonic()\n'
                  'r = ladder.run(work, 4.0, threading.Event())\n'
                  'print(r.code, r.seconds, r.timed_out, time.monotonic() - t0, flush=True)\n')
        builds = {}
        for told in ('told', 'untold'):
            for name, work in (('ctrl+z', '1.5'), ('busy', '60')):
                builds[name, told] = p = subprocess.Popen([sys.executable, '-c', script, str(ROOT / 'tools'), work, told],
                                                          stdout=subprocess.PIPE, text=True, start_new_session=True)
                self.addCleanup(self.end, p)
        for p in builds.values():
            self.assertEqual(p.stdout.readline(), 'ready\n')
        time.sleep(0.5)
        for told in ('told', 'untold'):
            os.killpg(builds['ctrl+z', told].pid, signal.SIGSTOP)   # (the build and its compiler)
            os.kill(builds['busy', told].pid, signal.SIGSTOP)       # (the build alone)
        time.sleep(7)
        for told in ('told', 'untold'):
            os.killpg(builds['ctrl+z', told].pid, signal.SIGCONT)
            os.kill(builds['busy', told].pid, signal.SIGCONT)
        said = {key: p.communicate(timeout=60)[0].split() for key, p in builds.items()}
        for told in ('told', 'untold'):
            with self.subTest(told):
                code, seconds, timed_out, took = said['ctrl+z', told]
                self.assertEqual((int(code), timed_out), (0, 'False'))
                self.assertLess(float(seconds), 4)
                self.assertGreater(float(took) - float(seconds), 6)   # (the stop, not counted)
                code, seconds, timed_out, took = said['busy', told]
                self.assertEqual(timed_out, 'True')
                if told == 'told' and hasattr(os, 'waitid'):
                    self.assertGreater(float(seconds), 7)
                    self.assertLess(float(took), 12)
                else:   # (the stop taken for a pause: 4 s of running, after it)
                    self.assertLess(float(seconds), 7)
                    self.assertGreater(float(took), 10)

    def end(self, p):
        """A build of the test above gone, with whatever it started."""
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        p.wait()
        p.stdout.close()

    def test_ended_by_itself_just_as_its_limit_came(self):
        """A compile that ends by itself between the last look and its kill: its own result (an object, an error, a
        crash), not a timeout, which would take it down the ladder. (On Windows taskkill's exit code is an error's: a
        compile that ends with an error then is taken for one stopped.)"""
        real = ladder.kill_tree

        def late(proc):   # (the compile ends just before it is killed)
            proc.wait()
            real(proc)
        for code in ((0, 1, -signal.SIGSEGV) if POSIX else (0,)):
            end = f'sys.exit({code})' if code >= 0 else 'os.kill(os.getpid(), signal.SIGSEGV)'
            with self.subTest(code=code), mock.patch.object(ladder, 'kill_tree', late):
                r = ladder.run([sys.executable, '-c', f'import os, signal, sys, time; time.sleep(1.5); {end}'], 1.0,
                               threading.Event())
                self.assertEqual((r.code, r.timed_out), (code, False))

    @unittest.skipUnless(os.name == 'nt', "Windows' clock")
    def test_windows_clock_leaves_out_sleep(self):
        """On Windows a compile is timed by QueryUnbiasedInterruptTime, which leaves out the time the PC is asleep
        (time.monotonic counts it there): it is there, and goes on as time.monotonic does while the PC is awake."""
        self.assertIsNot(ladder.clock, time.monotonic)
        a, m = ladder.clock(), time.monotonic()
        time.sleep(0.5)
        self.assertAlmostEqual(ladder.clock() - a, time.monotonic() - m, delta=0.1)

    def test_finished_just_as_its_limit_came(self):
        """Its own result, an object or an error, not a timeout (which would remove the object)."""
        for code in (0, 1):
            with self.subTest(code=code), tempfile.TemporaryDirectory() as d:
                obj, asked = Path(d, 'g.o'), []

                def limit():   # (the second look comes once the compile has finished: then its limit is up)
                    asked.append(1)
                    if len(asked) > 1:
                        time.sleep(1.5)
                    return 100.0 if len(asked) == 1 else 0.0
                r = ladder.run([sys.executable, '-c', f'import sys, time; time.sleep(1.2); open({str(obj)!r}, "w").write("o"); '
                                f'sys.exit({code})'], limit, threading.Event())
                self.assertEqual((r.code, r.timed_out), (code, False))
                self.assertTrue(obj.exists())


class LimitTests(unittest.TestCase):
    def info(self, model=10.0, estimate=10.0):
        return ladder.Info('g.c', 'main/g.c', 1000, 100, False, model, estimate, 'd')

    def test_from_the_estimate_at_this_pc_s_pace(self):
        steps = ladder.Ladder(lambda insns, biggest, level, cxx: 1000.0 if level == '/O1' else 1.0, 'clang-cl', env={})
        first = steps.limit(self.info(), ladder.Step('/O1'), True)
        self.assertEqual(first(), ladder.FLOOR)   # (4 x 10 s x 4, the pace unknown: under the floor)
        self.assertEqual(steps.limit(self.info(200.0), ladder.Step('/O1'), True)(), 4 * 200 * 4)
        self.assertEqual(steps.limit(self.info(10.0, 300.0), ladder.Step('/O1'), True)(), 4 * 300 * 4)   # (last time's)
        later = steps.limit(self.info(), ladder.Step('/O1', 20000, 'h', (), 5000), False)
        self.assertEqual(later(), 4 * 1000 * 4)   # (the try's own estimate: its level, the longest left)
        steps.pace.add(120.0, 60.0)              # (this build's pace measured: 2)
        self.assertEqual(first(), ladder.FLOOR)
        self.assertEqual(later(), 4 * 1000 * 2)   # (asked again as the build goes)
        steps.pace.add(0.0, 1000.0)              # (a fast PC: never under its estimate times 4)
        self.assertEqual(later(), 4 * 1000)
        self.assertEqual(ladder.Ladder(lambda *a: 100.0, 'clang', env={}).limit(None, ladder.Step('/O2'), True), None)

    def test_the_last_builds_pace_until_this_one_s_is_measured(self):
        steps = ladder.Ladder(lambda *a: 1000.0, 'clang', env={})
        steps.pace.known = 3.0
        self.assertEqual(steps.limit(self.info(), ladder.Step('/O1', 1, 'h'), False)(), 4 * 1000 * 3)

    def test_settings(self):
        self.assertEqual(ladder.Ladder(lambda *a: 1e6, 'clang', env={'SSBU_COMPILE_LIMIT': '30'})
                         .limit(self.info(), ladder.Step('/O2'), True), 30)
        self.assertIsNone(ladder.Ladder(lambda *a: 1e6, 'clang', env={'SSBU_COMPILE_LIMIT': '0'})
                          .limit(self.info(), ladder.Step('/O2'), True))
        for bad in ('ten', '-5'):
            with self.assertRaises(SystemExit):
                ladder.limit_setting({'SSBU_COMPILE_LIMIT': bad})
        self.assertIsNone(ladder.limit_setting({}))
        self.assertEqual(ladder.optnone_setting({'SSBU_OPTNONE_ABOVE': '20000'}), 20000)
        self.assertIsNone(ladder.optnone_setting({'SSBU_OPTNONE_ABOVE': '0'}))
        self.assertIsNone(ladder.optnone_setting({}))   # (off unless set)
        with self.assertRaises(SystemExit):
            ladder.optnone_setting({'SSBU_OPTNONE_ABOVE': '2.5'})
        self.assertEqual([ladder.longest_first_setting(env) for env in ({}, {'SSBU_LONGEST_FIRST': '0'}, {'SSBU_LONGEST_FIRST': '1'})],
                         [False, False, True])   # (off unless set)
        for bad in ('yes', '2', '-1'):
            with self.subTest(bad), self.assertRaisesRegex(SystemExit, f'^SSBU_LONGEST_FIRST={bad}: 1 to start'):
                ladder.longest_first_setting({'SSBU_LONGEST_FIRST': bad})
        self.assertIn('; SSBU_LONGEST_FIRST=1; clang',
                      ladder.Ladder(lambda *a: 1.0, 'clang', env={'SSBU_LONGEST_FIRST': '1'}).describe())


class TimesAndReportTests(unittest.TestCase):
    def test_times(self):
        with tempfile.TemporaryDirectory() as d:
            times = ladder.Times(d)
            self.assertIsNone(times.pace())
            times.put('main/g0000.c', 300.0, '/O2', 50.0, 'aa')
            times.put('main/g0001.c', 30.0, '/O1', 25.0, 'bb')
            times.put('main/g0002.c', 900.0, '/O1', 5.0, 'cc', 'over its time limit; compiled whole at /Od')
            times.save()
            times = ladder.Times(d)   # (read back)
            self.assertAlmostEqual(times.pace(), 330 / 75)   # (the normal compiles only)
            self.assertAlmostEqual(times.estimate('main/g0000.c', 'aa', '/O2', 50.0), 300 / (330 / 75))
            self.assertEqual(times.estimate('main/g0000.c', 'changed', '/O2', 50.0), 50.0)   # (other C: the model's)
            self.assertEqual(times.estimate('main/g0000.c', 'aa', '/O1', 51.0), 51.0)        # (another level)
            self.assertEqual(times.estimate('main/g0002.c', 'cc', '/O1', 5.0), 5.0)          # (the ladder's time)
            self.assertEqual(times.estimate('main/new.c', 'dd', '/O2', 7.0), 7.0)
            Path(d, ladder.TIMES_FILE).write_text('{not json')
            self.assertEqual(ladder.Times(d).files, {})   # (never needed)

    def test_report(self):
        with tempfile.TemporaryDirectory() as d:
            report, said = ladder.Report(), []
            report.end(d, said.append)
            self.assertEqual((said, os.listdir(d)), ([], []))   # (nothing to say: no file)
            report.add('main/g0001.c', 'over its time limit (10 min 00 s) at /O1; compiled whole at /Od', '/Od', 612.4)
            report.add('main/g0002.c', 'the compiler crashed 3 times (exception 0xC0000005)', None, 30)
            report.end(d, said.append, 'settings')
            path = Path(d, ladder.REPORT)
            self.assertEqual(said, [f'2 files compiled another way (also in {path}):',
                                    '  main/g0001.c  10 min 12 s  /Od  over its time limit (10 min 00 s) at /O1; compiled whole at /Od',
                                    '  main/g0002.c  30 s  not compiled  the compiler crashed 3 times (exception 0xC0000005)'])
            lines = path.read_text().splitlines()
            self.assertRegex(lines[0], r'^# \d{4}-\d\d-\d\d \d\d:\d\d  settings$')
            self.assertEqual(lines[1:], [s[2:] for s in said[1:]])
            report.add('main/g0003.c', 'x', '-O1', 1)
            report.end(d, said.append)
            self.assertEqual(len(path.read_text().splitlines()), 5)   # (each build's added)


class ProgressTests(unittest.TestCase):
    def test_time_left_when_the_ladder_steps_in(self):
        clock = types.SimpleNamespace(now=0.0)
        b = P.Build(1, link=None, clock=lambda: clock.now)
        b.module('m', 0, [('x', 60.0), ('a', 10.0), ('b', 10.0)])
        b.begin('compile', 'm')
        b.started(('x',))
        clock.now = 60.0
        b.finished(('x',), 60.0)                  # (this PC's pace: 1)
        b.started(('a',))
        clock.now = 660.0                         # (a's ten minutes, its time limit)
        self.assertAlmostEqual(b.status()[1], 600 / 4 + 10)   # (past its estimate: a quarter of its time again)
        b.stepped(('a',), 5.0)                    # (compiled again another way: that try's estimate, from now)
        self.assertAlmostEqual(b.status()[1], 5 + 10)
        clock.now = 665.0
        b.finished(('a',), 605.0)
        self.assertEqual(len(b.samples), 1)       # (its time is not this PC's pace)
        self.assertAlmostEqual(b.status()[1], 10)
        b.started(('b',))
        b.requeued(('b',), 12.0)                  # (crashed: compiled again once the others are done)
        self.assertEqual((b.running, b.queue), ({}, {'b': 12.0}))
        self.assertAlmostEqual(b.status()[1], 12)
        b.started(('b',))
        b.finished(('b',), 3.0)
        self.assertEqual((len(b.samples), b.compiled), (1, 3))


# ---- the Windows build: recomp.py compile_dir --------------------------------------------------------------
class WindowsLadderTests(made_up.Folder):
    def setUp(self):
        super().setUp()
        self.module = self.dir / 'build' / 'main'
        made_up.write(self.module / 'manifest.tsv', 'g0000.c\t40000\t5000\ng0001.c\t30000\t25000\n')
        made_up.write(self.module / 'g0000.c', generated(('f_7100000000', 30)))
        self.g0001(generated(('f_7100001000', 50), ('f_7100001400', 20), ('f_7100001800', 5)))
        made_up.write(self.module / 'g_module.c', '#include "module.h"\n')
        self.flags = ['/nologo', '/arch:AVX2', '-mfma', '/I', str(self.runtime)]
        for patch in (mock.patch.object(cc, 'CLANG', self.cc.name), mock.patch.object(cc, 'GAME_CFLAGS', self.flags),
                      mock.patch.object(cc, 'CFLAGS', list(self.flags)), mock.patch.object(cc, 'ISA', 'avx2'),
                      mock.patch.object(cc, 'RUNTIME', str(self.runtime)), mock.patch.object(ladder, 'THRESHOLDS', SMALL),
                      self.cc.patch()):
            patch.start()
            self.addCleanup(patch.stop)

    def g0001(self, text):
        made_up.write(self.module / 'g0001.c', text)

    def compile(self, env=None):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), mock.patch.dict(os.environ, env or {}):
            ok = recomp.compile_dir(str(self.module), 2)
        self.said = out.getvalue()
        return ok, self.cc.runs()

    def recorded(self):
        return json.loads((self.module / objcache.RECORD).read_text())['objects']

    def left_behind(self):
        """What a compile stopped by the ladder could have left: the header, half an object, clang's temporary file."""
        return sorted(n for n in os.listdir(self.module) if n.endswith(('.h', '.tmp')))

    def test_nothing_goes_wrong_nothing_changes(self):
        """Every compile command byte for byte as it was (cc.compile_command, no header), at normal priority (cc.py
        never gave clang-cl another), with the jobs asked for, started in the manifest's order (the cost model would
        start g0001.c first); no report; each file's seconds kept."""
        with started() as starts, pool():
            self.assertEqual(self.compile(), (True, ['g0000.c /O2', 'g0001.c /O1', 'g_module.c /O1']))
        self.assertEqual(Pool.handed, ['g0000.c', 'g0001.c', 'g_module.c'])
        self.assertGreater(P.file_cost(30000, 25000), P.file_cost(40000, 5000))
        m = str(self.module)
        want = sorted([self.cc.name, '/c', level, *self.flags, '-fno-crash-diagnostics', os.path.join(m, n + '.c'),
                       '/Fo' + os.path.join(m, n + '.obj')] for n, level in (('g0000', '/O2'), ('g0001', '/O1'), ('g_module', '/O1')))
        self.assertEqual(sorted(args for args, _kw in starts), want)
        self.assertEqual([kw for _args, kw in starts], [{}, {}, {}])
        self.assertEqual(Pool.sizes, [2])
        self.assertFalse(Path(self.dir / 'build', ladder.REPORT).exists())
        self.assertNotIn('compiled another way', self.said)
        times = json.loads(Path(self.dir / 'build', ladder.TIMES_FILE).read_text())['files']
        self.assertEqual(sorted(times), ['main/g0000.c', 'main/g0001.c', 'main/g_module.c'])
        self.assertEqual((times['main/g0001.c']['level'], times['main/g0001.c']['model']), ('/O1', round(P.file_cost(30000, 25000), 3)))
        self.assertNotIn('how', times['main/g0001.c'])
        self.assertEqual(self.left_behind(), [])
        # the next build, every file compiled again from the same C (a runtime header changed), each with the seconds it
        # took measured (g0001.c the longest, then g_module.c): the same, in the manifest's order again
        self.measured({'main/g0000.c': 1.0, 'main/g0001.c': 900.0, 'main/g_module.c': 80.0})
        made_up.write(self.runtime / 'recomp.h', (self.runtime / 'recomp.h').read_text() + '/* changed */\n')
        with mock.patch.dict(os.environ, {'SSBU_LONGEST_FIRST': '1'}):   # (what the order would be if asked)
            self.assertEqual([os.path.basename(t[0]) for t in recomp.stale_files(m)], ['g0001.c', 'g_module.c', 'g0000.c'])
        with started() as starts, pool():
            self.assertEqual(self.compile(), (True, ['g0000.c /O2', 'g0001.c /O1', 'g_module.c /O1']))
        self.assertEqual(Pool.handed, ['g0000.c', 'g0001.c', 'g_module.c'])
        self.assertEqual(sorted(args for args, _kw in starts), want)
        self.assertEqual([kw for _args, kw in starts], [{}, {}, {}])
        self.assertEqual(Pool.sizes, [2])

    def measured(self, seconds):
        """compile_times.json as a build on a real PC leaves it: these seconds for the files, each estimated at 30 s
        (enough for a pace)."""
        path = Path(self.dir / 'build', ladder.TIMES_FILE)
        data = json.loads(path.read_text())
        for name, s in seconds.items():
            data['files'][name].update(seconds=s, model=30.0)
        path.write_text(json.dumps(data))

    def test_over_its_time_limit_its_longest_function_unoptimised(self):
        self.g0001(generated(('f_7100001000', 50), ('f_7100001400', 20), note='slow unless optnone on f_7100001000'))
        t0 = time.time()
        self.assertEqual(self.compile(LIMIT), (True, ['g0000.c /O2', 'g0001.c /O1', 'g0001.c /O1', 'g_module.c /O1']))
        self.assertLess(time.time() - t0, 30)   # (not the minute the made-up compiler would have run)
        tries = [args for args in self.cc.commands() if any(a.endswith('g0001.c') for a in args)]
        header = str(self.module / 'g0001.optnone.h')
        self.assertEqual([[a for a in args if a.startswith('/FI')] for args in tries], [[], ['/FI' + header]])
        self.assertIn(' /FI' + header, (self.module / 'g0001.obj').read_text())
        self.assertEqual(self.left_behind(), [])                 # (header, half an object, temporary file: gone)
        if POSIX:
            self.assertTrue(gone(self.cc.grandchildren()))       # (what the compiler started, stopped with it)
        self.assertIn('main/g0001.c is still compiling after 2 s, its time limit: stopped, compiling it again with its '
                      'one function over 40 instructions unoptimised (optnone), the rest at /O1\n', self.said)
        self.assertRegex(self.said, r'main/g0001.c compiled with its one function over 40 instructions unoptimised \(optnone\), '
                                    r'the rest at /O1 in \d+ s \(\d+ s in all\)\n')
        report = Path(self.dir / 'build', ladder.REPORT)
        self.assertIn(f'1 file compiled another way (also in {report}):', self.said)
        self.assertRegex(report.read_text(), r'main/g0001.c  \d+ s  /O1  over its time limit \(2 s\) at /O1; compiled with its one '
                                             r'function over 40 instructions unoptimised \(optnone\), the rest at /O1\n')
        self.assertEqual(self.recorded()['g0001.obj']['ladder'], {'level': '/O1', 'above': 40})
        times = json.loads(Path(self.dir / 'build', ladder.TIMES_FILE).read_text())['files']
        self.assertIn('optnone', times['main/g0001.c']['how'])
        # kept: neither compiled again at every build...
        self.assertEqual(self.compile(LIMIT), (True, []))
        self.assertEqual(self.compile(), (True, []))   # (nor under the cost model's limit)
        # ...nor after its C, the flags or the header's content changed, nor with no time limit
        self.assertEqual(recomp.stale_files(str(self.module)), [])
        with mock.patch.dict(os.environ, {'SSBU_COMPILE_LIMIT': '0'}):
            self.assertEqual([os.path.basename(t[0]) for t in recomp.stale_files(str(self.module))], ['g0001.c'])
        self.flags[1] = '/arch:SSE4.2'
        self.assertEqual(recomp.plan(str(self.module))[3], {objcache.COMMAND: 3})
        self.flags[1] = '/arch:AVX2'
        self.g0001(generated(('f_7100001000', 51), ('f_7100001400', 20), note='slow unless optnone on f_7100001000'))
        self.assertEqual(recomp.plan(str(self.module))[3], {objcache.CHANGED: 1})
        with mock.patch.object(ladder, 'THRESHOLDS', (10, 5, 2)):
            todo, record, keys, _why, _infos = recomp.plan(str(self.module))
            step = ladder.Step('/O1', 40)
            a, b = (keys[str(self.module / 'g0001.obj')][1](ladder.with_header(s, str(self.module / 'g0001.c')))['key']
                    for s in (step, step._replace(above=10)))
            self.assertNotEqual(a, b)   # (another header, another key)

    def test_then_whole_at_Od(self):
        self.g0001(generated(('f_7100001000', 50), note='slow unless O0'))
        self.assertEqual(self.compile(LIMIT)[1], ['g0000.c /O2', 'g0001.c /O1', 'g0001.c /O1', 'g0001.c /Od', 'g_module.c /O1'])
        self.assertIn('main/g0001.c is still compiling after 2 s, its time limit: stopped, compiling it again whole at /Od',
                      self.said)
        text = (self.module / 'g0001.obj').read_text()
        self.assertIn(' /Od ', text)
        self.assertNotIn('/FI', text)
        self.assertEqual(self.recorded()['g0001.obj']['ladder'], {'level': '/Od'})
        self.assertRegex(Path(self.dir / 'build', ladder.REPORT).read_text(),
                         r'main/g0001.c  \d+ s  /Od  over its time limit \(2 s\) at /O1; with its one function over 40 instructions '
                         r'unoptimised \(optnone\), the rest at /O1: over its time limit again \(2 s\); compiled whole at /Od\n')
        self.assertEqual(self.left_behind(), [])
        self.assertEqual(self.compile(LIMIT), (True, []))

    @contextlib.contextmanager
    def no_header(self):
        """The optnone header cannot be written (a full disk; on Windows the file still held a moment after a compiler
        was stopped)."""
        real = open

        def held(path, *args, **kw):
            if str(path).endswith('.optnone.h') and args[:1] == ('w',):
                raise PermissionError(13, 'Permission denied', str(path))
            return real(path, *args, **kw)
        with mock.patch.object(ladder, 'open', held, create=True):
            yield

    def test_a_header_that_cannot_be_written(self):
        """The optnone header cannot be written: that try is not made, and the ladder goes on to /Od, which needs no
        header, rather than the build stopping. A file still over its time limit at /Od is explained as one, not by
        the header."""
        self.g0001(generated(('f_7100001000', 50), note='slow unless O0'))
        with self.no_header():
            self.assertEqual(self.compile(LIMIT), (True, ['g0000.c /O2', 'g0001.c /O1', 'g0001.c /Od', 'g_module.c /O1']))
        self.assertIn(' /Od ', (self.module / 'g0001.obj').read_text())
        self.assertEqual(self.recorded()['g0001.obj']['ladder'], {'level': '/Od'})
        self.assertIn('main/g0001.c: could not write ', self.said)
        self.assertRegex(Path(self.dir / 'build', ladder.REPORT).read_text(),
                         r'main/g0001.c  \d+ s  /Od  over its time limit \(2 s\) at /O1; with its one function over 40 instructions '
                         r'unoptimised \(optnone\), the rest at /O1: could not write \S+g0001.optnone.h \(\[Errno 13\] Permission '
                         r'denied: \S+\); compiled whole at /Od\n')
        self.assertEqual(self.left_behind(), [])
        self.g0001(generated(('f_7100001000', 50), note='slow'))
        with self.no_header():
            ok, runs = self.compile(LIMIT)
        self.assertEqual((ok, runs), (False, ['g0001.c /O1', 'g0001.c /Od']))
        self.assertIn('FAILED g0001.c:\nmain/g0001.c did not finish compiling within its time limit, unoptimised either', self.said)
        self.assertEqual(self.left_behind(), [])

    def test_optnone_above_s_header_that_cannot_be_written(self):
        """SSBU_OPTNONE_ABOVE's header cannot be written: the file compiled whole at its own level instead, recorded as
        the ladder's way from there (kept by the next build that starts it the same way), and said; a crash then
        compiled again at its level, then lower, without the header."""
        above = {'SSBU_OPTNONE_ABOVE': '40'}
        with self.no_header():
            self.assertEqual(self.compile(above), (True, ['g0000.c /O2', 'g0001.c /O2', 'g_module.c /O1']))
        self.assertNotIn('/FI', (self.module / 'g0001.obj').read_text())
        self.assertEqual(self.recorded()['g0001.obj']['ladder'], {'level': '/O2', 'from': 40})
        self.assertIn(f'could not write {self.module / "g0001.optnone.h"} ', self.said)
        self.assertIn(': compiling main/g0001.c whole at /O2 instead', self.said)
        self.assertRegex(Path(self.dir / 'build', ladder.REPORT).read_text(),
                         r'main/g0001.c  \d+ s  /O2  its optnone header could not be written: compiled whole at /O2\n')
        self.assertEqual(self.left_behind(), [])
        self.assertEqual(self.compile(above), (True, []))
        self.g0001(generated(('f_7100001000', 50), ('f_7100001400', 20), note='crash at /O2'))
        with self.no_header():
            self.assertEqual(self.compile(above), (True, ['g0001.c /O1', 'g0001.c /O2', 'g0001.c /O2']))   # (its own level, again, then /O1)
        self.assertNotIn('/FI', (self.module / 'g0001.obj').read_text())
        self.assertEqual(self.recorded()['g0001.obj']['ladder'], {'level': '/O1', 'from': 40})
        self.assertEqual(self.compile(above), (True, []))

    def test_a_file_that_never_finishes_stops_the_build_with_an_explanation(self):
        self.g0001(generated(('f_7100001000', 50), note='slow'))
        ok, runs = self.compile(LIMIT)
        self.assertFalse(ok)
        self.assertEqual(runs, ['g0000.c /O2', 'g0001.c /O1', 'g0001.c /O1', 'g0001.c /Od', 'g_module.c /O1'])
        self.assertIn('FAILED g0001.c:\nmain/g0001.c did not finish compiling within its time limit, unoptimised either', self.said)
        self.assertIn('SSBU_COMPILE_LIMIT=<seconds>', self.said)
        self.assertNotIn('g0001.obj', self.recorded())
        self.assertFalse((self.module / 'g0001.obj').exists())
        self.assertEqual(self.left_behind(), [])
        self.assertRegex(Path(self.dir / 'build', ladder.REPORT).read_text(), r'main/g0001.c  \d+ s  not compiled  over its time limit')

    def test_interrupted_in_the_middle_of_the_ladder(self):
        """Ctrl+C while the file is compiled with its header: the build stops at once, and leaves no header, no
        half an object and no key for it; the next build starts the file over."""
        self.g0001(generated(('f_7100001000', 50), note='slow'))
        real = ladder.run

        def interrupted(argv, limit=None, stop=None, **kw):
            if not any(a.startswith('/FI') for a in argv):
                return real(argv, limit, stop, **kw)
            t0 = time.monotonic()

            def ctrl_c():   # (the pressed key, a second into the try with the header)
                if time.monotonic() - t0 > 1:
                    raise KeyboardInterrupt
            return real(argv, ctrl_c, stop, **kw)
        t0 = time.time()
        with mock.patch.object(ladder, 'run', interrupted), self.assertRaises(KeyboardInterrupt), \
                mock.patch.dict(os.environ, LIMIT), contextlib.redirect_stdout(io.StringIO()):
            recomp.compile_dir(str(self.module), 2)
        self.assertLess(time.time() - t0, 30)
        self.assertEqual([bool([a for a in args if a.startswith('/FI')]) for args in self.cc.commands()
                          if any(a.endswith('g0001.c') for a in args)], [False, True])
        self.assertEqual(self.left_behind(), [])
        self.assertFalse((self.module / 'g0001.obj').exists())
        self.assertNotIn('g0001.obj', self.recorded())
        if POSIX:
            self.assertTrue(gone(self.cc.grandchildren()))
        self.assertEqual([(os.path.basename(t[0]), t[2]) for t in recomp.stale_files(str(self.module))], [('g0001.c', '/O1')])

    def test_a_build_paused_is_not_over_the_time_limit(self):
        """An hour in which the build did not run, its compilers with it, in the middle of a compile: Modern Standby,
        which Windows' clock counts (a PC asleep is not in it at all), and of which nothing tells (ladder.stopped:
        None, as on Windows). Taken for a pause: no file over its time limit, none compiled another way, and the hour
        not in its time."""
        self.g0001(generated(('f_7100001000', 50), note='takes 3 s'))
        with Clock(1.0, 3600.0, held=None).patched():
            self.assertEqual(self.compile(), (True, ['g0000.c /O2', 'g0001.c /O1', 'g_module.c /O1']))
        self.assertNotIn('still compiling', self.said)
        self.assertNotIn('ladder', self.recorded()['g0001.obj'])
        times = json.loads(Path(self.dir / 'build', ladder.TIMES_FILE).read_text())['files']['main/g0001.c']
        self.assertNotIn('how', times)
        self.assertLess(times['seconds'], 60)
        self.assertFalse(Path(self.dir / 'build', ladder.REPORT).exists())

    @unittest.skipUnless(POSIX, 'a signal to the main thread')
    def test_ctrl_c_on_the_main_thread(self):
        """Ctrl+C reaching the build's main thread while two files compile: their compilers are stopped (kill_running);
        neither leaves half an object, clang's temporary file or a key, nor is taken for a crash."""
        made_up.write(self.module / 'g0000.c', generated(('f_7100000000', 30), note='slow'))
        self.g0001(generated(('f_7100001000', 50), note='slow'))
        self.addCleanup(signal.signal, signal.SIGINT, signal.signal(signal.SIGINT, signal.default_int_handler))
        timer = threading.Timer(1.5, signal.pthread_kill, (threading.main_thread().ident, signal.SIGINT))
        t0 = time.time()
        timer.start()
        try:
            with self.assertRaises(KeyboardInterrupt), contextlib.redirect_stdout(io.StringIO()) as out:
                recomp.compile_dir(str(self.module), 2)
        finally:
            timer.cancel()
        self.assertLess(time.time() - t0, 30)
        self.assertEqual(self.cc.runs(), ['g0000.c /O2', 'g0001.c /O1'])   # (not compiled again as a crash)
        self.assertNotIn('crashed', out.getvalue())
        self.assertEqual(self.left_behind(), [])
        self.assertEqual([n for n in ('g0000.obj', 'g0001.obj') if (self.module / n).exists() or n in self.recorded()], [])
        self.assertTrue(gone(self.cc.grandchildren()))

    def test_stopped_with_the_build(self):
        """What compile()'s Ctrl+C handler does (stopping, then kill_running), done from another thread so that the
        workers' side is tested on every platform (a compiler taskkill stopped exits with 1, not a signal): the
        compiles end at once, neither is taken for a crash or an error, and neither leaves anything."""
        made_up.write(self.module / 'g0000.c', generated(('f_7100000000', 30), note='slow'))
        self.g0001(generated(('f_7100001000', 50), note='slow'))
        steps = ladder.Ladder(P.level_cost, 'clang-cl')
        timer = threading.Timer(1.5, lambda: (steps.stopping.set(), ladder.kill_running()))
        t0 = time.time()
        timer.start()
        with contextlib.redirect_stdout(io.StringIO()) as out:
            recomp.compile_dir(str(self.module), 2, steps=steps)
        timer.join()
        self.assertLess(time.time() - t0, 30)
        self.assertNotIn('crashed', out.getvalue())
        self.assertNotIn('FAILED', out.getvalue())
        self.assertEqual(self.left_behind(), [])
        self.assertEqual([n for n in ('g0000.obj', 'g0001.obj') if (self.module / n).exists() or n in self.recorded()], [])

    def test_optnone_above_and_the_ladder_kept_apart(self):
        """What the ladder made of a file is kept only by a build that starts the file the same way: the default build
        does not keep what it made of SSBU_OPTNONE_ABOVE's way, nor SSBU_OPTNONE_ABOVE's build what it made of the
        default's."""
        above = {'SSBU_OPTNONE_ABOVE': '40', **LIMIT}
        self.g0001(generated(('f_7100001000', 50), ('f_7100001400', 20), note='slow unless optnone on f_7100001000'))
        self.assertEqual(self.compile(LIMIT)[1], ['g0000.c /O2', 'g0001.c /O1', 'g0001.c /O1', 'g_module.c /O1'])
        self.assertEqual(self.recorded()['g0001.obj']['ladder'], {'level': '/O1', 'above': 40})
        self.assertEqual(self.compile(above), (True, ['g0001.c /O2']))   # (its own way: f_7100001000 unoptimised, at /O2)
        self.assertNotIn('ladder', self.recorded()['g0001.obj'])
        # SSBU_OPTNONE_ABOVE's ladder: optnone above a quarter of 40, at /O2
        self.g0001(generated(('f_7100001000', 50), ('f_7100001400', 20), note='slow unless optnone on f_7100001400'))
        self.assertEqual(self.compile(above)[1], ['g0001.c /O2', 'g0001.c /O2'])
        self.assertEqual(self.recorded()['g0001.obj']['ladder'], {'level': '/O2', 'above': 10, 'from': 40})
        self.assertEqual(self.compile(above), (True, []))
        self.assertEqual(self.compile(LIMIT)[1], ['g0001.c /O1', 'g0001.c /O1', 'g0001.c /O1'])   # (its own way, then the ladder)
        self.assertEqual(self.recorded()['g0001.obj']['ladder'], {'level': '/O1', 'above': 10})
        self.assertEqual(self.compile(LIMIT), (True, []))

    def test_a_runtime_file_the_compiler_crashed_on_is_in_the_report(self):
        """recomp.py link: in the report of a ladder of its own, written beside the exe; build_all's: in the build's,
        which it writes after the link."""
        made_up.write(self.runtime / 'host.h', '#pragma once\n#include "module.h"\n')
        made_up.write(self.runtime / 'host.c', '#include "host.h"\n/* crash at /O2 */\n')
        made_up.write(self.runtime / 'cpu_check.c', '#include "recomp.h"\n')
        exe, report = self.dir / 'build' / 'ssbu.exe', self.dir / 'build' / ladder.REPORT

        def link(**kw):
            with mock.patch.object(cc, 'link', lambda objs, exe, options=(): Path(exe).write_bytes(b'MZ')), \
                    mock.patch.object(cc, 'SDL3_DLL', None), mock.patch.object(cc, 'CHECK_CFLAGS', ['/nologo']), \
                    contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertTrue(recomp.link([], str(exe), **kw))
            return out.getvalue(), self.cc.runs()
        said, runs = link()
        self.assertEqual([r for r in runs if r.startswith('host.c')], ['host.c /O1', 'host.c /O2', 'host.c /O2'])
        self.assertRegex(report.read_text(), r'host.c  \d+ s  /O1  the compiler crashed \(exception 0xC0000005\): compiled again')
        self.assertIn(f'1 file compiled another way (also in {report}):', said)
        report.unlink()
        made_up.write(self.runtime / 'host.c', '#include "host.h"\n/* crash at /O2, again */\n')
        steps = ladder.Ladder(P.level_cost, 'clang-cl', log=lambda line: None)
        link(steps=steps)
        self.assertFalse(report.exists())   # (said at the build's end)
        self.assertEqual(len(steps.report.lines), 1)
        steps.end(str(self.dir / 'build'))
        self.assertIn('host.c', report.read_text())

    def test_a_setting_that_is_not_a_number_stops_the_build_at_once(self):
        for name, value, said in (('SSBU_COMPILE_LIMIT', '2h', 'the seconds'), ('SSBU_LONGEST_FIRST', 'yes', '1 to start')):
            with self.subTest(name), mock.patch.dict(os.environ, {name: value}), \
                    mock.patch.object(recomp, 'module_paths', side_effect=AssertionError('the build went on')), \
                    self.assertRaisesRegex(SystemExit, f'^{name}={value}: {said}'):
                recomp.build_all('exefs', str(self.dir / 'build'), 2)

    def test_the_longest_first_by_what_they_took_when_asked(self):
        """compile_times.json's times are the estimates; with SSBU_LONGEST_FIRST=1 (for benchmarking), once it has a
        time for one of the files to compile, they start the longest first: by what they took, else by the cost
        model's estimate; else in the manifest's order (as when nothing goes wrong)."""
        longest = {'SSBU_LONGEST_FIRST': '1'}
        with pool():
            self.compile(longest)
        self.assertEqual(Pool.handed, ['g0000.c', 'g0001.c', 'g_module.c'])
        folder = self.dir / 'build'
        digest = {n: objcache.file_sha(str(self.module / n))[0] for n in ('g0000.c', 'g0001.c')}
        (folder / ladder.TIMES_FILE).write_text(json.dumps({'version': 1, 'files': {
            'main/g0000.c': {'seconds': 400.0, 'level': '/O2', 'model': 3.9, 'source': digest['g0000.c']},
            'main/g0001.c': {'seconds': 20.0, 'level': '/O1', 'model': 10.2, 'source': digest['g0001.c']},
            'main/gone.c': {'seconds': 86.1, 'level': '/O2', 'model': 60.0, 'source': 'x'}}}))
        for n in ('g0000.c', 'g0001.c', 'g_module.c'):
            (self.module / n.replace('.c', '.obj')).unlink()
        pace = (400 + 20 + 86.1) / (3.9 + 10.2 + 60)
        todo = recomp.stale_files(str(self.module))
        self.assertEqual([os.path.basename(t[0]) for t in todo], ['g0000.c', 'g0001.c', 'g_module.c'])
        self.assertEqual([round(t[3], 6) for t in todo], [round(400 / pace, 6), round(20 / pace, 6), P.TABLE])
        b = P.Build(2, link=None)   # (the time left goes by them)
        b.module('main', 70000, [(t[0], t[3]) for t in todo])
        self.assertAlmostEqual(b.cost[('compile', 'main')], P.makespan([400 / pace, 20 / pace, 1.0], [0.0, 0.0]))
        with pool():
            self.compile(longest)
        self.assertEqual(Pool.handed, ['g0000.c', 'g0001.c', 'g_module.c'])
        # g0001.c measured the longest: first; g0000.c not measured (its C changed): by the cost model, after g_module.c
        digest.update({n: objcache.file_sha(str(self.module / n))[0] for n in ('g0001.c', 'g_module.c')})
        (folder / ladder.TIMES_FILE).write_text(json.dumps({'version': 1, 'files': {
            'main/g0001.c': {'seconds': 900.0, 'level': '/O1', 'model': 10.2, 'source': digest['g0001.c']},
            'main/g_module.c': {'seconds': 80.0, 'level': '/O1', 'model': 1.0, 'source': digest['g_module.c']},
            'main/gone.c': {'seconds': 60.0, 'level': '/O2', 'model': 60.0, 'source': 'x'}}}))
        made_up.write(self.module / 'g0000.c', generated(('f_7100000000', 31)))
        for n in ('g0001.obj', 'g_module.obj'):
            (self.module / n).unlink()
        with mock.patch.dict(os.environ, longest):
            self.assertEqual([os.path.basename(t[0]) for t in recomp.stale_files(str(self.module))],
                             ['g0001.c', 'g_module.c', 'g0000.c'])
        todo = recomp.stale_files(str(self.module))   # (not asked: the manifest's order, the same estimates)
        self.assertEqual([os.path.basename(t[0]) for t in todo], ['g0000.c', 'g0001.c', 'g_module.c'])
        self.assertAlmostEqual(todo[1][3], 900 / ((900 + 80 + 60) / (10.2 + 1.0 + 60)))

    def test_optnone_above_from_the_start_only_when_asked(self):
        """SSBU_OPTNONE_ABOVE (for benchmarking): a file's functions over it unoptimised from the start, the rest at
        the level of the longest left; off, the file is compiled again whole."""
        above = {'SSBU_OPTNONE_ABOVE': '40'}
        self.assertEqual(self.compile(above), (True, ['g0000.c /O2', 'g0001.c /O2', 'g_module.c /O1']))
        header = str(self.module / 'g0001.optnone.h')
        self.assertIn(' /FI' + header, (self.module / 'g0001.obj').read_text())
        self.assertNotIn('/FI', (self.module / 'g0000.obj').read_text())   # (no function that long)
        self.assertEqual(self.left_behind(), [])
        self.assertEqual(self.compile(above), (True, []))
        self.assertEqual(self.compile(), (True, ['g0001.c /O1']))
        self.assertNotIn('/FI', (self.module / 'g0001.obj').read_text())


# ---- the Linux build: tools/linux/build_linux.py -------------------------------------------------------------
class LinuxLadderTests(made_up.Folder):
    def setUp(self):
        super().setUp()
        bin_dir = self.dir / 'llvm' / 'bin'
        bin_dir.mkdir(parents=True)
        with mock.patch.dict(os.environ, {'SSBU_LLVM_BIN': str(bin_dir)}):
            spec = importlib.util.spec_from_file_location('linux_toolchain_under_test', ROOT / 'tools' / 'linux' / 'toolchain.py')
            self.tc = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(self.tc)
        self.cc.name = self.tc.CLANG
        self.tc.RUNTIME = str(self.runtime)
        self.tree, self.out = self.dir / 'tree', self.dir / 'build-linux'
        self.main = self.tree / 'build' / 'main'
        made_up.write(self.main / 'manifest.tsv', 'g0000.c\t40000\t5000\ng0001.c\t40000\t30000\n')
        made_up.write(self.main / 'g0000.c', generated(('f_7100000000', 30)))
        made_up.write(self.main / 'g0001.c', generated(('f_7100001000', 50), note='__try'))   # (landing sites: C++)
        made_up.write(self.main / 'g_module.c', '#include "module.h"\n')
        for patch in (self.cc.patch(), mock.patch.object(ladder, 'THRESHOLDS', SMALL)):
            patch.start()
            self.addCleanup(patch.stop)
        saved = sys.modules.get('toolchain'), sys.argv, list(sys.path)
        sys.modules['toolchain'] = self.tc
        sys.argv = ['build_linux.py', '--port', str(self.tree), '--out', str(self.out), '--jobs', '3']
        try:
            self.ns = runpy.run_path(str(ROOT / 'tools' / 'linux' / 'build_linux.py'), run_name='build_linux_under_test')['main'].__globals__
        finally:
            if saved[0] is None:
                sys.modules.pop('toolchain', None)
            else:
                sys.modules['toolchain'] = saved[0]
            sys.argv, sys.path[:] = saved[1], saved[2]
        self.ns['ISA'] = 'avx2'

    def build(self, env=None):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), warnings.catch_warnings(), mock.patch.dict(os.environ, env or {}):
            warnings.simplefilter('ignore', ResourceWarning)
            ok = self.ns['compile_all'](self.ns['game_jobs'](), 'game')
        self.said = out.getvalue()
        return ok, self.cc.runs()

    def test_nothing_goes_wrong_nothing_changes(self):
        """Each command as toolchain.compile_command made it, run as toolchain.run runs one: nice on Linux, the
        priority class cross-compiling on Windows; the jobs asked for; the biggest source first, as before (the cost
        model would start g0001's first)."""
        made_up.write(self.main / 'g0000.c', generated(('f_7100000000', 30), note='x' * 4000))
        o, m = self.out / 'main', self.main
        want = sorted([[self.tc.CLANG, '-c', '-O2', *self.tc.CFLAGS, str(m / 'g0000.c'), '-o', str(o / 'g0000.o')],
                       [self.tc.CLANG, '-c', '-O1', *self.tc.CFLAGS, '-x', 'c++', '-std=gnu++20', '-fexceptions',
                        str(o / 'g0001_cxx.cpp'), '-o', str(o / 'g0001.o')],
                       [self.tc.CLANG, '-c', '-O1', *self.tc.CFLAGS, str(m / 'g_module.c'), '-o', str(o / 'g_module.o')]])
        for host_linux in (False, True):
            with self.subTest(host_linux=host_linux):
                shutil.rmtree(self.out, ignore_errors=True)
                with mock.patch.object(self.tc, 'HOST_LINUX', host_linux), started() as starts, pool():
                    self.assertEqual(self.build(), (True, ['g0000.c -O2', 'g0001_cxx.cpp -O1', 'g_module.c -O1']))
                self.assertEqual(Pool.handed, ['g0000.c', 'g0001_cxx.cpp', 'g_module.c'])
                nice = ['nice', '-n', '10'] if host_linux else []
                self.assertEqual(sorted(args for args, _kw in starts), sorted(nice + w for w in want))
                self.assertEqual([kw for _a, kw in starts], [{} if host_linux else {'creationflags': self.tc._PRIORITY}] * 3)
                self.assertEqual(Pool.sizes, [3])
                self.assertFalse((self.out / ladder.REPORT).exists())
        self.assertGreater(P.clang_cost(40000, 30000, '-O1', True), P.clang_cost(40000, 5000, '-O2'))
        # each compiled again with what they took measured (g_module.c the longest, then g0001.c; g0000.c not): the
        # biggest source first still, the same commands; with SSBU_LONGEST_FIRST=1, the longest first by what they took,
        # else by the cost model's estimate
        first = json.loads((self.out / ladder.TIMES_FILE).read_text())
        for env, order in (({}, ['g0000.c', 'g0001_cxx.cpp', 'g_module.c']),
                           ({'SSBU_LONGEST_FIRST': '1'}, ['g_module.c', 'g0001_cxx.cpp', 'g0000.c'])):
            with self.subTest(env=env):
                times = json.loads(json.dumps(first))
                times['files']['main/g_module.c']['seconds'], times['files']['main/g0001.c']['seconds'] = 600.0, 100.0
                del times['files']['main/g0000.c']
                times['files']['main/gone.c'] = {'seconds': 60.0, 'level': '-O2', 'model': 60.0, 'source': 'x'}
                (self.out / ladder.TIMES_FILE).write_text(json.dumps(times))
                shutil.rmtree(self.out / 'main')
                with mock.patch.object(self.tc, 'HOST_LINUX', True), started() as starts, pool():
                    self.assertEqual(self.build(env), (True, ['g0000.c -O2', 'g0001_cxx.cpp -O1', 'g_module.c -O1']))
                self.assertEqual(Pool.handed, order)
                self.assertEqual(sorted(args for args, _kw in starts), sorted(['nice', '-n', '10'] + w for w in want))

    def test_a_crash_compiled_again_on_its_own_then_at_O1(self):
        made_up.write(self.main / 'g0000.c', generated(('f_7100000000', 30), note='crash at -O2'))
        self.assertEqual(self.build(), (True, ['g0000.c -O1', 'g0000.c -O2', 'g0000.c -O2', 'g0001_cxx.cpp -O1', 'g_module.c -O1']))
        self.assertIn('the compiler crashed on main/g0000.c (exception 0xC0000005): compiling it again on its own\n'
                      'the compiler crashed on main/g0000.c (exception 0xC0000005): compiling it again on its own, at -O1\n'
                      'main/g0000.c compiled (at -O1: a little less optimised, works the same) in 0 s\n', self.said)
        self.assertRegex((self.out / ladder.REPORT).read_text(),
                         r'main/g0000.c  \d+ s  -O1  the compiler crashed \(exception 0xC0000005\): compiled again on its own, at -O1')
        self.assertEqual(self.build(), (True, []))   # (kept under its own level's key, as on Windows)

    def test_clang_s_driver_and_a_signal(self):
        made_up.write(self.main / 'g0000.c', generated(('f_7100000000', 30), note='crash like clang at -O2'))
        self.assertTrue(self.build()[0])
        self.assertIn('the compiler crashed on main/g0000.c (Segmentation fault: 11): compiling it again on its own\n', self.said)
        if POSIX:
            made_up.write(self.main / 'g0000.c', generated(('f_7100000000', 31), note='crash by signal at -O2'))
            self.assertEqual(self.build(), (True, ['g0000.c -O1', 'g0000.c -O2', 'g0000.c -O2']))
            self.assertIn('the compiler crashed on main/g0000.c (SIGSEGV): compiling it again on its own\n', self.said)
            self.assertEqual([n for n in os.listdir(self.out / 'main') if n.endswith('.tmp')], [])   # (what it was writing: gone)

    def test_a_setting_that_is_not_a_number_stops_the_build_at_once(self):
        with mock.patch.dict(os.environ, {'SSBU_OPTNONE_ABOVE': 'lots'}), \
                self.assertRaisesRegex(SystemExit, r'^SSBU_OPTNONE_ABOVE=lots: the instructions'):
            self.ns['main']()   # (before it looks for vkd3d-shader)

    def test_a_cxx_file_over_its_time_limit(self):
        """A file with landing sites (compiled as C++: its wrapper includes the C) has the header given before the
        wrapper, its functions declared with C linkage; kept on the next build."""
        made_up.write(self.main / 'g0001.c', generated(('f_7100001000', 50), note='__try slow unless optnone on f_7100001000'))
        self.assertEqual(self.build(LIMIT), (True, ['g0000.c -O2', 'g0001_cxx.cpp -O1', 'g0001_cxx.cpp -O1', 'g_module.c -O1']))
        header = str(self.out / 'main' / 'g0001.optnone.h')
        text = (self.out / 'main' / 'g0001.o').read_text()
        self.assertIn(f'-include {header} ', text)
        self.assertIn('-x c++', text)
        self.assertEqual(sorted(n for n in os.listdir(self.out / 'main') if n.endswith(('.h', '.tmp'))), [])
        record = json.loads((self.out / 'main' / objcache.RECORD).read_text())['objects']
        self.assertEqual(record['g0001.o']['ladder'], {'level': '-O1', 'above': 40})
        self.assertIn('main/g0001.c', (self.out / ladder.REPORT).read_text())
        self.assertEqual(self.build(LIMIT), (True, []))


# ---- the macOS build: tools/mac/build_mac.py ------------------------------------------------------------------
def mac_toolchain(runtime):
    """What build_mac.py takes from tools/mac/toolchain.py, its clang the made-up compiler's name."""
    tc = types.ModuleType('toolchain')
    tc.AR, tc.CLANG, tc.CODESIGN, tc.NM = 'ar', 'fakeclang', '/usr/bin/codesign', 'nm'
    tc.TARGET = ['-arch', 'arm64', '-mmacosx-version-min=14.0', '-isysroot', '/sdk']
    tc.CFLAGS = tc.TARGET + ['-mcpu=apple-m1', '-ffp-contract=off', '-w']
    tc.CXXFLAGS = ['-x', 'c++', '-std=gnu++20', '-fexceptions']
    tc.MIN_MACOS, tc.REPO, tc.RUNTIME = '14.0', str(ROOT), str(runtime)
    tc.SDL3_DIR, tc.SDL3_FRAMEWORK = str(ROOT / 'deps' / 'SDL3'), str(ROOT / 'deps' / 'SDL3' / 'SDL3.framework')
    tc.compile_each = lambda jobs, n=16, extra=(): []
    tc.run = lambda cmd: (0, '')
    tc.wrapped = lambda cmd: (['nice', '-n', '10', *cmd], {})
    tc.compile_command = lambda src, obj, opt, flags=(), extra=(), header=None: [
        tc.CLANG, '-c', opt, *tc.CFLAGS, *(tc.CXXFLAGS if src.endswith('.cpp') else []), *extra, *flags,
        *(['-include', header] if header else []), src, '-o', obj]
    return tc


class MacLadderTests(made_up.Folder):
    def setUp(self):
        super().setUp()
        self.tree = self.dir / 'tree'
        self.main = self.tree / 'build' / 'main'
        made_up.write(self.main / 'manifest.tsv', 'g0000.c\t40000\t5000\ng0001.c\t30000\t25000\n')
        made_up.write(self.main / 'g0000.c', generated(('f_7100000000', 30)))
        made_up.write(self.main / 'g0001.c', generated(('f_7100001000', 50)))
        made_up.write(self.main / 'g_module.c', '#include "module.h"\n')
        self.tc = mac_toolchain(self.runtime)
        self.cc.name = 'fakeclang'
        for patch in (self.cc.patch(), mock.patch.object(ladder, 'THRESHOLDS', SMALL)):
            patch.start()
            self.addCleanup(patch.stop)
        saved = sys.modules.get('toolchain'), sys.argv, list(sys.path)
        sys.modules['toolchain'] = self.tc
        sys.argv = ['build_mac.py', '--port', str(self.tree), '--jobs', '3']
        try:
            self.ns = runpy.run_path(str(ROOT / 'tools' / 'mac' / 'build_mac.py'), run_name='build_mac_under_test')['main'].__globals__
        finally:
            if saved[0] is None:
                sys.modules.pop('toolchain', None)
            else:
                sys.modules['toolchain'] = saved[0]
            sys.argv, sys.path[:] = saved[1], saved[2]
        self.obj = Path(self.ns['OBJ'])

    def build(self, env=None):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), mock.patch.dict(os.environ, env or {}):
            ok = self.ns['compile_all'](self.ns['game_jobs'](), 'game', self.ns['record_compiled'])
        self.said = out.getvalue()
        return ok, self.cc.runs()

    def lines(self):
        return [line.split('\t') for line in Path(self.ns['GAME_SOURCES']).read_text().splitlines()[1:]]

    def test_nothing_goes_wrong_nothing_changes(self):
        """The biggest source first, as before (the cost model would start g0001.c first)."""
        made_up.write(self.main / 'g0000.c', generated(('f_7100000000', 30), note='x' * 4000))
        with started() as starts, pool():
            self.assertEqual(self.build(), (True, ['g0000.c -O2', 'g0001.c -O1', 'g_module.c -O1']))
        self.assertEqual(Pool.handed, ['g0000.c', 'g0001.c', 'g_module.c'])
        self.assertGreater(P.clang_cost(30000, 25000, '-O1'), P.clang_cost(40000, 5000, '-O2'))
        o = self.obj / 'main'
        want = sorted(['nice', '-n', '10', 'fakeclang', '-c', level, *self.tc.CFLAGS, '-I', str(self.runtime),
                       str(self.main / (n + '.c')), '-o', str(o / (n + '.o'))]
                      for n, level in (('g0000', '-O2'), ('g0001', '-O1'), ('g_module', '-O1')))
        self.assertEqual(sorted(args for args, _kw in starts), want)
        self.assertEqual([kw for _a, kw in starts], [{}, {}, {}])
        self.assertEqual(Pool.sizes, [3])
        self.assertEqual(sorted((p[0], p[1], p[2], len(p)) for p in self.lines()),
                         [(os.path.join('main', 'g0000.o'), '-O2', 'c', 5), (os.path.join('main', 'g0001.o'), '-O1', 'c', 5),
                          (os.path.join('main', 'g_module.o'), '-O1', 'c', 5)])   # (as before: five fields)
        self.assertFalse(Path(self.ns['OUT'], ladder.REPORT).exists())
        self.assertTrue((self.obj / ladder.TIMES_FILE).exists())
        # each compiled again with what they took measured (g_module.c the longest): the biggest source first still;
        # with SSBU_LONGEST_FIRST=1, the longest first
        first = json.loads((self.obj / ladder.TIMES_FILE).read_text())
        for env, order in (({}, ['g0000.c', 'g0001.c', 'g_module.c']),
                           ({'SSBU_LONGEST_FIRST': '1'}, ['g_module.c', 'g0001.c', 'g0000.c'])):
            with self.subTest(env=env):
                times = json.loads(json.dumps(first))
                for name, seconds in (('main/g0000.c', 1.0), ('main/g0001.c', 100.0), ('main/g_module.c', 600.0)):
                    times['files'][name].update(seconds=seconds, model=30.0)
                (self.obj / ladder.TIMES_FILE).write_text(json.dumps(times))
                shutil.rmtree(o)
                with started() as starts, pool():
                    self.assertEqual(self.build(env), (True, ['g0000.c -O2', 'g0001.c -O1', 'g_module.c -O1']))
                self.assertEqual(Pool.handed, order)
                self.assertEqual(sorted(args for args, _kw in starts), want)

    def test_a_setting_that_is_not_a_number_stops_the_build_at_once(self):
        with mock.patch.dict(os.environ, {'SSBU_COMPILE_LIMIT': '-1'}), \
                self.assertRaisesRegex(SystemExit, r'^SSBU_COMPILE_LIMIT=-1: the seconds'):
            self.ns['main']()   # (before it looks for SDL3)

    def test_a_crash_compiled_again(self):
        made_up.write(self.main / 'g0000.c', generated(('f_7100000000', 30), note='crash at -O2'))
        self.assertEqual(self.build(), (True, ['g0000.c -O1', 'g0000.c -O2', 'g0000.c -O2', 'g0001.c -O1', 'g_module.c -O1']))
        self.assertIn('main/g0000.c compiled (at -O1: a little less optimised, works the same)', self.said)
        self.assertIn([os.path.join('main', 'g0000.o'), '-O2', 'c'], [p[:3] for p in self.lines()])   # (its own level)
        self.assertEqual(self.build(), (True, []))

    def test_over_its_time_limit(self):
        made_up.write(self.main / 'g0001.c', generated(('f_7100001000', 50), note='slow unless optnone on f_7100001000'))
        self.assertEqual(self.build(LIMIT), (True, ['g0000.c -O2', 'g0001.c -O1', 'g0001.c -O1', 'g_module.c -O1']))
        header = str(self.obj / 'main' / 'g0001.optnone.h')
        self.assertIn(f'-include {header} ', (self.obj / 'main' / 'g0001.o').read_text())
        line = next(p for p in self.lines() if p[0] == os.path.join('main', 'g0001.o'))
        self.assertEqual(line[1:3], ['-O1', 'c'])
        self.assertRegex(line[5], r'^\|ladder -O1 optnone>40:[0-9a-f]{16}$')   # (how, a sixth field)
        self.assertIn('main/g0001.c', Path(self.ns['OUT'], ladder.REPORT).read_text())
        self.assertEqual(sorted(n for n in os.listdir(self.obj / 'main') if n.endswith(('.h', '.tmp'))), [])
        self.assertEqual(self.ns['game_jobs'](), [])   # (kept)
        with mock.patch.dict(os.environ, {'SSBU_COMPILE_LIMIT': '0'}):
            self.assertEqual([os.path.basename(j[1]) for j in self.ns['game_jobs']()], ['g0001.o'])
        self.assertEqual(self.build(LIMIT)[1], ['g0001.c -O1', 'g0001.c -O1'])   # (the line was dropped: again)
        made_up.write(self.main / 'g0001.c', generated(('f_7100001000', 51), note='slow unless optnone on f_7100001000'))
        os.utime(self.main / 'g0001.c', None)
        self.assertEqual([os.path.basename(j[1]) for j in self.ns['game_jobs']()], ['g0001.o'])   # (other C)


# ---- the real clang -------------------------------------------------------------------------------------------
RECOMP_H = '''#pragma once
typedef unsigned long long u64;
typedef unsigned int u32;
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Cpu { u64 x[31]; } Cpu;
typedef void GuestFn(Cpu *);
#ifdef __cplusplus
}
#endif
#define X(n) (c->x[n])
#define TAIL(f) { __attribute__((musttail)) return f(c); }
'''
# ten million guest tail calls back and forth, as the lifter writes them (TAIL): a host stack frame each
# without musttail
CHAIN = '''#include "recomp.h"

GuestFn f_7100000000;
GuestFn f_7100000010;
GuestFn f_7100000020;

void f_7100000000(Cpu *c) {
    X(0) = X(0) + 1;
    if (X(1)) { X(1) = X(1) - 1; TAIL(f_7100000010) }
}
void f_7100000010(Cpu *c) {
    X(2) = X(2) + 3;
    TAIL(f_7100000000)
}
void f_7100000020(Cpu *c) {
    X(1) = 10000000;
    f_7100000000(c);
}
'''
MAIN = '''#include <stdio.h>
#include "recomp.h"
GuestFn f_7100000020;
static Cpu cpu;
int main(void) { Cpu *c = &cpu; f_7100000020(c); printf("%llu %llu %llu\\n", X(0), X(1), X(2)); return 0; }
'''


def goto_heavy(n):
    """A made-up function of n instructions in the lifter's layout whose labels and gotos make clang's
    optimiser take seconds (3,000: 11.7 s at -O2, 0.2 s with optnone, 0.1 s at -O0, Apple clang 21)."""
    return ('#include "recomp.h"\n\nGuestFn f_7100000000;\n\nvoid f_7100000000(Cpu *c) {\n'
            + ''.join(f'L_{i:x}:;\n    X({i % 31}) = X({i * 7 % 31}) * {i | 1}u + X({i * 13 % 31}); '
                      f'if (X({i % 5}) & {1 << (i % 60)}ull) goto L_{i * 31 % n:x};\n' for i in range(n)) + '}\n')


@unittest.skipUnless(CLANG and POSIX, 'needs clang')
class RealClangTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='ladder clang-')   # (a space in its path, as in C:\Users\Jo Smith)
        self.addCleanup(self.tmp.cleanup)
        self.d = Path(self.tmp.name)
        made_up.write(self.d / 'recomp.h', RECOMP_H)

    def clang(self, *args, mode=()):
        r = subprocess.run([CLANG, *mode, *args], capture_output=True, text=True, cwd=self.d)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r

    def header_for(self, name, above):
        header, big = ladder.optnone_header(ladder.function_sizes(str(self.d / name)), above)
        made_up.write(self.d / 'g.optnone.h', header)
        return big

    def attributes(self, ll):
        """{function: its attributes} of an LLVM IR file."""
        text = (self.d / ll).read_text()
        groups = dict(re.findall(r'^attributes (#\d+) = \{ (.*) \}$', text, re.M))
        return {name: groups.get(group, '') for name, group in re.findall(r'^define .*@(f_\w+)\(.*\) .*?(#\d+) \{$', text, re.M)}

    def test_optnone_through_include_reaches_the_definitions(self):
        made_up.write(self.d / 'g.c', generated(('f_7100001000', 50), ('f_7100001400', 20)).replace('u32 nf', 'u64 nf')
                      .replace('void f_7100009000(Cpu *c) { hle_x(c); }\n', ''))
        self.assertEqual(self.header_for('g.c', 40), [(50, 'f_7100001000')])
        made_up.write(self.d / 'g_cxx.cpp', '#include "recomp.h"\nextern "C" {\n#include "g.c"\n}\n')
        for src, mode in (('g.c', []), ('g_cxx.cpp', ['-x', 'c++', '-std=gnu++20', '-fexceptions'])):
            with self.subTest(src=src):
                self.clang('-S', '-emit-llvm', '-O2', '-w', *mode, '-I', '.', '-include', 'g.optnone.h', src, '-o', 'g.ll')
                attrs = self.attributes('g.ll')
                self.assertIn('optnone', attrs['f_7100001000'])
                self.assertIn('noinline', attrs['f_7100001000'])
                self.assertNotIn('optnone', attrs['f_7100001400'])

    def test_clang_cl_s_FI_and_a_tail_call_at_Od(self):
        cl = ['--driver-mode=cl', '--target=x86_64-pc-windows-msvc']
        made_up.write(self.d / 'g.c', generated(('f_7100001000', 50), ('f_7100001400', 20)).replace('u32 nf', 'u64 nf')
                      .replace('void f_7100009000(Cpu *c) { hle_x(c); }\n', ''))
        self.header_for('g.c', 40)   # (/FI with the header's full path, as the ladder gives it)
        self.clang('/nologo', '/O2', '-w', '/I.', '/FI' + str(self.d / 'g.optnone.h'), '/clang:-S', '/clang:-emit-llvm',
                   '/clang:-o', '/clang:g.ll', 'g.c', mode=cl)
        attrs = self.attributes('g.ll')
        self.assertIn('optnone', attrs['f_7100001000'])
        self.assertNotIn('optnone', attrs['f_7100001400'])
        made_up.write(self.d / 'chain.c', CHAIN)
        self.clang('/nologo', '/Od', '-w', '/I.', '/clang:-S', '/clang:-o', '/clang:chain.s', 'chain.c', mode=cl)
        asm = (self.d / 'chain.s').read_text()
        self.assertRegex(asm, r'jmp\s+f_7100000010')   # (a jump at /Od: no stack frame for each guest tail call)
        self.assertNotRegex(asm, r'call\s+f_7100000010')

    def test_a_chain_of_musttail_calls_at_O0(self):
        """Ten million guest tail calls run at -O0, as C and as C++ (a file with landing sites), and with optnone on
        both functions at -O2; without musttail the same chain overflows the stack at -O0 (the test can tell)."""
        made_up.write(self.d / 'chain.c', CHAIN)
        made_up.write(self.d / 'chain_cxx.cpp', '#include "recomp.h"\nextern "C" {\n#include "chain.c"\n}\n')
        made_up.write(self.d / 'main.c', MAIN)
        self.clang('-c', '-O2', '-w', '-I', '.', 'main.c', '-o', 'main.o')
        self.header_for('chain.c', 1)
        for name, args in (('C at -O0', ['-O0', 'chain.c']),
                           ('C++ at -O0', ['-O0', '-x', 'c++', '-std=gnu++20', '-fexceptions', 'chain_cxx.cpp']),
                           ('optnone at -O2', ['-O2', '-include', 'g.optnone.h', 'chain.c'])):
            with self.subTest(name):
                self.clang('-c', '-w', '-I', '.', *args, '-o', 'chain.o')
                self.clang('main.o', 'chain.o', '-o', 'chain', *(['-lc++'] if sys.platform == 'darwin' else ['-lstdc++']))
                r = subprocess.run([str(self.d / 'chain')], capture_output=True, text=True, timeout=120)
                self.assertEqual((r.returncode, r.stdout), (0, '10000001 0 30000000\n'))
        made_up.write(self.d / 'recomp.h', RECOMP_H.replace('__attribute__((musttail)) return f(c);', 'f(c); return;'))
        self.clang('-c', '-O0', '-w', '-I', '.', 'chain.c', '-o', 'chain.o')
        self.clang('-c', '-O2', '-w', '-I', '.', 'main.c', '-o', 'main.o')
        self.clang('main.o', 'chain.o', '-o', 'chain')
        self.assertNotEqual(subprocess.run([str(self.d / 'chain')], capture_output=True, timeout=120).returncode, 0)

    def test_a_real_compile_over_its_time_limit(self):
        """clang stopped after 2 s on a function it takes seconds to optimise, then compiled with it unoptimised:
        no half-written object, the step reported to compiled()."""
        made_up.write(self.d / 'g.c', goto_heavy(3000))
        src, obj = str(self.d / 'g.c'), str(self.d / 'g.o')
        info = ladder.Info(src, 'main/g.c', 3000, 3000, False, 1.0, 1.0, 'd')
        done, said = [], []
        steps = ladder.Ladder(P.clang_cost, 'clang', said.append, env=LIMIT)

        def argv(job, level, header):
            return [CLANG, '-c', level, '-w', '-I', str(self.d), *(['-include', header] if header else []), job[0], '-o', job[1]]
        t0 = time.time()
        with mock.patch.object(ladder, 'THRESHOLDS', (1000, 250, 62)):
            failures = steps.compile([(src, obj, '-O2')], 1, argv, {obj: info}, compiled=lambda *a: done.append(a))
        self.assertEqual(failures, [])
        if not said:   # (measured with Apple clang 21; another clang or a faster PC may not need the ladder)
            self.skipTest(f'this clang optimised the function within the time limit, in {time.time() - t0:.1f} s')
        self.assertLess(time.time() - t0, 11.7)   # (sooner than the compile it stopped would have taken)
        self.assertEqual([(a[1], a[2].level, a[2].above) for a in done], [('-O2', '-O2', 1000)])
        self.assertTrue(said[0].startswith('main/g.c is still compiling after 2 s, its time limit: stopped'), said)
        self.assertEqual(sorted(os.listdir(self.d)), ['g.c', 'g.o', 'recomp.h'])
        nm = subprocess.run(['nm', obj], capture_output=True, text=True).stdout
        self.assertIn('f_7100000000', nm)


if __name__ == '__main__':
    unittest.main()
