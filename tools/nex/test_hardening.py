"""What untrusted clients may not do to the NEX server (and the online lobby's NEX streams): the
problems the review of 2026-10-18 found, each tried the way an attacker would, with the in-process
clients of nex.selftest.   python -m nex.test_hardening   (from tools/; nex.selftest runs it too)

The test names say which finding of the review they are about (item 1 ... item 10); test_logs_*
are about the server's log, which held every player's NSA id (the account's secret) until 2026-10-22."""
import asyncio
import contextlib
import hashlib
import io
import json
import os
import secrets
import struct
import sys
import tempfile
import time
import unittest

from . import rmc, selftest
from .crypto import RC4
from .matchmaking import MAX_ATTRIBS, NO_GATHERING, Session
from .prudp import (DATA, F_ACK, F_HAS_SIZE, F_NEED_ACK, F_RELIABLE, FRAGMENT, HOLD_WINDOW, MAX_CONNS_PER_HOST,
                    MAX_MESSAGE, O_FRAGMENT, O_SIGNATURE, O_SUPPORT, SYN, Connection, Packet, PrudpServer, Signer,
                    decode, decode_lite, encode_lite)
from .ranked import MAX_CATEGORIES, MAX_RATING, ROUND_LIFETIME, WIN, LOSS
from .server import ACCESS_KEY, AUTH_PORT, MAX_RESPONSE, SECURE_PORT, Accounts, make_server
from .streams import StreamIn, StreamOut


class Net(selftest.Net):
    """selftest's network, with the server's log kept rather than printed."""

    def __init__(self, state_dir=None, verbose=1):
        self.boxes = {}
        self.log = []
        self.server = make_server(lambda addr, port, data, src=None: self.boxes.setdefault(addr, []).append(data),
                                  state_dir=state_dir, verbose=verbose, log=lambda *a: self.log.append(" ".join(map(str, a))))


def u32s(*values):
    out = StreamOut()
    for v in values:
        out.u32(v)
    return out.get()


def call(c, protocol, method, body=b""):
    """The server's answer to one request (an rmc.Message, error or not)."""
    cid = c.call
    c.call += 1
    p = Packet(DATA, F_RELIABLE | F_NEED_ACK | F_HAS_SIZE, seq=c.seq, options={O_FRAGMENT: b"\0"},
               payload=c.rc4_out.crypt(rmc.request(protocol, cid, method, body)))
    c.seq += 1
    c.send(p, c.session_key, c.server_sig)
    for m in c.received:
        if not m.request and m.call_id == cid:
            c.received.remove(m)
            return m
    raise AssertionError(f"no answer to {rmc.name_of(protocol, method)}")


def session_param(extra=(), max_participants=2):
    """CreateMatchmakeSessionWithParam's parameter."""
    ms = Session()
    ms.data_level = 0
    ms.min_participants, ms.max_participants = 2, max_participants
    ms.game_mode, ms.attribs, ms.app_buffer = 3, [1, 0, 0, 0, 0, 0], b"rules"
    param = StreamOut()

    def p(o):
        ms.encode(o, 0)
        o.list(list(extra), StreamOut.pid)
        o.u32(0)
        o.u32(0)
        o.string("")
        o.u16(1)
    param.struct(p)
    return param.get()


def join_param(gid, extra=()):
    """JoinMatchmakeSessionWithParam's parameter."""
    out = StreamOut()

    def p(o):
        o.u32(gid)
        o.list(list(extra), StreamOut.pid)
        o.u32(0)
        o.u32(0)
        o.u8(0)
        o.string("")
        o.string("")
        o.string("")
    out.struct(p)
    return out.get()


def round_param(category, gid, pids):
    out = StreamOut()
    out.struct(lambda o: (o.u32(category), o.u32(gid), o.list(list(pids), StreamOut.pid)))
    return out.get()


def end_param(rid, results):
    out = StreamOut()

    def p(o):
        o.u64(rid)
        o.u32(len(results))
        for pid, winloss in results:
            o.struct(lambda y: (y.pid(pid), y.u32(0), y.u32(winloss), y.s32(0), y.qbuffer(b"")))
    out.struct(p)
    return out.get()


def stats_param(category, initial):
    out = StreamOut()
    out.struct(lambda o: (o.u32(category), o.u32(initial)))
    return out.get()


