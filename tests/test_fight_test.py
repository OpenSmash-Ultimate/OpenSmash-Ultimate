"""The fight test switches (docs/TESTING-RUNS.md): SSBU_FIGHT_HASH, SSBU_DETERMINISTIC, SSBU_FIXED_SEED,
SSBU_DEBUG_MATCH(_LIST) and SSBU_DEBUG_END. Their logic (runtime/fight_test.h) through
tools/dev/fighttest.c, with the numbers it prints worked out here on our own: the hash line and its
FNV-1a, the generators' fixed state, Hash40, XXH64 and the seed's streams. Then runtime/hle_fight_test.c
itself through tools/dev/fightruntest.c: its wraps and frame hooks (with runtime/hle_each_frame.c's
wraps) called as the game would, on made-up guest memory mapped at the game addresses, one scenario a
run with its switches set. Made-up numbers throughout."""
from pathlib import Path
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

M64 = (1 << 64) - 1
POSIX_CLANG = shutil.which('clang')
if os.name == 'nt':
    HAVE = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
    WHY = 'needs clang-cl (deps\\llvm, or Visual Studio\'s C++ Clang tools)'
else:
    HAVE = POSIX_CLANG is not None
    WHY = 'needs clang'


# ---- the numbers, worked out here ------------------------------------------------------------
def fnv(data, h=0xcbf29ce484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & M64
    return h


def hash40(s):
    return len(s) << 32 | zlib.crc32(s.encode())


def xxh64(data, seed=0):
    if isinstance(data, str):
        data = data.encode()
    p1, p2, p3, p4, p5 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5
    rotl = lambda v, n: ((v << n) | (v >> (64 - n))) & M64  # noqa: E731
    rnd = lambda acc, w: rotl((acc + w * p2) & M64, 31) * p1 & M64  # noqa: E731
    n, i = len(data), 0
    if n >= 32:
        v = [(seed + p1 + p2) & M64, (seed + p2) & M64, seed, (seed - p1) & M64]
        while i + 32 <= n:
            v = [rnd(v[k], int.from_bytes(data[i + 8 * k:i + 8 * k + 8], 'little')) for k in range(4)]
            i += 32
        h = (rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18)) & M64
        for k in range(4):
            h = ((h ^ rnd(0, v[k])) * p1 + p4) & M64
    else:
        h = (seed + p5) & M64
    h = (h + n) & M64
    while i + 8 <= n:
        h = (rotl(h ^ rnd(0, int.from_bytes(data[i:i + 8], 'little')), 27) * p1 + p4) & M64
        i += 8
    if i + 4 <= n:
        h = (rotl(h ^ (int.from_bytes(data[i:i + 4], 'little') * p1 & M64), 23) * p2 + p3) & M64
        i += 4
    while i < n:
        h = rotl(h ^ (data[i] * p5 & M64), 11) * p1 & M64
        i += 1
    h ^= h >> 33
    h = h * p2 & M64
    h ^= h >> 29
    h = h * p3 & M64
    return h ^ (h >> 32)


GOLDEN = 0x9E3779B97F4A7C15


def mix(z):
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9 & M64
    z = (z ^ (z >> 27)) * 0x94D049BB133111EB & M64
    return z ^ (z >> 31)


class Stream:
    """SSBU_FIXED_SEED's random bytes: splitmix64 from n * golden + 1, 8 bytes a step, the last cut."""
    def __init__(self, n):
        self.state = (n * GOLDEN + 1) & M64

    def take(self, size):
        out = b''
        for i in range(0, size, 8):
            self.state = (self.state + GOLDEN) & M64
            out += struct.pack('<Q', mix(self.state))[:min(8, size - i)]
        return out


def entropy(n, sub):
    return mix(((n * GOLDEN + 1) + GOLDEN * (sub + 1)) & M64)


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def rng_words_sum(words):
    return sum(words[5 * i + 4] for i in range(9)) & 0xFFFFFFFF, fnv(struct.pack('<45I', *words))


