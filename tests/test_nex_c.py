"""The NEX game server inside the game (runtime/net_nex*.c, plan/EOS_MIGRATION.md phase 2) against tools/nex: both are
fed the same requests and must answer byte for byte the same.

tools/dev/nextest.c drives the C server from stdin. A Tee stands where tools/nex's tests expect their server: every
stream packet, stream close, datagram and tick goes to tools/nex's NexServer and to the C one, and what the two send
back is compared before the test's clients get it. Randomness (session keys, tickets, signature keys, arena IDs,
passwords) is one stream both draw from in the same order - MD5 of a seed and a counter - and the clocks are fixed,
so every byte is comparable, encrypted tickets included. PIDs come from nex_pid_of (FNV-1a of the NSA id) on both
sides, as the C server has no account database.

Run through it: nex.selftest's whole flow (login, register, Quickplay, a ranked round), nex.test_hardening's
matchmaking / tickets / ranked cases and nex.test_arenas' - over raw PRUDP Lite and over a WebSocket (the C server's
stream layer: HTTP upgrade, masked frames, pings) - and Pia's NAT check. Skipped where there is no clang-cl."""
from pathlib import Path
import contextlib
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402
from nex import arenas as py_arenas, crypto as py_crypto, matchmaking as py_mm, prudp as py_prudp  # noqa: E402
from nex import selftest, server as py_server, test_arenas, test_hardening  # noqa: E402
from nex.prudp import lite_size  # noqa: E402

HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)
SEED = b'opensmash-nex-c'
DATETIME = (2026 << 26) | (11 << 22) | (11 << 17) | (12 << 12) | (34 << 6) | 56
ROUND_BASE = 0x6900000000000
SOURCES = ['tools/dev/nextest.c', 'runtime/net_nex_core.c', 'runtime/net_nex_match.c', 'runtime/net_nex_ranked.c']
EXE = None


def build():
    global EXE
    if EXE:
        return EXE
    out = tempfile.mkdtemp()
    exe = os.path.join(out, 'nextest.exe')
    r = subprocess.run([cc.CLANG, '/nologo', '/O2', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, *[str(ROOT / s) for s in SOURCES],
                        '/Fe' + exe, '/Fo' + out + os.sep, '/link', *cc.LINK_OPTIONS], capture_output=True, text=True, cwd=out)
    if r.returncode:
        raise AssertionError('nextest.c did not build:\n' + r.stdout + r.stderr)
    EXE = exe
    return exe


def pid_of(key):
    """runtime/net_nex_core.c's nex_pid_of."""
    h = 0xcbf29ce484222325
    for b in key.encode():
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return 1000000 + h % (0x7FFFFFFF - 1000000)


class Draws:
    """secrets as both servers draw from it in the tests: MD5(seed + counter) blocks, one stream."""

    def __init__(self, seed):
        self.seed, self.counter, self.pool = seed, 0, b''

    def take(self, n):
        while len(self.pool) < n:
            self.pool += hashlib.md5(self.seed + struct.pack('<Q', self.counter)).digest()
            self.counter += 1
        out, self.pool = self.pool[:n], self.pool[n:]
        return out

    def token_bytes(self, n):
        return self.take(n)

    def randbits(self, k):
        n = (k + 7) // 8
        return int.from_bytes(self.take(n), 'little') & ((1 << k) - 1)

    def choice(self, seq):
        return seq[self.take(1)[0] % len(seq)]

    def randbelow(self, n):
        return int.from_bytes(self.take(4), 'little') % n


class Clock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now


def ip_of(text):
    return text


