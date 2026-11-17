"""tools/weblauncher/discord_account.py: the Discord sign-in the web launcher shares with the game and OpenSmash.exe
(config/discord_token.txt), checked without Discord: the token file, the application id file, the PKCE sign-in
against a made-up browser and token endpoint, a refresh, the GSP from the lobbies' game server (signed as
tools/nex/board.py checks), the choice to show the name on the leaderboard, and the position read from
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
import discord_account as da  # noqa: E402
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


class Lobbies(unittest.TestCase):
    """The GSP and the shown name, against the game server's own checks (tools/nex/board.py)."""

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

    def test_unknown_install_is_registered_with_the_key_once(self):
        self.sent = []
        del self.nex.accounts.data["users"][self.nsa]       # (the lobby does not know this install)
        self.nex.board.index(0.0, force=True)
        t = {"user_id": "80351110224678912", "access_token": "AT"}
        self.assertEqual(da.set_shown(self.root, t, True, ["a:1"], send=self.send), 1)
        self.assertEqual([c for _u, c in self.sent], [403, 200])
        self.assertEqual(self.nex.board.profiles.discord_id(self.nex.accounts.data["users"][self.nsa]["pid"]), "80351110224678912")

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


if __name__ == '__main__':
    unittest.main()
