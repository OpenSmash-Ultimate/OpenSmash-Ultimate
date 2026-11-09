"""Run-ahead (runtime/runahead.h, runtime/hle_rb_core.c "Run-ahead"; docs/TECHNICAL.md): its schedule on a toy game
through tools/dev/ratest.c, with no game - the real logic frames the same as without run-ahead, each picture the
one without run-ahead n frames on (always with n = 1), a press drawn in its own frame shown instead of the next,
the pause, a refused save or load. Then what ties it into the runtime, read from the sources: the switch
(SSBU_RUNAHEAD, display.ini's runahead, the settings menu's row), the live pads in its frames (no input tape),
the predicted frames kept out of SSBU_SEQ and the fight hash log, and tools/runahead_trace.py on a made-up trace."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402
import runahead_trace  # noqa: E402

if os.name == 'nt':
    HAVE_CLANG = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
else:
    HAVE_CLANG = shutil.which('clang') is not None


def src(name):
    return (ROOT / 'runtime' / name).read_text(encoding='utf-8')


@unittest.skipUnless(HAVE_CLANG, 'needs clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools), or clang')
class ToyGame(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='ratest ')
        cls.addClassCleanup(shutil.rmtree, cls.dir, ignore_errors=True)
        source = str(ROOT / 'tools' / 'dev' / 'ratest.c')
        if os.name == 'nt':
            cls.exe = os.path.join(cls.dir, 'ratest.exe')
            cmd = [cc.CLANG, '/nologo', '/O2', '/W3', '/WX', source, '/Fe' + cls.exe, '/Fo' + cls.dir + os.sep]
        else:
            cls.exe = os.path.join(cls.dir, 'ratest')
            cmd = ['clang', '-O2', '-Wall', '-Werror', source, '-o', cls.exe]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('ratest did not build:\n' + r.stdout + r.stderr)

    def test_schedule(self):
        r = subprocess.run([self.exe], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'(^|\n)\d+ checks, 0 failed\n$')
        self.assertIn('run-ahead 1: ', r.stdout)
        self.assertRegex(r.stdout, r'run-ahead 1: \d+ segments for 2400 frames shown, 0 of \d+ pictures predicted differently')


class Wiring(unittest.TestCase):
    def test_switch(self):
        core, kernel, disp, menu = src('hle_rb_core.c'), src('kernel.c'), src('gpu_display.c'), src('gpu_menu.c')
        self.assertIn('getenv("SSBU_RUNAHEAD")', kernel, 'SSBU_RUNAHEAD switches the savestates on')
        self.assertIn('kernel_rb_wanted', kernel)
        self.assertIn('kernel_rb_wanted = 1', core, "display.ini's runahead switches them on before the first module")
        self.assertIn('"runahead = 0\\n"', disp, "display.ini's default: off")
        self.assertIn('!_stricmp(key, "runahead")', disp)
        self.assertIn('R_RUNAHEAD', menu)
        self.assertIn('"Run-ahead (experimental)"', menu)
        self.assertIn('set_display("runahead", v)', menu)
        self.assertIn('note_env(r, "SSBU_RUNAHEAD")', menu)

    def test_only_in_local_fights(self):
        core = src('hle_rb_core.c')
        self.assertIn("return net_mode || ring_from >= 0 || ver_at >= 0 || bench_from >= 0 || auto_at >= 0;", core,
                      'never with the net mode or the savestate tests')
        self.assertIn('if (S.in_fight && (!S.idle || ra_engaged) && runahead_point(c, frame)) return;', core,
                      'only in a fight, and not in one that started with run-ahead off (it starts at the next fight)')
        self.assertIn('if (!S.idle && !ra_engaged && rb_idle()) {', core, 'turned off mid-fight: no savestate work from then on')
        self.assertIn('if (!nbusy || waited >= quiet_limit_ms || rb_fast > 0 || ra_engaged || S.idle) break;', core,
                      'no wait for the other threads when nothing needs the savestates')
        self.assertIn('ra_fight_over();', core)

    def test_frames(self):
        core, hid, fight = src('hle_rb_core.c'), src('hle_hid.c'), src('hle_fight_test.c')
        self.assertIn('if (!tape_on || ra_engaged) return;', core, 'every pad read live in run-ahead')
        self.assertIn('InterlockedExchange(&rb_fast, hide ? 1 : 0);', core, 'hidden frames as a catch-up\'s (their GPU draws left out: test_rb_state_map.py)')
        self.assertIn('if (rb_ra_predicting()) {', hid, 'a predicted frame does not move SSBU_SEQ on')
        self.assertIn('    if (rb_ra_predicting()) return;\n    u64 agent', fight, 'nor counts as a fight frame')
        self.assertIn('int rb_ra_predicting(void) { return 0; }', core, 'off where rollback is not built')


class Trace(unittest.TestCase):
    def write(self, lines):
        f = tempfile.NamedTemporaryFile('w', suffix='.trace', delete=False, encoding='utf-8')
        f.write('\n'.join(lines) + '\n')
        f.close()
        self.addCleanup(os.unlink, f.name)
        return f.name

    def test_delay(self):
        # without run-ahead: a press read in frame shown 5 (logic frame 5) changes logic frame 6's state, drawn in frame shown 6
        ref, ra = ['P 0 1 0'], ['P 0 1 0']
        for k in range(1, 12):
            st = 10 if k >= 6 else 0
            ref += [f'R {k} {k:016x} {st} 1', f'S {k} {k} {k:016x} {st} 1']
        ref.insert(12, 'P 5 5 1')
        # run-ahead 1: the same real frames, each drawn a frame shown sooner
        for k in range(1, 12):
            st = 10 if k >= 6 else 0
            ra += [f'R {k} {k:016x} {st} 1', f'S {k - 1} {k} {k:016x} {st} 1']
        ra.insert(12, 'P 5 5 1')
        a, b = self.write(ra), self.write(ref)
        real, shown, pads, conflicts = runahead_trace.read(b)
        self.assertEqual(conflicts, 0)
        self.assertEqual([d[2] for d in runahead_trace.delays(real, shown, pads)], [1])
        real, shown, pads, _ = runahead_trace.read(a)
        self.assertEqual([d[2] for d in runahead_trace.delays(real, shown, pads)], [0])
        self.assertEqual(runahead_trace.main(['x', a, b]), 0)
        bad = self.write([line.replace('R 7 0000000000000007', 'R 7 00000000000000ff') for line in ra])
        self.assertEqual(runahead_trace.main(['x', bad, b]), 1, 'a real frame that differs fails')


if __name__ == '__main__':
    unittest.main()
