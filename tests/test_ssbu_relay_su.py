#!/usr/bin/env python3
"""Tests for the lobby's side for ssbu-recomp's games (tools/ssbu_relay.py, protocol "SU" version 1):
fake players on loopback, no game. What ssbu-recomp/tools/net/test_ssbu_lobby.py checked of its own
lobby, here against this one, and the rules WindowSmash's side has (limits, players verified before
anything of theirs is carried, introductions only once their games talk) on this side too.

    python -I tests/test_ssbu_relay_su.py [-v]"""
import http.client
import json
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
from test_ssbu_relay import Player, Server, info_of   # noqa: E402

BUILD = b"ssbu-recomp-ldn1-13.0.5"
BCAST = R.BROADCASTS[0]
GAME_PORT = 12345
LOCAL = R.address_number("127.0.0.1")


def until(cond, wait=3.0):
    """Whether cond() comes true within wait seconds (looked at every 20 ms)."""
    end = time.monotonic() + wait
    while not cond():
        if time.monotonic() >= end:
            return False
        time.sleep(0.02)
    return True


class SuServer(Server):
    """A lobby that serves SU too (on a free port)."""
    def __init__(self, **over):
        super().__init__(**{"su_port": 0, **over})
        self.su_port = self.lobby.su.transport.get_extra_info("sockname")[1]


class SuPlayer:
    """A game of ssbu-recomp's: what it sends, and an inbox of what came and was not looked at yet."""
    def __init__(self, server, code="test", build=BUILD, name="P", mods=None, direct=True, lan=(0, 0), bind="127.0.0.1"):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.s.bind((bind, 0))
        except OSError:
            self.s.close()
            raise
        self.addr = ("127.0.0.1", server.su_port)
        self.nonce = secrets.token_bytes(8)
        self.code, self.build, self.name, self.mods = code, build, name, mods
        self.direct, self.lan = direct, lan
        self.ip = self.cookie = None
        self.inbox = []
        self.have = 0   # the version of the newest PEERS seen

    def close(self):
        self.s.close()

    def send(self, kind, body=b""):
        self.s.sendto(R.su_header(kind) + body, self.addr)

    def read(self, end):
        """One more packet into the inbox, waiting until end (monotonic time) at most (past it: one that
        is there already): False if none came."""
        self.s.settimeout(max(end - time.monotonic(), 0))
        try:
            data, _ = self.s.recvfrom(65536)
        except (socket.timeout, BlockingIOError, ConnectionResetError):
            return False
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

    def join_packet(self, version=R.SU_PROTOCOL, size=None):
        p = (b"SU" + bytes((version, R.SU_JOIN)) + self.nonce + self.code.encode().ljust(32, b"\0") +
             self.name.encode().ljust(32, b"\0") + self.build.ljust(40, b"\0") +
             struct.pack(">IHB", *self.lan, 1 if self.direct else 0) + (b"MS" + self.mods if self.mods is not None else b""))
        return p if size is None else p[:size]

    def join(self, verify=True):
        self.s.sendto(self.join_packet(), self.addr)
        w = self.first(R.SU_WELCOME)
        assert w is not None, "no WELCOME"
        assert len(w) == 24 and w[4:12] == self.nonce, w.hex()
        self.ip, self.cookie = struct.unpack_from(">IQ", w, 12)
        if verify:
            assert self.ping() is not None, "no PONG"
        return self

    def ping_packet(self, direct=0):
        return R.su_header(R.SU_PING) + struct.pack(">QQB", self.cookie, 1234, direct)

    def ping(self, direct=0):
        self.s.sendto(self.ping_packet(direct), self.addr)
        return self.first(R.SU_PONG)

    def leave(self):
        self.send(R.SU_LEAVE, struct.pack(">Q", self.cookie))

    def list(self, seq=7, other=None):
        """The rooms of a LIST (other: with that flags byte): [(host, NetworkInfo, build or None)]."""
        self.send(R.SU_LIST, struct.pack(">QI", self.cookie, seq) + (b"" if other is None else bytes((other,))))
        end = self.first(R.SU_LIST_END)
        assert end is not None, "no LIST_END"
        assert struct.unpack_from(">I", end, 4)[0] == seq and len(end) == 9
        rooms = []
        for p in self.all(R.SU_LISTING, 0):
            if struct.unpack_from(">I", p, 4)[0] == seq:
                rooms.append((struct.unpack_from(">I", p, 8)[0], p[12:12 + R.NI_SIZE], p[12 + R.NI_SIZE:] or None))
        assert end[8] == len(rooms), (end[8], len(rooms))
        return rooms

    def rooms(self, seq=3):
        """The room browser: [(host, players, most, flags, host name, build, mods)]."""
        self.send(R.SU_ROOMS, struct.pack(">QI", self.cookie, seq))
        end = self.first(R.SU_ROOMS_END)
        assert end is not None, "no ROOMS_END"
        assert struct.unpack_from(">I", end, 4)[0] == seq and len(end) == 9
        out = []
        for p in self.all(R.SU_ROOM_INFO, 0):
            b = p[4:]
            if struct.unpack_from(">I", b)[0] == seq:
                host, players, most, flags = struct.unpack_from(">IBBB", b, 4)
                out.append((host, players, most, flags, b[11:43].rstrip(b"\0"), b[43:83].rstrip(b"\0"), b[83:]))
        assert end[8] == len(out), (end[8], len(out))
        return out

    def match(self, op, sid):
        self.send(R.SU_MATCH, struct.pack(">QBI", self.cookie, op, sid))

    def state(self, wait=1.0):
        """The next MATCH_STATE: (id, state, role, flags, the other's address, its name, players
        searching, players, seconds waited), or None."""
        p = self.first(R.SU_MATCH_STATE, wait)
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
        self.send(R.SU_RB, struct.pack(">QI", self.cookie, dst) + message)

    def rbs(self, wait=0.3):
        """The rollback messages that came: [(source, message)]."""
        return [(struct.unpack_from(">I", p, 4)[0], p[8:]) for p in self.all(R.SU_RB, wait)]

    def data(self, dst, payload, sport=GAME_PORT, dport=GAME_PORT, src=None):
        self.send(R.SU_DATA, struct.pack(">IIHH", self.ip if src is None else src, dst, sport, dport) + payload)

    def ldn(self, dst, kind, payload):
        self.data(dst, b"L" + bytes((kind, 0, 0)) + payload, R.LDN_PORT, R.LDN_PORT)

    def advertise(self, session, stations=(), **kw):
        self.ldn(BCAST, R.LM_ADVERTISE, info_of(session, self.ip, stations, **kw))

    def bcast(self, skip, payload, dst=BCAST):
        self.send(R.SU_BCAST, bytes((len(skip),)) + b"".join(struct.pack(">I", ip) for ip in skip) +
                  struct.pack(">IIHH", self.ip, dst, GAME_PORT, GAME_PORT) + payload)

    def datas(self, wait=0.4):
        """The DATA packets that came: (source, destination, source port, payload)."""
        return [(*struct.unpack_from(">IIH", p, 4), p[16:]) for p in self.all(R.SU_DATA, wait)]

    def peers(self, wait=0.4):
        """The newest PEERS that came (within wait seconds): (version, [(address, name, public
        address and port, LAN address and port, secret)]), or None."""
        lists = self.all(R.SU_PEERS, wait)
        if not lists:
            return None
        p = lists[-1]
        version, count = struct.unpack_from(">IB", p, 4)
        assert len(p) == 9 + count * R.SU_PEER.size, (len(p), count)
        self.have = version
        out = []
        for i in range(count):
            ip, name, pip, pport, lip, lport, secret = R.SU_PEER.unpack_from(p, 9 + i * R.SU_PEER.size)
            out.append((ip, name.rstrip(b"\0"), (pip, pport), (lip, lport), secret))
        return version, out


