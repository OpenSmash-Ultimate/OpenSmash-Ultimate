"""The test-run tools (docs/TESTING-RUNS.md) through tools/dev/testruntest.c, on made-up sequences
and numbers: SSBU_SEQ's parser and step machine (runtime/hle_input_seq.c: parse errors, exact holds,
waits, the steps hook, the end), SSBU_SPEED's pacing and SSBU_SKIP_MOVIES' count (runtime/testrun.h)
and the game address mapping (runtime/game_addr.h); and the documentation's example sequence, which
must parse; and that SSBU_SPEED gives a refresh only for a frame shown (runtime/ipc_vi.c). Plain
C: clang-cl on Windows (skipped without it), clang elsewhere."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

SOURCES = [ROOT / 'tools' / 'dev' / 'testruntest.c', ROOT / 'runtime' / 'hle_input_seq.c']
DOC = ROOT / 'docs' / 'TESTING-RUNS.md'
POSIX_CLANG = shutil.which('clang')
if os.name == 'nt':
    HAVE = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
    WHY = 'needs clang-cl (deps\\llvm, or Visual Studio\'s C++ Clang tools)'
else:
    HAVE = POSIX_CLANG is not None
    WHY = 'needs clang'


def build(out):
    """The command that builds testruntest into the folder `out`, and the program it makes."""
    if os.name == 'nt':
        exe = os.path.join(out, 'testruntest.exe')
        return [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, *map(str, SOURCES), '/Fe' + exe,
                '/Fo' + out + os.sep, '/link', *cc.LINK_OPTIONS], exe
    exe = os.path.join(out, 'testruntest')
    return [POSIX_CLANG, '-O1', '-Wall', '-Werror', *map(str, SOURCES), '-o', exe], exe


@unittest.skipUnless(HAVE, WHY)
class TestRunToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.addClassCleanup(shutil.rmtree, cls.dir, ignore_errors=True)
        cmd, cls.exe = build(cls.dir)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('testruntest.c did not build:\n' + r.stdout + r.stderr)

    def run_test(self, *args):
        return subprocess.run([self.exe, *args], capture_output=True, text=True, timeout=60)

    def test_checks(self):
        r = self.run_test()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'^\d+ checks, 0 failed\n$')

    def test_documented_example_parses(self):
        text = DOC.read_text(encoding='utf-8')
        m = re.search(r'<!-- example sequence[^>]*-->\n```text\n(.*?)```', text, re.S)
        self.assertIsNotNone(m, 'no example sequence in docs/TESTING-RUNS.md')
        path = os.path.join(self.dir, 'example.seq')
        with open(path, 'w', encoding='utf-8') as f:
            f.write(m.group(1))
        r = self.run_test(path)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout, '7 steps\n')

    def test_documented_gate_sequence_parses(self):
        text = DOC.read_text(encoding='utf-8')
        m = re.search(r'<!-- gate sequence[^>]*-->\n```text\n(.*?)```', text, re.S)
        self.assertIsNotNone(m, 'no gate sequence in docs/TESTING-RUNS.md')
        path = os.path.join(self.dir, 'gate.seq')
        with open(path, 'w', encoding='utf-8') as f:
            f.write(m.group(1))
        r = self.run_test(path)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout, '58 steps\n')


class SpeedLoopTests(unittest.TestCase):
    """SSBU_SPEED has no clock of refreshes (docs/TESTING-RUNS.md): runtime/ipc_vi.c's speed loop
    signals the game's vsync event in one place only, right after it showed a frame. A refresh
    with no frame (as an idle refresh after a stall would be) counts as a dropped frame in the
    game and runs an extra logic frame."""

    def test_one_refresh_per_frame_shown(self):
        text = (ROOT / 'runtime' / 'ipc_vi.c').read_text(encoding='utf-8')
        m = re.search(r'^static void speed_loop\(void\) \{\n(.*?)^\}\n', text, re.S | re.M)
        self.assertIsNotNone(m, 'no speed_loop in runtime/ipc_vi.c')
        body = m.group(1)
        self.assertEqual(body.count('kernel_signal_event('), 1, body)
        self.assertRegex(body, r'if \(oldest >= 0\) \{\s*show\(\);\s*frames\+\+;\s*kernel_signal_event\(queue\.vsync_event\);')


class DocumentedTests(unittest.TestCase):
    """Each switch is read where the documentation says, and documented (CONTRIBUTING: one line
    where the switches are listed)."""
    SWITCHES = {'SSBU_SPEED': 'gpu_display.c', 'SSBU_SEQ': 'hle_hid.c', 'SSBU_SEQ_LOG': 'hle_hid.c',
                'SSBU_SEQ_KEYBOARD': 'hle_hid.c', 'SSBU_SKIP_MOVIES': 'hle_movie_skip.c',
                'SSBU_FIGHT_HASH': 'hle_fight_test.c', 'SSBU_DETERMINISTIC': 'hle_fight_test.c', 'SSBU_DET_OFF': 'hle_fight_test.c',
                'SSBU_FIXED_SEED': 'hle_fight_test.c', 'SSBU_DEBUG_MATCH': 'hle_fight_test.c',
                'SSBU_DEBUG_MATCH_LIST': 'hle_fight_test.c', 'SSBU_DEBUG_END': 'hle_fight_test.c'}

    def test_switches(self):
        doc, technical = DOC.read_text(encoding='utf-8'), (ROOT / 'docs' / 'TECHNICAL.md').read_text(encoding='utf-8')
        for name, source in self.SWITCHES.items():
            with self.subTest(name):
                self.assertIn('getenv("%s")' % name, (ROOT / 'runtime' / source).read_text(encoding='utf-8'))
                self.assertIn('`%s=' % name, doc)
                self.assertIn('`%s`' % name, technical)

    def test_every_fight_switch_is_listed(self):
        text = (ROOT / 'runtime' / 'hle_fight_test.c').read_text(encoding='utf-8')
        read = set(re.findall(r'getenv\("(SSBU_\w+)"\)', text))
        self.assertEqual(read - set(self.SWITCHES), set())
        self.assertEqual(len(read), 7)


if __name__ == '__main__':
    unittest.main()