class CServer:
    """nextest.exe: one command, its output lines."""

    def __init__(self, verbose=0):
        self.p = subprocess.Popen([build()], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL if not os.environ.get('NEX_C_LOG') else None,
                                  text=True, bufsize=1)
        self.cmd(f'init {SEED.hex()} {DATETIME} {ROUND_BASE} {verbose}')

    def cmd(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        out = []
        while True:
            got = self.p.stdout.readline()
            if not got:
                raise AssertionError(f'nextest.exe stopped at {line[:80]!r}')
            got = got.rstrip('\n')
            if got == '.':
                return out
            out.append(got)

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(10)
        except Exception:
            self.p.kill()


def ws_payloads(frames):
    """The payload of each server frame (unmasked, whole) in order."""
    out = []
    for f in frames:
        n, i = f[1] & 0x7F, 2
        if n == 126:
            n, i = struct.unpack('>H', f[2:4])[0], 4
        elif n == 127:
            n, i = struct.unpack('>Q', f[2:10])[0], 10
        assert f[0] == 0x82 and len(f) == i + n, f'not one binary frame: {f[:16].hex()}'
        out.append(f[i:])
    return out


def ws_client_frame(data, opcode=2, mask=b'\x37\xfa\x21\x3d'):
    n = len(data)
    head = bytes((0x80 | opcode,))
    head += bytes((0x80 | n,)) if n < 126 else bytes((0x80 | 126,)) + struct.pack('>H', n) if n < 65536 else bytes((0x80 | 127,)) + struct.pack('>Q', n)
    return head + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(data))


UPGRADE = (b'GET /nex HTTP/1.1\r\nHost: 10.13.0.1:60000\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
           b'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: NEX\r\nSec-WebSocket-Version: 13\r\n\r\n')
ACCEPT = b's3pPLMBiTxaQ9kYGzzhZRbK+xOo='   # (RFC 6455's example)


