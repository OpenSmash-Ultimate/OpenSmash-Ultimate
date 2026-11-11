"""Battle Arenas on the NEX server (nex/arenas.py), with the in-process clients of nex.selftest.
python -m nex.test_arenas   (from tools/; nex.selftest runs it too)"""
import sys
import unittest

from . import rmc
from .arenas import ID_LENGTH, ID_LETTERS
from .matchmaking import NO_GATHERING, Session
from .streams import StreamIn, StreamOut
from .test_hardening import Case, call, join_param, session_param, u32s


def text(s):
    out = StreamOut()
    out.string(s)
    return out.get()


def criteria(game_mode="3", attribs=("1", "", "", "", "", "")):
    """BrowseMatchmakeSessionNoHolderNoResultRange's parameter, as the game sends it (Advanced search)."""
    out = StreamOut()

    def c(o):
        o.list(list(attribs), StreamOut.string)
        o.string(game_mode)
        o.string("")
        o.string("")
        o.string("0")   # (the system type; the game's arenas are 1, session_param's 0)
        o.bool(True)
        o.bool(True)
        o.bool(True)
        o.u32(0)
        o.u16(1)
        o.struct(lambda m: m.u32(0))
        o.write(bytes(4) + bytes.fromhex("0100000000010000000800000000000000320000"))
    out.struct(c)
    return out.get()


def found(m):
    r = StreamIn(m.body)
    return [Session.parse(r).gid for _ in range(r.u32())]


class Arenas(Case):
    def arena(self, c, max_participants=4):
        """An arena as the game makes it: CreateMatchmakeSessionWithParam, then its arena ID (method 55)."""
        g = Session.parse(self.ok(call(c, 109, 38, session_param(max_participants=max_participants))))
        r = StreamIn(self.ok(call(c, 109, 55, u32s(g.gid))).struct()[1])
        return g.gid, r.string(), r.datetime()

    def test_an_arena_gets_an_id(self):
        a = self.player(2, "Alpha")
        gid, code, when = self.arena(a)
        self.assertEqual(len(code), ID_LENGTH)
        self.assertTrue(set(code) <= set(ID_LETTERS))
        self.assertGreater(when, 0)
        # asked again: the same ID
        r = StreamIn(self.ok(call(a, 109, 55, u32s(gid))).struct()[1])
        self.assertEqual(r.string(), code)

    def test_only_its_players_get_an_arena_id(self):
        a, x = self.player(2, "Alpha"), self.player(4, "Mallory")
        gid, _code, _ = self.arena(a)
        self.refused(call(x, 109, 55, u32s(gid)), rmc.RENDEZVOUS_NOT_PARTICIPATED)
        self.refused(call(x, 109, 55, u32s(gid + 99)), NO_GATHERING)

    def test_an_arena_id_finds_its_gathering_while_it_is_there(self):
        a, b = self.player(2, "Alpha"), self.player(3, "Bravo")
        gid, code, _ = self.arena(a)
        self.assertEqual(self.ok(call(b, 109, 56, text(code.lower()))).u32(), gid)
        self.refused(call(b, 109, 56, text("NOPE1")), NO_GATHERING)
        # the arena's last player leaves: the ID names nothing any more
        self.ok(call(a, 50, 1, u32s(gid) + text("")))
        self.assertNotIn(gid, self.mm.sessions)
        self.refused(call(b, 109, 56, text(code)), NO_GATHERING)

    def test_players_join_an_arena(self):
        a, b, c = self.player(2, "Alpha"), self.player(3, "Bravo"), self.player(5, "Charlie")
        gid, _code, _ = self.arena(a, max_participants=3)
        self.ok(call(b, 109, 39, join_param(gid)))
        self.ok(call(c, 109, 39, join_param(gid)))
        self.assertEqual(self.mm.sessions[gid].participants, [a.pid, b.pid, c.pid])
        self.assertTrue(self.server.may_talk(a.ip, c.ip))


    def test_a_search_finds_open_arenas_of_its_own_build(self):
        a, b, c, d = self.player(2, "Alpha"), self.player(3, "Bravo"), self.player(5, "Charlie"), self.player(6, "Delta")
        f = self.player(9, "Foreign")
        ga, _, _ = self.arena(a)
        gc, _, _ = self.arena(c, max_participants=2)
        self.ok(call(d, 109, 39, join_param(gc)))           # (full now)
        gf, _, _ = self.arena(f)                             # (another build or lobby code)
        m = call(b, 109, 52, criteria())
        self.ok(m)
        self.assertEqual(found(m), [ga])
        # not the asker's own arena, and nothing when the criteria fit none
        self.assertEqual(found(call(a, 109, 52, criteria())), [])
        self.assertEqual(found(call(b, 109, 52, criteria(game_mode="7"))), [])
        # what a search shows of an arena: no session key
        r = StreamIn(call(b, 109, 52, criteria()).body)
        r.u32()
        self.assertEqual(Session.parse(r).session_key, b"")
        # a closed arena is not found
        self.ok(call(a, 109, 1, u32s(ga)))
        self.assertEqual(found(call(b, 109, 52, criteria())), [])
        self.assertNotIn(gf, found(call(f, 109, 52, criteria())))


class ArenaRollback(unittest.TestCase):
    """The online lobby passes rollback messages between the players of one Online match (a Battle Arena's ring, a
    Quickplay match) on the public code, as their game traffic; not to anyone else."""

    def test_rollback_messages_between_the_players_of_a_match(self):
        import struct
        import tempfile
        import time
        import ssbu_online_lobby as lobby
        import ssbu_relay as relay
        with tempfile.TemporaryDirectory() as tmp:
            o = lobby.parse_args(["--state", tmp, "--nex-log", "0"])
            lob = lobby.OnlineLobby(o)
            now = time.monotonic()
            a, b, x = (relay.Client(("127.0.0.1", 5 + i), b"n" * 8, relay.SUBNET + 2 + i, n, ("sn", "b", "smash"), now, o)
                       for i, n in enumerate(("A", "B", "X")))
            for c in (a, b, x):
                c.verified, c.code = True, relay.V3_PUBLIC
                lob.by_ip[c.ip] = c
                lob.parts.setdefault(c.part, set()).add(c)
            sent = []
            lob.to = lambda c, data: sent.append(c)
            together = {(a.ip, b.ip), (b.ip, a.ip)}
            lob.nex.may_talk = lambda i, j: (lobby.ip_int(i), lobby.ip_int(j)) in together

            def rb(src, dst):
                sent.clear()
                lob.on_rb(src, bytes(12) + struct.pack(">I", dst.ip) + b"rollback", now)
                return sent[:]
            self.assertEqual(rb(a, b), [b])
            self.assertEqual(rb(b, a), [a])
            self.assertEqual(rb(x, a), [])      # (a stranger)
            self.assertEqual(rb(a, x), [])


def run():
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(run())
