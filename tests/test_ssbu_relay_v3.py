#!/usr/bin/env python3
"""Tests for protocol SN version 3 (tools/ssbu_relay.py): version 2's messages on the same port, and
the room browser, mod sets, matchmaking and rollback relay, for players parted by build,
simulation version, mod set and lobby code. Fake players on loopback, no game; waits are for
conditions, not for fixed times.

    python -I tests/test_ssbu_relay_v3.py [-v]"""
import os
import secrets
import socket
import struct
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import ssbu_relay as R   # noqa: E402
from test_ssbu_relay import BUILD, OTHER_BUILD, Player, Server, info_of   # noqa: E402

BCAST = R.BROADCASTS[0]
GAME_PORT = 12345
SIM = 7   # (a made-up simulation version)


def until(cond, wait=3.0):
    """Whether cond() comes true within wait seconds (looked at every 20 ms)."""
    end = time.monotonic() + wait
    while not cond():
        if time.monotonic() >= end:
            return False
        time.sleep(0.02)
    return True


def mods(*entries):
    """A mod set: (name, hash), ..."""
    return bytes((len(entries),)) + b"".join(struct.pack(">IB", h, len(n)) + n for n, h in entries)


NO_MODS = mods()


class V3Player:
    """A game of protocol SN version 3: what it sends, and an inbox of what came and was not looked at
    yet. Every packet the lobby sends it must be of version 3."""
    def __init__(self, server, room="v3test", build=BUILD, name="P", sim=SIM, mod_set=NO_MODS, ext=True, direct=True,
                 lan=(0, 0), bind="127.0.0.1"):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.s.bind((bind, 0))
        except OSError:
            self.s.close()
            raise
        self.addr = ("127.0.0.1", server.port)
        self.nonce = secrets.token_bytes(8)
        self.room, self.build, self.name, self.sim, self.mod_set = room, build, name, sim, mod_set
        self.ext, self.direct, self.lan = ext, direct, lan
        self.ip = self.cookie = self.flags = None
        self.inbox = []
        self.have = 0   # the version of the newest PEERS seen (what its PINGs say)

    def close(self):
        self.s.close()

    def send(self, kind, body=b""):
        self.s.sendto(R.header(kind, R.V3) + body, self.addr)

    def read(self, end):
        """One more packet into the inbox, waiting until end (monotonic time) at most (past it: one that
        is there already): False if none came."""
        self.s.settimeout(max(end - time.monotonic(), 0))
        try:
            data, _ = self.s.recvfrom(65536)
        except (socket.timeout, BlockingIOError, ConnectionResetError):
            return False
        assert data[:3] == b"SN\x03", ("not version 3", data[:4])
        self.inbox.append(data)
        return True

    def first(self, kind, wait=1.0):
        """The first packet of this kind, from the inbox or within wait seconds (None: none came)."""
        end, i = time.monotonic() + wait, 0
        while True:
            while i < len(self.inbox):
                if self.inbox[i][3] == kind:
                    return self.inbox.pop(i)
                i += 1
            if not self.read(end):
                return None

    def all(self, kind, wait=0.4):
        """Every packet of this kind in the inbox and coming within wait seconds."""
        end = time.monotonic() + wait
        while self.read(end):
            pass
        out = [p for p in self.inbox if p[3] == kind]
        self.inbox = [p for p in self.inbox if p[3] != kind]
        return out

    def join_packet(self):
        return (R.header(R.JOIN, R.V3) + self.nonce + self.room.encode().ljust(32, b"\0") + self.name.encode().ljust(32, b"\0") +
                self.build.encode().ljust(40, b"\0") + struct.pack(">I", self.sim) + self.mod_set)

    def join(self, verify=True):
        self.s.sendto(self.join_packet(), self.addr)
        w = self.first(R.WELCOME)
        assert w is not None, "no WELCOME"
        assert len(w) == 26 and w[4:12] == self.nonce and w[25] == 0, w.hex()
        self.ip, self.cookie, self.flags = struct.unpack_from(">IQB", w, 12)
        if verify:
            assert self.ping() is not None, "no PONG"
        return self

    def ping(self, have=None):
        """A PING (with direct connections' part when ext); the PONG: (players, rooms, flags,
        searching, players of the lobby code), or None."""
        p = R.header(R.PING, R.V3) + self.nonce + struct.pack(">QQH", 1234, self.cookie, 20)
        if self.ext:
            p += struct.pack(">IIHBB", self.have if have is None else have, *self.lan, 1 if self.direct else 0, 0)
        self.s.sendto(p, self.addr)
        pong = self.first(R.PONG)
        if pong is None:
            return None
        assert len(pong) == 21 and pong[4:12] == struct.pack(">Q", 1234), pong.hex()
        return struct.unpack_from(">HHBHH", pong, 12)

    def leave(self):
        self.send(R.LEAVE, self.nonce)

    def list(self, seq=7, every=None):
        """The rooms of a LIST (every: with that flags byte): [(host, NetworkInfo, what follows it)]."""
        self.send(R.LIST, struct.pack(">QI", self.cookie, seq) + (b"" if every is None else bytes((every,))))
        end = self.first(R.LIST_END)
        assert end is not None, "no LIST_END"
        assert struct.unpack_from(">I", end, 4)[0] == seq and len(end) == 9
        rooms = []
        for p in self.all(R.LISTING, 0):
            if struct.unpack_from(">I", p, 4)[0] == seq:
                rooms.append((struct.unpack_from(">I", p, 8)[0], p[12:12 + R.NI_SIZE], p[12 + R.NI_SIZE:]))
        assert end[8] == len(rooms), (end[8], len(rooms))
        return rooms

    def rooms(self, seq=3, cookie=None):
        """The room browser: [(host, players, most, flags, host name, build, simulation version, mods)]."""
        self.send(R.ROOMS, struct.pack(">QI", self.cookie if cookie is None else cookie, seq))
        end = self.first(R.ROOMS_END)
        assert end is not None, "no ROOMS_END"
        assert struct.unpack_from(">I", end, 4)[0] == seq and len(end) == 9
        out = []
        for p in self.all(R.ROOM_INFO, 0):
            b = p[4:]
            if struct.unpack_from(">I", b)[0] == seq:
                host, players, most, flags = struct.unpack_from(">IBBB", b, 4)
                out.append((host, players, most, flags, b[11:43].rstrip(b"\0"), b[43:83].rstrip(b"\0"),
                            struct.unpack_from(">I", b, 83)[0], b[87:]))
        assert end[8] == len(out), (end[8], len(out))
        return out

    def match(self, op, sid):
        self.send(R.MATCH, struct.pack(">QBI", self.cookie, op, sid))

    def state(self, wait=1.0):
        """The next MATCH_STATE: (id, state, role, flags, the other's address, its name, players
        searching, players, seconds waited), or None."""
        p = self.first(R.MATCH_STATE, wait)
        if p is None:
            return None
        assert len(p) == 4 + 49, len(p)
        return struct.unpack_from(">IBBBI", p, 4) + (p[15:47].rstrip(b"\0"),) + struct.unpack_from(">HHH", p, 47)

    def until_state(self, want, wait=3.0, flags=0):
        """MATCH_STATEs until one is in state want (with these flags set), passing over the others; None
        if none comes within wait seconds."""
        end = time.monotonic() + wait
        while True:
            st = self.state(end - time.monotonic())
            if st is None or (st[1] == want and st[3] & flags == flags):
                return st

    def rb(self, dst, message):
        self.send(R.RB, struct.pack(">QI", self.cookie, dst) + message)

    def rbs(self, wait=0.3):
        """The rollback messages that came: [(source, message)]."""
        return [(struct.unpack_from(">I", p, 4)[0], p[8:]) for p in self.all(R.RB, wait)]

    def data(self, dst, payload, sport=GAME_PORT, dport=GAME_PORT, src=None):
        self.send(R.DATA, struct.pack(">IIHH", self.ip if src is None else src, dst, sport, dport) + payload)

    def ldn(self, dst, kind, payload):
        self.data(dst, b"L" + bytes((kind, 0, 0)) + payload, R.LDN_PORT, R.LDN_PORT)

    def advertise(self, session, stations=(), **kw):
        self.ldn(BCAST, R.LM_ADVERTISE, info_of(session, self.ip, stations, **kw))

    def bcast(self, skip, payload, dst=BCAST):
        self.send(R.BCAST, bytes((len(skip),)) + b"".join(struct.pack(">I", ip) for ip in skip) +
                  struct.pack(">IIHH", self.ip, dst, GAME_PORT, GAME_PORT) + payload)

    def datas(self, wait=0.4):
        """The DATA packets that came: (source, destination, source port, payload)."""
        return [(*struct.unpack_from(">IIH", p, 4), p[16:]) for p in self.all(R.DATA, wait)]

    def peers(self, wait=0.4):
        """The newest PEERS that came (within wait seconds): (version, [(address, name, flags, public
        address and port, LAN address and port, secret)]), or None."""
        lists = self.all(R.PEERS, wait)
        if not lists:
            return None
        p = lists[-1]
        cookie, version, count = struct.unpack_from(">QIB", p, 4)
        assert cookie == self.cookie and len(p) == 17 + count * R.PEER.size, (len(p), count)
        self.have = version
        out = []
        for i in range(count):
            ip, name, flags, pip, pport, lip, lport, secret = R.PEER.unpack_from(p, 17 + i * R.PEER.size)
            out.append((ip, name.rstrip(b"\0"), flags, (pip, pport), (lip, lport), secret))
        return version, out