class Tee:
    """tools/nex's NexServer and the C server side by side; attribute reads go to tools/nex's (the tests look inside)."""

    def __init__(self, net, verbose=1, ws=False):
        object.__setattr__(self, 'net', net)
        object.__setattr__(self, 'ws', ws)
        object.__setattr__(self, 'writers', {})
        object.__setattr__(self, 'pending', [])
        object.__setattr__(self, 'clock', Clock())
        object.__setattr__(self, 'mismatches', [])
        draws = Draws(SEED)
        saved = [(py_crypto, 'secrets', py_crypto.secrets), (py_prudp, 'secrets', py_prudp.secrets),
                 (py_mm, 'secrets', py_mm.secrets), (py_arenas, 'secrets', py_arenas.secrets),
                 (py_server, 'datetime_now', py_server.datetime_now), (py_mm, 'datetime_now', py_mm.datetime_now),
                 (py_arenas, 'datetime_now', py_arenas.datetime_now), (py_mm, 'time', py_mm.time)]
        object.__setattr__(self, 'saved', saved)
        for mod in (py_crypto, py_prudp, py_mm, py_arenas):
            mod.secrets = draws
        for mod in (py_server, py_mm, py_arenas):
            mod.datetime_now = lambda: DATETIME
        py_mm.time = self.clock
        py = py_server.make_server(self.py_datagram, verbose=verbose, log=lambda *a: net.log.append(' '.join(map(str, a))))
        py.ranked.next_round = ROUND_BASE
        acc = py.accounts

        def login(nsa, name, now):
            pid = pid_of(nsa)
            acc.names[pid] = name
            acc.nsa_of[pid] = nsa
            return pid
        acc.login = login
        object.__setattr__(self, 'py', py)
        object.__setattr__(self, 'c', CServer())
        object.__setattr__(self, 'parts', {})
        object.__setattr__(self, 'offset', 0.0)   # (seconds added to the clients' clock)

    def restore(self):
        for mod, name, value in self.saved:
            setattr(mod, name, value)
        self.c.close()

    def __getattr__(self, name):
        return getattr(self.py, name)

    def __setattr__(self, name, value):
        if name == 'offset':
            object.__setattr__(self, name, value)
        else:
            setattr(self.py, name, value)

    # ---- what tools/nex sends ----
    def py_datagram(self, addr, port, data, src_ip=None):
        self.pending.append(('d', addr[0], addr[1], src_ip or '10.13.0.1', port, bytes(data)))

    def py_writer(self, addr, port):
        def write(data):
            self.pending.append(('s', addr[0], addr[1], port, bytes(data)))
        return write

    def c_lines(self, lines):
        out = []
        for line in lines:
            f = line.split(' ')
            if f[0] == 's':
                data = bytes.fromhex(f[4]) if len(f) > 4 else b''
                if self.ws:
                    (data,) = ws_payloads([data])
                out.append(('s', f[1], int(f[2]), int(f[3]), data))
            elif f[0] == 'd':
                out.append(('d', f[1], int(f[2]), f[3], int(f[4]), bytes.fromhex(f[5]) if len(f) > 5 else b''))
            else:
                raise AssertionError(f'nextest.exe: {line}')
        return out

    def compare(self, what, c_out):
        py_out = list(self.pending)
        self.pending.clear()
        if py_out != c_out:
            def show(o):
                return [(x[:-1] + (x[-1].hex(),)) for x in o]
            raise AssertionError(f'{what}: tools/nex and the C server answer differently\n'
                                 f'tools/nex: {show(py_out)}\nC:         {show(c_out)}')
        for x in py_out:
            if x[0] == 's':
                w = self.writers.get(((x[1], x[2]), x[3]))
                if w:
                    w(x[4])

    def set_now(self, now):
        self.clock.now = now
        self.c.cmd(f'now {now!r}')

    def send_part(self, ip):
        part = self.py.part_of(ip)
        if self.parts.get(ip, 0) != part:
            self.parts[ip] = part
            self.c.cmd(f'part {ip} {"-" if part is None else part}')

    # ---- what the tests' clients do ----
    def stream_open(self, addr, port, write):
        r = self.py.stream_open(addr, port, self.py_writer(addr, port))
        rc = self.c.cmd(f'open {addr[0]} {addr[1]} {port}')
        assert rc == [f'ret {int(r)}'], (r, rc)
        if r:
            self.writers[(addr, port)] = write
            if self.ws:
                got = self.c.cmd(f'bytes {addr[0]} {addr[1]} {port} {UPGRADE.hex()}')
                assert got[-1] == 'ret 1', got
                (line,) = got[:-1]
                resp = bytes.fromhex(line.split(' ')[4])
                assert resp.startswith(b'HTTP/1.1 101 Switching Protocols\r\n') and b'Sec-WebSocket-Accept: ' + ACCEPT + b'\r\n' in resp, resp
        return r

    def stream_packet(self, addr, port, data, now):
        now += self.offset
        self.send_part(addr[0])
        self.set_now(now)
        self.py.stream_packet(addr, port, data, now)
        if self.ws:
            lines = self.c.cmd(f'bytes {addr[0]} {addr[1]} {port} {ws_client_frame(data).hex()}')
            assert lines[-1] == 'ret 1', lines
            lines = lines[:-1]
        else:
            lines = self.c.cmd(f'pkt {addr[0]} {addr[1]} {port} {bytes(data).hex()}')
        self.compare(f'packet from {addr} to {port}', self.c_lines(lines))

    def stream_closed(self, addr, port):
        self.py.stream_closed(addr, port)
        self.writers.pop((addr, port), None)
        self.compare(f'stream from {addr} closed', self.c_lines(self.c.cmd(f'close {addr[0]} {addr[1]} {port}')))

    def datagram(self, addr, port, data, now, to_ip=None):
        self.set_now(now)
        self.py.datagram(addr, port, data, now, to_ip)
        self.compare(f'datagram from {addr} to {port}',
                     self.c_lines(self.c.cmd(f'dgram {addr[0]} {addr[1]} {port} {to_ip or "10.13.0.1"} {bytes(data).hex()}')))

    def tick(self, now):
        self.clock.now = now
        self.py.tick(now)
        self.compare('tick', self.c_lines(self.c.cmd(f'tick {now!r}')))


TEES = []


class TeeNet:
    """nex.selftest.Net / nex.test_hardening.Net with the Tee as its server."""
    ws = False

    def __init__(self, state_dir=None, verbose=1):
        self.boxes = {}
        self.log = []
        self.server = Tee(self, verbose, ws=self.ws)
        TEES.append(self.server)


class WsTeeNet(TeeNet):
    ws = True