def target_param(pid, category):
    out = StreamOut()
    out.struct(lambda o: (o.pid(pid), o.u32(category)))
    return out.get()


def probe_param(targets, source):
    out = StreamOut()
    out.list(targets, StreamOut.string)
    out.string(source)
    return out.get()


NSA = 0x7E57C0DE5EC2E7A1          # (made up for the test: no player's)
NAME = "Tester"
TOKEN = f"ssbu-port.1.{NSA:016x}.{NAME}"


def secret_forms(nsa=NSA, token=TOKEN):
    """Every way the test player's NSA id or token could be written into a log: as text (hex in
    either case, decimal - the user name nex.selftest logs in with - and the token), and those or
    the id's u64 as hex dumps, with spaces (streams.hexdump) or none (bytes.hex)."""
    texts = [f"{nsa:016x}", f"{nsa:016X}", str(nsa), token]
    raw = [t.encode() for t in texts] + [struct.pack("<Q", nsa), struct.pack(">Q", nsa)]
    return texts + [b.hex(" ") for b in raw] + [b.hex() for b in raw]


def login_param(token=TOKEN, username=str(NSA), corrupt=False):
    """ValidateAndRequestTicketWithParam's parameter as nex.selftest.authenticate writes it, with any
    token (corrupt: AnyDataHolder lengths that disagree, which the server fails to read)."""
    info = StreamOut()
    info.string(token)
    info.u32(4)
    info.u8(2)
    info.u32(0)
    data = StreamOut()
    data.struct(lambda d: None)
    data.struct(lambda d: d.write(info.get()))
    param = StreamOut()

    def p(o):
        o.u32(3)
        o.string(username)
        o.string("AuthenticationInfo")
        o.u32(len(data.get()) + 4 + (1 if corrupt else 0))
        o.u32(len(data.get()))
        o.write(data.get())
        o.bool(False)
        o.u32(40603)
        o.u32(0)
    param.struct(p)
    return param.get()


def send_raw(c, message):
    """Any bytes as one RMC message (DATA), with no answer looked for."""
    p = Packet(DATA, F_RELIABLE | F_NEED_ACK | F_HAS_SIZE, seq=c.seq, options={O_FRAGMENT: b"\0"},
               payload=c.rc4_out.crypt(message))
    c.seq += 1
    c.send(p, c.session_key, c.server_sig)


class Case(unittest.TestCase):
    lite = False

    def setUp(self):
        selftest.LITE = self.lite
        self.net = Net()
        self.server = self.net.server
        self.mm = self.server.matchmaking
        self.ranked = self.server.ranked
        # players at 10.13.0.9 are of another game build or lobby code
        self.server.part_of = lambda ip: "elsewhere" if ip == "10.13.0.9" else "here"

    def player(self, n, name):
        with contextlib.redirect_stdout(io.StringIO()):
            c = selftest.login(self.net, (f"10.13.0.{n}", 49152), 0x1000 + n, name)
        c.ip = f"10.13.0.{n}"
        return c

    def auto(self, c):
        with contextlib.redirect_stdout(io.StringIO()):
            return selftest.auto_matchmake(c)

    def ok(self, m):
        self.assertTrue(m.success, f"{m!r}")
        return StreamIn(m.body)

    def refused(self, m, code):
        self.assertFalse(m.success, f"{m!r} was answered")
        self.assertEqual(m.error, code, f"{m.error:#x}, not {code:#x}")

    def stats(self, c, category, initial=1000):
        r = StreamIn(self.ok(call(c, 120, 12, stats_param(category, initial))).struct()[1])
        r.u64(), r.u32(), r.pid()
        for _ in range(12):
            r.u32()
        return r.u32()