def mods(*entries):
    """A mod set: (name, hash), ..."""
    return bytes((len(entries),)) + b"".join(struct.pack(">IB", h, len(n)) + n for n, h in entries)


def su_talk(*players):
    """Game packets between each two of these players (both ways), through the lobby."""
    for a in players:
        for b in players:
            if a is not b:
                a.data(b.ip, b"game")
    for p in players:
        p.all(R.SU_DATA, 0.15)


def talk_until_introduced(a, b, wait=3.0):
    """su_talk(a, b) until a's PEERS has b, for up to wait seconds: (version, [entries]), or the newest
    PEERS if none does (None if none came). As two games do: their packets keep coming."""
    newest, end = None, time.monotonic() + wait
    while time.monotonic() < end:
        su_talk(a, b)
        a.ping()
        got = a.peers(0.3)
        if got:
            newest = got
            if any(e[0] == b.ip for e in got[1]):
                break
    return newest


class SuTest(unittest.TestCase):
    server_options = {}

    def setUp(self):
        self.server = SuServer(**self.server_options)
        self.players = []

    def tearDown(self):
        for p in self.players:
            p.close()
        self.server.stop()

    def player(self, join=True, **kw):
        p = SuPlayer(self.server, **kw)
        self.players.append(p)
        return p.join() if join else p

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
            p.all(R.SU_DATA, 0.2)
        return host, session, joined


class Joining(SuTest):
    server_options = {"pending_timeout": 2.0}

    def test_welcome(self):
        a, b = self.player(), self.player()
        self.assertNotEqual(a.ip, b.ip)
        for p in (a, b):
            self.assertEqual(p.ip >> 16, 0x0A0D)
            self.assertNotIn(p.ip & 255, (0, 255))
        self.assertEqual({c.wire for c in self.server.lobby.by_addr.values()}, {self.server.lobby.su})

    def test_any_build_and_name(self):
        p = self.player(build=b"build-x", name="")   # (any build, as ssbu-recomp's own lobby; no name: "Player")
        self.assertEqual(self.server.lobby.by_ip[p.ip].name, "Player")

    def test_join_again_same_address(self):
        p = self.player()
        ip, cookie = p.ip, p.cookie
        p.join()
        self.assertEqual((p.ip, p.cookie), (ip, cookie))

    def test_other_version_and_short_join_ignored(self):
        p = self.player(join=False)
        p.s.sendto(p.join_packet(version=2), p.addr)
        p.s.sendto(p.join_packet(size=R.SU_JOIN_SIZE - 1), p.addr)
        p.s.sendto(R.header(R.JOIN) + p.join_packet()[4:], p.addr)   # (SN's header on SU's port)
        self.assertEqual(p.all(R.SU_WELCOME, 0.3) + p.all(R.SU_REJECT, 0), [])
        self.assertEqual(len(self.server.lobby.by_addr), 0)

    def test_counts_in_pong(self):
        a = self.player()
        self.player()
        self.player(code="friends")
        self.player(build=b"ssbu-recomp-ldn1-13.0.4")
        pong = a.ping()
        self.assertEqual(struct.unpack_from(">QHH", pong, 4), (1234, 2, 0))

    def test_wrong_cookie_not_verified(self):
        a = self.player(join=False).join(verify=False)
        b = self.player()
        a.send(R.SU_PING, struct.pack(">QQB", a.cookie ^ 1, 1, 0))
        self.assertIsNone(a.first(R.SU_PONG, 0.3))
        a.advertise(secrets.token_bytes(16))   # (not verified: nothing it sends is carried)
        a.data(b.ip, b"hello")
        self.assertEqual(b.datas(0.3), [])
        self.assertEqual(self.server.lobby.listings, {})
        self.assertIsNotNone(a.ping())         # (its cookie came back: now it is)
        a.advertise(secrets.token_bytes(16))
        self.assertTrue(until(lambda: len(self.server.lobby.listings) == 1))

    def test_unverified_player_expires(self):
        self.player(join=False).join(verify=False)
        b = self.player()
        self.assertTrue(until(lambda: len(self.server.lobby.by_addr) == 1, 4.0))
        self.assertEqual([c.ip for c in self.server.lobby.by_addr.values()], [b.ip])

    def test_leave_takes_the_cookie(self):
        a = self.player()
        a.send(R.SU_LEAVE, struct.pack(">Q", a.cookie ^ 1))
        time.sleep(0.1)
        self.assertEqual(len(self.server.lobby.by_addr), 1)
        a.leave()
        self.assertTrue(until(lambda: len(self.server.lobby.by_addr) == 0))