def with_tee(cls, net=TeeNet, skip=()):
    """tools/nex's test case cls, run over streams with the Tee for its server."""
    class Teed(cls):
        lite = True

        def setUp(self):
            self.saved_nets = test_hardening.Net, selftest.Net
            test_hardening.Net = selftest.Net = net
            self.addCleanup(self.untee)
            super().setUp()

        def untee(self):
            test_hardening.Net, selftest.Net = self.saved_nets
            while TEES:
                TEES.pop().restore()
    for name in skip:
        setattr(Teed, name, None)
    Teed.__name__ = Teed.__qualname__ = cls.__name__ + ('Ws' if net.ws else '') + 'AgainstC'
    return Teed


def quiet(fn, *a):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*a)


SKIP_MM = ('test_far_ratings_still_meet',                       # (changes tools/nex's rating function alone)
           'test_players_of_a_match_talk_a_little_after_one_left')   # (leaves a match inside tools/nex alone)
SKIP_TICKETS = ('test_item5_accounts_written_in_batches_and_made_at_a_bounded_rate',)   # (tools/nex's account file)

if HAVE_CLANG:
    MatchmakingAgainstC = with_tee(test_hardening.Matchmaking, skip=SKIP_MM)
    TicketsAgainstC = with_tee(test_hardening.Tickets, skip=SKIP_TICKETS)
    LiteTicketsAgainstC = with_tee(test_hardening.LiteTickets)
    RankedAgainstC = with_tee(test_hardening.Ranked)
    ArenasAgainstC = with_tee(test_arenas.Arenas)
    MatchmakingWsAgainstC = with_tee(test_hardening.Matchmaking, WsTeeNet, skip=SKIP_MM)
    ArenasWsAgainstC = with_tee(test_arenas.Arenas, WsTeeNet)


