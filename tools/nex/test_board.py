"""The GSP leaderboard and a player's own page over HTTP (nex/board.py, tools/ssbu_online_lobby.py's http):
positions overall and per fighter, names only for players who chose to show one, signed requests, and the lobby's TCP
port answering HTTP beside NEX's streams.   python -m nex.test_board   (from tools/; nex.selftest runs it too)"""
import asyncio
import json
import os
import struct
import sys
import tempfile
import time
import unittest

from .board import MAX_SKEW, player_id, signature
from .server import make_server

NSA = {"a": "00000000000000a1", "b": "00000000000000b2", "c": "00000000000000c3"}


def record(fighter, gsp):
    return struct.pack("<BIIIQI", fighter, gsp, 123, gsp, 0, 1)


class Board(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.nex = make_server(lambda *a: None, state_dir=self.dir.name, verbose=0, log=lambda *a: None)
        self.pid = {k: self.nex.accounts.login(nsa, "In-game " + k, 0.0) for k, nsa in NSA.items()}
        put = self.nex.services.powers.put
        put(self.pid["a"], record(43, 1500))
        put(self.pid["a"], record(0, 900))
        put(self.pid["b"], record(43, 2000))
        put(self.pid["c"], record(5, 0))      # (a GSP of 0: not ranked)
        self.board = self.nex.board

    def get(self, path, headers=None, now=1000.0):
        code, body = self.board.http("GET", path, headers or {}, b"", now)
        return code, json.loads(body)

    def signed(self, who, what, payload="", t=None):
        t = int(time.time()) if t is None else t
        p = player_id(NSA[who])
        return p, t, signature(NSA[who], what, p, t, payload)

    def me_headers(self, who, **kw):
        p, t, sig = self.signed(who, "me", **kw)
        return {"x-opensmash-player": p, "x-opensmash-time": str(t), "x-opensmash-sig": sig}

    def profile(self, who, name, discord="", t=None, sig=None):
        p, t, s = self.signed(who, "profile", f"{name}\n{discord}", t)
        body = json.dumps({"player": p, "t": t, "name": name, "discord": discord, "sig": sig or s}).encode()
        return self.board.http("POST", "/gsp/profile", {}, body, 1000.0)

    def test_status_counts_only(self):
        from .matchmaking import Session
        code, d = self.get("/status.json")
        self.assertEqual(code, 200)
        self.assertEqual((d["online"], d["rooms"], d["arenas"], d["open_rooms"], d["playing"], d["ranked"]),
                         (0, 0, 0, 0, 0, 2))
        mm = self.nex.matchmaking
        full, half, empty = Session(), Session(), Session()
        for gid, ms, who in ((1, full, ("a", "b")), (2, half, ("c",)), (3, empty, ())):
            ms.gid, ms.max_participants = gid, 2
            ms.participants = [self.pid[w] for w in who]
            mm.sessions[gid] = ms
        self.nex.arenas.id_of[2] = "ABCD"
        self.nex.by_pid[self.pid["a"]] = object()
        self.board.cache = None
        code, d = self.get("/status.json")
        self.assertEqual((d["online"], d["rooms"], d["arenas"], d["open_rooms"], d["playing"]), (1, 2, 1, 1, 2))
        self.assertEqual(set(d), {"updated", "online", "rooms", "arenas", "open_rooms", "playing", "ranked"})
        self.assertEqual(self.board.http("POST", "/status.json", {}, b"", 1000.0)[0], 405)

    def test_status_counts_every_lobby_game(self):
        class Lobby:   # (ssbu_online_lobby.py sets board.lobby: every connected game, its Local Wireless rooms)
            by_addr, pending, listings = {1: 0, 2: 0, 3: 0, 4: 0}, 1, {"room": 0}
        self.board.lobby = Lobby()
        self.board.cache = None
        code, d = self.get("/status.json")
        self.assertEqual((d["online"], d["rooms"], d["open_rooms"]), (3, 1, 1))

    def test_leaderboard_positions(self):
        code, d = self.get("/leaderboard.json")
        self.assertEqual(code, 200)
        self.assertEqual(d["ranked"], 2)
        rows = d["players"]
        self.assertEqual([r["id"] for r in rows], [player_id(NSA["b"]), player_id(NSA["a"])])
        self.assertEqual(rows[1], {"id": player_id(NSA["a"]), "rank": 2, "gsp": 1500, "fighter": 43,
                                   "fighters": {"0": {"gsp": 900, "rank": 1}, "43": {"gsp": 1500, "rank": 2}}})

    def test_no_personal_data_unless_shown(self):
        _, d = self.get("/leaderboard.json")
        text = json.dumps(d)
        for k, nsa in NSA.items():
            self.assertNotIn(nsa, text)
            self.assertNotIn("In-game " + k, text)
            self.assertNotIn(str(self.pid[k]), text)
        self.assertNotIn("name", d["players"][0])

    def test_profile_shows_and_hides_a_name(self):
        code, _ = self.profile("a", "Nelly <b>\n", "ab12")
        self.assertEqual(code, 200)
        _, d = self.get("/leaderboard.json", now=2000.0)
        a = d["players"][1]
        self.assertEqual((a["name"], a["discord"]), ("Nelly <b>", "ab12"))
        self.assertEqual(self.profile("a", "")[0], 200)
        _, d = self.get("/leaderboard.json", now=3000.0)
        self.assertNotIn("name", d["players"][1])

    def test_profile_refused_when_not_signed_by_the_player(self):
        p, t, _ = self.signed("a", "profile", "x\n")
        forged = signature(NSA["b"], "profile", p, t, "x\n")   # (another player's key)
        self.assertEqual(self.profile("a", "x", sig=forged)[0], 403)
        self.assertEqual(self.profile("a", "x", t=int(time.time()) - MAX_SKEW - 5)[0], 403)
        code, _ = self.board.http("POST", "/gsp/profile", {}, b"{not json", 0.0)
        self.assertEqual(code, 400)
        p, t, s = self.signed("a", "profile", "other\n")
        body = json.dumps({"player": p, "t": t, "name": "x", "discord": "", "sig": s}).encode()   # (signed another name)
        self.assertEqual(self.board.http("POST", "/gsp/profile", {}, body, 0.0)[0], 403)
        self.assertEqual(self.board.http("POST", "/gsp/profile", {}, b'{"player":"ffff","t":1,"name":"x","sig":"00"}', 0.0)[0], 403)

    def test_me(self):
        code, d = self.get("/gsp/me", self.me_headers("a"))
        self.assertEqual(code, 200)
        self.assertEqual((d["id"], d["rank"], d["ranked"], d["fighter"], d["gsp"], d["where"]),
                         (player_id(NSA["a"]), 2, 2, 0, 900, None))
        self.assertEqual(d["fighters"], [{"fighter": 43, "gsp": 1500, "rank": 2}, {"fighter": 0, "gsp": 900, "rank": 1}])
        self.assertEqual(self.get("/gsp/me", {})[0], 403)
        h = self.me_headers("a")
        h["x-opensmash-sig"] = "0" * 64
        self.assertEqual(self.get("/gsp/me", h)[0], 403)

    def test_me_where(self):
        from .matchmaking import Session
        ms = Session()
        ms.gid, ms.max_participants, ms.participants = 77, 4, [self.pid["a"], self.pid["b"]]
        self.nex.matchmaking.sessions[77] = ms
        self.assertEqual(self.get("/gsp/me", self.me_headers("a"))[1]["where"],
                         {"mode": "quickplay", "arena": "", "players": 2, "max": 4, "open": True})
        code = self.nex.arenas.new_id(77)
        self.assertEqual(self.get("/gsp/me", self.me_headers("b"))[1]["where"]["arena"], code)

    def test_a_new_account_is_found(self):
        self.nex.accounts.login("00000000000000d4", "D", 0.0)
        NSA["d"] = "00000000000000d4"
        try:
            self.board.indexed_at = None
            self.assertEqual(self.get("/gsp/me", self.me_headers("d"))[1]["rank"], 0)
        finally:
            del NSA["d"]

    def test_profiles_kept(self):
        self.profile("b", "Bee")
        self.nex.flush(0.0, force=True)
        again = make_server(lambda *a: None, state_dir=self.dir.name, verbose=0, log=lambda *a: None)
        self.assertEqual(again.board.profiles.of_pid[str(self.pid["b"])]["name"], "Bee")

    def test_unknown_paths(self):
        self.assertEqual(self.board.http("GET", "/nope", {}, b"", 0.0)[0], 404)
        self.assertEqual(self.board.http("POST", "/leaderboard.json", {}, b"", 0.0)[0], 405)


class OverTheLobby(unittest.TestCase):
    """The online lobby's TCP port: HTTP beside NEX's streams."""

    def test_http_on_the_stream_port(self):
        sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        import ssbu_online_lobby as lobby_mod
        with tempfile.TemporaryDirectory() as d:
            o = lobby_mod.parse_args(["--port", "0", "--state", d, "--nex-log", "0"])
            lobby = lobby_mod.OnlineLobby(o)
            lobby.nex.accounts.login(NSA["a"], "A", 0.0)
            lobby.nex.services.powers.put(lobby.nex.accounts.data["users"][NSA["a"]]["pid"], record(1, 1234))

            async def scenario():
                server = await asyncio.start_server(lobby.stream, "127.0.0.1", 0)
                port = server.sockets[0].getsockname()[1]

                async def ask(raw):
                    r, w = await asyncio.open_connection("127.0.0.1", port)
                    w.write(raw)
                    await w.drain()
                    data = await asyncio.wait_for(r.read(), 5)
                    w.close()
                    return data
                got = await ask(b"GET /leaderboard.json HTTP/1.1\r\nHost: x\r\n\r\n")
                p = player_id(NSA["a"])
                t = int(time.time())
                body = json.dumps({"player": p, "t": t, "name": "Ann", "discord": "",
                                   "sig": signature(NSA["a"], "profile", p, t, "Ann\n")}).encode()
                posted = await ask(b"POST /gsp/profile HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
                big = await ask(b"POST /gsp/profile HTTP/1.1\r\nContent-Length: 999999\r\n\r\n")
                server.close()
                await server.wait_closed()
                return got, posted, big
            got, posted, big = asyncio.run(scenario())
            head, _, body = got.partition(b"\r\n\r\n")
            self.assertTrue(head.startswith(b"HTTP/1.0 200"), head)
            self.assertIn(b"Access-Control-Allow-Origin: *", head)
            self.assertEqual(json.loads(body)["players"][0]["gsp"], 1234)
            self.assertTrue(posted.startswith(b"HTTP/1.0 200"), posted)
            self.assertEqual(big, b"")   # (a body over HTTP_BODY: closed)
            lobby.close_ports()


def run():
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(run())
