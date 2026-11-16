"""tools/discord_status_bot.py: the status line it shows, without Discord."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import discord_status_bot as bot   # noqa: E402


class StatusText(unittest.TestCase):
    def test_counts(self):
        self.assertEqual(bot.status_text({"online": 21, "open_rooms": 0}), "🟢 21 players in the OpenSmash lobbies")
        self.assertEqual(bot.status_text({"online": 1}), "🟢 1 player in the OpenSmash lobbies")
        self.assertEqual(bot.status_text({"online": 24, "open_rooms": 3}), "🟢 24 players in the OpenSmash lobbies · 3 open rooms")

    def test_offline(self):
        self.assertEqual(bot.status_text(None), "🔴 Lobby offline")

    def test_token_from_credentials(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "token"), "w") as f:
                f.write("abc\n")
            old = os.environ.get("CREDENTIALS_DIRECTORY")
            os.environ["CREDENTIALS_DIRECTORY"] = d
            try:
                self.assertEqual(bot.read_token(), "abc")
            finally:
                if old is None:
                    del os.environ["CREDENTIALS_DIRECTORY"]
                else:
                    os.environ["CREDENTIALS_DIRECTORY"] = old


if __name__ == "__main__":
    unittest.main()