@unittest.skipUnless(HAVE_CLANG, 'needs Windows and clang-cl (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class Flows(unittest.TestCase):
    def tearDown(self):
        while TEES:
            TEES.pop().restore()

    def selftest(self, net):
        saved = selftest.Net, selftest.LITE
        selftest.Net, selftest.LITE = net, True
        try:
            quiet(selftest.main)
        finally:
            selftest.Net, selftest.LITE = saved

    def test_selftest_over_lite(self):
        """nex.selftest: two players log in, register, meet in Quickplay and play a ranked round."""
        self.selftest(TeeNet)

    def test_selftest_over_a_websocket(self):
        self.selftest(WsTeeNet)

    def test_ratings_and_elo(self):
        """Quickplay by rating (the closest gathering, the window widening while one waits) and the Elo of a
        three-player round with a draw, through both servers."""
        net = TeeNet()
        t = net.server
        h = test_hardening
        p = {n: quiet(selftest.login, net, (f'10.13.0.{n}', 49152), 0x3000 + n, f'P{n}') for n in range(2, 8)}

        def stats(c, initial, category=7):
            m = h.call(c, 120, 12, h.stats_param(category, initial))
            self.assertTrue(m.success)

        for n, rating in ((2, 1500), (3, 2600), (4, 1580), (5, 9000), (6, 2000), (7, 2010)):
            stats(p[n], rating)

        def create(c):
            return py_mm.Session.parse(h.StreamIn(h.call(c, 109, 38, h.session_param()).body))
        ga, gb, gc = create(p[4]), create(p[5]), create(p[7])          # (1580, 9000, 2010: waiting)
        self.assertEqual(quiet(selftest.auto_matchmake, p[6]).gid, gc.gid)   # (10 apart)
        t.offset = 30.0                                                # (they have waited: 150 + 25 * 30 wide)
        self.assertEqual(quiet(selftest.auto_matchmake, p[2]).gid, ga.gid)   # (80 apart; 9000's: a fallback)
        self.assertEqual(quiet(selftest.auto_matchmake, p[3]).gid, gb.gid)   # (only the far one left: met anyway)
        # a free-for-all of three (a gathering of four made for it)
        a, b, c = p[2], p[4], p[3]
        m = h.call(p[5], 109, 39, h.join_param(ga.gid))
        self.assertFalse(m.success)                                    # (full: a gathering of two)
        g = py_mm.Session.parse(h.StreamIn(h.call(a, 109, 38, h.session_param(max_participants=4)).body))
        for x in (b, c):
            self.assertTrue(h.call(x, 109, 39, h.join_param(g.gid)).success)
        rid = h.StreamIn(h.call(a, 120, 1, h.round_param(7, g.gid, [a.pid, b.pid, c.pid])).body).u64()
        self.assertTrue(h.call(b, 120, 3, h.end_param(rid, [(c.pid, 1), (a.pid, 3), (b.pid, 3), (a.pid, 2)])).success)
        for x in (a, b, c):
            stats(x, 1000)
        self.assertTrue(h.call(c, 120, 9, struct.pack('<I', 2) + h.target_param(a.pid, 7) + h.target_param(c.pid, 7)).success)
        self.assertTrue(h.call(c, 120, 10, h.target_param(a.pid, 0)).success)
        self.assertTrue(h.call(a, 120, 4, struct.pack('<Q', rid)).success)
        self.assertTrue(h.call(c, 109, 36, struct.pack('<I', g.gid)).success is False)   # (not its owner)
        self.assertTrue(h.call(a, 109, 36, struct.pack('<I', g.gid)).success)            # (a system password)
        self.assertTrue(h.call(a, 21, 41, struct.pack('<I', g.gid)).success)
        self.assertTrue(h.call(a, 50, 2, struct.pack('<I', g.gid) + b'\1').success)
        self.assertTrue(h.call(b, 50, 1, struct.pack('<I', g.gid) + test_arenas.text('bye')).success)
        t.stream_closed(a.addr, py_server.SECURE_PORT)                 # (the owner gone: c owns it now)
        self.assertTrue(h.call(c, 21, 2, struct.pack('<I', g.gid)).success)

    def test_pids(self):
        c = CServer()
        try:
            for key in ('0000000000001111', '7e57c0de5ec2e7a1', '00020000aabbccdd0011223344556677'):
                self.assertEqual(c.cmd(f'pid {key}'), [f'ret {pid_of(key)}'])
        finally:
            c.close()

    def test_nat_check(self):
        """Pia's NAT check: 101 and 102 to nncs1, 103 to nncs2, each player its own address; the keepalive's port goes
        into the station URL."""
        net = TeeNet()
        t = net.server
        for kind, port, to in ((101, 10025, None), (102, 10025, None), (102, 10125, None), (103, 10125, '10.13.255.254'),
                               (104, 10025, None), (101, 10025, '10.13.255.254')):
            t.datagram(('10.13.0.7', 50123), port, struct.pack('>I', kind) + bytes(12), 0.0, to)
        t.datagram(('10.13.0.7', 50123), 10025, struct.pack('>I', 101) + bytes(11), 0.0)   # (short: nothing)
        t.datagram(('10.13.0.7', 49999), 33335, bytes.fromhex('32ab9864') + bytes(20), 0.0)
        t.datagram(('10.13.0.7', 40000), 33335, bytes.fromhex('32ab9864'), 0.0, '10.13.255.254')   # (to nncs2: not kept)
        a = quiet(selftest.login, net, ('10.13.0.7', 49152), 0x7777, 'Golf')
        reg = test_hardening.call(a, 11, 3, struct.pack('<IQ', 0, a.pid))
        self.assertTrue(reg.success)

    def test_websocket_stream(self):
        """The stream layer alone: the upgrade, packets cut across frames and several in one, a ping, a close."""
        c = CServer()
        try:
            self.assertEqual(c.cmd('open 10.13.0.2 5000 60000'), ['ret 1'])
            self.assertEqual(c.cmd('open 10.13.0.2 5000 60000'), ['ret 0'])
            out = c.cmd('bytes 10.13.0.2 5000 60000 ' + UPGRADE[:30].hex())
            self.assertEqual(out, ['ret 1'])
            out = c.cmd('bytes 10.13.0.2 5000 60000 ' + UPGRADE[30:].hex())
            self.assertEqual(out[-1], 'ret 1')
            self.assertIn(b'Sec-WebSocket-Accept: ' + ACCEPT, bytes.fromhex(out[0].split(' ')[4]))
            syn = py_prudp.Packet(py_prudp.SYN, py_prudp.F_NEED_ACK | py_prudp.F_HAS_SIZE, src=0x1F, dst=0x01,
                                  options={py_prudp.O_SUPPORT: b'\4\0\0\0', py_prudp.O_SIGNATURE: bytes(16)})
            syn = py_prudp.encode_lite(syn)
            ping = py_prudp.encode_lite(py_prudp.Packet(py_prudp.LITE_PING, py_prudp.F_NEED_ACK, src=0x2F, dst=0x01, seq=3))   # (no connection: no answer)
            self.assertEqual(lite_size(syn), len(syn))
            both = ws_client_frame(syn[:5]) + ws_client_frame(syn[5:] + ping) + ws_client_frame(b'hi', 9)
            out = c.cmd('bytes 10.13.0.2 5000 60000 ' + both.hex())
            self.assertEqual(out[-1], 'ret 1')
            frames = [bytes.fromhex(line.split(' ')[4]) for line in out[:-1]]
            self.assertEqual(len(frames), 2, out)
            (synack,) = ws_payloads(frames[:1])
            self.assertEqual(synack[0], 0x80)
            self.assertEqual(synack[8] & 0xF, 0)   # (SYN)
            self.assertEqual(frames[1], bytes((0x8A, 2)) + b'hi')   # (the pong)
            self.assertEqual(c.cmd('bytes 10.13.0.2 5000 60000 ' + ws_client_frame(b'', 8).hex()), ['ret 0'])
            self.assertEqual(c.cmd('bytes 10.13.0.2 5001 60000 ' + b'\x16\x03\x01'.hex()), ['ret 0'])   # (no such stream)
            self.assertEqual(c.cmd('open 10.13.0.2 5001 60000'), ['ret 1'])
            self.assertEqual(c.cmd('bytes 10.13.0.2 5001 60000 ' + b'\x16\x03\x01'.hex()), ['ret 0'])   # (TLS: not NEX)
        finally:
            c.close()


class Wiring(unittest.TestCase):
    """What ties the server into the game, read from the sources (the game itself is run elsewhere)."""

    def src(self, name):
        return (ROOT / 'runtime' / name).read_text(encoding='utf-8')

    def test_streams_and_datagrams(self):
        online, net = self.src('hle_online.c'), self.src('hle_net.c')
        self.assertIn('return net_configured() && net_backend == &net_eos_backend;', online, 'over EOS, the server is in the game')
        self.assertIn('const char *v = getenv("SSBU_NEX_LOCAL");', online)
        self.assertIn('int ok = ns && nex_stream_bytes(ns, vip, vport, dport, (const u8 *)p, left);', online,
                      'a stream to 10.13.0.1 goes to the server in the game')
        self.assertIn('if (ns) nex_stream_closed(ns, vip, vport, dport);', online)
        self.assertIn('nex_datagram(s, src_ip, sport, dport, dst == server ? NEX_HOST : NEX_NAT_SECOND, data, len);', online)
        self.assertIn('vsock_deliver(src_ip, sport, ip, port, data, (u32)n);', online, 'the NAT check is answered to the player')
        self.assertIn('!online_nex_datagram(me, sport, dst, dport, data, (u32)len)', net)

    def test_rollback_rewind_kept(self):
        online = self.src('hle_online.c')
        self.assertIn('if (rb_keep) { hist_put(&st->hist, st->taken, st->buf, n); st->taken += n; }', online)
        self.assertIn('if (st->shist && (u8)*p == st->shist[st->resend % HIST]) { p++; left--; st->resend++;', online)
        self.assertIn('if (st->local) { p = data; left = len; }', online,
                      'a resend that differs goes to the server in the game whole (a WebSocket frame, a repeat)')
        start = online.index('static void nex_stream_out(')
        self.assertIn('memcpy(st->buf + st->len, data, n);', online[start:start + 1500],
                      'the server\'s answers go into the stream\'s buffer, which a load rewinds')


if __name__ == '__main__':
    unittest.main()