class Limits(SuTest):
    server_options = {"per_ip": 2, "packet_rate": 20.0}

    def test_per_ip_cap(self):
        self.player()
        self.player()
        p = self.player(join=False)
        p.s.sendto(p.join_packet(), p.addr)
        r = p.first(R.SU_REJECT)
        self.assertIsNotNone(r)
        self.assertEqual((len(r), r[4:12]), (4 + 8 + 64, p.nonce))   # (no reason byte: nonce, text)
        self.assertEqual(R.text(r[12:]), "too many players from your address (2)")

    def test_across_protocols(self):
        self.player()
        Player(self.server).join().close()   # (WindowSmash's game from the same address: the same cap)
        p = self.player(join=False)
        p.s.sendto(p.join_packet(), p.addr)
        self.assertIsNotNone(p.first(R.SU_REJECT))

    def test_rate_limit(self):
        host, session, (guest,) = self.room(1)
        for i in range(200):
            guest.data(BCAST, b"x%d" % i)
        got = host.datas(0.5)
        self.assertLess(len(got), 100)
        self.assertGreater(len(got), 10)

    def test_too_big_dropped(self):
        """As in ssbu-recomp's lobby: a body (past its addresses and ports) of 1500 bytes and a
        NetworkInfo at most, for LDN messages that carry one."""
        self.assertEqual(self.server.lobby.o.su_max_data, 2652)
        host, session, (guest,) = self.room(1)
        guest.data(host.ip, bytes(2653))
        guest.data(host.ip, bytes(2652))
        self.assertEqual([len(p) for *_, p in host.datas()], [2652])
        guest.bcast([], bytes(2653))
        guest.bcast([], bytes(2652))
        self.assertEqual([len(p) for *_, p in host.datas()], [2652])


class Banning(SuTest):
    def setUp(self):
        self.ban = os.path.join(os.environ.get("TEMP", "/tmp"), f"ssbu-su-banlist-{os.getpid()}.txt")
        with open(self.ban, "w") as f:
            f.write("10.0.0.0/8\n")
        self.server_options = {"banlist": self.ban}
        super().setUp()

    def tearDown(self):
        super().tearDown()
        os.remove(self.ban)

    def test_banned_then_not(self):
        self.player()
        with open(self.ban, "w") as f:
            f.write("127.0.0.0/8\n")
        os.utime(self.ban, (time.time() + 10, time.time() + 10))
        self.server.lobby.ban_checked = 0
        self.assertTrue(until(lambda: len(self.server.lobby.by_addr) == 0))
        p = self.player(join=False)
        p.s.sendto(p.join_packet(), p.addr)
        self.assertIsNone(p.first(R.SU_WELCOME, 0.3))


class Rooms(SuTest):
    def test_list_join_play_close(self):
        """ssbu-recomp's own lobby test: list, join, accept, play, an outsider kept out, closed."""
        a, b = self.player(name="Alice"), self.player(name="Bob")
        session = secrets.token_bytes(16)
        a.advertise(session)
        self.assertTrue(self.listed(session))
        (owner, info, build), = b.list(7)
        self.assertEqual((owner, info[R.NI_SESSION:R.NI_SESSION + 16], build), (a.ip, session, None))
        b.ldn(a.ip, R.LM_JOIN, session + b"Bob".ljust(32, b"\0") + b"\0\0")
        j = a.first(R.SU_DATA)
        self.assertEqual(j[16:18], bytes((ord("L"), R.LM_JOIN)))
        a.ldn(b.ip, R.LM_ACCEPT, info_of(session, a.ip, [b.ip]))
        self.assertEqual(b.first(R.SU_DATA)[17], R.LM_ACCEPT)
        a.advertise(session, [b.ip])
        self.assertEqual(b.first(R.SU_DATA)[17], R.LM_ADVERTISE)
        a.data(BCAST, b"game-broadcast")
        self.assertEqual([p for *_, p in b.datas()], [b"game-broadcast"])
        b.data(a.ip, b"game-unicast")
        self.assertEqual([p for *_, p in a.datas()], [b"game-unicast"])
        c = self.player(name="Carol")
        c.data(a.ip, b"outsider")
        c.data(BCAST, b"outsider")
        self.assertEqual(a.datas(0.3) + b.datas(0), [])
        a.ldn(BCAST, R.LM_GONE, session)
        gone = b.first(R.SU_DATA)
        self.assertEqual((gone[:4], gone[17]), (R.su_header(R.SU_DATA), R.LM_GONE))
        self.assertTrue(until(lambda: not self.server.lobby.listings))
        self.assertEqual(b.list(8), [])

    def test_listed_to_same_build_and_code(self):
        host, session, _ = self.room()
        same, other_code, other_build = self.player(), self.player(code="friends"), self.player(build=b"ssbu-recomp-ldn1-13.0.4")
        self.assertEqual([o for o, *_ in same.list()], [host.ip])
        self.assertEqual(other_code.list(), [])
        self.assertEqual(other_build.list(), [])
        self.assertEqual(host.list(), [])   # (not its own)

    def test_broadcast_skips_the_listed(self):
        host, session, (a, b) = self.room(2)
        a.bcast([host.ip], b"to b only")
        self.assertEqual([(d, p) for s, d, sp, p in b.datas()], [(BCAST, b"to b only")])
        self.assertEqual(host.datas(0.1), [])
        a.bcast([], b"all")
        self.assertEqual([p for *_, p in host.datas()], [b"all"])

    def test_forged_ldn_messages_dropped(self):
        host, session, (a,) = self.room(1)
        evil = self.player()
        evil.ldn(a.ip, R.LM_GONE, session)
        evil.ldn(a.ip, R.LM_ACCEPT, info_of(session, host.ip))
        evil.ldn(a.ip, R.LM_KICK, session)
        evil.ldn(host.ip, R.LM_KEEP, session)
        evil.data(a.ip, b"x", src=host.ip)
        self.assertEqual(a.datas(0.3) + host.datas(0.1), [])

    def test_station_leaving_the_lobby_tells_its_host(self):
        host, session, (a,) = self.room(1)
        a.leave()
        left = [(s, p) for s, d, sp, p in host.datas() if p[1] == R.LM_LEAVE]
        self.assertEqual([s for s, _ in left], [a.ip])