def hash_line(run, f, entry, st, mot, mf, x, y, dmg, rngcalls, rh):
    """ssbu-recomp's line and the running hash after it."""
    mf, x, y, dmg = f32(mf), f32(x), f32(y), f32(dmg)
    run = fnv(struct.pack('<qiiQffffQ', f, entry, st if st < 1 << 31 else st - (1 << 32), mot, mf, x, y, dmg, rh), run)
    line = (f'F {f} e {entry} st {st & 0xFFFFFFFF:x} mot {mot:x} mf {bits(mf):08x} x {bits(x):08x} y {bits(y):08x} dmg {bits(dmg):08x} '
            f'rngcalls {rngcalls} rng {(rh ^ (rh >> 32)) & 0xFFFFFFFF:08x} H {run:016x}  ({x:.3f} {y:.3f} frame {mf:.2f} {dmg:.2f}%)')
    return line, run


def rng_fixed():
    w = []
    for i in range(9):
        w += [(0x12345678 + 0x9E3779B9 * i) & 0xFFFFFFFF, 0x2468ACE1 ^ i, 0x0F1E2D3C + i, (0x89ABCDEF - i) & 0xFFFFFFFF, 0]
    return w


class Numbers(unittest.TestCase):
    """(The worked-out numbers above, against published values.)"""

    def test_xxh64_published_values(self):
        self.assertEqual(xxh64(''), 0xEF46DB3751D8E999)
        self.assertEqual(xxh64('a'), 0xD24EC4F1A98C6E5B)
        self.assertEqual(xxh64('abc'), 0x44BC2CF5AD770999)
        self.assertEqual(xxh64('Nobody inspects the spammish repetition'), 0xFBCEA83C8A378BF1)

    def test_fnv1a_published_values(self):
        self.assertEqual(fnv(b''), 0xcbf29ce484222325)
        self.assertEqual(fnv(b'a'), 0xaf63dc4c8601ec8c)
        self.assertEqual(fnv(b'foobar'), 0x85944171f73967e8)


# ---- tools/dev/fighttest.c -------------------------------------------------------------------
def build_logic(out):
    src = str(ROOT / 'tools' / 'dev' / 'fighttest.c')
    if os.name == 'nt':
        exe = os.path.join(out, 'fighttest.exe')
        return [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, src, '/Fe' + exe, '/Fo' + out + os.sep,
                '/link', *cc.LINK_OPTIONS], exe
    exe = os.path.join(out, 'fighttest')
    return [POSIX_CLANG, '-O1', '-Wall', '-Werror', src, '-o', exe], exe


@unittest.skipUnless(HAVE, WHY)
class LogicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.addClassCleanup(shutil.rmtree, cls.dir, ignore_errors=True)
        cmd, cls.exe = build_logic(cls.dir)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('fighttest.c did not build:\n' + r.stdout + r.stderr)
        r = subprocess.run([cls.exe, 'dump'], capture_output=True, text=True, timeout=60)
        cls.dump = r.stdout.splitlines()

    def lines(self, tag):
        return [line[len(tag) + 1:] for line in self.dump if line.startswith(tag + ' ')]

    def test_checks(self):
        r = subprocess.run([self.exe], cwd=self.dir, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'^\d+ checks, 0 failed\n$')

    def test_the_generators_fixed_state(self):
        self.assertEqual([int(w, 16) for w in self.lines('rng_fixed')[0].split()], rng_fixed())

    def test_the_generators_sum_and_hash(self):
        words = [(0x01010101 * i + 7) & 0xFFFFFFFF for i in range(45)]
        calls, rh = rng_words_sum(words)
        self.assertEqual(self.lines('rng_sum'), [f'{calls} {rh:016x}'])
        self.assertEqual(self.lines('rng_none'), ['0 cbf29ce484222325'])

    def test_the_hash_lines(self):
        words = [(0x01010101 * i + 7) & 0xFFFFFFFF for i in range(45)]
        calls, rh = rng_words_sum(words)
        run, want = 0xcbf29ce484222325, []
        for frame in ((1, 0, 0x1a, 0x123456789abc, 1.5, -12.25, 30.0, 0.0, calls, rh),
                      (1, 1, 0, 0, 0.0, 3.140625, -0.5, 12.5, 0, 0xcbf29ce484222325),
                      (2, -1, 0x7fffffff, M64, -1.0, 100000.0, 1e-3, 999.9, 4000000000, 0xfedcba9876543210)):
            line, run = hash_line(run, *frame)
            want.append(line)
        self.assertEqual(self.lines('line'), want)

    def test_hash40(self):
        for line in self.lines('hash40'):
            name, value = re.fullmatch(r"'(.*)' ([0-9a-f]{16})", line).groups()
            self.assertEqual(int(value, 16), hash40(name), name)
        self.assertEqual(len(self.lines('hash40')), 5)

    def test_xxh64(self):
        for line in self.lines('xxh64'):
            seed, data, value = re.fullmatch(r"([01]) '(.*)' ([0-9a-f]{16})", line).groups()
            self.assertEqual(int(value, 16), xxh64(data, M64 if seed == '1' else 0), (seed, data))
        self.assertEqual(len(self.lines('xxh64')), 12)

    def test_the_seed_streams(self):
        for line in self.lines('fill'):
            n, first, more = line.split()
            s = Stream(int(n))
            self.assertEqual(first, s.take(21).hex())
            self.assertEqual(more, s.take(3).hex())
        for line in self.lines('entropy'):
            n, sub0, sub3 = line.split()
            self.assertEqual((int(sub0, 16), int(sub3, 16)), (entropy(int(n), 0), entropy(int(n), 3)))
        self.assertEqual(len(self.lines('fill')), 3)