class V3Test(unittest.TestCase):
    server_options = {}

    def setUp(self):
        self.server = Server(**self.server_options)
        self.players = []

    def tearDown(self):
        for p in self.players:
            p.close()
        self.server.stop()

    def player(self, join=True, **kw):
        p = V3Player(self.server, **kw)
        self.players.append(p)
        return p.join() if join else p

    def v2(self, **kw):
        p = Player(self.server, **kw)
        self.players.append(p)
        return p.join()

    def listed(self, session):
        """Whether the lobby has the room of that session (within a few seconds)."""
        return until(lambda: session in self.server.lobby.listings)

    def room(self, stations=0, **kw):
        """A host with a room, and stations that joined it (asked and were accepted)."""
        host = self.player(name="Host", **kw)
        session = secrets.token_bytes(16)
        host.advertise(session)
        self.assertTrue(self.listed(session))
        joined = []
        for _ in range(stations):
            s = self.player(name="Guest", **kw)
            s.ldn(host.ip, R.LM_JOIN, session + b"Guest".ljust(32, b"\0") + b"\0\0")
            self.assertTrue(any(src == s.ip for src, *_ in host.datas()), "the JOIN did not reach the host")
            joined.append(s)
            host.advertise(session, [j.ip for j in joined])
            host.ldn(s.ip, R.LM_ACCEPT, info_of(session, host.ip, [j.ip for j in joined]))
        for p in [host, *joined]:
            p.all(R.DATA, 0.2)
        return host, session, joined