class Timeouts(SuTest):
    server_options = {"client_timeout": 1.0, "listing_timeout": 2.0, "listed_for": 0.5}

    def test_host_timeout_tells_stations(self):
        host, session, (a,) = self.room(1)
        end, gone = time.monotonic() + 1.6, []
        while time.monotonic() < end and not gone:   # (the station keeps itself alive, without waiting for the PONG)
            a.s.sendto(a.ping_packet(), a.addr)
            gone = [p for s, d, sp, p in a.datas(0.2) if p[1] == R.LM_GONE]
        self.assertTrue(gone, "no LM_GONE after the host timed out")
        self.assertEqual(gone[0][4:20], session)


class Peers(SuTest):
    def test_introduced_once_their_games_talk(self):
        host, session, (a,) = self.room(1)
        self.assertIsNone(host.peers(0.2))   # (in one room, but their games have not talked: nobody to introduce)
        to_a = {e[0]: e for e in talk_until_introduced(host, a)[1]}[a.ip]
        to_host = {e[0]: e for e in talk_until_introduced(a, host)[1]}[host.ip]
        self.assertEqual(to_a[1:3], (b"Guest", (LOCAL, a.s.getsockname()[1])))   # (where the lobby sees each)
        self.assertEqual(to_host[1:3], (b"Host", (LOCAL, host.s.getsockname()[1])))
        self.assertEqual(to_a[3], (0, 0))   # (no LAN address told)
        self.assertEqual(to_a[4], to_host[4])
        self.assertNotEqual(to_a[4], 0)

    def test_direct_off_never_introduced(self):
        host = self.player(name="Host")
        session = secrets.token_bytes(16)
        host.advertise(session)
        self.assertTrue(self.listed(session))
        shy = self.player(name="Shy", direct=False)
        shy.ldn(host.ip, R.LM_JOIN, session + b"Shy".ljust(32, b"\0") + b"\0\0")
        host.advertise(session, [shy.ip])
        for _ in range(3):
            su_talk(host, shy)
        self.assertIsNone(host.peers(0.3))
        self.assertIsNone(shy.peers(0))
        host.data(shy.ip, b"still relayed")
        self.assertEqual([p for *_, p in shy.datas()], [b"still relayed"])

    def test_lan_address_only_behind_the_same_router(self):
        host = self.player(name="Host", lan=(0x0A000005, 5000))
        session = secrets.token_bytes(16)
        host.advertise(session)
        self.assertTrue(self.listed(session))
        a = self.player(name="Guest", lan=(0x0A000006, 6000))
        a.ldn(host.ip, R.LM_JOIN, session + bytes(34))
        host.advertise(session, [a.ip])
        self.assertEqual({e[0]: e for e in talk_until_introduced(host, a)[1]}[a.ip][3], (0x0A000006, 6000))   # (both 127.0.0.1)
        try:
            far = self.player(name="Far", lan=(0x0A000007, 7000), bind="127.0.0.2")
        except OSError:
            self.skipTest("no 127.0.0.2 here")
        far.ldn(host.ip, R.LM_JOIN, session + bytes(34))
        host.advertise(session, [a.ip, far.ip])
        entry = {e[0]: e for e in talk_until_introduced(host, far)[1]}[far.ip]
        self.assertEqual(entry[2:4], ((R.address_number("127.0.0.2"), far.s.getsockname()[1]), (0, 0)))   # (another router)

    def test_left_the_room_empties(self):
        host, session, (a,) = self.room(1)
        talk_until_introduced(host, a)
        talk_until_introduced(a, host)
        a.ldn(host.ip, R.LM_LEAVE, session)
        got = None
        end = time.monotonic() + 3
        while time.monotonic() < end and (got is None or got[1]):
            got = a.peers(0.3) or got
        self.assertEqual(got[1], [])

    def test_lost_peers_comes_again(self):
        old, R.SU_PEERS_AGAIN = R.SU_PEERS_AGAIN, 0.3
        try:
            host, session, (a,) = self.room(1)
            v, entries = talk_until_introduced(host, a)
            time.sleep(0.4)
            self.assertEqual(host.peers(0), None)
            host.ping()
            self.assertEqual(host.peers(), (v, entries))   # (the same, again: its game takes a newer one only)
        finally:
            R.SU_PEERS_AGAIN = old


class ModSets(unittest.TestCase):
    def test_block(self):
        good = mods((b"Alpha", 0x11), (b"B" * R.MOD_NAME, 0x22))
        self.assertEqual(R.mods_block(good), good)
        self.assertEqual(R.mods_block(good + b"more"), good)   # (cut to its own length)
        self.assertEqual(R.mods_block(mods()), b"\0")
        self.assertIsNone(R.mods_block(b""))
        self.assertIsNone(R.mods_block(good[:-1]))                                  # (cut short)
        self.assertIsNone(R.mods_block(mods((b"C" * (R.MOD_NAME + 1), 1))))        # (a name too long)
        self.assertIsNone(R.mods_block(bytes((R.MODS_MAX + 1,)) + bytes(5) * 40))  # (too many)
        self.assertIsNone(R.mods_block(bytes((3, 0, 0, 0, 1, 40)) + b"x" * 40))    # (ssbu-recomp's test's broken block)


