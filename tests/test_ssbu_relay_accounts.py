#!/usr/bin/env python3
"""Tests for the official lobby's rules (tools/ssbu_relay.py --relay-only, --ticket-secret-file,
--min-client-level): lobby tickets and the online level after the JOIN's mod set, and no game ever
introduced to another. Fake players on loopback, no game.

    python -I tests/test_ssbu_relay_accounts.py [-v]"""
import hashlib
import hmac
import os
import struct
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import ssbu_relay as R   # noqa: E402
from test_ssbu_relay import Server   # noqa: E402
from test_ssbu_relay_v3 import NO_MODS, V3Player, mods, until   # noqa: E402

SECRET = b"test-secret-not-a-real-one"


def ticket(uid="00000000-0000-4000-8000-000000000001", name="Alice", exp=None, secret=SECRET):
    body = f"{uid}|{name}|{int(time.time()) + 3600 if exp is None else exp}"
    return (body + "." + hmac.new(secret, body.encode(), hashlib.sha256).hexdigest()).encode()


def blocks(tk=None, cv=None, gc=None):
    out = b""
    if tk is not None:
        out += b"TK" + struct.pack(">H", len(tk)) + tk
    if cv is not None:
        out += b"CV" + struct.pack(">H", cv)
    if gc is not None:
        out += b"GC" + struct.pack(">H", len(gc)) + gc
    return out


class TailPlayer(V3Player):
    def __init__(self, server, tail=b"", **kw):
        super().__init__(server, **kw)
        self.tail = tail

    def join_packet(self):
        return super().join_packet() + self.tail

    def refused(self):
        self.s.sendto(self.join_packet(), self.addr)
        r = self.first(R.REJECT)
        return None if r is None else (r[12], r[13:77].rstrip(b"\0").decode())


class Parsing(unittest.TestCase):
    def test_blocks(self):
        tk = ticket()
        self.assertEqual(R.join_blocks(blocks(tk, 3, b"abc")), {"TK": tk, "CV": 3, "GC": b"abc"})
        self.assertEqual(R.join_blocks(blocks(cv=2)), {"CV": 2})
        self.assertEqual(R.join_blocks(b""), {})
        self.assertEqual(R.join_blocks(b"TK\x00\x50short"), {})   # (cut short: nothing)

    def test_tail_after_mods(self):
        p = TailPlayer.__new__(TailPlayer)
        head = R.header(R.JOIN, R.V3) + bytes(R.JOIN_SIZE - 4)
        m = mods((b"a", 1), (b"bb", 2))
        self.assertEqual(R.join_tail(head + struct.pack(">I", 1) + m + b"XYZ", True), b"XYZ")
        v2 = R.header(R.JOIN) + bytes(R.JOIN_SIZE - 4) + b"WS" + struct.pack(">I", 1) + NO_MODS + b"Q"
        self.assertEqual(R.join_tail(v2, False), b"Q")
        self.assertEqual(R.join_tail(R.header(R.JOIN) + bytes(R.JOIN_SIZE - 4), False), b"")
        del p

    def test_ticket(self):
        self.assertEqual(R.ticket_check(SECRET, ticket()), ("00000000-0000-4000-8000-000000000001", "Alice"))
        self.assertIsNone(R.ticket_check(SECRET, ticket(secret=b"other")))
        self.assertIsNone(R.ticket_check(SECRET, ticket(exp=int(time.time()) - 5)))
        self.assertIsNone(R.ticket_check(SECRET, b"garbage"))
        self.assertIsNone(R.ticket_check(SECRET, None))
        four = "u|Bob|%d|%d" % (time.time() + 60, time.time())   # (SSBU Arena's tickets of version 2: "|issued")
        t4 = (four + "." + hmac.new(SECRET, four.encode(), hashlib.sha256).hexdigest()).encode()
        self.assertEqual(R.ticket_check(SECRET, t4), ("u", "Bob"))


class OfficialLobby(unittest.TestCase):
    def setUp(self):
        self.server = Server(relay_only=True, ticket_secret=SECRET, min_client=2)
        self.players = []

    def tearDown(self):
        for p in self.players:
            p.close()
        self.server.stop()

    def player(self, tail, **kw):
        p = TailPlayer(self.server, tail, **kw)
        self.players.append(p)
        return p

    def test_no_ticket_refused(self):
        self.assertEqual(self.player(blocks(cv=2)).refused(), (R.REJECT_ACCOUNT, R.SIGN_IN))

    def test_forged_ticket_refused(self):
        self.assertEqual(self.player(blocks(ticket(secret=b"x"), 2)).refused(), (R.REJECT_ACCOUNT, R.SIGN_IN))

    def test_old_game_told_to_update(self):
        self.assertEqual(self.player(b"").refused(), (R.REJECT_OLD, R.UPDATE))
        self.assertEqual(self.player(blocks(ticket(), 1)).refused(), (R.REJECT_OLD, R.UPDATE))

    def test_ticket_name_and_relay_only(self):
        a = self.player(blocks(ticket(name="Alice"), 2, b"gc1"), name="Player1234").join()
        b = self.player(blocks(ticket(uid="u2", name="Bob"), 2), name="Player9").join()
        self.assertEqual(a.flags & (R.LOBBY | R.INTRODUCES), R.LOBBY)   # (a lobby that introduces nobody)
        ca, cb = self.server.lobby.by_ip[a.ip], self.server.lobby.by_ip[b.ip]
        self.assertEqual((ca.name, ca.uid, cb.name, cb.uid), ("Alice", "00000000-0000-4000-8000-000000000001", "Bob", "u2"))
        self.assertTrue(until(lambda: not ca.direct_ok and not cb.direct_ok))
        self.assertEqual(self.server.lobby.pair_secret(None, ca, cb, time.monotonic()), 0)


class OpenLobby(unittest.TestCase):
    """Without the new options the lobby is as before: no blocks needed, INTRODUCES on."""
    def test_as_before(self):
        server = Server()
        try:
            p = TailPlayer(server, b"")
            p.join()
            self.assertEqual(p.flags & R.INTRODUCES, R.INTRODUCES)
            p.close()
        finally:
            server.stop()


if __name__ == "__main__":
    unittest.main()