class Joining(V3Test):
    def test_welcome_and_pong(self):
        p = self.player(join=False)
        p.join(verify=False)
        self.assertTrue(p.ip & 0xFFFF0000 == R.SUBNET)
        self.assertEqual(p.flags & (R.LOBBY | R.INTRODUCES), R.LOBBY | R.INTRODUCES)
        self.assertEqual(p.ping(), (1, 0, 1, 0, 1))   # (players, rooms, flags, searching, players of the code)
        c = self.server.lobby.by_ip[p.ip]
        self.assertTrue(c.verified)
        self.assertIs(c.wire, self.server.lobby.sn3)
        self.assertEqual(c.part, ("SN3", BUILD.encode(), SIM, (), "v3test"))

    def test_version_2_as_before(self):
        """A game of version 2 in the same lobby hears version 2, in its own layouts."""
        self.player()
        old = self.v2()
        old.send(old.ping_packet())
        pong = old.first(R.PONG)
        self.assertEqual((pong[:3], len(pong)), (b"SN\x02", 17))
        old.send(old.join_packet())
        w = old.first(R.WELCOME)
        self.assertEqual((w[:3], len(w), w[24]), (b"SN\x02", 26, R.LOBBY | R.INTRODUCES))

    def test_other_versions_told(self):
        p = self.player(join=False)
        p.s.sendto(b"SN\x04" + p.join_packet()[3:], p.addr)
        p.s.settimeout(1.0)
        r, _ = p.s.recvfrom(4096)
        self.assertEqual((r[:4], r[12]), (b"SN\x04" + bytes((R.REJECT,)), 2))
        self.assertIn(b"lobby 2 or 3, you 4", r)

    def test_short_join_ignored(self):
        p = self.player(join=False)
        p.s.sendto(p.join_packet()[:R.JOIN_SIZE + 4], p.addr)   # (no mod set)
        self.assertIsNone(p.first(R.WELCOME, 0.3))
        self.assertEqual(len(self.server.lobby.by_addr), 0)

    def test_bad_mod_set_refused(self):
        for bad in (bytes((R.MODS_MAX + 1,)) + bytes(5 * (R.MODS_MAX + 1)),   # (too many)
                    mods((b"x" * (R.MOD_NAME + 1), 1)),                         # (a name too long)
                    mods((b"Mod", 1))[:-1]):                                     # (cut short)
            p = self.player(join=False, mod_set=bad)
            p.s.sendto(p.join_packet(), p.addr)
            r = p.first(R.REJECT)
            self.assertIsNotNone(r, bad.hex())
            self.assertEqual(r[12], 5)
        self.assertEqual(len(self.server.lobby.by_addr), 0)

    def test_any_build(self):
        """Version 3 lets in every build (its players meet only their own); version 2, 13.0.5's only."""
        self.assertIsNotNone(self.player(build=OTHER_BUILD).ip)
        old = Player(self.server, build=OTHER_BUILD)
        self.players.append(old)
        old.send(old.join_packet())
        self.assertEqual(old.first(R.REJECT)[12], 4)

    def test_another_version_from_the_same_place(self):
        """A game that falls back to version 2 (or goes to 3) with the same nonce starts anew."""
        p = self.player()
        first = p.ip
        old = Player(self.server)
        old.s.close()
        old.s, old.nonce = p.s, p.nonce
        old.send(old.join_packet())
        w = old.first(R.WELCOME)
        self.assertEqual(w[:3], b"SN\x02")
        old.ip, old.cookie = struct.unpack_from(">IQ", w, 12)
        self.assertNotEqual(old.ip, first)
        self.assertEqual([c.wire for c in self.server.lobby.by_addr.values()], [self.server.lobby.sn])
        p.send(R.PING, p.nonce + struct.pack(">QQH", 1, p.cookie, 20))   # (version 3's, with the old cookie)
        old.s.settimeout(0.3)
        self.assertRaises(socket.timeout, old.s.recvfrom, 4096)

    def test_leave(self):
        p = self.player()
        p.leave()
        self.assertTrue(until(lambda: len(self.server.lobby.by_addr) == 0))