class RoomBrowser(SuTest):
    """ssbu-recomp's room browser test: in the lobby code "test", games of two versions, of two mod
    sets, and an older game that names no mods and sends LIST without flags."""
    V4, V5M = b"ssbu-recomp-ldn1-13.0.4", b"ssbu-recomp-ldn1-13.0.5+m1234abcd"
    MODS = mods((b"Alpha", 0x11), (b"Beta", 0x22))

    def setUp(self):
        super().setUp()
        self.v4 = self.player(name="V4", build=self.V4, mods=mods())
        self.v5m = self.player(name="V5m", build=self.V5M, mods=self.MODS)
        self.old = self.player(name="Old")
        self.v5 = self.player(name="V5", mods=mods())
        self.s4, self.s5m = secrets.token_bytes(16), secrets.token_bytes(16)
        self.v4.advertise(self.s4)
        self.v5m.advertise(self.s5m)
        self.assertTrue(self.listed(self.s4) and self.listed(self.s5m))

    def test_older_game_sees_its_own_build_only(self):
        self.assertEqual(self.old.list(1), [])
        self.assertEqual(self.v5.list(2), [])   # (no flags: the same)

    def test_list_every_build(self):
        got = {host: build for host, info, build in self.v5.list(2, other=1)}
        self.assertEqual(got, {self.v4.ip: self.V4.ljust(40, b"\0"), self.v5m.ip: self.V5M.ljust(40, b"\0")})

    def test_own_build_first(self):
        so = secrets.token_bytes(16)
        self.old.advertise(so)
        self.assertTrue(self.listed(so))
        rooms = self.v5.list(3, other=1)
        self.assertEqual((rooms[0][0], len(rooms)), (self.old.ip, 3))
        self.assertEqual([h for h, *_ in self.v5.rooms(4)][0], self.old.ip)

    def test_rooms(self):
        info = {r[4]: r for r in self.v5.rooms(3)}
        self.assertEqual(info[b"V4"], (self.v4.ip, 1, 8, 1 | 4, b"V4", self.V4, b"\0"))
        self.assertEqual(info[b"V5m"], (self.v5m.ip, 1, 8, 1 | 4, b"V5m", self.V5M, self.MODS))
        mine = {r[0]: r[3] for r in self.v4.rooms(4)}
        self.assertEqual(mine, {self.v4.ip: 1 | 2 | 4, self.v5m.ip: 1 | 4})   # (2: its own)
        self.assertEqual(self.player(code="friends").rooms(5), [])

    def test_older_games_room(self):
        so = secrets.token_bytes(16)
        self.old.advertise(so)
        self.assertTrue(self.listed(so))
        flags = {r[4]: r[3] for r in self.v5.rooms(5)}
        self.assertEqual(flags[b"Old"], 1)   # (open; its mods not known)

    def test_order(self):
        full, free = self.player(name="Full", mods=mods()), self.player(name="Free", mods=mods())
        sf, so = secrets.token_bytes(16), secrets.token_bytes(16)
        full.advertise(sf, most=1)
        free.advertise(so)
        self.assertTrue(self.listed(sf) and self.listed(so))
        rooms = [(r[4], r[3]) for r in self.v5.rooms(6)]
        self.assertEqual(rooms[:2], [(b"Free", 1 | 4), (b"Full", 4)])   # (its own build's first, open before full)
        self.assertEqual({r for r, _ in rooms[2:]}, {b"V4", b"V5m"})

    def test_join_across_builds_never_reaches_the_host(self):
        self.v5.ldn(self.v4.ip, R.LM_JOIN, self.s4 + b"V5".ljust(32, b"\0") + b"\0\0")
        self.assertEqual(self.v4.datas(0.3), [])
        so = secrets.token_bytes(16)
        self.old.advertise(so)
        self.assertTrue(self.listed(so))
        self.v5.ldn(self.old.ip, R.LM_JOIN, so + b"V5".ljust(32, b"\0") + b"\0\0")   # (the same build: an older game's room)
        self.assertEqual([p[:2] for s, d, sp, p in self.old.datas()], [b"L" + bytes((R.LM_JOIN,))])

    def test_broken_mod_block(self):
        bad = self.player(name="Bad", build=b"ssbu-recomp-ldn1-13.0.5+m0", mods=bytes((3, 0, 0, 0, 1, 40)) + b"x" * 40)
        sb = secrets.token_bytes(16)
        bad.advertise(sb)
        self.assertTrue(self.listed(sb))
        flags = {r[4]: (r[3], r[6]) for r in self.v5.rooms(6)}
        self.assertEqual(flags[b"Bad"], (1, b"\0"))   # (in the lobby, its mods not known)


