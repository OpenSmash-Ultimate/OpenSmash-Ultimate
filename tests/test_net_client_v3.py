"""The game's netplay client (runtime/net_relay.c) in protocol SN versions 2 and 3, through
tools/dev/netv3test.c (the client with no game around it, driven line by line), against the real
lobby (tools/ssbu_relay.py, on 127.0.0.1 with free ports), the lobby as it was before version 3, a
private room's relay in another netv3test, and a fake lobby that checks every byte: without
SSBU_NET_PROTOCOL the client's packets are version 2's, as before; with SSBU_NET_PROTOCOL=3 its JOIN
says the simulation version (runtime/sim_version.h) and a mod set of none, two games of one key meet
(matchmaking, direct connections in version 3) and games of another simulation version or of
version 2 don't, the room browser says why a room can't be joined, and a lobby or relay of version 2
gets the game in version 2. Made-up data only, loopback only; waits are for conditions.

Built with the platform's compiler as tests/test_kernel_threads.py builds kerneltest.c: clang-cl on
Windows, clang with the platform layer elsewhere (runtime/linux/win32.c, with runtime/macos/'s memory
and platform parts on macOS). Skipped where there is no such compiler, and on hosts the platform
layer is not written for (Linux on x86-64 and macOS on Apple silicon only, as the game)."""
from pathlib import Path
import os
import platform
import queue
import re
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
import cc  # noqa: E402
import ssbu_relay as R  # noqa: E402
from test_ssbu_relay import BUILD, Player, Server, info_of  # noqa: E402
from test_ssbu_relay_v3 import V3Player, mods, until  # noqa: E402

if os.name == 'nt':
    HAVE_CLANG = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
else:
    HAVE_CLANG = shutil.which('clang') is not None and platform.machine() in (('arm64',) if sys.platform == 'darwin' else ('x86_64', 'AMD64'))

SIM = int(re.search(r'#define SIM_VERSION (\d+)u', (ROOT / 'runtime' / 'sim_version.h').read_text()).group(1))
LDN = R.LDN_PORT
GAME = 12345
BCAST = R.BROADCASTS[0]
FAKE_IP, FAKE_COOKIE = R.SUBNET + 0x0105, 0x1122334455667788   # (what the fake lobby gives)