class Keys(V3Test):
    """Players meet by build, simulation version, mod set and lobby code; never by OS or CPU (no
    field says them)."""
    def test_same_key_meet(self):
        host, session, _ = self.room(mod_set=mods((b"Alpha", 1), (b"Beta", 2)))
        for kw in ({"mod_set": mods((b"Beta", 2), (b"Alpha", 1))},           # (listed in another order)
                   {"mod_set": mods((b"alpha", 1), (b"Beta (2)", 2))},       # (named otherwise: the hashes count)
                   {"mod_set": mods((b"Alpha", 1), (b"Beta", 2)), "build": BUILD.lower()}):
            p = self.player(**kw)
            self.assertEqual([h for h, *_ in p.list()], [host.ip], kw)

    def test_apart(self):
        host, session, _ = self.room(mod_set=mods((b"Alpha", 1)))
        for kw in ({"sim": SIM + 1}, {"mod_set": NO_MODS}, {"mod_set": mods((b"Alpha", 2))},
                   {"mod_set": mods((b"Alpha", 1), (b"Alpha", 1))}, {"build": OTHER_BUILD}, {"room": "other"}):
            p = self.player(**{"mod_set": mods((b"Alpha", 1)), **kw})
            self.assertEqual(p.list(), [], kw)
            p.ldn(host.ip, R.LM_JOIN, session + bytes(34))
            p.data(host.ip, b"hello")
        self.assertEqual(host.datas(0.3), [])

    def test_apart_from_version_2(self):
        """Version 2's players are a pool of their own: their games say no simulation version or mods."""
        old = self.v2(room="v3test")
        session = secrets.token_bytes(16)
        old.advertise(session)
        self.assertTrue(self.listed(session))
        p = self.player(mod_set=NO_MODS)
        self.assertEqual(p.list(), [])
        p.ldn(old.ip, R.LM_JOIN, session + bytes(34))
        self.assertEqual(old.datas(0.3), [])
        host, mine, _ = self.room()
        self.assertEqual(old.list(), [])
        self.assertEqual(self.server.lobby.listings[mine].owner.part[0], "SN3")