class Matchmaking(SuTest):
    """ssbu-recomp's matchmaking test, split up."""
    server_options = {"search_for": 30.0, "match_for": 30.0}
    OTHER = b"ssbu-recomp-ldn1-13.0.4"

    def pair(self, a, b, sa=101, sb=102):
        """a searches, then b: matched, a hosts."""
        a.match(1, sa)
        self.assertEqual(a.state()[:2], (sa, R.MS_SEARCHING))
        b.match(1, sb)
        return a.until_state(R.MS_MATCHED), b.until_state(R.MS_MATCHED)

    def test_paired_same_build_and_code_only(self):
        a, b = self.player(name="MA", mods=mods()), self.player(name="MB", mods=mods())
        y, e = self.player(name="MY", build=self.OTHER, mods=mods()), self.player(name="ME", code="other", mods=mods())
        old = self.player(name="MOld")   # (an older game: it never sends MATCH)
        a.match(1, 101)
        self.assertEqual(a.state(), (101, R.MS_SEARCHING, 0, 0, 0, b"", 1, 3, 0))   # (searching 1, players of its build and code 3)
        y.match(1, 201)   # (another version, another code: never paired with MA)
        self.assertEqual(y.state()[:2], (201, R.MS_SEARCHING))
        e.match(1, 301)
        self.assertEqual(e.state()[:2], (301, R.MS_SEARCHING))
        pong = a.ping()
        self.assertEqual((len(pong), struct.unpack_from(">HHHH", pong, 12)), (20, (3, 0, 1, 4)))   # (and every build of "test": 4)
        a.match(1, 101)   # (the same search asked again: still searching, not a new one)
        self.assertEqual(a.state()[:2], (101, R.MS_SEARCHING))
        b.match(1, 102)
        sa, sb = a.until_state(R.MS_MATCHED), b.until_state(R.MS_MATCHED)
        self.assertEqual(sa[:7], (101, R.MS_MATCHED, R.ROLE_HOST, 0, b.ip, b"MB", 0))   # (MA waited longer: MA hosts)
        self.assertEqual(sb[:7], (102, R.MS_MATCHED, R.ROLE_JOIN, 0, a.ip, b"MA", 0))
        b.match(1, 102)   # (asked again, as if an answer was lost: the same match)
        self.assertEqual(b.state()[:3], (102, R.MS_MATCHED, R.ROLE_JOIN))
        self.assertIsNone(old.state(0.2))
        self.assertIsNone(y.state(0))

    def test_room_first_then_together(self):
        a, b = self.player(name="MA", mods=mods()), self.player(name="MB", mods=mods())
        self.pair(a, b)
        o = self.player(name="MOther", mods=mods())   # (another room of the same build, open already)
        so = secrets.token_bytes(16)
        o.advertise(so)
        self.assertTrue(self.listed(so))
        session = secrets.token_bytes(16)
        a.advertise(session)
        self.assertIsNotNone(b.until_state(R.MS_MATCHED, flags=1), "the guest was not told the host's room is open")
        rooms = b.list(9, other=1)
        self.assertEqual(([r[0] for r in rooms][0], len(rooms)), (a.ip, 2))   # (the matched host's room first)
        self.assertEqual([r[0] for r in b.list(10)][0], a.ip)
        b.ldn(a.ip, R.LM_JOIN, session + b"MB".ljust(32, b"\0") + b"\0\0")
        self.assertTrue(a.datas())
        a.advertise(session, [b.ip])
        ta, tb = a.until_state(R.MS_TOGETHER), b.until_state(R.MS_TOGETHER)
        self.assertEqual(ta[:6], (101, R.MS_TOGETHER, R.ROLE_HOST, 0, b.ip, b"MB"))
        self.assertEqual(tb[:6], (102, R.MS_TOGETHER, R.ROLE_JOIN, 0, a.ip, b"MA"))
        b.match(1, 102)   # (asked again: how it ended, not a new search)
        self.assertEqual(b.state()[:2], (102, R.MS_TOGETHER))
        self.assertEqual(self.server.lobby.matches, set())

    def test_cancel_tells_the_other(self):
        f, g = self.player(name="MF", mods=mods()), self.player(name="MG", mods=mods())
        self.pair(f, g, 1, 2)
        g.match(0, 2)
        self.assertEqual(g.until_state(R.MS_IDLE)[0], 2)
        self.assertEqual(f.until_state(R.MS_PEER_LEFT)[4:6], (g.ip, b"MG"))

    def test_leaving_tells_the_other(self):
        h, i = self.player(name="MH", mods=mods()), self.player(name="MI", mods=mods())
        self.pair(h, i, 5, 6)
        h.leave()
        self.assertEqual(i.until_state(R.MS_PEER_LEFT)[5], b"MH")

    def test_searching_again_ends_the_match(self):
        h, i = self.player(name="MH", mods=mods()), self.player(name="MI", mods=mods())
        self.pair(h, i, 5, 6)
        i.match(1, 7)
        self.assertEqual(h.until_state(R.MS_PEER_LEFT)[5], b"MI")
        self.assertEqual(i.until_state(R.MS_SEARCHING)[0], 7)

    def test_an_open_room_hosts(self):
        k, m = self.player(name="MK", mods=mods()), self.player(name="MM", mods=mods())
        sk = secrets.token_bytes(16)
        k.advertise(sk)
        self.assertTrue(self.listed(sk))
        m.match(1, 7)
        self.assertEqual(m.state()[1], R.MS_SEARCHING)
        k.match(1, 8)   # (searched second, but has an open room: hosts)
        sk_, sm = k.until_state(R.MS_MATCHED), m.until_state(R.MS_MATCHED)
        self.assertEqual((sk_[2], sm[2], sm[3] & 1, sm[4]), (R.ROLE_HOST, R.ROLE_JOIN, 1, k.ip))

    def test_rate(self):
        a = self.player(mods=mods())
        for _ in range(40):
            a.match(1, 9)
        self.assertLess(len(a.all(R.SU_MATCH_STATE, 0.5)), 15)


class MatchTimeouts(SuTest):
    server_options = {"search_for": 0.5, "match_for": 1.0}

    def test_search_times_out(self):
        y = self.player(name="MY", mods=mods())
        y.match(1, 201)
        self.assertEqual(y.state()[:2], (201, R.MS_SEARCHING))
        self.assertEqual(y.until_state(R.MS_TIMEOUT, 4.0)[0], 201)
        y.match(1, 201)   # (the search that ended, asked again: its end again)
        self.assertEqual(y.state()[:2], (201, R.MS_TIMEOUT))
        y.match(1, 202)   # (a new search)
        self.assertEqual(y.state()[:2], (202, R.MS_SEARCHING))
        y.match(0, 202)
        self.assertEqual(y.until_state(R.MS_IDLE)[0], 202)

    def test_match_nobody_follows_expires(self):
        self.server.lobby.o.search_for = 30.0   # (a slow runner must not end k's search before m's comes)
        k, m = self.player(name="MK", mods=mods()), self.player(name="MM", mods=mods())
        k.match(1, 7)
        m.match(1, 8)
        self.assertIsNotNone(k.until_state(R.MS_MATCHED))
        self.assertEqual(k.until_state(R.MS_EXPIRED, 4.0)[4], m.ip)
        self.assertEqual(m.until_state(R.MS_EXPIRED, 4.0)[4], k.ip)


