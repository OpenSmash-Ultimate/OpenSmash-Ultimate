"""tools/deploy/discord_threads.py: reply posts only in the OpenSmash-Ultimate server's threads, without pings."""
import os
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "deploy"))
sys.modules.setdefault("pwd", types.ModuleType("pwd"))   # (Unix only; reply does not use it)
import discord_threads as D  # noqa: E402


class Args:
    def __init__(self, tid, text=None, file=None, reply_to=None):
        self.threads, self.text, self.file, self.reply_to = [tid], text, file, reply_to


class ReplyTest(unittest.TestCase):
    def run_reply(self, channel, **kw):
        calls = []

        def fake(method, path, body=None, raw=False):
            calls.append((method, path, body))
            return channel if method == "GET" else {"id": "99"}
        with mock.patch.object(D, "call", fake):
            D.cmd_reply(Args("123", **kw))
        return calls

    def test_posts_without_pings(self):
        calls = self.run_reply({"type": 11, "guild_id": D.REPLY_GUILD, "name": "t"}, text=" hi @everyone ")
        method, path, body = calls[-1]
        self.assertEqual((method, path), ("POST", "/channels/123/messages"))
        self.assertEqual(body["content"], "hi @everyone")
        self.assertEqual(body["allowed_mentions"], {"parse": []})

    def test_other_server_refused(self):
        with self.assertRaises(SystemExit):
            self.run_reply({"type": 11, "guild_id": "432115299282845697"}, text="x")

    def test_not_a_thread_refused(self):
        with self.assertRaises(SystemExit):
            self.run_reply({"type": 0, "guild_id": D.REPLY_GUILD}, text="x")

    def test_limits_and_file(self):
        with self.assertRaises(SystemExit):
            D.reply_body("x" * 2001)
        with self.assertRaises(SystemExit):
            D.reply_body("   ")
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
            f.write("from a file")
        try:
            calls = self.run_reply({"type": 11, "guild_id": D.REPLY_GUILD}, file=f.name, reply_to="5")
        finally:
            os.unlink(f.name)
        self.assertEqual(calls[-1][2]["content"], "from a file")
        self.assertEqual(calls[-1][2]["message_reference"]["message_id"], "5")


if __name__ == "__main__":
    unittest.main()