class RoomBrowser(V3Test):
    """ROOMS: every room of the lobby code, of every key, with what the game needs to say why it can
    or can't join one; LIST's flag 1 the same in the game's own Rooms List."""
    def setUp(self):
        super().setUp()
        self.mine = mods((b"Alpha", 1))
        self.a, self.sa, _ = self.room(mod_set=self.mine)                       # (of the asker's key)
        self.b = self.player(name="Sim", mod_set=self.mine, sim=SIM + 1)         # (another simulation version)
        self.c = self.player(name="Mods", mod_set=mods((b"Alpha", 2)))          # (another copy of the mod)
        self.d = self.player(name="Code", mod_set=self.mine, room="elsewhere")   # (another lobby code)
        self.old = self.v2(room="v3test", name="Old")                            # (version 2, the same code)
        for h in (self.b, self.c, self.d, self.old):
            h.session = secrets.token_bytes(16)
            h.advertise(h.session)
            self.assertTrue(self.listed(h.session))

    def test_rooms(self):
        p = self.player(name="Asker", mod_set=self.mine)
        self.assertTrue(p.flags & R.HAS_ROOMS)
        got = p.rooms()
        self.assertEqual([r[0] for r in got], [self.a.ip, self.b.ip, self.c.ip])   # (its key's first, then the oldest)
        by = {r[0]: r for r in got}
        self.assertEqual(by[self.a.ip], (self.a.ip, 1, 8, 1 | 4 | 8, b"Host", BUILD.encode(), SIM, self.mine))
        self.assertEqual(by[self.b.ip][3:7], (1 | 4, b"Sim", BUILD.encode(), SIM + 1))
        self.assertEqual(by[self.c.ip][3:], (1 | 4, b"Mods", BUILD.encode(), SIM, mods((b"Alpha", 2))))
        self.assertEqual(self.a.rooms()[0][3], 1 | 2 | 4 | 8)   # (its own)

    def test_list_every_key(self):
        p = self.player(name="Asker", mod_set=self.mine)
        self.assertEqual([(h, tail) for h, _, tail in p.list()], [(self.a.ip, b"")])
        got = p.list(every=1)
        self.assertEqual(got[0][0], self.a.ip)   # (its own key's first)
        self.assertEqual({h for h, _, _ in got}, {self.a.ip, self.b.ip, self.c.ip})
        tails = {h: tail for h, _, tail in got}
        self.assertEqual(tails[self.b.ip], BUILD.encode().ljust(40, b"\0") + struct.pack(">I", SIM + 1) + self.mine)
        self.assertEqual(tails[self.c.ip], BUILD.encode().ljust(40, b"\0") + struct.pack(">I", SIM) + mods((b"Alpha", 2)))
        self.assertEqual(self.old.list(), [])   # (and version 2's see none of them)

    def test_wrong_cookie_unanswered(self):
        p = self.player(mod_set=self.mine)
        p.send(R.ROOMS, struct.pack(">QI", p.cookie ^ 1, 3))
        self.assertIsNone(p.first(R.ROOMS_END, 0.3))


