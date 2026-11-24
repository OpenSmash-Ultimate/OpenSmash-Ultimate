"""tools/weblauncher/discord_account.py: the Discord sign-in the web launcher shares with the game and OpenSmash.exe
(config/discord_token.txt), checked without Discord: the token file, the application id file, the PKCE sign-in
against a made-up browser and token endpoint, a refresh, the GSP from the lobbies' game server (signed as
the lobby's board.py checks), the choice to show the name on the leaderboard, and the position read from
leaderboard.json."""
import base64
import hashlib
import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'weblauncher'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import discord_account as da  # noqa: E402
from netproto import HAVE_LOBBY, NO_LOBBY  # noqa: E402
if HAVE_LOBBY:   # (the game server's own checks, from the lobby server's checkout)
    from nex import board  # noqa: E402

APP = "123456789012345678"


class TokenFile(unittest.TestCase):
    def test_round_trip_and_sign_out(self):
        with tempfile.TemporaryDirectory() as root:
            t = {"access_token": "a.b", "refresh_token": "r=1", "expires_at": 1900000000, "scope": "identify",
                 "user_id": "80351110224678912", "username": "nelly", "display_name": "Nelly", "avatar": "abc"}
            da.save(root, t)
            self.assertEqual(da.load(root), t)
            with open(da.token_path(root), encoding='utf-8') as f:
                text = f.read()
            self.assertTrue(text.startswith('# OpenSmash'))
            self.assertIn('refresh_token=r=1\n', text)
            da.sign_out(root)
            self.assertIsNone(da.load(root))

    def test_the_games_file_reads(self):
        """(the format runtime/net_discord_core.c writes, CRLF and spaces too)"""
        t = da.parse("# c\r\n access_token = x \r\nexpires_at=12\r\nother=1\r\n")
        self.assertEqual((t["access_token"], t["expires_at"]), ("x", 12))
        self.assertIsNone(da.parse("# nothing\n"))
        with self.assertRaises(ValueError):
            da.format_token({"access_token": "a\nb"})

    def test_fresh(self):
        self.assertTrue(da.fresh({"access_token": "a", "expires_at": 1000}, 800, 120))
        self.assertFalse(da.fresh({"access_token": "a", "expires_at": 1000}, 900, 120))
        self.assertFalse(da.fresh(None, 0, 0))

    def test_app_id(self):
        self.assertEqual(da.parse_app_id("# x\nApplicationId=" + APP + "\n"), APP)
        self.assertEqual(da.parse_app_id(APP), APP)
        self.assertEqual(da.parse_app_id("ClientSecret=" + APP), "")
        self.assertEqual(da.parse_app_id("ApplicationId=12"), "")
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(da.app_id(d), "")
            with open(os.path.join(d, "discord.txt"), "w") as f:
                f.write("ApplicationId=" + APP + "\n")
            self.assertEqual(da.app_id(d), APP)