# ---- tools/dev/fightruntest.c: runtime/hle_fight_test.c on a made-up game --------------------
NAME = 'FIGHTER_INSTANCE_WORK_ID_INT_ENTRY_ID'
MATCH = 'p1=mario,p2=link,p2=cpu,p3=kirby,stage=fd,form=omega,hazards=off,items=on,cpu=9,stock=2'


def made_up_hash_lines(frames, fighters):
    """The lines fightruntest's made-up fighters give (its fighter_frame), frame by frame."""
    words = [(0x01010101 * i + 7) & 0xFFFFFFFF for i in range(45)]
    run, out = 0xcbf29ce484222325, []
    for f in range(1, frames + 1):
        for i in range(fighters):
            words[5 * i + 4] = 100 * i + f
        calls, rh = rng_words_sum(words)
        for i in range(fighters):
            line, run = hash_line(run, f, i, 0x10 * i + f, 0x1000000000 * (i + 1) + f, 0.5 * f, -10 + 1.25 * (f + i), 0.75 * i,
                                  3.5 * f, calls, rh)
            out.append(line)
    return out


@unittest.skipUnless(os.name != 'nt' and POSIX_CLANG, 'needs clang and the runtime\'s POSIX headers (macOS or Linux)')
class RuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.addClassCleanup(shutil.rmtree, cls.dir, ignore_errors=True)
        cls.exe = os.path.join(cls.dir, 'fightruntest')
        rt = ROOT / 'runtime'
        r = subprocess.run([POSIX_CLANG, '-O1', '-Wall', '-Werror', '-pthread', '-I', str(rt / 'macos' / 'include'), '-I', str(rt / 'linux' / 'include'),
                            '-I', str(rt), str(ROOT / 'tools' / 'dev' / 'fightruntest.c'), str(rt / 'hle_fight_test.c'), str(rt / 'hle_each_frame.c'),
                            '-o', cls.exe],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('fightruntest did not build:\n' + r.stdout + r.stderr)

    def run_scenario(self, name, **switches):
        env = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
        env.update(switches)
        r = subprocess.run([self.exe, name], env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'(^|\n)\d+ checks, 0 failed\n$')
        return r

    def test_nothing_set_nothing_changes(self):
        r = self.run_scenario('unset')
        self.assertEqual(r.stderr, '', 'not a word in the log either')

    def test_fight_hash(self):
        path = os.path.join(self.dir, 'hash.txt')
        r = self.run_scenario('hash', SSBU_FIGHT_HASH=path)
        self.assertIn(f'key {xxh64(NAME, M64):016x}\n', r.stdout)
        with open(path) as f:
            self.assertEqual(f.read().splitlines(), made_up_hash_lines(3, 2))
        self.assertIn('[fight] first battle frame (fighter entry 0); input mark "fight" raised', r.stderr)

    def test_fight_hash_on_stdout(self):
        r = self.run_scenario('hash', SSBU_FIGHT_HASH='1')
        self.assertEqual([line for line in r.stdout.splitlines() if line.startswith('F ')], made_up_hash_lines(3, 2))

    def test_fight_hash_on_stdout_line_by_line(self):
        """Written to a file, the standard output is fully buffered on Windows: each hash line goes
        out whole before the next line of the log, which goes to the same file (2>&1)."""
        env = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
        env['SSBU_FIGHT_HASH'] = '1'
        r = subprocess.run([self.exe, 'stdout'], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout)
        lines = r.stdout.splitlines()
        self.assertEqual([line for line in lines if line.startswith('F ')], made_up_hash_lines(3, 2), r.stdout)
        self.assertEqual([line[:2] for line in lines if line.startswith(('F ', '[display]'))], ['F ', '[d'] * 6, r.stdout)

    def test_fight_hash_0_is_off(self):
        r = self.run_scenario('unset', SSBU_FIGHT_HASH='0', SSBU_DETERMINISTIC='0')
        self.assertEqual(r.stderr, '')

    def test_set_but_empty_is_off(self):
        """(As a script clears a switch: SSBU_DEBUG_MATCH= and the rest.)"""
        r = self.run_scenario('unset', **{k: '' for k in ('SSBU_FIGHT_HASH', 'SSBU_DETERMINISTIC', 'SSBU_DET_OFF', 'SSBU_FIXED_SEED',
                                                           'SSBU_DEBUG_MATCH', 'SSBU_DEBUG_MATCH_LIST', 'SSBU_DEBUG_END')})
        self.assertEqual(r.stderr, '')

    def test_deterministic(self):
        r = self.run_scenario('det', SSBU_DETERMINISTIC='1')
        self.assertIn('[fight] SSBU_DETERMINISTIC: the 9 script random generators set to a fixed state, counters 0', r.stderr)

    def test_deterministic_off_mask(self):
        r = self.run_scenario('off', SSBU_DETERMINISTIC='1', SSBU_DET_OFF='7')
        self.assertNotIn('set to a fixed state', r.stderr)

    def test_debug_match(self):
        r = self.run_scenario('match', SSBU_DEBUG_MATCH=MATCH)
        chara = lambda name: 0xc1 << 56 | hash40('ui_chara_' + name)  # noqa: E731
        self.assertIn(f'stage {0x69 << 56 | hash40("ui_stage_end"):016x} form 2 hazards 0 items 2\n', r.stdout)
        self.assertIn(f'state 0 chara {chara("mario"):016x} type 0 colour 0 level 0 stocks 2 x224 00000000\n', r.stdout)
        self.assertIn(f'state 1 chara {chara("link"):016x} type 1 colour 0 level 9 stocks 2 x224 ffffffff\n', r.stdout)
        self.assertIn(f'state 2 chara {chara("kirby"):016x} type 1 colour 2 level 9 stocks 2 x224 ffffffff\n', r.stdout)
        self.assertIn('state 3 chara c1000000000000ad type 3 colour 0 level 0 stocks 3 x224 00000000\n', r.stdout)   # (not asked for)
        self.assertIn('[fight] fight 1 on (the game\'s ready-go) at frame 2 shown', r.stderr)

    def test_debug_match_list(self):
        path = os.path.join(self.dir, 'list.txt')
        with open(path, 'w') as f:
            f.write('p1=mario,stage=battlefield\n\np1=link,items=off\n')
        r = self.run_scenario('list', SSBU_DEBUG_MATCH_LIST=path)
        chara = lambda name: 0xc1 << 56 | hash40('ui_chara_' + name)  # noqa: E731
        stage = 0x69 << 56 | hash40('ui_stage_battle_field')
        got = re.findall(r'^state 0 chara ([0-9a-f]+) .*\nstage ([0-9a-f]+) form \d+ hazards \d+ items (\d+)$', r.stdout, re.M)
        self.assertEqual(got, [(f'{chara("mario"):016x}', f'{stage:016x}', '7'), (f'{chara("mario"):016x}', f'{stage:016x}', '7'),
                               (f'{chara("link"):016x}', f'{stage:016x}', '0'), (f'{chara("link"):016x}', f'{stage:016x}', '0')])

    def test_fixed_seed(self):
        r = self.run_scenario('seed', SSBU_FIXED_SEED='42')
        s = Stream(42)
        self.assertIn(f'bytes {(s.take(21) + s.take(3)).hex()}\n', r.stdout)
        clocks = re.findall(r'^\[fight\] fixed seed: the clock read at ([0-9a-f]+) gave the constant$', r.stderr, re.M)
        self.assertEqual(clocks, ['7100047d20', '7100138534', '710366d7a0', '710366d938'], 'each once, the first time')

    def test_fixed_seed_from_many_threads(self):
        r = self.run_scenario('race', SSBU_FIXED_SEED='42')
        clocks = re.findall(r'^\[fight\] fixed seed: the clock read at ([0-9a-f]+) gave the constant$', r.stderr, re.M)
        self.assertEqual(sorted(clocks), ['7100047d20', '7100138534', '710366d7a0', '710366d938'], 'each once')

    def test_debug_end(self):
        """A fight from frame 3 to 52, then the next from 56: 120 fight frames are the 120th frame of
        the second fight (the count starts again), frame 175. 2s is 120 frames; so is 120."""
        for end in ('2s', '120'):
            with self.subTest(end=end):
                path = os.path.join(self.dir, 'end.txt')
                r = self.run_scenario('end', SSBU_DEBUG_END=end, SSBU_FIGHT_HASH=path)
                self.assertIn('[fight] SSBU_DEBUG_END: 120 fight frames at frame 175 shown: the run ends\n', r.stderr)
                self.assertIn('fight off (the game\'s ready-go cleared) at frame 53 shown, after 50 fight frames', r.stderr)
                self.assertEqual(re.findall(r'\] fight (\d+) on .* at frame (\d+) shown', r.stderr), [('1', '3'), ('2', '56')])
                with open(path) as f:
                    self.assertEqual(len(f.read().splitlines()), 19, 'the hash lines written out at the end')
                # as the window's close: the relay's port mapping removed, then ExitProcess (no atexit handlers)
                self.assertTrue(r.stderr.endswith('the run ends\n(the relay\'s port mapping removed)\n'), r.stderr[-300:])
                self.assertNotIn('atexit', r.stdout)


class WrapTests(unittest.TestCase):
    def test_the_hooks(self):
        """The wraps the switches stand on (the frame end and the record builder by address,
        test_game_addr; their signatures: test_wrap_signature). The frame end and the frame line
        are the frame hooks' (runtime/hle_each_frame.c, test_frame_hooks), which the switches
        register on; the rest are this file's own."""
        text = (ROOT / 'runtime' / 'hle_fight_test.c').read_text(encoding='utf-8')
        self.assertEqual(re.findall(r'^HLE_WRAP\((\w+)\)', text, re.M), ['71017880c0'])
        self.assertEqual(re.findall(r'^HLE_WRAP_EXPORT\((\w+)\)', text, re.M),
                         ['_ZN2nn2os13GetSystemTickEv', 'nnosGetSystemTick', '_ZN2nn2os19GenerateRandomBytesEPvm'])
        frames = (ROOT / 'runtime' / 'hle_each_frame.c').read_text(encoding='utf-8')
        self.assertEqual(re.findall(r'^HLE_WRAP\((\w+)\)', frames, re.M), ['71038701e0'])
        self.assertEqual(re.findall(r'^HLE_WRAP_EXPORT\((\w+)\)', frames, re.M),
                         ['_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv'])
        self.assertIn('frame_hook_add(FRAME_HOOK_LINE, FRAME_ORDER_FIGHT_TEST, frame_line_hook', text)
        self.assertIn('frame_hook_add(FRAME_HOOK_END, FRAME_ORDER_FIGHT_TEST, NULL, frame_end_hook', text)


if __name__ == '__main__':
    unittest.main()