class Matchmaking(V3Test):
    """MATCH and MATCH_STATE as SU's, for the players of one key."""
    server_options = {"search_for": 30.0, "match_for": 30.0}

    def test_paired_within_the_key_only(self):
        a, b = self.player(name="MA"), self.player(name="MB")
        self.assertTrue(a.flags & R.HAS_MATCH)
        y, z = self.player(name="MY", sim=SIM + 1), self.player(name="MZ", mod_set=mods((b"Alpha", 1)))
        old = self.v2(room="v3test")
        a.match(1, 101)
        self.assertEqual(a.state(), (101, R.MS_SEARCHING, 0, 0, 0, b"", 1, 2, 0))   # (searching 1, players of its key 2)
        y.match(1, 201)
        self.assertEqual(y.state()[:2], (201, R.MS_SEARCHING))
        z.match(1, 301)
        self.assertEqual(z.state()[:2], (301, R.MS_SEARCHING))
        old.send(R.header(R.MATCH) + struct.pack(">QBI", old.cookie, 1, 401))   # (version 2 has no MATCH)
        self.assertEqual(a.ping(), (2, 0, 1, 1, 4))   # (players and searching of its key; players of the code: 4)
        b.match(1, 102)
        sa, sb = a.until_state(R.MS_MATCHED), b.until_state(R.MS_MATCHED)
        self.assertEqual(sa[:7], (101, R.MS_MATCHED, R.ROLE_HOST, 0, b.ip, b"MB", 0))   # (MA waited longer: MA hosts)
        self.assertEqual(sb[:7], (102, R.MS_MATCHED, R.ROLE_JOIN, 0, a.ip, b"MA", 0))
        self.assertEqual(y.state(0.2), None)
        self.assertEqual(z.state(0), None)
        self.assertEqual(old.recv(0.2), [])

    def test_room_then_together(self):
        a, b = self.player(name="MA"), self.player(name="MB")
        a.match(1, 1)
        self.assertEqual(a.state()[1], R.MS_SEARCHING)
        b.match(1, 2)
        self.assertIsNotNone(a.until_state(R.MS_MATCHED))
        self.assertIsNotNone(b.until_state(R.MS_MATCHED))
        session = secrets.token_bytes(16)
        a.advertise(session)
        self.assertIsNotNone(b.until_state(R.MS_MATCHED, flags=1), "the guest was not told the host's room is open")
        self.assertEqual(b.list()[0][0], a.ip)
        b.ldn(a.ip, R.LM_JOIN, session + b"MB".ljust(32, b"\0") + b"\0\0")
        self.assertTrue(a.datas())
        a.advertise(session, [b.ip])
        self.assertEqual(a.until_state(R.MS_TOGETHER)[:6], (1, R.MS_TOGETHER, R.ROLE_HOST, 0, b.ip, b"MB"))
        self.assertEqual(b.until_state(R.MS_TOGETHER)[:6], (2, R.MS_TOGETHER, R.ROLE_JOIN, 0, a.ip, b"MA"))

    def test_cancel_tells_the_other(self):
        f, g = self.player(name="MF"), self.player(name="MG")
        f.match(1, 1)
        self.assertEqual(f.state()[1], R.MS_SEARCHING)
        g.match(1, 2)
        self.assertIsNotNone(g.until_state(R.MS_MATCHED))
        g.match(0, 2)
        self.assertEqual(g.until_state(R.MS_IDLE)[0], 2)
        self.assertEqual(f.until_state(R.MS_PEER_LEFT)[4:6], (g.ip, b"MG"))

    def test_rate(self):
        a = self.player()
        for _ in range(40):
            a.match(1, 9)
        self.assertLess(len(a.all(R.MATCH_STATE, 0.5)), 15)