class Matchmaking(Case):
    def test_item1_modify_attribute_index_is_bounded(self):
        a, x = self.player(2, "Alpha"), self.player(4, "Mallory")
        g = self.auto(a)
        self.refused(call(a, 109, 8, u32s(g.gid, 0xFFFFFFFF, 7)), rmc.CORE_INVALID_ARGUMENT)   # (was: 4G zeros)
        self.assertEqual(len(self.mm.sessions[g.gid].attribs), 6)
        self.ok(call(a, 109, 8, u32s(g.gid, MAX_ATTRIBS - 1, 7)))
        self.assertEqual(self.mm.sessions[g.gid].attribs[-1], 7)
        self.refused(call(a, 109, 8, u32s(g.gid, MAX_ATTRIBS, 7)), rmc.CORE_INVALID_ARGUMENT)
        self.refused(call(x, 109, 8, u32s(g.gid, 0, 9)), rmc.RENDEZVOUS_NOT_PARTICIPATED)

    def test_item3_nobody_is_brought_into_a_gathering(self):
        a, b, x = self.player(2, "Alpha"), self.player(3, "Bravo"), self.player(4, "Mallory")
        gb = self.auto(b)
        gx = Session.parse(self.ok(call(x, 109, 38, session_param(extra=[a.pid, b.pid]))))
        self.assertEqual(self.mm.sessions[gx.gid].participants, [x.pid])
        self.assertEqual(self.mm.sessions[gb.gid].participants, [b.pid])
        self.assertFalse(self.server.may_talk(x.ip, a.ip))
        self.assertFalse(self.server.may_talk(x.ip, b.ip))
        # joining an open gathering with room is fine, bringing someone along is not
        self.ok(call(x, 109, 39, join_param(gb.gid, extra=[a.pid])))
        self.assertEqual(self.mm.sessions[gb.gid].participants, [b.pid, x.pid])
        self.assertNotIn(gx.gid, self.mm.sessions)   # (x left its own, which closed)
        self.assertTrue(self.server.may_talk(x.ip, b.ip))
        self.assertFalse(self.server.may_talk(x.ip, a.ip))

    def test_item3_join_checks_room_openness_and_part(self):
        a, c, d = self.player(2, "Alpha"), self.player(3, "Charlie"), self.player(4, "Delta")
        e, f = self.player(5, "Echo"), self.player(9, "Foreign")
        g = self.auto(a)
        self.ok(call(c, 109, 39, join_param(g.gid)))
        self.refused(call(d, 109, 39, join_param(g.gid)), rmc.RENDEZVOUS_SESSION_FULL)
        g2 = Session.parse(self.ok(call(e, 109, 38, session_param(max_participants=4))))
        self.refused(call(d, 109, 1, u32s(g2.gid)), rmc.RENDEZVOUS_NOT_PARTICIPATED)   # (only its owner closes it)
        self.ok(call(e, 109, 1, u32s(g2.gid)))
        self.refused(call(d, 109, 39, join_param(g2.gid)), rmc.RENDEZVOUS_SESSION_CLOSED)
        self.ok(call(e, 109, 2, u32s(g2.gid)))
        self.refused(call(f, 109, 39, join_param(g2.gid)), NO_GATHERING)   # another build or lobby code
        self.ok(call(d, 109, 39, join_param(g2.gid)))
        self.assertEqual(self.mm.sessions[g2.gid].participants, [e.pid, d.pid])

    def test_item3_only_members_and_owners_change_a_gathering(self):
        a, b, x = self.player(2, "Alpha"), self.player(3, "Bravo"), self.player(4, "Mallory")
        g = self.auto(a)
        self.auto(b)
        gid = g.gid
        self.assertEqual(self.mm.sessions[gid].participants, [a.pid, b.pid])
        part = StreamOut()
        part.struct(lambda o: (o.u32(gid), o.u32(0xF), o.list([0], StreamOut.u32), o.bool(False), o.buffer(b"x"),
                               o.u8(9)))
        outsider = [(21, 42, u32s(gid) + b"\1"), (21, 2, u32s(gid)), (109, 11, u32s(gid, 1) + b"x"),
                    (109, 36, u32s(gid)), (109, 37, u32s(gid)), (109, 1, u32s(gid)), (109, 2, u32s(gid)),
                    (109, 12, u32s(gid, 1, 0)), (109, 44, part.get()), (109, 34, u32s(gid) + b"\x09"),
                    (21, 44, u32s(gid, 1) + struct.pack("<Q", x.pid))]
        for protocol, method, body in outsider:
            self.refused(call(x, protocol, method, body), rmc.RENDEZVOUS_NOT_PARTICIPATED)
        for protocol, method, body in [(21, 2, u32s(gid)), (109, 11, u32s(gid, 1) + b"y"), (109, 36, u32s(gid)),
                                       (109, 44, part.get()), (21, 44, u32s(gid, 1) + struct.pack("<Q", b.pid))]:
            self.refused(call(b, protocol, method, body), rmc.RENDEZVOUS_PERMISSION_DENIED)
        ms = self.mm.sessions[gid]
        self.assertEqual((ms.owner, ms.host, ms.app_buffer, ms.open), (a.pid, a.pid, b"rules", True))
        self.ok(call(a, 109, 11, u32s(gid, 1) + b"z"))
        self.assertEqual(ms.app_buffer, b"z")
        # Pia's host migration: a participant becomes the host (and may become the owner)
        self.ok(call(b, 21, 42, u32s(gid) + b"\1"))
        self.assertEqual((ms.owner, ms.host), (b.pid, b.pid))
        self.ok(call(b, 21, 2, u32s(gid)))
        self.assertNotIn(gid, self.mm.sessions)
        self.assertFalse(self.server.may_talk(a.ip, b.ip))

    def test_item10_find_by_gid_keeps_the_session_key_to_participants(self):
        a, x = self.player(2, "Alpha"), self.player(4, "Mallory")
        g = self.auto(a)
        mine = Session.parse(self.ok(call(a, 109, 41, u32s(g.gid))))
        theirs = Session.parse(self.ok(call(x, 109, 41, u32s(g.gid))))
        self.assertEqual(len(mine.session_key), 32)
        self.assertEqual((theirs.session_key, theirs.user_password), (b"", ""))
        self.assertEqual(theirs.gid, g.gid)

    def test_item6_probes_only_within_a_gathering_and_once(self):
        a, b, x = self.player(2, "Alpha"), self.player(3, "Bravo"), self.player(4, "Mallory")
        self.auto(a)
        self.auto(b)
        url = {c: f"prudp:/address={c.ip};port=12345;type=3" for c in (a, b, x)}

        def probes(c):
            c.collect()
            n = sum(1 for m in c.received if m.request and (m.protocol, m.method) == (3, 2))
            c.received = [m for m in c.received if not (m.request and m.protocol == 3)]
            return n
        probes(a), probes(b)
        self.ok(call(x, 3, 3, probe_param([url[a], url[b]] * 4, url[x])))   # not in their gathering
        self.assertEqual((probes(a), probes(b)), (0, 0))
        self.ok(call(a, 3, 3, probe_param([url[b]] * 8, url[a])))
        self.assertEqual(probes(b), 1)
        self.refused(call(a, 3, 3, probe_param([url[b]], url[x])), rmc.RENDEZVOUS_PERMISSION_DENIED)
        self.refused(call(a, 3, 3, probe_param([url[b]] * 100, url[a])), rmc.CORE_INVALID_ARGUMENT)
        self.assertEqual(probes(b), 0)