def build(folder):
    src = str(ROOT / 'tools' / 'dev' / 'netv3test.c')
    if os.name == 'nt':   # (as tests/test_settings.py builds netcfgtest.c, which includes net_relay.c too)
        exe = os.path.join(folder, 'netv3test.exe')
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, src, '/Fe' + exe, '/Fo' + os.path.join(folder, 'netv3test.obj'),
               '/link', *cc.LINK_OPTIONS]
    else:
        exe = os.path.join(folder, 'netv3test')
        layer = ['runtime/linux/win32.c']   # (the platform layer)
        if sys.platform == 'darwin':
            layer += ['runtime/macos/memory.c', 'runtime/macos/platform.c']
        else:
            layer += ['-lpthread']
        cmd = ['clang', '-O1', '-w', '-I', 'runtime/macos/include', '-I', 'runtime/linux/include', '-I', 'runtime', src, *layer, '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError('netv3test.c did not build:\n' + r.stdout + r.stderr)
    return exe


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Game:
    """One game's netplay client (a netv3test), configured as the launchers configure the game: a
    command in, its answer out; what it logs is kept for the failure messages."""
    def __init__(self, exe, relay=None, protocol=None, room='v3client', name='P', **more):
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SSBU_NET_') and k.upper() != 'NETV3TEST_BUILD'}
        env.update(SSBU_NET_ROOM=room, SSBU_NET_NAME=name, SSBU_NET_UPNP='0', NETV3TEST_BUILD=BUILD)
        if relay is not None:
            env['SSBU_NET_RELAY'] = relay
        if protocol is not None:
            env['SSBU_NET_PROTOCOL'] = str(protocol)
        env.update({k: str(v) for k, v in more.items()})
        self.name = name
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env,
                                  cwd=os.path.dirname(exe))   # (in the build's folder: nothing it writes lands in the checkout)
        self.answers, self.log = queue.Queue(), []
        for stream, keep in ((self.p.stdout, self.answers.put), (self.p.stderr, self.log.append)):
            threading.Thread(target=lambda s=stream, k=keep: [k(line) for line in s], daemon=True).start()
        self.ip = None

    def send(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()

    def answer(self, wait=15.0):
        try:
            return self.answers.get(timeout=wait).rstrip('\n')
        except queue.Empty:
            raise AssertionError(f'{self.name}: no answer; its log:\n' + ''.join(self.log)) from None

    def ask(self, line, wait=15.0):
        self.send(line)
        return self.answer(wait)

    def joined(self, line=None):
        """'joined VERSION ADDRESS FLAGS FELL_BACK MEMBERS' as numbers (None: not joined)."""
        words = (line or self.answer()).split()
        if words[0] != 'joined':
            return None
        version, self.ip, flags, fell_back, members = map(int, words[1:])
        return version, self.ip, flags, fell_back, members

    def join(self, wait=5):
        got = self.joined(self.ask(f'join {wait}', wait + 10))
        assert got is not None, f'{self.name} did not join; its log:\n' + ''.join(self.log)
        return got

    def rooms(self, wait=3):
        """The room browser: (state, [dict of a room])."""
        self.send(f'rooms {wait}')
        return self.rooms_answer(wait + 10)

    def rooms_answer(self, wait=13):
        state, n = map(int, self.answer(wait).split()[1:])
        out = []
        for _ in range(n):
            head, name, b, mod_names, why = self.answer().split('|')
            host, players, most, is_open, mine, joinable, sim, n_mods = map(int, head.split()[1:])
            out.append(dict(host=host, players=players, most=most, open=is_open, mine=mine, joinable=joinable, sim=sim, nmods=n_mods,
                            name=name, build=b, mods=mod_names, why=why))
        return state, out

    def match(self, state=-1, wait=0):
        head, name = self.ask(f'match {state} {wait}', wait + 10).split('|')
        keys = ('state', 'role', 'open', 'peer', 'waited', 'players', 'searching', 'code_players')
        return dict(zip(keys, map(int, head.split()[1:])), name=name)

    def list(self, seq=1, wait=2):
        """The game's own Rooms List: its rooms' hosts, or None when the lobby did not answer."""
        words = list(map(int, self.ask(f'list {seq} {wait}', wait + 10).split()[1:]))
        return None if words[0] < 0 else words[1:]

    def datagram(self, dst, data, sport=GAME, dport=GAME):
        self.assertok(self.ask(f'send {dst} {sport} {dport} {data.hex()}'))

    def ldn(self, dst, kind, payload):
        self.datagram(dst, b'L' + bytes((kind, 0, 0)) + payload, LDN, LDN)

    def received(self):
        n = int(self.ask('recv').split()[1])
        out = []
        for _ in range(n):
            src, sport, dst, dport, data = self.answer().split(' ')
            out.append((int(src), int(sport), int(dst), int(dport), bytes.fromhex(data)))
        return out

    def peers(self, wait=0, want=0):
        direct, n = map(int, self.ask(f'peers {wait} {want}', wait + 10).split()[1:])
        out = []
        for _ in range(n):
            ip, state, *counts = self.answer().split()[1:]
            out.append((int(ip), state, *map(int, counts)))
        return direct, out

    def status(self):
        """net_status's line (asking it starts the client, as the game's overlay does)."""
        return self.ask('status')[len('status '):]

    def assertok(self, line):
        assert line == 'ok', line

    def close(self):
        if self.p.poll() is None:
            try:
                self.send('quit')
                self.p.wait(10)
            except (OSError, subprocess.TimeoutExpired):
                self.p.kill()
                self.p.wait(10)
        for f in (self.p.stdin, self.p.stdout, self.p.stderr):
            try:
                f.close()
            except OSError:
                pass


class V2OnlyLobby(R.Lobby):
    """The lobby as it was before version 3: any other version's JOIN is answered with REJECT reason 2,
    in that version (main's tools/ssbu_relay.py), and nothing else of it is read."""
    def packet(self, p, at, now):
        if p[2] != R.PROTOCOL:
            if p[3] == R.JOIN and len(p) >= R.V1_JOIN_SIZE and self.gate(at, now):
                self.reject(self.sn, at, p[4:12], 2, f"different netplay version (lobby {R.PROTOCOL}, you {p[2]})", p[2])
            return
        super().packet(p, at, now)


@unittest.skipUnless(HAVE_CLANG, 'needs clang on x86-64 Linux or an Apple silicon Mac, or clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class ClientTest(unittest.TestCase):
    exe = None

    @classmethod
    def setUpClass(cls):
        if ClientTest.exe is None:   # (built once for every class here)
            folder = tempfile.mkdtemp(prefix='netv3test ')
            ClientTest.folder = folder
            ClientTest.exe = build(folder)

    def setUp(self):
        self.games = []
        self.fakes = []

    def tearDown(self):
        for g in self.games:
            g.close()
        for f in self.fakes:
            f.close()

    def game(self, relay, **kw):
        g = Game(self.exe, relay, **kw)
        self.games.append(g)
        return g


def tearDownModule():
    if ClientTest.exe is not None:
        shutil.rmtree(ClientTest.folder, ignore_errors=True)


class FakeLobby:
    """A UDP socket the game takes for its lobby: the test reads each packet and answers it by hand."""
    def __init__(self):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(('127.0.0.1', 0))
        self.port = self.s.getsockname()[1]
        self.address = f'127.0.0.1:{self.port}'

    def next(self, kind=None, wait=3.0):
        """The next packet (of that kind: the others before it are passed over) and where it came from."""
        end = time.monotonic() + wait
        while True:
            self.s.settimeout(max(end - time.monotonic(), 0.001))
            try:
                data, at = self.s.recvfrom(65536)
            except (socket.timeout, ConnectionResetError):
                raise AssertionError(f'no packet of kind {kind} came') from None
            if kind is None or (len(data) >= 4 and data[3] == kind):
                return data, at

    def kinds(self, wait=0.5):
        """The kinds of the packets that come within wait seconds."""
        out, end = [], time.monotonic() + wait
        while time.monotonic() < end:
            self.s.settimeout(max(end - time.monotonic(), 0.001))
            try:
                data, _ = self.s.recvfrom(65536)
            except (socket.timeout, ConnectionResetError):
                break
            out.append((data[2], data[3]))
        return out

    def close(self):
        self.s.close()


class Bytes(ClientTest):
    """Every packet the client sends, byte by byte, to a fake lobby and a fake player: version 2's
    without the switch (today's layouts, field by field), version 3's with it."""
    def exchange(self, version, protocol):
        lobby, peer = FakeLobby(), FakeLobby()   # (the "peer": another player, reached directly)
        self.fakes += [lobby, peer]
        g = self.game(lobby.address, protocol=protocol, room='bytes', name='Alpha')
        head = lambda kind: b'SN' + bytes((version, kind))   # noqa: E731
        g.send('join 5')
        join, at = lobby.next(R.JOIN)
        nonce = join[4:12]
        want = head(R.JOIN) + nonce + b'bytes'.ljust(32, b'\0') + b'Alpha'.ljust(32, b'\0') + BUILD.encode().ljust(40, b'\0')
        if version == R.V3:
            want += struct.pack('>I', SIM) + b'\0'   # (the simulation version, a mod set of none)
        self.assertEqual(join, want)
        flags = 3 if version == R.PROTOCOL else 31
        lobby.s.sendto(head(R.WELCOME) + nonce + struct.pack('>IQB', FAKE_IP, FAKE_COOKIE, flags) + b'\0', at)
        self.assertEqual(g.joined(), (version, FAKE_IP, flags, 0, 0))
        ping, _ = lobby.next(R.PING)
        sent = ping[12:20]
        # (no PONG yet: ping 0xFFFF; peers version 0, its own address and port, direct connections allowed, none direct)
        self.assertEqual(ping, head(R.PING) + nonce + sent + struct.pack('>QH', FAKE_COOKIE, 0xFFFF) +
                         struct.pack('>IIHBB', 0, 0x7F000001, at[1], 1, 0))
        pong = head(R.PONG) + sent + struct.pack('>HHB', 4, 2, 1)
        lobby.s.sendto(pong + (struct.pack('>HH', 3, 9) if version == R.V3 else b''), at)
        g.send('list 9 3')
        self.assertEqual(lobby.next(R.LIST)[0], head(R.LIST) + struct.pack('>QI', FAKE_COOKIE, 9))   # (no flags byte: rooms it can join)
        lobby.s.sendto(head(R.LIST_END) + struct.pack('>IB', 9, 0), at)
        self.assertEqual(g.answer(), 'list 0')
        g.datagram(FAKE_IP + 1, b'through the lobby')
        self.assertEqual(lobby.next(R.DATA)[0], head(R.DATA) + struct.pack('>IIHH', FAKE_IP, FAKE_IP + 1, GAME, GAME) + b'through the lobby')
        # introduced to the fake player: it punches to it, and once answered sends straight there
        secret, pip = 0x0F0E0D0C0B0A0908, FAKE_IP + 1
        entry = R.PEER.pack(pip, b'Bravo', 1, 0x7F000001, peer.port, 0, 0, secret)
        lobby.s.sendto(head(R.PEERS) + struct.pack('>QIB', FAKE_COOKIE, 1, 1) + entry, at)
        hello, hat = peer.next(R.PUNCH)
        self.assertEqual(hat, at)
        self.assertEqual(hello[:21], head(R.PUNCH) + struct.pack('>QIIB', secret, FAKE_IP, pip, 0))
        self.assertEqual(len(hello), 29)
        peer.s.sendto(head(R.PUNCH) + struct.pack('>QIIB', secret, pip, FAKE_IP, 1) + hello[21:29], at)   # (its answer)
        direct, (p,) = g.peers(3, 1)
        self.assertEqual((direct, p[:2]), (1, (pip, 'direct')))
        g.datagram(pip, b'straight')
        self.assertEqual(peer.next(R.DIRECT)[0], head(R.DIRECT) + struct.pack('>QIIHH', secret, FAKE_IP, pip, GAME, GAME) + b'straight')
        other = R.PROTOCOL if version == R.V3 else R.V3   # (a direct packet of the other version: dropped)
        for v in (other, version):
            peer.s.sendto(b'SN' + bytes((v, R.DIRECT)) + struct.pack('>QIIHH', secret, pip, FAKE_IP, GAME, GAME) + b'v%d' % v, at)
        self.assertTrue(until(lambda: g.peers()[1][0][2] >= 1))
        self.assertEqual([d[4] for d in g.received()], [b'v%d' % version])
        return g, lobby, nonce, head

    def test_version_2_as_before(self):
        """Without SSBU_NET_PROTOCOL: JOIN, PING, LIST, DATA, PUNCH, DIRECT and LEAVE as version 2 has
        always had them, and no room browser or matchmaking."""
        g, lobby, nonce, head = self.exchange(R.PROTOCOL, None)
        self.assertEqual(g.rooms(0)[0], 0)   # (NET_ROOMS_OFF)
        g.assertok(g.ask('find'))
        self.assertEqual(g.match()['state'], 8)   # (NET_MATCH_OFF)
        self.assertEqual({k for _, k in lobby.kinds(0.6)} - {R.PING}, set())   # (nothing else went: no ROOMS, no MATCH)
        g.send('quit')
        self.assertEqual(lobby.next(R.LEAVE)[0], head(R.LEAVE) + nonce)

    def test_version_2_asked_for(self):
        lobby = FakeLobby()
        self.fakes.append(lobby)
        g = self.game(lobby.address, protocol=2)
        g.send('join 1')
        join, _ = lobby.next(R.JOIN)
        self.assertEqual((join[:4], len(join)), (b'SN\x02\x01', R.JOIN_SIZE))

    def test_version_3(self):
        """SSBU_NET_PROTOCOL=3: the same in version 3's header, its JOIN with the simulation version and
        a mod set of none; the room browser and matchmaking ask the lobby, and PONG's counts are read."""
        g, lobby, nonce, head = self.exchange(R.V3, 3)
        m = g.match()
        self.assertEqual((m['state'], m['searching'], m['code_players']), (0, 3, 9))   # (idle; PONG's counts)
        g.send('rooms 3')
        rooms, at = lobby.next(R.ROOMS)
        seq = rooms[12:16]
        self.assertEqual(rooms, head(R.ROOMS) + struct.pack('>Q', FAKE_COOKIE) + seq)
        info = (head(R.ROOM_INFO) + seq + struct.pack('>IBBB', FAKE_IP + 7, 2, 4, 1) + b'Charlie'.ljust(32, b'\0') +
                BUILD.encode().ljust(40, b'\0') + struct.pack('>I', SIM) + mods((b'Alpha', 5)))
        lobby.s.sendto(info, at)
        lobby.s.sendto(head(R.ROOMS_END) + seq + b'\1', at)
        state, got = g.rooms_answer()
        self.assertEqual(state, 3)   # (NET_ROOMS_LISTED: the answer that came)
        self.assertEqual({k: got[0][k] for k in ('host', 'players', 'most', 'open', 'joinable', 'sim', 'nmods', 'name', 'mods', 'why')},
                         dict(host=FAKE_IP + 7, players=2, most=4, open=1, joinable=0, sim=SIM, nmods=1, name='Charlie', mods='Alpha',
                              why="Mods you don't have: Alpha"))
        g.assertok(g.ask('find'))
        match = lobby.next(R.MATCH)[0]
        sid = match[13:17]
        self.assertEqual(match, head(R.MATCH) + struct.pack('>QB', FAKE_COOKIE, 1) + sid)
        g.assertok(g.ask('cancel'))
        self.assertEqual(lobby.next(R.MATCH)[0], head(R.MATCH) + struct.pack('>QB', FAKE_COOKIE, 0) + sid)
        g.send('quit')
        self.assertEqual(lobby.next(R.LEAVE)[0], head(R.LEAVE) + nonce)

    def test_falls_back_on_reject_2(self):
        """A REJECT of reason 2 (here in version 3, as the lobby before version 3 answered): the next JOIN
        is version 2's, at once, and a late answer to the JOIN of version 3 changes nothing."""
        lobby = FakeLobby()
        self.fakes.append(lobby)
        g = self.game(lobby.address, protocol=3)
        g.status()   # (started; "join" would wait, and the test asks more meanwhile)
        join, at = lobby.next(R.JOIN)
        self.assertEqual(join[:3], b'SN\x03')
        reject = b'SN\x03' + bytes((R.REJECT,)) + join[4:12] + b'\2' + b'different netplay version (lobby 2, you 3)'.ljust(64, b'\0')
        lobby.s.sendto(reject, at)
        again, _ = lobby.next(R.JOIN, wait=1.0)   # (no 2 s pause, as after any other REJECT)
        self.assertEqual(again, b'SN\x02\x01' + join[4:R.JOIN_SIZE])
        lobby.s.sendto(reject, at)   # (late)
        self.assertTrue(until(lambda: 'connecting' in g.status()))   # (not refused)
        lobby.s.sendto(b'SN\x02' + bytes((R.WELCOME,)) + join[4:12] + struct.pack('>IQB', FAKE_IP, FAKE_COOKIE, 3) + b'\0', at)
        self.assertEqual(g.joined(g.ask('join 3')), (2, FAKE_IP, 3, 1, 0))
        self.assertEqual(lobby.next(R.PING)[0][:3], b'SN\x02')
        self.assertEqual(g.rooms(0)[0], 0)
        g.assertok(g.ask('find'))
        self.assertEqual(g.match()['state'], 8)

    def test_reject_5_said(self):
        """REJECT 5 (a mod set the lobby can't read) is said, as any refusal: the game stays in version 3."""
        lobby = FakeLobby()
        self.fakes.append(lobby)
        g = self.game(lobby.address, protocol=3)
        g.status()
        join, at = lobby.next(R.JOIN)
        lobby.s.sendto(b'SN\x03' + bytes((R.REJECT,)) + join[4:12] + b'\5' + b'the lobby cannot read your game\'s mod list'.ljust(64, b'\0'), at)
        self.assertTrue(until(lambda: 'cannot read your game' in g.status(), 3))
        self.assertEqual(lobby.next(R.JOIN, wait=4.0)[0], join)   # (asked again, in version 3)


class Lobby(ClientTest):
    """Games against tools/ssbu_relay.py."""
    def setUp(self):
        super().setUp()
        self.server = Server(search_for=30.0, match_for=30.0)
        self.relay = f'127.0.0.1:{self.server.port}'

    def tearDown(self):
        super().tearDown()
        self.server.stop()

    def fake(self, cls=V3Player, **kw):
        if cls is V3Player:
            kw.setdefault('sim', SIM)   # (this game's, unless said: test_ssbu_relay_v3's fakes are of another)
        p = cls(self.server, **kw)
        self.fakes.append(p)
        return p.join()

    def hosting(self, cls=V3Player, **kw):
        """A fake game with an open room."""
        p = self.fake(cls, name='Host', **kw)
        p.session = secrets.token_bytes(16)
        p.advertise(p.session)
        self.assertTrue(until(lambda: p.session in self.server.lobby.listings))
        return p

    def client_of(self, g):
        return self.server.lobby.by_ip[g.ip]

    def test_version_3_and_its_key(self):
        g = self.game(self.relay, protocol=3, room='keys', name='Alpha')
        version, ip, flags, fell_back, _ = g.join()
        self.assertEqual((version, flags & (R.HAS_ROOMS | R.HAS_MATCH | R.HAS_RB), fell_back), (3, 28, 0))
        c = self.client_of(g)
        self.assertTrue(until(lambda: c.verified))
        self.assertIs(c.wire, self.server.lobby.sn3)
        self.assertEqual(c.part, ('SN3', BUILD.encode(), SIM, (), 'keys'))   # (build, simulation version, no mods, code)

    def test_version_2_by_default(self):
        g = self.game(self.relay, room='keys')
        self.assertEqual(g.join()[0], 2)
        self.assertEqual(self.client_of(g).part, ('SN', BUILD.encode(), 'keys'))

    def test_two_games_of_one_key_matched(self):
        a = self.game(self.relay, protocol=3, name='Alpha')
        b = self.game(self.relay, protocol=3, name='Bravo')
        a.join()
        b.join()
        a.assertok(a.ask('find'))
        self.assertEqual(a.match(1, 5)['state'], 1)   # (searching)
        b.assertok(b.ask('find'))
        ma, mb = a.match(2, 5), b.match(2, 5)
        self.assertEqual((ma['state'], ma['role'], ma['peer'], ma['name']), (2, 1, b.ip, 'Bravo'))   # (Alpha waited longer: it hosts)
        self.assertEqual((mb['state'], mb['role'], mb['peer'], mb['name']), (2, 2, a.ip, 'Alpha'))
        b.assertok(b.ask('cancel'))
        self.assertEqual(b.match(0, 5)['state'], 0)
        self.assertEqual(a.match(4, 5)['state'], 4)   # (the other cancelled)

    def test_two_games_of_one_key_connect_directly(self):
        """In one room, introduced, their games punch and send straight to each other in version 3."""
        host = self.game(self.relay, protocol=3, name='Host')
        guest = self.game(self.relay, protocol=3, name='Guest')
        host.join()
        guest.join()
        session = secrets.token_bytes(16)
        host.ldn(BCAST, R.LM_ADVERTISE, info_of(session, host.ip))
        self.assertTrue(until(lambda: session in self.server.lobby.listings))
        self.assertEqual(guest.list(), [host.ip])
        guest.ldn(host.ip, R.LM_JOIN, session + b'Guest'.ljust(32, b'\0') + b'\0\0')
        host.ldn(BCAST, R.LM_ADVERTISE, info_of(session, host.ip, [guest.ip]))
        host.ldn(guest.ip, R.LM_ACCEPT, info_of(session, host.ip, [guest.ip]))
        self.assertTrue(until(lambda: guest.ip in self.server.lobby.listings[session].nodes))
        host.datagram(guest.ip, b'hello guest')   # (their games talk: they are introduced)
        guest.datagram(host.ip, b'hello host')
        for g, other in ((host, guest), (guest, host)):
            direct, peers = g.peers(5, 1)
            self.assertEqual((direct, peers[0][:2]), (1, (other.ip, 'direct')), ''.join(g.log))
        host.datagram(guest.ip, b'straight')
        self.assertTrue(until(lambda: guest.peers()[1][0][2] >= 1))   # (in_direct)
        self.assertIn(b'straight', [d[4] for d in guest.received()])

    def test_another_simulation_version_apart(self):
        same = self.hosting(room='sims')
        other = self.hosting(sim=SIM + 1, room='sims')
        modded = self.hosting(mod_set=mods((b'Alpha', 1)), room='sims')
        g = self.game(self.relay, protocol=3, room='sims')
        g.join()
        self.assertEqual(g.list(), [same.ip])   # (the game's own Rooms List: the room it can join)
        state, rooms = g.rooms()
        self.assertEqual(state, 3)
        by = {r['host']: r for r in rooms}
        self.assertEqual(rooms[0]['host'], same.ip)   # (its key's first)
        self.assertEqual(set(by), {same.ip, other.ip, modded.ip})
        self.assertEqual((by[same.ip]['joinable'], by[same.ip]['why'], by[same.ip]['open']), (1, '', 1))
        self.assertEqual((by[other.ip]['joinable'], by[other.ip]['sim']), (0, SIM + 1))
        self.assertIn(f'simulation version is {SIM + 1}, yours {SIM}', by[other.ip]['why'])
        self.assertEqual((by[modded.ip]['joinable'], by[modded.ip]['mods'], by[modded.ip]['why']), (0, 'Alpha', "Mods you don't have: Alpha"))
        other.match(1, 77)   # (searching, of another key: never this game's opponent)
        self.assertEqual(other.state()[1], R.MS_SEARCHING)
        g.assertok(g.ask('find'))
        m = g.match(1, 5)
        self.assertEqual((m['state'], m['searching']), (1, 1))   # (searching alone, in its key)
        self.assertIsNone(other.state(0.5))

    def test_version_2_and_3_apart(self):
        old_room = self.hosting(Player, room='mixed')
        new_room = self.hosting(room='mixed')
        old = self.game(self.relay, room='mixed', name='Old')
        new = self.game(self.relay, protocol=3, room='mixed', name='New')
        self.assertEqual(old.join()[0], 2)
        self.assertEqual(new.join()[0], 3)
        self.assertEqual(old.list(), [old_room.ip])
        self.assertEqual(new.list(), [new_room.ip])
        self.assertEqual([r['host'] for r in new.rooms()[1]], [new_room.ip])   # (the browser: version 3's rooms only)
        self.assertEqual(old.rooms(0)[0], 0)


class FallBack(ClientTest):
    """Games of version 3 where version 2 is all there is."""
    def test_lobby_of_version_2(self):
        server = Server(make=V2OnlyLobby)
        self.addCleanup(server.stop)
        relay = f'127.0.0.1:{server.port}'
        old = self.game(relay, room='fallback', name='Old')
        new = self.game(relay, protocol=3, room='fallback', name='New')
        version, _, _, fell_back, _ = old.join()
        self.assertEqual((version, fell_back), (2, 0))
        version, ip, flags, fell_back, _ = new.join()
        self.assertEqual((version, flags, fell_back), (2, R.LOBBY | R.INTRODUCES, 1))
        self.assertEqual(server.lobby.by_ip[ip].part, ('SN', BUILD.encode(), 'fallback'))
        self.assertEqual(new.rooms(0)[0], 0)
        new.assertok(new.ask('find'))
        self.assertEqual(new.match()['state'], 8)
        session = secrets.token_bytes(16)   # (and plays with the players of version 2, as one of them)
        old.ldn(BCAST, R.LM_ADVERTISE, info_of(session, old.ip))
        self.assertTrue(until(lambda: session in server.lobby.listings))
        self.assertEqual(new.list(), [old.ip])

    def test_private_room_relay(self):
        """A host player's game runs the relay of a private room (version 2): its own game joins it in
        version 2, another of version 3 is told another version and joins it in version 2."""
        port = free_udp_port()
        host = self.game(None, protocol=3, name='Host', SSBU_NET_HOST=1, SSBU_NET_PORT=port, SSBU_NET_BIND='127.0.0.1')
        version, _, _, fell_back, _ = host.join()
        self.assertEqual((version, fell_back), (2, 0))   # (version 2 from the start)
        guest = self.game(f'127.0.0.1:{port}', protocol=3, name='Guest')
        version, ip, flags, fell_back, members = guest.join()
        self.assertEqual((version, flags, fell_back, members), (2, 0, 1, 2))
        guest.datagram(host.ip, b'through the relay')
        self.assertTrue(until(lambda: any(d[4] == b'through the relay' for d in host.received())))
        self.assertEqual(guest.rooms(0)[0], 0)


if __name__ == '__main__':
    unittest.main()