class Rollback(V3Test):
    """RB as SU's, within the key; on the public code ("smash") only to an opponent or a player of
    one's room."""
    server_options = {"search_for": 30.0, "match_for": 30.0}

    def test_rollback_match_on_the_public_code(self):
        r1, r2, s = (self.player(name=n, room=R.V3_PUBLIC) for n in ("R1", "R2", "RS"))
        self.assertTrue(r1.flags & R.HAS_RB)
        r1.match(2, 401)
        self.assertEqual(r1.state()[:2], (401, R.MS_SEARCHING))
        r2.match(2, 402)
        self.assertEqual(r1.until_state(R.MS_MATCHED)[:5], (401, R.MS_MATCHED, R.ROLE_HOST, 0, r2.ip))
        self.assertEqual(r2.until_state(R.MS_MATCHED)[:5], (402, R.MS_MATCHED, R.ROLE_JOIN, 0, r1.ip))
        s.rb(r1.ip, b"stranger")   # (not its opponent: dropped)
        s.rb(0, b"to all")         # (to everyone, on the public code: dropped)
        self.assertEqual(r1.rbs() + r2.rbs(0), [])
        r1.rb(r2.ip, b"from r1")
        self.assertEqual(r2.rbs(), [(r1.ip, b"from r1")])
        r2.rb(r1.ip, b"from r2")
        self.assertEqual(r1.rbs(), [(r2.ip, b"from r2")])
        self.assertEqual(r1.until_state(R.MS_TOGETHER)[:2], (401, R.MS_TOGETHER))
        self.assertEqual(r2.until_state(R.MS_TOGETHER)[:2], (402, R.MS_TOGETHER))

    def test_public_code_room_players(self):
        host, session, (a,) = self.room(1, room=R.V3_PUBLIC)
        stranger = self.player(room=R.V3_PUBLIC)
        a.rb(host.ip, b"in the room")
        stranger.rb(host.ip, b"not")
        self.assertEqual(host.rbs(), [(a.ip, b"in the room")])

    def test_private_code_within_the_key(self):
        a, b, c = (self.player(name=n, room="friends") for n in "ABC")
        other = self.player(room="friends", sim=SIM + 1)
        old = self.v2(room="friends")
        a.rb(b.ip, b"to b")
        self.assertEqual(b.rbs(), [(a.ip, b"to b")])
        a.rb(0, b"to all")   # (a private code: every other player of its key)
        self.assertEqual(b.rbs() + c.rbs(0), [(a.ip, b"to all")] * 2)
        a.rb(other.ip, b"another key")
        a.rb(old.ip, b"version 2")
        self.assertEqual(other.rbs() + a.rbs(0), [])
        self.assertEqual(old.recv(0.2), [])
        old.send(R.header(R.RB) + struct.pack(">QI", old.cookie, a.ip) + b"from version 2")   # (version 2 has no RB)
        self.assertEqual(a.rbs(), [])

    def test_size(self):
        a, b = self.player(room="friends"), self.player(room="friends")
        a.rb(b.ip, bytes(R.RB_MAX - 16 + 1))
        a.rb(b.ip, bytes(R.RB_MAX - 16))
        self.assertEqual([len(m) for _, m in b.rbs()], [R.RB_MAX - 16])


def talk(*players):
    """Game packets between each two of these players (both ways), through the lobby."""
    for a in players:
        for b in players:
            if a is not b:
                a.data(b.ip, b"game")
    for p in players:
        p.all(R.DATA, 0.15)