class SignIn(unittest.TestCase):
    def test_pkce_round_trip(self):
        posted = []

        def post(url, form):
            posted.append((url, form))
            return {"access_token": "AT", "refresh_token": "RT", "expires_in": 600, "scope": "identify"}

        def user(t):
            t.update(user_id="42", username="u", display_name="U", avatar="")

        def listen():   # (a free port instead of 80)
            s = socket.socket()
            s.bind(("127.0.0.1", 0))
            s.listen(4)
            return s, f"http://127.0.0.1:{s.getsockname()[1]}/callback"

        def browser(url):   # (Discord: the user agrees; the browser comes back with the code)
            q = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(url).query))
            self.assertEqual((q["client_id"], q["code_challenge_method"], q["response_type"]), (APP, "S256", "code"))
            seen["challenge"], seen["redirect"] = q["code_challenge"], q["redirect_uri"]

            def go():
                time.sleep(0.2)
                for state in ("wrong", q["state"]):
                    try:
                        urllib.request.urlopen(f"{q['redirect_uri']}?code=CODE&state={state}", timeout=5).read()
                    except urllib.error.HTTPError as e:
                        self.assertEqual(e.code, 400)   # (another state: refused, still waiting)
            threading.Thread(target=go, daemon=True).start()
            return True

        seen = {}
        with tempfile.TemporaryDirectory() as root:
            t = da.sign_in(root, APP, open_url=browser, post=post, user=user, timeout=20, listen=listen)
            self.assertEqual((t["access_token"], t["user_id"]), ("AT", "42"))
            self.assertEqual(da.load(root)["refresh_token"], "RT")
        (url, form), = posted
        self.assertEqual(url, da.API + "/oauth2/token")
        self.assertEqual((form["code"], form["grant_type"], form["redirect_uri"], form["client_id"]),
                         ("CODE", "authorization_code", seen["redirect"], APP))
        self.assertNotIn("client_secret", form)
        want = base64.urlsafe_b64encode(hashlib.sha256(form["code_verifier"].encode()).digest()).rstrip(b"=").decode()
        self.assertEqual(want, seen["challenge"])

    def test_no_application_no_sign_in(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaises(da.SignInError):
                da.sign_in(root, "", listen=lambda: self.fail("listened"))

    def test_refresh(self):
        with tempfile.TemporaryDirectory() as root:
            da.save(root, {"access_token": "old", "refresh_token": "R1", "expires_at": 10})
            t = da.current(root, APP, post=lambda url, form: {"access_token": "new", "refresh_token": "R2", "expires_in": 3600}
                           if form["refresh_token"] == "R1" else None, user=lambda t: None)
            self.assertEqual((t["access_token"], da.load(root)["refresh_token"]), ("new", "R2"))

            def refused(url, form):
                raise da.SignInError("no")
            da.save(root, {"access_token": "old", "refresh_token": "R2", "expires_at": 10})
            self.assertIsNone(da.current(root, APP, post=refused, user=lambda t: None))


@unittest.skipUnless(HAVE_LOBBY, NO_LOBBY)
class Lobbies(unittest.TestCase):
    """The GSP and the shown name, against the game server's own checks (the lobby server's nex/board.py)."""

    def setUp(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        self.root = d.name
        with open(os.path.join(self.root, "netplay_id.txt"), "w") as f:
            f.write("0123456789abcdef 0123456789abcd4e\n")   # (the game's: two numbers, XORed: the NSA id)
        self.nsa = "00000000000000a1"
        from nex.server import make_server
        import struct
        self.nex = make_server(lambda *a: None, state_dir=self.root, verbose=0, log=lambda *a: None)
        self.nex.board.discord_me = lambda token: {"id": "80351110224678912", "username": "nelly", "global_name": "Nelly"} \
            if token == "AT" else None
        pid = self.nex.accounts.login(self.nsa, "x", 0.0)
        self.nex.services.powers.put(pid, struct.pack("<BIIIQI", 43, 1500, 1, 1500, 0, 1))
        self.nex.services.powers.put(pid, struct.pack("<BIIIQI", 0, 900, 1, 900, 0, 1))

    def ask(self, url, headers=None, body=None):   # (the lobby, without a network)
        path = urllib.parse.urlsplit(url).path
        code, data = self.nex.board.http("POST" if body is not None else "GET", path,
                                         {k.lower(): v for k, v in (headers or {}).items()}, body or b"", 0.0)
        return json.loads(data) if code == 200 else None

    def test_nsa_id(self):
        self.assertEqual(da.nsa_id(self.root), self.nsa)
        self.assertEqual(da.player_id(self.nsa), board.player_id(self.nsa))
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(da.nsa_id(empty), "")

    def test_records_merged_from_the_lobbies(self):
        self.assertEqual(da.records(self.root, ["a:1", "b:2"], ask=self.ask), [(43, 1500), (0, 900)])
        self.assertEqual(da.records(self.root, ["a:1"], ask=lambda *a: None), [])

    def send(self, url, body):   # (the lobby, without a network)
        code, data = self.nex.board.http("POST", urllib.parse.urlsplit(url).path, {}, body, 1000.0 + len(self.sent))
        self.sent.append((url, code))
        return code, json.loads(data)

    def test_shown_name(self):
        self.sent = []
        t = {"user_id": "80351110224678912", "display_name": "Nelly", "access_token": "AT"}
        self.assertEqual(da.set_shown(self.root, t, True, ["a:1"], send=self.send), 1)
        self.assertTrue(da.shows_name(self.root))
        pid = self.nex.accounts.data["users"][self.nsa]["pid"]
        shown = self.nex.board.profiles.of_pid[str(pid)]
        self.assertEqual((shown["name"], shown["discord"], shown["discord_id"], shown["show"]),
                         ("Nelly", da.account_hash("80351110224678912"), "80351110224678912", True))
        self.assertEqual(self.sent, [("http://a:1/gsp/link", 200)])
        self.assertEqual(da.set_shown(self.root, t, False, ["a:1"], send=self.send), 1)   # (link again: 429; hidden by the profile call)
        self.assertFalse(da.shows_name(self.root))
        self.assertFalse(self.nex.board.profiles.of_pid[str(pid)]["show"])
        self.assertEqual(self.sent[-1], ("http://a:1/gsp/profile", 200))

    def test_unknown_install_is_not_registered_by_its_key(self):
        self.sent = []
        del self.nex.accounts.data["users"][self.nsa]       # (the lobby does not know this install: only the game's
        self.nex.board.index(0.0, force=True)               # own login on its game server makes it known there)
        t = {"user_id": "80351110224678912", "access_token": "AT"}
        self.assertEqual(da.set_shown(self.root, t, True, ["a:1"], send=self.send), 0)
        self.assertEqual([c for _u, c in self.sent], [403, 403])
        self.assertNotIn(self.nsa, self.nex.accounts.data["users"])

    def test_refused_token_and_unreachable_lobby(self):
        self.sent = []
        self.assertEqual(da.set_shown(self.root, {"access_token": "bad"}, True, ["a:1"], send=self.send), 0)
        self.assertEqual(da.set_shown(self.root, None, True, ["a:1"], send=self.send), 0)
        self.assertEqual(da.set_shown(self.root, {"access_token": "AT"}, True, ["a:1"], send=lambda u, b: (0, {})), 0)
        self.assertEqual(self.nex.board.profiles.of_pid, {})

    def test_link_body_is_signed_for_the_lobby(self):
        d = json.loads(da.link_body(self.nsa, "AT", True, key=True))
        self.assertEqual((d["token"], d["show"], d["key"]), ("AT", True, self.nsa))
        self.assertEqual(d["sig"], board.signature(self.nsa, "link", d["player"], d["t"], "AT\n1"))
        self.assertNotIn("key", json.loads(da.link_body(self.nsa, "AT", False)))

    def test_send_reads_error_bodies(self):
        import io
        import urllib.error

        def refuse(req, timeout):
            raise urllib.error.HTTPError(req.full_url, 403, "no", {}, io.BytesIO(b'{"error":"unknown player"}'))
        self.assertEqual(da._send("http://x/gsp/link", b"{}", opener=refuse), (403, {"error": "unknown player"}))

        def down(req, timeout):
            raise urllib.error.URLError("down")
        self.assertEqual(da._send("http://x/gsp/link", b"{}", opener=down), (0, {}))

    def test_name_shown_by_default(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertTrue(da.shows_name(empty))      # (no choice made: on)
            os.makedirs(os.path.join(empty, "config"))
            with open(da.shown_path(empty), "w") as f:
                f.write("off")
            self.assertFalse(da.shows_name(empty))

    def test_lobbies(self):
        self.assertEqual(da.lobbies("auto"), list(da.PUBLIC_LOBBIES))
        self.assertEqual(da.lobbies("my.host mycode"), list(da.PUBLIC_LOBBIES) + ["my.host:39420"])
        self.assertEqual(da.lobbies(""), list(da.PUBLIC_LOBBIES))
        # (every region's lobby is asked: a player's records are merged from all of them; the one played on first)
        self.assertEqual(da.PUBLIC_LOBBIES, ("eu.lobby.opensmashultimate.com:39421", "na.lobby.opensmashultimate.com:39421"))
        self.assertEqual(da.lobbies("na.lobby.opensmashultimate.com:39421 code"), list(da.PUBLIC_LOBBIES))   # (a region's lobby means the official one)
        self.assertEqual(da.LEADERBOARD_URL, "http://eu.lobby.opensmashultimate.com:39421/leaderboard.json")
        for older in ("opensmash-lobby.duckdns.org:39421 code", "smash.recomps.wtf:39421", "na", "eu", da.OFFICIAL_LOBBY):
            self.assertEqual(da.lobbies(older), list(da.PUBLIC_LOBBIES), older)   # (the older public lobbies mean the official one)
        os.makedirs(os.path.join(self.root, "config"), exist_ok=True)
        with open(os.path.join(self.root, "config", "official_lobby.txt"), "w") as f:
            f.write("lobby.example:39421" + chr(10))
        try:
            self.assertEqual(da.lobbies("auto", self.root), ["lobby.example:39421"])   # (the release's lobby)
            self.assertEqual(da.leaderboard_url(self.root), "http://lobby.example:39421/leaderboard.json")
        finally:
            os.remove(os.path.join(self.root, "config", "official_lobby.txt"))
        # the server region chosen: its lobby first, its leaderboard
        region = os.path.join(self.root, "config", "server_region.txt")
        with open(region, "w") as f:
            f.write("na-east" + chr(10))
        try:
            self.assertEqual(da.lobbies("auto", self.root), list(reversed(da.PUBLIC_LOBBIES)))
            self.assertEqual(da.official_lobby(self.root), "na.lobby.opensmashultimate.com:39421")
            self.assertEqual(da.leaderboard_url(self.root), "http://na.lobby.opensmashultimate.com:39421/leaderboard.json")
        finally:
            os.remove(region)
        self.assertEqual(da.official_lobby(self.root), da.OFFICIAL_LOBBY)

    def test_ranks(self):
        board_json = {"players": [{"rank": 1, "id": "1", "fighters": {"0": {"rank": 3}}},
                                  {"rank": 2, "id": "42", "fighters": {"43": {"rank": 1, "gsp": 5}, "x": {}}}]}
        self.assertEqual(da.ranks_in(board_json, "42"), (2, {43: 1}))
        self.assertEqual(da.ranks_in(board_json, "7"), (0, {}))
        self.assertEqual(da.ranks_in(None, "42"), (0, {}))
        self.assertEqual(da.fighter_name(43), "Lucario")
        self.assertEqual(da.fighter_name(86), "Fighter 86")

    def test_fighter_names_by_gsp_number(self):
        # (a GSP record's fighter byte is the game's save_no in ui_chara_db, not the fighter kind: issue #389, 0x1d = G&W)
        for number, name in ((0, "Mario"), (15, "Bowser"), (16, "Ice Climbers"), (17, "Sheik"), (29, "Mr. Game & Watch"),
                             (30, "Meta Knight"), (37, "Pokemon Trainer"), (64, "Richter"), (65, "Inkling"),
                             (74, "Piranha Plant"), (83, "Pyra & Mythra"), (85, "Sora")):
            self.assertEqual(da.fighter_name(number), name)
        self.assertEqual(len(da.NAMES), 86)
        self.assertEqual(da.fighter_name(-1), "Fighter -1")


class Mastery(unittest.TestCase):
    """Mastery points (as OpenSmash.Core/Mastery.cs): the regions summed, the largest veteran bonus, 404 lobbies skipped."""
    TICKET = "u|n|1800000000." + "f" * 64

    def test_merge(self):
        eu = {"region": "eu", "fighters": [{"fighter": "mario", "points": 300, "veteran": 1000, "games": 4, "wins": 3},
                                           {"fighter": "ui_chara_pzenigame", "points": 1000, "games": 9, "wins": 2},
                                           {"fighter": "bad key!", "points": 5}, {"fighter": 7}]}
        na = {"region": "na-east", "fighters": [{"fighter": "mario", "points": 200, "veteran": 1000, "games": 2, "wins": 1}]}
        rows = da.merge_mastery([eu, None, na])
        self.assertEqual([r["fighter"] for r in rows], ["mario", "pzenigame"])
        self.assertEqual((rows[0]["points"], rows[0]["veteran"], rows[0]["games"], rows[0]["wins"]), (1500, 1000, 6, 4))
        self.assertEqual(rows[1]["name"], "Squirtle")
        self.assertIsNone(da.merge_mastery([None, {"error": "not found"}]))
        self.assertEqual(da.fighter_key_name("miiswordsman"), "Mii Swordfighter")

    def test_board_merged_by_id(self):
        top = da.merge_mastery_board([{"total": [{"id": "a", "name": "Ann", "points": 900, "veteran": 1000},
                                                 {"id": "b", "name": "", "points": 1500, "veteran": 0}]},
                                      {"total": [{"id": "b", "points": 700, "veteran": 1000}, {"id": "", "points": 9}]}, None])
        self.assertEqual(top, [{"id": "b", "name": "Player", "points": 3200}, {"id": "a", "name": "Ann", "points": 1900}])

    def test_asked_with_the_stored_ticket_official_lobbies_only(self):
        with tempfile.TemporaryDirectory() as root:
            asked = []

            def ask(url, headers=None):
                asked.append((url, dict(headers or {})))
                if url.endswith("/mastery/me") and len(asked) == 1:
                    return {"fighters": [{"fighter": "fox", "points": 120, "veteran": 1000, "games": 1, "wins": 1}]}
                return None   # (the other lobby is older: 404)
            self.assertIsNone(da.mastery_me(root, ask))   # (no stored ticket: nothing asked)
            self.assertEqual(asked, [])
            os.makedirs(os.path.join(root, "build", "cache"))
            with open(os.path.join(root, "build", "cache", "online_pass.txt"), "w", encoding="ascii") as f:
                f.write(self.TICKET)
            rows = da.mastery_me(root, ask)
            self.assertEqual(rows[0]["points"], 1120)
            self.assertTrue(asked and all(h.get("X-OpenSmash-Ticket") == self.TICKET for _, h in asked))
            self.assertEqual({u.split("/")[2] for u, _ in asked}, {x for x in da.server_region.official_lobbies(root)})
            self.assertIsNone(da.mastery_board(root, lambda url, headers=None: None))

    def test_view_and_switches(self):
        with tempfile.TemporaryDirectory() as root:
            acc = da.Account(root, root)
            old = os.environ.get("OPENSMASH_MASTERY_UI")
            try:
                os.environ["OPENSMASH_MASTERY_UI"] = "0"
                self.assertEqual(acc.mastery_view(me=lambda r: self.fail("asked"))["on"], False)
                os.environ.pop("OPENSMASH_MASTERY_UI")
                v = acc.mastery_view(me=lambda r: None, board=lambda r: None, service=lambda r: None)
                self.assertTrue(v["on"] and not v["available"] and v["top"] is None and v["arena_url"].startswith("https://www.ssbuarena.com"))
                acc.mastery_at = 0
                v = acc.mastery_view(me=lambda r: [{"fighter": "fox", "name": "Fox", "points": 50, "veteran": 0, "games": 1, "wins": 0}],
                                     board=lambda r: [{"id": "x", "name": "Xia", "points": 5}], service=lambda r: None)
                self.assertTrue(v["available"] and v["total"] == 50 and v["top"][0]["name"] == "Xia")
                acc.mastery_at = 0
                v = acc.mastery_view(me=lambda r: None, board=lambda r: None, service=lambda r: None)   # (all down: the last answers kept)
                self.assertEqual((v["total"], len(v["top"])), (50, 1))
                # the accounts service's board first: the lobbies' list not asked when it answers
                acc.mastery_at = 0
                v = acc.mastery_view(me=lambda r: None, board=lambda r: self.fail("lobbies asked"),
                                     service=lambda r: [{"id": "a", "name": "Ann", "points": 5200, "pos": 1}])
                self.assertEqual(v["top"][0]["name"], "Ann")
            finally:
                if old is None:
                    os.environ.pop("OPENSMASH_MASTERY_UI", None)
                else:
                    os.environ["OPENSMASH_MASTERY_UI"] = old

    def _env(self, **values):
        old = {k: os.environ.get(k) for k in values}

        def back():
            for k, v in old.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
        self.addCleanup(back)
        for k, v in values.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def test_account_service_as_the_launchers_read_it(self):
        self._env(OPENSMASH_SUPABASE_URL=None, OPENSMASH_SUPABASE_KEY=None)
        with tempfile.TemporaryDirectory() as root:
            self.assertIsNone(da.account_service(root))
            os.makedirs(os.path.join(root, "OpenSmash.Core"))
            with open(os.path.join(root, "OpenSmash.Core", "account_service.json"), "w", encoding="utf-8") as f:
                json.dump({"url": "https://built.example/", "publishable_key": "pk-built"}, f)
            self.assertEqual(da.account_service(root), ("https://built.example", "pk-built"))
            os.makedirs(os.path.join(root, "config"))
            with open(os.path.join(root, "config", "account_service.json"), "w", encoding="utf-8") as f:
                json.dump({"url": "https://own.example"}, f)
            self.assertEqual(da.account_service(root), ("https://own.example", "pk-built"))
            self._env(OPENSMASH_SUPABASE_URL="http://127.0.0.1:5555", OPENSMASH_SUPABASE_KEY="test-key")
            self.assertEqual(da.account_service(root), ("http://127.0.0.1:5555", "test-key"))
            self._env(OPENSMASH_SUPABASE_URL="http://remote.example")   # (plain http elsewhere than this PC: none)
            self.assertIsNone(da.account_service(root))

    def test_service_board(self):
        self._env(OPENSMASH_SUPABASE_URL="http://127.0.0.1:5555", OPENSMASH_SUPABASE_KEY="test-key")
        asked = []

        def ask(url, headers=None, body=None):
            asked.append((url, dict(headers or {}), json.loads(body)))
            return [{"pos": 1, "id": "a", "name": "Ann", "tag": 7, "total": 5200, "games": 40, "total_count": 3},
                    {"pos": 2, "id": "b", "name": None, "total": 3100}, {"id": "", "total": 1}, "junk"]
        with tempfile.TemporaryDirectory() as root:
            top = da.service_board(root, ask, take=5)
            self.assertEqual(top, [{"id": "a", "name": "Ann", "points": 5200, "pos": 1, "fighter": None}, {"id": "b", "name": "Anonymous", "points": 3100, "pos": 2, "fighter": None}])
            url, headers, body = asked[0]
            self.assertEqual(url, "http://127.0.0.1:5555/rest/v1/rpc/opensmash_mastery_board")
            self.assertEqual((headers["apikey"], headers["Authorization"]), ("test-key", "Bearer test-key"))
            self.assertEqual(body, {"p_fighter": None, "p_limit": 5, "p_offset": 0})
            self.assertIsNone(da.service_board(root, lambda url, headers=None, body=None: None))   # (404 / down: the lobbies)
            self.assertIsNone(da.service_board(root, lambda url, headers=None, body=None: {"message": "x"}))

    def test_fighters_and_icons(self):
        self.assertEqual([da.fighter_key_any(v) for v in ("Joker", "jack", "ui_chara_jack", "Pyra / Mythra", "R.O.B.", "PT", "nobody")],
                         ["jack", "jack", "jack", "element", "robot", "ptrainer", None])
        self.assertEqual(da.fighter_icon("Joker"), {"name": "Joker", "icon": "https://www.ssbuarena.com/fighters/jack/1.webp"})
        self.assertEqual(da.fighter_icon("Ridley")["icon"], "https://www.ssbuarena.com/fighters/ridley/5.webp")
        self.assertEqual(da.fighter_icon("element")["icon"], "https://www.ssbuarena.com/fighters/eflame/1.webp")
        self.assertEqual(da.fighter_icon("nobody")["icon"], "")

    def test_ranked_board(self):
        self._env(OPENSMASH_SUPABASE_URL="http://127.0.0.1:5555", OPENSMASH_SUPABASE_KEY="test-key")
        asked = []

        def ask(url, headers=None, body=None):
            asked.append((url, json.loads(body)))
            return [{"rank": 1, "username": "Vex", "tag": 7, "rating": 2586.18, "wins": 9, "losses": 2, "tier": "Grandmaster", "tier_color": "#FF5A6A",
                     "fighters": [{"fighter": "Joker", "games": 9}, {"fighter": "Fox"}, {"fighter": "Ike"}, {"fighter": "Wolf"}]},
                    {"rank": 2, "username": "", "rating": "1500"}, {"rank": 3, "username": "Ann", "rating": "x", "tier_color": "red"}]
        with tempfile.TemporaryDirectory() as root:
            rows = da.ranked_board(root, "eu", 10, ask)
            self.assertEqual(asked[0][0], "http://127.0.0.1:5555/rest/v1/rpc/ranked_board")
            self.assertEqual(asked[0][1]["p_region"], "eu")
            self.assertEqual((rows[0]["name"], rows[0]["tag"], rows[0]["rating"], len(rows[0]["fighters"]), rows[0]["fighters"][0]["name"]), ("Vex", "0007", 2586, 3, "Joker"))
            self.assertEqual((len(rows), rows[1]["tier_color"], rows[1]["tier"], rows[1]["rating"]), (2, "#8C9AAD", "Unranked", 0))
            da.ranked_board(root, "moon", 10, ask)
            self.assertIsNone(asked[1][1]["p_region"])
            self.assertIsNone(da.ranked_board(root, None, 10, lambda url, headers=None, body=None: None))
    def test_service_board_against_the_fake_service(self):
        """tools/dev/fake_accounts.py's board (anyone may read it), and its --no-mastery 404."""
        import socket as _s
        import subprocess
        here = os.path.dirname(os.path.abspath(__file__))
        fake = os.path.join(here, "..", "tools", "dev", "fake_accounts.py")
        for extra, want in (([], 4), (["--no-mastery"], None)):
            with _s.socket() as s0:
                s0.bind(("127.0.0.1", 0))
                port = s0.getsockname()[1]
            p = subprocess.Popen([sys.executable, "-I", fake, "--port", str(port), "--name", "Tester"] + extra,
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            try:
                self.assertIn("fake accounts service on", p.stdout.readline())
                self._env(OPENSMASH_SUPABASE_URL="http://127.0.0.1:%d" % port, OPENSMASH_SUPABASE_KEY="test-key")
                with tempfile.TemporaryDirectory() as root:
                    top = da.service_board(root, take=10)
                if want is None:
                    self.assertIsNone(top)
                else:
                    self.assertEqual(len(top), want)
                    self.assertEqual((top[0]["name"], top[1]["name"], top[2]["name"], top[2]["points"]), ("Ann", "Anonymous", "Tester", 1420))
            finally:
                p.kill()
                p.wait()
                p.stdout.close()

    def test_view_unchanged_below(self):
        with tempfile.TemporaryDirectory() as root:
            acc = da.Account(root, root)
            old = os.environ.get("OPENSMASH_MASTERY_UI")
            try:
                os.environ.pop("OPENSMASH_MASTERY_UI", None)
                v = acc.mastery_view(me=lambda r: None, board=lambda r: [{"id": "x", "name": "Xia", "points": 5}], service=lambda r: None)
                self.assertEqual(v["top"][0]["name"], "Xia")   # (the service failing: the lobbies' list)
            finally:
                if old is None:
                    os.environ.pop("OPENSMASH_MASTERY_UI", None)
                else:
                    os.environ["OPENSMASH_MASTERY_UI"] = old


if __name__ == '__main__':
    unittest.main()