class Rollback(SuTest):
    """ssbu-recomp's rollback matchmaking test (lobby 1.4: MATCH op 2), on the public lobby code."""
    server_options = {"search_for": 30.0, "match_for": 30.0}

    def test_rollback_match(self):
        r1, r2, r3, s = (self.player(name=n, code=R.SU_PUBLIC, mods=mods()) for n in ("R1", "R2", "R3", "RS"))
        r1.match(2, 401)
        self.assertEqual(r1.state()[:2], (401, R.MS_SEARCHING))
        r2.match(1, 402)   # (a Local Wireless search: never paired with a rollback search)
        self.assertEqual(r2.state()[:2], (402, R.MS_SEARCHING))
        self.assertIsNone(r1.state(0.3))
        r3.match(2, 403)
        a, b = r1.until_state(R.MS_MATCHED), r3.until_state(R.MS_MATCHED)
        self.assertEqual(a[:6], (401, R.MS_MATCHED, R.ROLE_HOST, 0, r3.ip, b"R3"))   # (R1 waited longer: player 1)
        self.assertEqual(b[:6], (403, R.MS_MATCHED, R.ROLE_JOIN, 0, r1.ip, b"R1"))
        r3.match(2, 403)   # (asked again: the same match)
        self.assertEqual(r3.state()[:3], (403, R.MS_MATCHED, R.ROLE_JOIN))
        s.rb(r1.ip, b"Hstranger")   # (a stranger on the public code: dropped)
        r2.rb(0, b"Hall")            # (to everyone, on the public code: dropped)
        self.assertEqual(r1.rbs(), [])
        r1.rb(r3.ip, b"Hchoice-from-r1")
        self.assertEqual(r3.rbs(), [(r1.ip, b"Hchoice-from-r1")])
        self.assertEqual(s.rbs(0.1) + r2.rbs(0), [])
        r3.rb(r1.ip, b"Ipropose-from-r3")
        self.assertEqual(r1.rbs(), [(r3.ip, b"Ipropose-from-r3")])
        ta, tb = r1.until_state(R.MS_TOGETHER), r3.until_state(R.MS_TOGETHER)   # (messages both ways: together)
        self.assertEqual(ta[:5], (401, R.MS_TOGETHER, R.ROLE_HOST, 0, r3.ip))
        self.assertEqual(tb[:5], (403, R.MS_TOGETHER, R.ROLE_JOIN, 0, r1.ip))
        r1.rb(r3.ip, b"pads")   # (after: still carried, the match goes on)
        self.assertEqual(r3.rbs(), [(r1.ip, b"pads")])
        r2.match(0, 402)
        self.assertEqual(r2.until_state(R.MS_IDLE)[0], 402)

    def test_public_code_room_players(self):
        host, session, (a,) = self.room(1, code=R.SU_PUBLIC)
        stranger = self.player(code=R.SU_PUBLIC)
        a.rb(host.ip, b"in the room")
        stranger.rb(host.ip, b"not")
        self.assertEqual(host.rbs(), [(a.ip, b"in the room")])

    def test_private_code(self):
        a, b, c = (self.player(name=n, code="friends") for n in "ABC")
        other = self.player(code="friends", build=b"ssbu-recomp-ldn1-13.0.4")
        a.rb(b.ip, b"to b")
        self.assertEqual(b.rbs(), [(a.ip, b"to b")])
        a.rb(0, b"to all")   # (a private code: to every other player of its build and code)
        self.assertEqual(b.rbs() + c.rbs(0), [(a.ip, b"to all")] * 2)
        a.rb(other.ip, b"another build")
        self.assertEqual(other.rbs() + a.rbs(0), [])

    def test_size_and_rate(self):
        self.assertEqual(R.RB_MAX, 1500)   # (ssbu-recomp's lobby: 16 < length <= 1500, its header included)
        self.server.lobby.o.packet_rate = 20.0
        a, b = self.player(code="friends"), self.player(code="friends")
        a.send(R.SU_RB, struct.pack(">QI", a.cookie, b.ip))   # (no message)
        a.rb(b.ip, bytes(R.RB_MAX - 16 + 1))
        a.rb(b.ip, bytes(R.RB_MAX - 16))
        self.assertEqual([len(m) for _, m in b.rbs()], [R.RB_MAX - 16])
        flood = self.player(code="friends")
        for i in range(200):
            flood.rb(b.ip, b"x%d" % i)
        self.assertLess(len(b.rbs(0.5)), 100)


class BothProtocols(SuTest):
    def test_apart(self):
        """WindowSmash's games and ssbu-recomp's in one lobby: their own addresses, their own rooms."""
        sn = Player(self.server).join()
        self.addCleanup(sn.close)
        su = self.player(code="smash", build=R.REQUIRED_BUILD)   # (even with WindowSmash's build and lobby code)
        self.assertNotEqual(sn.ip, su.ip)
        session = secrets.token_bytes(16)
        sn.advertise(session)
        self.assertTrue(self.listed(session))
        self.assertEqual(su.list(), [])
        su.ldn(sn.ip, R.LM_JOIN, session + bytes(34))
        su.data(sn.ip, b"hello")
        self.assertEqual(sn.datas(0.3), [])
        self.assertEqual(struct.unpack_from(">HH", sn.ping(), 12), (1, 1))   # (one player and one room here)
        self.assertEqual(struct.unpack_from(">HH", su.ping(), 12), (1, 0))

    def test_data_caps(self):
        """SU's larger cap is SU's only: WindowSmash's games keep 1500 bytes."""
        host, guest = Player(self.server, name="Host").join(), Player(self.server, name="Guest").join()
        self.addCleanup(host.close)
        self.addCleanup(guest.close)
        session = secrets.token_bytes(16)
        host.advertise(session)
        self.assertTrue(self.listed(session))
        guest.ldn(host.ip, R.LM_JOIN, session + b"Guest".ljust(32, b"\0") + b"\0\0")
        self.assertTrue(any(src == guest.ip for src, *_ in host.datas()), "the JOIN did not reach the host")
        host.advertise(session, [guest.ip])
        host.ldn(guest.ip, R.LM_ACCEPT, info_of(session, host.ip, [guest.ip]))
        guest.recv(0.2)
        guest.data(host.ip, bytes(1501))
        guest.data(host.ip, bytes(1500))
        self.assertEqual([len(p) for *_, p in host.datas()], [1500])