class Rules(V3Test):
    """Version 3 under version 2's rules: verified before anything of its is carried, a room's
    traffic between its players only, introductions once their games talk, limits."""
    server_options = {"per_ip": 3}

    def test_nothing_carried_before_verified(self):
        p = self.player(join=False)
        p.join(verify=False)
        session = secrets.token_bytes(16)
        p.advertise(session)
        p.send(R.RB, struct.pack(">QI", p.cookie ^ 1, 0) + b"rollback")   # (not its cookie: no proof of its address)
        self.assertFalse(until(lambda: session in self.server.lobby.listings, 0.3))
        self.assertFalse(self.server.lobby.by_ip[p.ip].verified)
        p.rooms()   # (its cookie came back: verified)
        p.advertise(session)
        self.assertTrue(self.listed(session))

    def test_room_traffic_between_its_players(self):
        host, session, (guest,) = self.room(1)
        stranger = self.player(name="Stranger")
        guest.data(host.ip, b"in the room")
        stranger.data(host.ip, b"not")
        self.assertEqual([m for *_, m in host.datas()], [b"in the room"])

    def test_introduced_once_they_talk(self):
        host, session, (guest,) = self.room(1)
        host.ping()
        got = host.peers()
        self.assertIsNotNone(got)
        self.assertEqual([(e[0], e[2] & 1) for e in got[1]], [(guest.ip, 0)])   # (listed, not introduced yet)
        newest, end = None, time.monotonic() + 3.0
        while time.monotonic() < end:   # (as two games do: their packets keep coming)
            talk(host, guest)
            host.ping()
            newest = host.peers(0.3) or newest
            if newest and any(e[0] == guest.ip and e[2] & 1 for e in newest[1]):
                break
        entry = next(e for e in newest[1] if e[0] == guest.ip)
        self.assertEqual((entry[2] & 1, entry[3][1] != 0), (1, True))
        self.assertNotEqual(entry[5], 0)

    def test_per_address(self):
        for _ in range(3):
            self.player()
        p = self.player(join=False)
        p.s.sendto(p.join_packet(), p.addr)
        r = p.first(R.REJECT)
        self.assertEqual(r[12], 1)

    def test_rate(self):
        self.server.lobby.o.packet_rate = 20.0
        host, session, (guest,) = self.room(1)
        for i in range(200):
            guest.data(host.ip, b"x%d" % i)
        self.assertLess(len(host.datas(0.5)), 100)

    def test_status_counts_version_3(self):
        import json
        import urllib.request
        server = Server(status_port=0)
        self.addCleanup(server.stop)
        p = V3Player(server)
        self.addCleanup(p.close)
        p.join()
        port = server.lobby.status_server.sockets[0].getsockname()[1]
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/status", timeout=3) as r:
            doc = json.loads(r.read())
        self.assertEqual(doc["players"], 1)
        self.assertIn(f"SN {R.PROTOCOL} and {R.V3}", doc["version"])


class OnlineLobby(unittest.TestCase):
    def test_game_server_answers_in_version_3(self):
        """The online lobby's game server (NEX) writes to a game of version 3 in version 3."""
        import tempfile
        import ssbu_online_lobby as online
        with tempfile.TemporaryDirectory() as tmp:
            lobby = online.OnlineLobby(online.parse_args(["--state", tmp, "--nex-log", "0"]))
            sent = []
            lobby.send = lambda data, at, wire=None: sent.append((data, wire))
            c = R.Client(("127.0.0.1", 5), b"n" * 8, R.SUBNET + 2, "Tester", ("SN3", b"b", SIM, (), "c"), time.monotonic(),
                         lobby.o, lobby.sn3)
            lobby.by_ip[c.ip] = c
            lobby.nex_send((R.ip_text(c.ip), 1000), 60000, b"nex")
            self.assertEqual(sent, [(R.header(R.DATA, R.V3) + struct.pack(">IIHH", online.GATEWAY, c.ip, 60000, 1000) + b"nex",
                                     lobby.sn3)])


if __name__ == "__main__":
    unittest.main()