class Tickets(Case):
    def test_item2_ticket_only_for_the_pid_logged_in(self):
        victim = self.player(3, "Victim")
        pids = struct.pack("<QQ", victim.pid, 2)
        anon = selftest.Client(self.net, ("10.13.0.66", 49152), AUTH_PORT)
        anon.connect()
        self.refused(call(anon, 10, 3, pids), rmc.CORE_ACCESS_DENIED)   # (was: the victim's ticket)
        with contextlib.redirect_stdout(io.StringIO()):
            auth, pid, _ticket, _key = selftest.authenticate(self.net, ("10.13.0.67", 49152), 0x6666, "Mallory")
        self.refused(call(auth, 10, 3, pids), rmc.CORE_ACCESS_DENIED)
        r = self.ok(call(auth, 10, 3, struct.pack("<QQ", pid, 2)))
        self.assertEqual(r.u32(), rmc.SUCCESS)
        ticket, key = r.buffer(), r.string()
        with contextlib.redirect_stdout(io.StringIO()):
            selftest.connect_secure(self.net, ("10.13.0.67", 49152), pid, ticket, key, "Mallory")
        self.assertIn(victim.pid, self.server.by_pid)
        self.assertIn(pid, self.server.by_pid)

    def test_item10_auth_server_serves_ticket_granting_alone(self):
        anon = selftest.Client(self.net, ("10.13.0.66", 49152), AUTH_PORT)
        anon.connect()
        for protocol, method, body in [(120, 12, stats_param(7, 1000)), (109, 38, session_param()), (11, 1, u32s(0))]:
            self.refused(call(anon, protocol, method, body), rmc.CORE_NOT_IMPLEMENTED)
        self.assertEqual(self.ranked.stats, {})
        self.assertEqual(self.mm.sessions, {})
        a = self.player(2, "Alpha")   # and the secure server does not grant tickets
        self.refused(call(a, 10, 3, struct.pack("<QQ", a.pid, 2)), rmc.CORE_NOT_IMPLEMENTED)

    def test_item5_accounts_written_in_batches_and_made_at_a_bounded_rate(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.net = Net(tmp)
            self.server = self.net.server
            path = os.path.join(tmp, "nex_accounts.json")
            self.player(2, "Alpha")
            self.assertFalse(os.path.exists(path))
            self.server.tick(1.0)
            self.player(3, "Bravo")
            self.server.tick(5.0)
            with open(path, encoding="utf-8") as f:
                self.assertEqual(len(json.load(f)["users"]), 1)
            self.server.tick(11.5)
            with open(path, encoding="utf-8") as f:
                self.assertEqual(len(json.load(f)["users"]), 2)
        accounts = Accounts(None)
        made = [accounts.login(f"{n:016x}", "x", 0.0) for n in range(100)]
        self.assertEqual(sum(p is not None for p in made), 60)
        self.assertIsNotNone(accounts.login(f"{1:016x}", "x", 0.0))   # (an old one always may)
        self.assertIsNone(accounts.login(f"{500:016x}", "x", 0.5))
        self.assertIsNotNone(accounts.login(f"{500:016x}", "x", 1.5))
        self.assertEqual(accounts.name(made[0]), "x")


class LiteTickets(Case):
    lite = True

    def test_item10_logging_in_again_over_a_stream_closes_the_old_connection(self):
        a1 = self.player(2, "Alpha")
        old = self.server.by_pid[a1.pid]
        self.assertIs(old.server, self.server.secure_lite)
        with contextlib.redirect_stdout(io.StringIO()):
            a2 = selftest.login(self.net, ("10.13.0.2", 49153), 0x1002, "Alpha")
        self.assertEqual(a2.pid, a1.pid)
        self.assertIsNot(self.server.by_pid[a1.pid], old)
        self.assertNotIn(old, self.server.secure_lite.conns.values())
        self.assertEqual(self.server.by_pid[a1.pid].unacked, {})   # (a stream keeps nothing to resend)


class Transport(Case):
    def test_item8_answers_of_more_than_255_fragments(self):
        sent = []
        srv = PrudpServer("t", ACCESS_KEY, lambda addr, d: sent.append(d), lambda c, p: b"", lambda c, d: None,
                          lambda c: None, secure=True)
        c = Connection(srv, ("10.13.0.2", 1), 0xAF, 0.0)
        c.local_vport, c.state, c.session_key = 0xA1, "open", b"k" * 32
        c.rc4_out = RC4(c.session_key)
        message = secrets.token_bytes(400_000)
        srv.send_message(c, message, 0.0)   # (was: ValueError at the 256th fragment, the stream half sent)
        packets = [decode(d) for d in sent]
        n = -(-len(message) // FRAGMENT)
        self.assertEqual(len(packets), n)
        ids = [p.options[O_FRAGMENT][0] for p in packets]
        self.assertEqual(ids[:256], list(range(1, 256)) + [1])
        self.assertEqual(ids[-1], 0)
        self.assertNotIn(0, ids[:-1])
        rc4 = RC4(c.session_key)
        self.assertEqual(b"".join(rc4.crypt(p.payload) for p in packets), message)
        self.assertEqual(c.out_seq, n + 1)

    def test_item8_server_refuses_to_send_huge_answers_and_goes_on(self):
        a = self.player(2, "Alpha")
        self.server.register(200, 1, lambda conn, s: b"\0" * 400_000)
        self.server.register(200, 2, lambda conn, s: b"\1" * 1000)
        self.refused(call(a, 200, 1), rmc.CORE_BUFFER_OVERFLOW)
        self.assertEqual(self.ok(call(a, 200, 2)).read(1000), b"\1" * 1000)   # (the stream is still in step)
        self.assertGreater(MAX_RESPONSE, 300_000)

    def test_item4_connections_from_one_address_are_bounded(self):
        signer = Signer(ACCESS_KEY)
        for port in range(40000, 40010):
            syn = Packet(SYN, F_NEED_ACK | F_HAS_SIZE, src=0xAF, dst=0xA1,
                         options={O_SUPPORT: b"\4\0\0\0", O_SIGNATURE: bytes(16)})
            self.server.datagram(("10.13.0.50", port), SECURE_PORT, signer.encode(syn, b"", b""), 0.0)
        self.assertEqual(len(self.server.secure.conns), MAX_CONNS_PER_HOST)
        self.assertEqual(len(self.server.secure.hosts["10.13.0.50"]), MAX_CONNS_PER_HOST)

    def test_item4_packets_far_ahead_are_not_held(self):
        a = self.player(2, "Alpha")
        conn = self.server.by_pid[a.pid]

        def data(seq, frag, size=FRAGMENT):
            p = Packet(DATA, F_RELIABLE | F_NEED_ACK | F_HAS_SIZE, seq=seq & 0xFFFF, options={O_FRAGMENT: bytes((frag,))},
                       payload=secrets.token_bytes(size))
            return [q for q in a.send(p, a.session_key, a.server_sig) if q.type == DATA and q.flags & F_ACK]
        self.assertEqual(data(conn.in_seq + HOLD_WINDOW + 5, 1), [])   # not acknowledged, not held
        self.assertEqual(conn.held, {})
        self.assertEqual(len(data(conn.in_seq + 5, 1)), 1)
        self.assertEqual(len(conn.held), 1)

    def test_item4_messages_are_bounded(self):
        a = self.player(2, "Alpha")
        conn = self.server.by_pid[a.pid]
        seq = conn.in_seq
        for i in range(MAX_MESSAGE // FRAGMENT + 2):
            p = Packet(DATA, F_RELIABLE | F_NEED_ACK | F_HAS_SIZE, seq=seq + i, options={O_FRAGMENT: b"\1"},
                       payload=secrets.token_bytes(FRAGMENT))
            a.send(p, a.session_key, a.server_sig)
        self.assertNotIn(conn, self.server.secure.conns.values())
        self.assertNotIn(a.pid, self.server.by_pid)


class Ranked(Case):
    def setUp(self):
        super().setUp()
        self.a, self.b, self.x = self.player(2, "Alpha"), self.player(3, "Bravo"), self.player(4, "Mallory")
        self.g = self.auto(self.a)
        self.auto(self.b)

    def start(self, c, category, pids):
        return self.ok(call(c, 120, 1, round_param(category, self.g.gid, pids))).u64()

    def test_item9_rounds_only_for_the_gathering_and_its_players(self):
        a, b, x = self.a, self.b, self.x
        self.refused(call(x, 120, 1, round_param(7, self.g.gid, [x.pid, a.pid])), rmc.RENDEZVOUS_NOT_PARTICIPATED)
        rid = self.start(a, 7, [a.pid, b.pid, x.pid, a.pid])
        r = StreamIn(self.ok(call(b, 120, 2, struct.pack("<Q", rid))).struct()[1])
        self.assertEqual((r.u32(), r.u32(), r.list(StreamIn.pid)), (7, self.g.gid, [a.pid, b.pid]))
        before = self.stats(a, 7), self.stats(b, 7)
        self.refused(call(x, 120, 3, end_param(rid, [(x.pid, WIN), (a.pid, LOSS)])), rmc.RENDEZVOUS_PERMISSION_DENIED)
        self.refused(call(x, 120, 4, struct.pack("<Q", rid)), rmc.RENDEZVOUS_PERMISSION_DENIED)
        self.ok(call(b, 120, 3, end_param(rid, [(b.pid, WIN), (a.pid, LOSS), (x.pid, WIN), (b.pid, LOSS)])))
        after = self.stats(a, 7), self.stats(b, 7)
        self.assertLess(after[0], before[0])
        self.assertGreater(after[1], before[1])
        self.assertNotIn(x.pid, self.ranked.of_pid)
        # a second, different report: logged, not counted
        self.ok(call(a, 120, 3, end_param(rid, [(a.pid, WIN), (b.pid, LOSS)])))
        self.assertEqual((self.stats(a, 7), self.stats(b, 7)), after)
        self.assertTrue(any("MISMATCH" in line for line in self.net.log))

    def test_item9_extreme_initial_ratings_do_not_break_a_round(self):
        a, b = self.a, self.b
        self.assertEqual(self.stats(b, 9, 0xFFFFFFFF), MAX_RATING)
        self.assertEqual(self.stats(a, 9, 1), 1)
        rid = self.start(a, 9, [a.pid, b.pid])
        self.ok(call(a, 120, 3, end_param(rid, [(b.pid, LOSS), (a.pid, WIN)])))   # (was: OverflowError half-way)
        self.assertGreater(self.stats(a, 9), 1)
        self.assertLess(self.stats(b, 9), MAX_RATING)

    def test_item5_statistics_bounded_and_only_ones_own(self):
        a, x = self.a, self.x
        self.ok(call(x, 120, 8, target_param(a.pid, 4242)))
        self.assertNotIn(4242, self.ranked.of_pid.get(a.pid, {}))
        for category in range(MAX_CATEGORIES + 5):
            self.stats(x, 1000 + category)
        self.assertEqual(len(self.ranked.of_pid[x.pid]), MAX_CATEGORIES)
        self.start(a, 7, [a.pid, self.b.pid])
        self.ranked.tick(self.server.now + ROUND_LIFETIME + 1)
        self.assertEqual(self.ranked.rounds, {})


class Logs(Case):
    """The log holds no NSA id or token at any --nex-log level (the online lobby's service runs at
    1, where the login line had both the id and the user name - in nex.selftest's logins, the id
    again, in decimal)."""

    def scenario(self, verbose, state_dir):
        """Every way a player's secret went into the log: logging in, logins refused, Ticket
        Granting calls the server does not know or cannot read, secure calls holding the NSA id.
        Returns the log, and whatever went to stdout and stderr meanwhile."""
        self.net = Net(state_dir, verbose)
        self.server = self.net.server
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            c = selftest.login(self.net, ("10.13.0.2", 49152), NSA, NAME)
            auth = selftest.Client(self.net, ("10.13.0.3", 49152), AUTH_PORT)
            auth.connect()
            for token in (TOKEN.upper(), f"ssbu-port.1.{NSA:016X}.{NAME}", f"ssbu-port.1.{NSA:016x}"):
                self.refused(call(auth, 10, 6, login_param(token)), rmc.RENDEZVOUS_INVALID_USERNAME)
            self.refused(call(auth, 10, 2, login_param()), rmc.CORE_NOT_IMPLEMENTED)
            self.refused(call(auth, 10, 6, login_param(corrupt=True)), rmc.CORE_INVALID_ARGUMENT)
            send_raw(auth, struct.pack("<I", 2) + rmc.request(10, 99, 6, login_param())[4:])   # (undecodable)
            self.refused(call(c, 10, 2, login_param()), rmc.CORE_NOT_IMPLEMENTED)   # (to the secure server)
            self.refused(call(c, 110, 99, struct.pack("<Q", NSA) + str(NSA).encode()), rmc.CORE_NOT_IMPLEMENTED)
            self.refused(call(c, 110, 98, b"harmless"), rmc.CORE_NOT_IMPLEMENTED)
            # (an application buffer can hold account data too: it must not reach the log either)
            selftest.auto_matchmake(c, app_buffer=b"rules" + struct.pack("<Q", NSA))
        return "\n".join(self.net.log) + "\n" + out.getvalue()

    def test_logs_hold_no_nsa_id_or_token_at_any_level(self):
        tag = hashlib.sha256(f"{NSA:016x}".encode()).hexdigest()[:8]
        for verbose in (0, 1, 2, 3):
            with self.subTest(verbose=verbose), tempfile.TemporaryDirectory() as tmp:
                log = self.scenario(verbose, tmp)
                for form in secret_forms():
                    self.assertNotIn(form, log)
                if verbose:   # (and the log still says what happened)
                    self.assertIn(f"login: {NAME} (account {tag}, platform 3) -> pid", log)
                    self.assertEqual(log.count("login refused"), 3)
                    self.assertIn("UNHANDLED TicketGranting.ValidateAndRequestTicketWithCustomData", log)
                    self.assertIn("undecodable RMC message", log)
                    self.assertIn("they hold the account's NSA id", log)
                    self.assertIn(b"harmless".hex(" "), log)   # (other calls' parameters are still shown)
                    self.assertIn("AutoMatchmake from", log)
                    self.assertIn("B (not shown)", log)
                requests = os.path.join(tmp, "requests")
                saved = os.listdir(requests) if os.path.isdir(requests) else []
                self.assertEqual([f for f in saved if f.startswith("10-")], [])   # (nor are logins kept)


class LiteLogs(Logs):
    lite = True


class Lobby(unittest.TestCase):
    """The online lobby's NEX streams (TCP, WebSocket), on a real local socket."""

    def test_item7_unmask(self):
        import ssbu_online_lobby as lobby
        mask = b"\x12\x34\x56\x78"
        for n in (0, 1, 3, 4, 5, 1000, 65536):
            data = secrets.token_bytes(n)
            self.assertEqual(lobby.ws_unmask(data, mask), bytes(b ^ mask[i & 3] for i, b in enumerate(data)))

    def test_item7_websocket_pings_count_against_the_rate(self):
        import ssbu_online_lobby as lobby
        import ssbu_relay as relay
        with tempfile.TemporaryDirectory() as tmp:
            with contextlib.redirect_stdout(io.StringIO()):
                pongs, eof, synack, big = asyncio.run(self.flood(lobby, relay, tmp))
        self.assertTrue(synack, "no SYN-ACK through the WebSocket")
        self.assertTrue(big, "a 126-byte ping was answered")
        self.assertTrue(eof, "the stream was not closed")
        self.assertLess(pongs, 1200)   # (was: all of them, without limit)
        self.assertGreater(pongs, 100)

    async def flood(self, lobby, relay, tmp):
        o = lobby.parse_args(["--state", tmp, "--nex-log", "0"])
        lob = lobby.OnlineLobby(o)
        server = await asyncio.start_server(lob.stream, "127.0.0.1", 0)
        port = server.sockets[0].getsockname()[1]
        c = relay.Client(("127.0.0.1", 5), b"n" * 8, relay.SUBNET + 2, "Tester", ("b", "c"), time.monotonic(), o)
        c.verified = True
        lob.by_ip[c.ip] = c

        async def open_ws():
            r, w = await asyncio.open_connection("127.0.0.1", port)
            w.write(lobby.PREFACE + struct.pack(">IHH", c.ip, 5000 + len(lob.streams.get(c.ip, ())), SECURE_PORT))
            w.write(b"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: NEX\r\n\r\n")
            await w.drain()
            assert b" 101 " in await asyncio.wait_for(r.readuntil(b"\r\n\r\n"), 5)
            return r, w

        def frame(opcode, payload):
            mask = secrets.token_bytes(4)
            n = len(payload)
            head = bytes((0x80 | opcode, 0x80 | n)) if n < 126 else bytes((0x80 | opcode, 0x80 | 126)) + struct.pack(">H", n)
            return head + mask + lobby.ws_unmask(payload, mask)

        async def until_eof(r):
            n = 0
            try:
                while True:
                    b0, b1 = await asyncio.wait_for(r.readexactly(2), 5)
                    await r.readexactly(b1 & 0x7F)
                    n += (b0 & 0x0F) == 10
            except (asyncio.IncompleteReadError, ConnectionError):
                return n, True
            except asyncio.TimeoutError:
                return n, False

        # PRUDP Lite through it: a SYN gets its SYN-ACK
        r, w = await open_ws()
        syn = Packet(SYN, F_NEED_ACK | F_HAS_SIZE, src=0x1F, dst=0x01, options={O_SUPPORT: b"\4\0\0\0"})
        w.write(frame(2, encode_lite(syn)))
        await w.drain()
        b0, b1 = await asyncio.wait_for(r.readexactly(2), 5)
        ack = decode_lite(await r.readexactly(b1 & 0x7F))
        synack = ack is not None and ack.type == SYN and bool(ack.flags & F_ACK)
        # a ping larger than a control frame may be: the stream goes
        w.write(frame(9, b"p" * 126))
        await w.drain()
        _n, big = await until_eof(r)
        w.close()
        # pings, as fast as they go: answered until the stream's rate is spent, then it goes
        r, w = await open_ws()
        reading = asyncio.ensure_future(until_eof(r))   # (read while writing: neither side waits on the other)
        try:
            for _ in range(1200):
                w.write(frame(9, b"p" * 125))
                await w.drain()
        except ConnectionError:
            pass
        pongs, eof = await reading
        w.close()
        server.close()
        await server.wait_closed()
        return pongs, eof, synack, big


def run():
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(run())
