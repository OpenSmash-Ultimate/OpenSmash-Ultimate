"""The GSP leaderboard and a player's own page over HTTP (nex/board.py, tools/ssbu_online_lobby.py's http):
positions overall and per fighter among registered players (an install linked to a Discord account, verified once
with Discord, that shows its name; one row per Discord account), signed requests, the results a
game with its own NEX server uploads, the board reset tool, and the lobby's TCP port answering HTTP beside NEX's streams.   python -m nex.test_board   (from tools/; nex.selftest runs it too)"""
import asyncio
import json
import os
import shutil
import struct
import sys
import tempfile
import threading
import time
import unittest

from . import reset_board
from .board import LINK_EVERY, MAX_SKEW, account_hash, player_id, signature
from .server import make_server

NSA = {"a": "00000000000000a1", "b": "00000000000000b2", "c": "00000000000000c3"}


def read(path, raw=False):
    with open(path, "rb" if raw else "r", encoding=None if raw else "utf-8") as f:
        return f.read() if raw else json.load(f)


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
        self.discord = {"tok-a": {"id": "1001", "username": "ann", "global_name": "Ann"},
                        "tok-b": {"id": "1002", "username": "bee"}}
        self.board.discord_me = lambda token: self.discord.get(token)
        self.board.profiles.link(self.pid["a"], "1001", "Ann", True, 1.0)     # (a and b are registered, c is not)
        self.board.profiles.link(self.pid["b"], "1002", "Bee", True, 2.0)
        self.da, self.db = account_hash("1001"), account_hash("1002")

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

    def link(self, who, token, show=True, key=None, t=None, sig=None, nsa=None, now=1000.0, raw=None):
        nsa = nsa or NSA[who]
        p, t = player_id(nsa), int(time.time()) if t is None else t
        d = {"player": p, "t": t, "token": token, "show": show,
             "sig": sig or signature(nsa, "link", p, t, token + "\n" + ("1" if show else "0"))}
        if key is not None:
            d["key"] = key
        code, body = self.board.http("POST", "/gsp/link", {}, raw or json.dumps(d).encode(), now)
        return code, json.loads(body)

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
        self.assertEqual([r["id"] for r in rows], [self.db, self.da])
        self.assertEqual(rows[1], {"id": self.da, "rank": 2, "gsp": 1500, "fighter": 43,
                                   "name": "Ann", "discord": self.da, "fighters": {"0": {"gsp": 900, "rank": 1}, "43": {"gsp": 1500, "rank": 2}}})

    def test_no_personal_data_unless_shown(self):
        _, d = self.get("/leaderboard.json")
        text = json.dumps(d)
        for k, nsa in NSA.items():
            self.assertNotIn(nsa, text)
            self.assertNotIn("In-game " + k, text)
            self.assertNotIn(str(self.pid[k]), text)
        self.assertEqual({r["name"] for r in d["players"]}, {"Ann", "Bee"})   # (c: not registered, not listed)

    def test_profile_hides_and_shows_a_linked_player(self):
        code, _ = self.profile("a", "Nelly <b>\n", "ab12")      # (the name sent is ignored: Discord's stays)
        self.assertEqual(code, 200)
        _, d = self.get("/leaderboard.json", now=2000.0)
        a = d["players"][1]
        self.assertEqual((a["name"], a["discord"]), ("Ann", self.da))
        self.assertEqual(self.profile("a", "")[0], 200)   # (hide)
        _, d = self.get("/leaderboard.json", now=3000.0)
        self.assertEqual((len(d["players"]), d["ranked"]), (1, 1))
        self.assertEqual(self.board.profiles.of_pid[str(self.pid["a"])]["discord_id"], "1001")   # (still linked)
        self.assertEqual(self.profile("a", "x")[0], 200)   # (show again)
        self.assertEqual(self.get("/leaderboard.json", now=4000.0)[1]["ranked"], 2)

    def test_profile_hash_alone_does_not_register(self):
        self.assertEqual(self.profile("c", "Cee", "cc33")[0], 200)
        self.assertNotIn(str(self.pid["c"]), self.board.profiles.of_pid)
        self.assertFalse(self.board.registered(self.pid["c"]))
        self.assertEqual(self.get("/leaderboard.json", now=2000.0)[1]["ranked"], 2)
        self.board.profiles.of_pid[str(self.pid["c"])] = {"name": "Cee", "discord": "cc33"}   # (an old entry: unverified)
        self.board.cache = None
        self.assertFalse(self.board.registered(self.pid["c"]))
        self.assertEqual(self.get("/leaderboard.json", now=3000.0)[1]["ranked"], 2)

    def test_link_verifies_once_and_registers(self):
        asked = []
        self.board.discord_me = lambda token: asked.append(token) or {"id": "1003", "username": "cee", "global_name": "Cee <3>"}
        self.nex.services.powers.put(self.pid["c"], record(5, 1700))
        self.assertEqual(self.get("/leaderboard.json", now=2000.0)[1]["ranked"], 2)
        code, d = self.link("c", "tok-c")
        self.assertEqual((code, d), (200, {"ok": True, "name": "Cee <3>", "discord": account_hash("1003")}))
        self.assertEqual(asked, ["tok-c"])
        self.assertNotIn("tok-c", json.dumps(self.board.profiles.of_pid))      # (the token is not kept)
        mine = self.board.profiles.of_pid[str(self.pid["c"])]
        self.assertEqual(mine, {"name": "Cee <3>", "discord": account_hash("1003"), "discord_id": "1003", "show": True, "at": mine["at"]})
        _, board = self.get("/leaderboard.json", now=3000.0)
        self.assertEqual((board["ranked"], board["players"][1]["id"], board["players"][1]["gsp"]), (3, account_hash("1003"), 1700))
        _, me = self.get("/gsp/me", self.me_headers("c"), now=3000.0)
        self.assertEqual((me["id"], me["rank"], me["ranked"]), (account_hash("1003"), 2, 3))

    def test_link_with_show_false_hides(self):
        self.discord["tok-c"] = {"id": "1003", "username": "cee"}
        self.nex.services.powers.put(self.pid["c"], record(5, 1700))
        self.assertEqual(self.link("c", "tok-c", show=False)[0], 200)
        self.assertEqual(self.get("/leaderboard.json", now=2000.0)[1]["ranked"], 2)
        _, me = self.get("/gsp/me", self.me_headers("c"), now=2000.0)
        self.assertEqual((me["rank"], me["gsp"], me["fighters"][0]["gsp"]), (0, 1700, 1700))   # (own GSP, no position)

    def test_link_refused_token_and_discord_down(self):
        code, d = self.link("c", "bad")
        self.assertEqual((code, d), (403, {"error": "discord did not accept the token"}))
        self.assertNotIn(str(self.pid["c"]), self.board.profiles.of_pid)

        def down(token):
            raise OSError("unreachable")
        self.board.discord_me = down
        self.assertEqual(self.link("c", "tok-c", now=1100.0)[0], 502)
        self.board.discord_me = lambda token: {"id": "not a number"}
        self.assertEqual(self.link("c", "tok-c", now=1200.0)[0], 403)
        self.assertNotIn(str(self.pid["c"]), self.board.profiles.of_pid)

    def test_link_rate_limit(self):
        self.assertEqual(self.link("c", "bad")[0], 403)
        self.assertEqual(self.link("c", "tok-a", now=1000.0 + LINK_EVERY - 1)[0], 429)
        self.assertEqual(self.link("c", "tok-a", now=1000.0 + LINK_EVERY + 1)[0], 200)
        self.assertEqual(self.link("b", "tok-b", now=1000.0)[0], 200)      # (another player: not limited)

    def test_link_signature_and_body(self):
        p, t = player_id(NSA["c"]), int(time.time())
        good = signature(NSA["c"], "link", p, t, "tok-a\n1")
        self.assertEqual(self.link("c", "tok-a", show=False, t=t, sig=good)[0], 403)       # (show is signed)
        self.assertEqual(self.link("c", "tok-b", t=t, sig=good)[0], 403)                  # (so is the token)
        self.assertEqual(self.link("c", "tok-a", t=t - MAX_SKEW - 5)[0], 403)
        self.assertEqual(self.link("c", "tok-a", sig=signature(NSA["c"], "profile", p, t, "tok-a\n1"), t=t)[0], 403)
        self.assertEqual(self.link("c", "tok-a", raw=b"{no")[0], 400)
        self.assertEqual(self.link("c", "tok-a", raw=b"[]")[0], 400)
        self.assertEqual(self.link("c", "tok a")[0], 400)
        self.assertEqual(self.link("c", "x" * 600)[0], 400)
        self.assertEqual(self.link("c", "tok-a", raw=b" " * 5000)[0], 413)
        self.assertEqual(self.board.http("POST", "/gsp/link", {}, json.dumps(
            {"player": p, "t": t, "token": "tok-a", "show": 1, "sig": good}).encode(), 0.0)[0], 400)    # (show must be a bool)
        self.assertEqual(self.board.http("GET", "/gsp/link", {}, b"", 0.0)[0], 405)
        self.assertNotIn(str(self.pid["c"]), self.board.profiles.of_pid)

    def test_link_registers_an_unknown_player_with_a_key(self):
        new = "00000000000000e5"
        NSA["e"] = new
        try:
            self.discord["tok-e"] = {"id": "1005", "username": "eee"}
            self.assertEqual(self.link("e", "tok-e")[0], 403)
            self.assertEqual(self.link("e", "tok-e", key="00000000000000e6")[0], 403)
            self.assertNotIn(new, self.nex.accounts.data["users"])
            self.assertEqual(self.link("e", "tok-e", key=new)[0], 200)
            pid = self.nex.accounts.data["users"][new]["pid"]
            self.assertEqual(self.board.profiles.discord_id(pid), "1005")
        finally:
            del NSA["e"]

    def test_two_installs_one_player(self):
        NSA["d"] = "00000000000000d4"
        try:
            pid_d = self.nex.accounts.login(NSA["d"], "D", 0.0)
            self.nex.services.powers.put(pid_d, record(43, 2600))     # (more than a's 1500 on 43)
            self.nex.services.powers.put(pid_d, record(7, 1200))
            self.board.index(0.0, force=True)
            self.discord["tok-d"] = {"id": "1001", "username": "ann", "global_name": "Ann again"}   # (a's Discord account)
            self.assertEqual(self.link("d", "tok-d")[0], 200)
            _, d = self.get("/leaderboard.json", now=2000.0)
            self.assertEqual((d["ranked"], [r["id"] for r in d["players"]]), (2, [self.da, self.db]))
            ann = d["players"][0]
            self.assertEqual((ann["name"], ann["gsp"], ann["fighter"]), ("Ann again", 2600, 43))   # (the latest link's name)
            self.assertEqual({f: v["gsp"] for f, v in ann["fighters"].items()}, {"0": 900, "7": 1200, "43": 2600})
            self.assertEqual(d["players"][1]["fighters"]["43"], {"gsp": 2000, "rank": 2})
            for who in ("a", "d"):    # (both installs see the merged GSP and the same position)
                _, me = self.get("/gsp/me", self.me_headers(who), now=2000.0)
                self.assertEqual((me["rank"], me["ranked"], me["id"]), (1, 2, self.da))
                self.assertEqual({f["fighter"]: f["gsp"] for f in me["fighters"]}, {0: 900, 7: 1200, 43: 2600})
            self.assertEqual(self.board.records(self.pid["a"]), {0: 900, 7: 1200, 43: 2600})
            self.assertEqual(self.get("/status.json", now=2000.0)[1]["ranked"], 2)
            # nothing was copied: a's own store is unchanged, and the sync still stores under the pid
            self.assertEqual(sorted(f for f, _ in self.nex.services.powers.records(self.pid["a"])), [0, 43])
            self.assertEqual(self.sync("d", self.payload([record(43, 2700)]))[0], 200)
            self.assertEqual(self.board.records(self.pid["a"])[43], 2700)
            self.assertEqual(self.board.records(self.nex.accounts.data["users"][NSA["d"]]["pid"])[43], 2700)
        finally:
            del NSA["d"]

    def test_new_install_gets_the_records_back_after_linking(self):
        NSA["d"] = "00000000000000d4"
        try:
            self.nex.accounts.login(NSA["d"], "D", 0.0)
            self.board.index(0.0, force=True)
            self.assertEqual(self.get("/gsp/records", self.records_headers("d"))[1], {"records": []})
            self.discord["tok-d"] = {"id": "1001", "username": "ann"}
            self.assertEqual(self.link("d", "tok-d")[0], 200)
            code, mine = self.get("/gsp/records", self.records_headers("d"))
            self.assertEqual(code, 200)
            self.assertEqual(sorted(mine["records"]), sorted(r.hex() for r in (record(0, 900), record(43, 1500))))
        finally:
            del NSA["d"]

    def test_unlinked_players_are_not_on_the_board(self):
        self.nex.services.powers.put(self.pid["c"], record(5, 9999))
        _, d = self.get("/leaderboard.json", now=2000.0)
        self.assertEqual((d["ranked"], len(d["players"])), (2, 2))
        self.assertNotIn("9999", json.dumps(d))
        _, me = self.get("/gsp/me", self.me_headers("c"), now=2000.0)
        self.assertEqual((me["rank"], me["gsp"]), (0, 9999))

    def test_unregistered_player_is_not_ranked(self):
        self.board.profiles.set_show(self.pid["b"], False, 5.0)    # b no longer shows
        self.board.cache = None
        code, d = self.get("/leaderboard.json")
        self.assertEqual([r["id"] for r in d["players"]], [self.da])
        self.assertEqual(d["players"][0]["rank"], 1)
        _, me = self.get("/gsp/me", self.me_headers("b"))
        self.assertEqual((me["rank"], me["ranked"], me["gsp"]), (0, 1, 2000))
        self.assertEqual([f["rank"] for f in me["fighters"]], [0])
        self.assertEqual(self.get("/status.json")[1]["ranked"], 1)

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
                         (self.da, 2, 2, 0, 900, None))
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

    # ---- /gsp/sync and /gsp/records ----------------------------------------------------------------
    def sync(self, who, data, key=None, t=None, sig=None, nsa=None, now=1000.0):
        nsa = nsa or NSA[who]
        p, t = player_id(nsa), int(time.time()) if t is None else t
        d = {"player": p, "t": t, "data": data, "sig": sig or signature(nsa, "sync", p, t, data)}
        if key is not None:
            d["key"] = key
        code, body = self.board.http("POST", "/gsp/sync", {}, json.dumps(d).encode(), now)
        return code, json.loads(body)

    @staticmethod
    def payload(records=(), ranked=()):
        return json.dumps({"records": [r.hex() for r in records], "ranked": list(ranked)})

    def test_sync_stores_records_and_ratings(self):
        data = self.payload([record(5, 1700), record(43, 2500)],
                            [{"category": 7, "rating": 1650, "win": 3, "loss": 2, "draw": 1, "disconnect": 0},
                             {"category": 8, "rating": 99999999, "win": 1, "loss": 0, "draw": 0, "disconnect": 4}])
        code, d = self.sync("a", data)
        self.assertEqual((code, d), (200, {"ok": True, "records": 2, "ranked": 2}))
        self.assertEqual(self.board.records(self.pid["a"]), {0: 900, 5: 1700, 43: 2500})   # (newest wins per fighter)
        e = self.nex.ranked.entry(self.pid["a"], 7)
        self.assertEqual((e["ratingValue"], e["totalWin"], e["totalLoss"], e["totalDraw"], e["totalDisconnect"]),
                         (1650, 3, 2, 1, 0))
        self.assertTrue(self.nex.ranked.dirty)
        from .ranked import MAX_RATING
        self.assertEqual(self.nex.ranked.entry(self.pid["a"], 8)["ratingValue"], MAX_RATING)
        _, board = self.get("/leaderboard.json", now=5000.0)    # (the board cache was dropped)
        self.assertEqual(board["players"][0]["gsp"], 2500)
        code, mine = self.get("/gsp/records", self.records_headers("a"))
        self.assertEqual(code, 200)
        self.assertEqual(sorted(mine["records"]), sorted(r.hex() for r in (record(0, 900), record(5, 1700), record(43, 2500))))

    def records_headers(self, who):
        p, t, sig = self.signed(who, "records")
        return {"x-opensmash-player": p, "x-opensmash-time": str(t), "x-opensmash-sig": sig}

    def test_records_signed_only(self):
        self.assertEqual(self.get("/gsp/records", {})[0], 403)
        h = self.me_headers("a")      # (signed as "me": not valid for "records")
        self.assertEqual(self.get("/gsp/records", h)[0], 403)
        self.assertEqual(self.board.http("POST", "/gsp/records", {}, b"", 0.0)[0], 405)
        self.assertEqual(self.board.http("GET", "/gsp/sync", {}, b"", 0.0)[0], 405)

    def test_sync_unknown_player_and_registration(self):
        new = "00000000000000e5"
        NSA["e"] = new
        try:
            data = self.payload([record(9, 1800)])
            code, d = self.sync("e", data)
            self.assertEqual((code, d), (403, {"error": "unknown player"}))
            code, d = self.sync("e", data, key=new.upper())      # (not lowercase)
            self.assertEqual(code, 403)
            code, d = self.sync("e", data, key="00000000000000e6")   # (not that player's key)
            self.assertEqual(code, 403)
            self.assertNotIn(new, self.nex.accounts.data["users"])
            code, d = self.sync("e", data, key=new, sig="0" * 64)    # (bad signature)
            self.assertEqual(code, 403)
            code, d = self.sync("e", data, key=new, now=2000.0)
            self.assertEqual((code, d["records"]), (200, 1))
            pid = self.nex.accounts.data["users"][new]["pid"]
            self.assertEqual(self.board.records(pid), {9: 1800})
            self.assertEqual(self.sync("e", self.payload())[0], 200)   # (now known: no key needed)
        finally:
            del NSA["e"]

    def test_sync_registration_rate_limit(self):
        self.nex.accounts.new_accounts.take = lambda now: False
        new = "00000000000000f7"
        NSA["f"] = new
        try:
            code, d = self.sync("f", self.payload(), key=new)
            self.assertEqual(code, 503)
        finally:
            del NSA["f"]

    def test_sync_refuses_bad_data(self):
        good = record(1, 1000).hex()
        row = {"category": 1, "rating": 1, "win": 0, "loss": 0, "draw": 0, "disconnect": 0}
        for data in ("not json", "[]", json.dumps({"records": "x"}), json.dumps({"records": [good[:-2]]}),
                     json.dumps({"records": ["zz" * 25]}), json.dumps({"records": [good] * 129}),
                     json.dumps({"ranked": [dict(row, win=-1)]}), json.dumps({"ranked": [dict(row, rating=1.5)]}),
                     json.dumps({"ranked": [dict(row, loss=True)]}), json.dumps({"ranked": [{"category": 1}]}),
                     json.dumps({"ranked": [row] * 129}), json.dumps({"ranked": [5]})):
            before = self.board.records(self.pid["a"])
            self.assertEqual(self.sync("a", data)[0], 400, data[:40])
            self.assertEqual(self.board.records(self.pid["a"]), before)
        code, _ = self.board.http("POST", "/gsp/sync", {}, b"{no", 0.0)
        self.assertEqual(code, 400)
        code, _ = self.board.http("POST", "/gsp/sync", {}, json.dumps({"player": "x", "data": 5}).encode(), 0.0)
        self.assertEqual(code, 400)
        full = self.payload([record(1, 1000)] * 128)
        self.assertEqual(self.sync("a", full)[0], 200)
        big = b" " * (64 * 1024 + 1)
        self.assertEqual(self.board.http("POST", "/gsp/sync", {}, big, 0.0)[0], 413)

    def test_sync_signature_covers_the_data(self):
        p, t = player_id(NSA["a"]), int(time.time())
        sig = signature(NSA["a"], "sync", p, t, self.payload([record(1, 100)]))
        self.assertEqual(self.sync("a", self.payload([record(1, 9999)]), t=t, sig=sig)[0], 403)
        self.assertEqual(self.sync("a", self.payload(), t=t - MAX_SKEW - 5)[0], 403)
        self.assertEqual(self.sync("b", self.payload(), sig=signature(NSA["a"], "sync", player_id(NSA["b"]), t, self.payload()), t=t)[0], 403)

    def test_profile_with_a_key_makes_an_account_but_no_registration(self):
        new = "00000000000000e5"
        p, t = player_id(new), int(time.time())
        d = {"player": p, "t": t, "name": "New", "discord": "ee55", "sig": signature(new, "profile", p, t, "New\nee55")}
        self.assertEqual(self.board.http("POST", "/gsp/profile", {}, json.dumps(d).encode(), 0.0)[0], 403)
        d["key"] = new
        self.assertEqual(self.board.http("POST", "/gsp/profile", {}, json.dumps(d).encode(), 0.0)[0], 200)
        pid = self.nex.accounts.data["users"][new]["pid"]
        self.assertFalse(self.board.registered(pid))
        self.assertNotIn(str(pid), self.board.profiles.of_pid)

    def test_links_kept(self):
        self.link("c", "tok-a")
        self.nex.flush(0.0, force=True)
        again = make_server(lambda *a: None, state_dir=self.dir.name, verbose=0, log=lambda *a: None)
        self.assertEqual(again.board.profiles.discord_id(self.pid["c"]), "1001")
        self.assertEqual(sorted(again.board.profiles.linked_pids(self.pid["a"])), sorted([self.pid["a"], self.pid["c"]]))

    # ---- the reset tool ---------------------------------------------------------------------------
    def test_reset_board(self):
        self.nex.ranked.entry(self.pid["a"], 3)
        self.nex.flush(0.0, force=True)
        d = self.dir.name
        said = []
        self.assertIsNone(reset_board.reset(d, say=said.append))      # (without --yes: nothing done)
        self.assertEqual([n for n in os.listdir(d) if n.startswith("backup")], [])
        self.assertTrue(read(os.path.join(d, "nex_powers.json")))
        accounts = read(os.path.join(d, "nex_accounts.json"), raw=True)
        folder = reset_board.reset(d, yes=True, now=0, say=said.append)
        self.assertEqual(os.path.basename(folder), "backup-board-19700101-000000")
        self.assertEqual(sorted(os.listdir(folder)), ["nex_powers.json", "nex_profiles.json", "nex_ratings.json"])
        self.assertTrue(read(os.path.join(folder, "nex_powers.json")))
        self.assertEqual(read(os.path.join(d, "nex_powers.json")), {})
        self.assertEqual(read(os.path.join(d, "nex_ratings.json")), {})
        self.assertEqual(set(read(os.path.join(d, "nex_profiles.json"))), {str(self.pid["a"]), str(self.pid["b"])})
        self.assertEqual(read(os.path.join(d, "nex_accounts.json"), raw=True), accounts)
        self.assertFalse([n for n in os.listdir(d) if n.endswith(".tmp")])
        again = make_server(lambda *a: None, state_dir=d, verbose=0, log=lambda *a: None)
        self.assertEqual(json.loads(again.board.leaderboard(0.0))["players"], [])
        reset_board.reset(d, profiles=True, yes=True, now=5, say=said.append)
        self.assertEqual(read(os.path.join(d, "nex_profiles.json")), {})
        self.assertEqual(reset_board.main(["--state-dir", d]), 0)

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
            pid = lobby.nex.accounts.data["users"][NSA["a"]]["pid"]
            lobby.nex.services.powers.put(pid, record(1, 1234))
            lobby.nex.board.profiles.link(pid, "1001", "Ann", True, 1.0)
            lobby.nex.accounts.login(NSA["b"], "B", 0.0)
            seen = []

            def me(token):      # (Discord, slowly: the lobby must keep answering meanwhile)
                seen.append(threading.get_ident())
                time.sleep(0.5)
                return {"id": "1002", "username": "bee"}
            lobby.nex.board.discord_me = me

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
                body = json.dumps({"player": p, "t": t, "name": "Ann", "discord": "aa11",
                                   "sig": signature(NSA["a"], "profile", p, t, "Ann\naa11")}).encode()
                posted = await ask(b"POST /gsp/profile HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
                pb = player_id(NSA["b"])
                lbody = json.dumps({"player": pb, "t": t, "token": "tok", "show": True,
                                    "sig": signature(NSA["b"], "link", pb, t, "tok\n1")}).encode()
                linking = asyncio.ensure_future(ask(b"POST /gsp/link HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % len(lbody) + lbody))
                await asyncio.sleep(0.15)
                started = time.monotonic()
                during = await ask(b"GET /status.json HTTP/1.1\r\nHost: x\r\n\r\n")     # (answered while Discord is asked)
                quick = time.monotonic() - started
                linked = await linking
                big = await ask(b"POST /gsp/sync HTTP/1.1\r\nContent-Length: 999999\r\n\r\n")
                server.close()
                await server.wait_closed()
                return got, posted, big, during, quick, linked
            got, posted, big, during, quick, linked = asyncio.run(scenario())
            self.assertTrue(during.startswith(b"HTTP/1.0 200"), during)
            self.assertLess(quick, 0.3)
            self.assertTrue(linked.startswith(b"HTTP/1.0 200"), linked)
            self.assertIn(b'"discord"', linked)
            self.assertEqual(len(seen), 1)
            self.assertNotEqual(seen[0], threading.get_ident())    # (off the event loop's thread)
            head, _, body = got.partition(b"\r\n\r\n")
            self.assertTrue(head.startswith(b"HTTP/1.0 200"), head)
            self.assertIn(b"Access-Control-Allow-Origin: *", head)
            self.assertEqual(json.loads(body)["players"][0]["gsp"], 1234)
            self.assertTrue(posted.startswith(b"HTTP/1.0 200"), posted)
            self.assertTrue(big.startswith(b"HTTP/1.0 413"), big)   # (a body over HTTP_BODY: refused, not read)
            lobby.close_ports()


def run():
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(run())
