"""tools/weblauncher/discord_account.py: the Discord sign-in the web launcher shares with the game and OpenSmash.exe
(config/discord_token.txt), checked without Discord: the token file, the PKCE sign-in against a made-up browser and
token endpoint, a refresh, the GSP records and the position read from leaderboard.json."""
import base64
import hashlib
import os
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools', 'weblauncher'))
import discord_account as da  # noqa: E402


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
            self.assertEqual((q["client_id"], q["code_challenge_method"], q["response_type"]), (da.APP_ID, "S256", "code"))
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
            t = da.sign_in(root, open_url=browser, post=post, user=user, timeout=20, listen=listen)
            self.assertEqual((t["access_token"], t["user_id"]), ("AT", "42"))
            self.assertEqual(da.load(root)["refresh_token"], "RT")
        (url, form), = posted
        self.assertEqual(url, da.API + "/oauth2/token")
        self.assertEqual((form["code"], form["grant_type"], form["redirect_uri"]), ("CODE", "authorization_code", seen["redirect"]))
        self.assertNotIn("client_secret", form)
        want = base64.urlsafe_b64encode(hashlib.sha256(form["code_verifier"].encode()).digest()).rstrip(b"=").decode()
        self.assertEqual(want, seen["challenge"])

    def test_refresh(self):
        with tempfile.TemporaryDirectory() as root:
            da.save(root, {"access_token": "old", "refresh_token": "R1", "expires_at": 10})
            t = da.current(root, post=lambda url, form: {"access_token": "new", "refresh_token": "R2", "expires_in": 3600}
                           if form["refresh_token"] == "R1" else None, user=lambda t: None)
            self.assertEqual((t["access_token"], da.load(root)["refresh_token"]), ("new", "R2"))

            def refused(url, form):
                raise da.SignInError("no")
            da.save(root, {"access_token": "old", "refresh_token": "R2", "expires_at": 10})
            self.assertIsNone(da.current(root, post=refused, user=lambda t: None))


class Records(unittest.TestCase):
    @staticmethod
    def entry(pid, fighter, gsp, when):
        return struct.pack("<QBIIIQI", pid, fighter, gsp, 0, gsp, when, 1)

    def test_newest_players_records(self):
        self.assertEqual(len(self.entry(1, 2, 3, 4)), da.ENTRY)
        data = self.entry(7, 43, 1460, 5) + self.entry(9, 0, 999999, 1) + self.entry(7, 0, 2000, 6)
        self.assertEqual(da.records_of(data), [(0, 2000), (43, 1460)])
        self.assertEqual(da.records_of(b""), [])

    def test_ranks(self):
        board = {"players": [{"rank": 1, "discord_id": "1", "fighters": {"0": {"rank": 3}}},
                             {"rank": 2, "discord_id": "42", "fighters": {"43": {"rank": 1, "gsp": 5}, "x": {}}}]}
        self.assertEqual(da.ranks_in(board, "42"), (2, {43: 1}))
        self.assertEqual(da.ranks_in(board, "7"), (0, {}))
        self.assertEqual(da.ranks_in(None, "42"), (0, {}))
        self.assertEqual(da.fighter_name(43), "Olimar")
        self.assertEqual(da.fighter_name(77), "Fighter 77")


if __name__ == '__main__':
    unittest.main()