class Status(SuTest):
    """The status page (--status-port): counts only, for a web page."""
    server_options = {"status_port": 0, "search_for": 30.0}

    def setUp(self):
        super().setUp()
        self.port = self.server.lobby.status_server.sockets[0].getsockname()[1]

    def ask(self, method="GET", path="/status"):
        """(status, headers, body) of one request."""
        h = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        try:
            h.request(method, path)
            r = h.getresponse()
            return r.status, {k.lower(): v for k, v in r.getheaders()}, r.read()
        finally:
            h.close()

    def counts(self):
        code, headers, body = self.ask()
        self.assertEqual(code, 200)
        return json.loads(body)

    def closed(self, s):
        """Whether the lobby closes this connection (within a few seconds) without an answer."""
        s.settimeout(3)
        try:
            return s.recv(4096) == b""
        except ConnectionResetError:
            return True

    def test_counts(self):
        """Players (verified ones, either protocol), rooms, players searching; nothing about them."""
        doc = self.counts()
        self.assertEqual(set(doc), {"up", "players", "rooms", "searching", "version", "uptime_s"})
        self.assertEqual((doc["up"], doc["players"], doc["rooms"], doc["searching"]), (True, 0, 0, 0))
        self.assertIn(f"SU {R.SU_PROTOCOL}", doc["version"])
        host, session, _ = self.room(1, code="hidden-code")
        seeker = self.player(name="Seeker", code="hidden-code")
        seeker.match(1, 55)
        self.assertTrue(until(lambda: self.server.lobby.searching))
        sn = Player(self.server, name="Sneaky").join()
        self.addCleanup(sn.close)
        self.player(join=False, name="Waiting").join(verify=False)   # (not verified: not counted)
        code, headers, body = self.ask()
        doc = json.loads(body)
        self.assertEqual((doc["players"], doc["rooms"], doc["searching"]), (4, 1, 1))
        self.assertEqual(headers["content-type"], "application/json")
        self.assertEqual(headers["access-control-allow-origin"], "*")
        self.assertEqual(headers["cache-control"], "no-store")
        self.assertEqual(int(headers["content-length"]), len(body))
        for leak in (b"127.0.0.1", b"10.13.", b"Host", b"Guest", b"Seeker", b"Sneaky", b"Waiting", b"hidden", BUILD,
                     R.REQUIRED_BUILD):
            self.assertNotIn(leak, body)
        host.ldn(BCAST, R.LM_GONE, session)
        self.assertTrue(until(lambda: self.counts()["rooms"] == 0))

    def test_other_requests(self):
        self.assertEqual(self.ask(path="/status/?x=1")[0], 200)
        code, _, body = self.ask(path="/players")
        self.assertEqual((code, json.loads(body)), (404, {"error": "not found"}))
        code, headers, body = self.ask("HEAD")
        self.assertEqual((code, body), (200, b""))
        self.assertGreater(int(headers["content-length"]), 0)
        code, headers, body = self.ask("OPTIONS")
        self.assertEqual((code, body), (204, b""))
        self.assertEqual(headers["access-control-allow-methods"], "GET, OPTIONS")
        code, headers, _ = self.ask("POST")
        self.assertEqual(code, 405)
        self.assertIn("GET", headers["allow"])

    def test_slow_and_large(self):
        """A request that does not come in time, or that is too large, is closed unanswered."""
        old = R.STATUS_TIME
        R.STATUS_TIME = 0.3
        self.addCleanup(setattr, R, "STATUS_TIME", old)
        slow = socket.create_connection(("127.0.0.1", self.port))
        self.addCleanup(slow.close)
        slow.sendall(b"GET /sta")
        self.assertTrue(self.closed(slow))
        large = socket.create_connection(("127.0.0.1", self.port))
        self.addCleanup(large.close)
        try:
            large.sendall(b"GET /status HTTP/1.0\r\nX: " + b"a" * (R.STATUS_REQUEST + 100) + b"\r\n\r\n")
        except OSError:
            pass   # (closed while it sent)
        self.assertTrue(self.closed(large))
        self.assertTrue(until(lambda: self.server.lobby.status_open == 0))
        self.assertTrue(self.counts()["up"])

    def test_at_once(self):
        """Past STATUS_OPEN requests at once, more are closed; once those end, it answers again."""
        old = R.STATUS_OPEN
        R.STATUS_OPEN = 2
        self.addCleanup(setattr, R, "STATUS_OPEN", old)
        waiting = [socket.create_connection(("127.0.0.1", self.port)) for _ in range(2)]
        for w in waiting:
            self.addCleanup(w.close)
        self.assertTrue(until(lambda: self.server.lobby.status_open == 2))
        more = socket.create_connection(("127.0.0.1", self.port))
        self.addCleanup(more.close)
        self.assertTrue(self.closed(more))
        for w in waiting:
            w.close()
        self.assertTrue(until(lambda: self.server.lobby.status_open == 0))
        self.assertTrue(self.counts()["up"])

    def test_off(self):
        """No status page unless asked for."""
        self.assertIsNone(SuTest.server_options.get("status_port"))
        self.assertIsNone(R.parse_args([]).status_port)
        self.assertEqual(R.parse_args(["--status-port", "8080"]).status_bind, "127.0.0.1")


if __name__ == "__main__":
    unittest.main()
