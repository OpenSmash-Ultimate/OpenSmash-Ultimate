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
        self.assertEqual(bot.status_text({"online": 2, "open_rooms": 1, "matches": {"total": 12345, "today": 9}}),
                         "🟢 2 players in the OpenSmash lobbies · 1 open room · 12,345 matches played")
        self.assertEqual(bot.status_text({"online": 2, "matches": {"total": 1}}), "🟢 2 players in the OpenSmash lobbies · 1 match played")
        self.assertEqual(bot.status_text({"online": 2, "matches": {"total": 0}}), "🟢 2 players in the OpenSmash lobbies")
        self.assertEqual(bot.status_text({"online": 2, "matches": "x"}), "🟢 2 players in the OpenSmash lobbies")

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


# ---- feedback threads, against a fake Discord REST API ----------------------------------------------------------

import io   # noqa: E402
import json   # noqa: E402
import tempfile   # noqa: E402
import urllib.error   # noqa: E402

BOT = "999"
NOW = 1763400000.0   # 2025-11-17
BUGS, HELP, GENERAL = "1561545181965189142", "1561480589591314594", "1561342605151707259"


def sid(t):
    """A Discord id made at unix time t."""
    return str((int(t * 1000) - bot.DISCORD_EPOCH_MS) << 22)


class FakeDiscord:
    """The routes of Discord's REST API v10 the watcher uses, kept in dicts; every call is recorded."""

    def __init__(self):
        self.calls = []
        self.messages = {}     # thread id -> [message], newest first as Discord returns them
        self.threads = []      # active thread objects
        self.archived = {}     # channel id -> [thread]
        self.posted = []

    def call(self, method, path, body=None):
        self.calls.append((method, path))
        parts = path.split("?")[0].strip("/").split("/")
        if method == "GET" and parts[0] == "guilds" and parts[2:] == ["threads", "active"]:
            return {"threads": list(self.threads)}
        if method == "GET" and parts[0] == "channels" and parts[2:4] == ["threads", "archived"]:
            return {"threads": list(self.archived.get(parts[1], [])), "has_more": False}
        if parts[0] == "channels" and parts[2] == "messages":
            tid = parts[1]
            if method == "POST":
                m = dict(body, id=sid(NOW + 1), author={"id": BOT, "bot": True}, attachments=[])
                self.messages.setdefault(tid, []).insert(0, m)
                self.posted.append((tid, body))
                return m
            if len(parts) == 4:
                for m in self.messages.get(tid, []):
                    if m["id"] == parts[3]:
                        return m
                raise bot.DiscordError(404, "Unknown Message")
            return list(self.messages.get(tid, []))
        raise AssertionError(f"unexpected call {method} {path}")


def thread(t=NOW - 70, parent=BUGS, guild=bot.GUILD_ID, owner="42", name="Crash on start"):
    i = sid(t)
    return {"id": i, "guild_id": guild, "parent_id": parent, "owner_id": owner, "name": name,
            "last_message_id": i, "message_count": 0, "thread_metadata": {"archived": False}}


def starter(th, files=(), author="42", **who):
    return {"id": th["id"], "author": dict({"id": author, "username": "player" + author}, **who),
            "attachments": [{"filename": f} for f in files]}


class Threads(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.api = FakeDiscord()
        self.logs = []

    def tearDown(self):
        self.tmp.cleanup()

    def watcher(self, content_ok=True, **cfg):
        c = bot.ThreadConfig(state_dir=self.tmp.name, **cfg)
        return bot.ThreadWatcher(self.api, c, BOT, content_ok, log=self.logs.append, sleep=lambda s: None,
                                 now=lambda: NOW)

    def test_asks_for_the_log_once(self):
        th = thread()
        self.api.messages[th["id"]] = [starter(th, ["screenshot.png"])]
        w = self.watcher()
        self.assertEqual(w.on_new_thread(th), "replied")
        self.assertEqual(len(self.api.posted), 1)
        tid, body = self.api.posted[0]
        self.assertEqual(tid, th["id"])
        for words in ("build\\play.log", "Report a problem", "reports\\", "version", "<@42>"):
            self.assertIn(words, body["content"])
        self.assertEqual(body["allowed_mentions"], {"users": ["42"]})
        self.assertEqual(body["message_reference"]["message_id"], th["id"])
        # the same event again, and after a restart (a new watcher reading the saved ids): no second reply
        self.assertEqual(w.on_new_thread(th), "already")
        self.assertEqual(self.watcher().on_new_thread(th), "already")
        self.assertEqual(len(self.api.posted), 1)

    def test_no_reply_when_a_log_is_there(self):
        for files in (["play.log"], ["play (1).log"], ["crash_2026-11-17.txt"], ["OpenSmash_report.zip"]):
            th = thread(name=str(files))
            self.api.messages[th["id"]] = [starter(th, files)]
            self.assertEqual(self.watcher().on_new_thread(th), "has-log", files)
        th = thread(t=NOW - 60)   # the log came in the author's second post
        self.api.messages[th["id"]] = [{"id": sid(NOW - 10), "author": {"id": "42"},
                                        "attachments": [{"filename": "play.log"}]}, starter(th)]
        self.assertEqual(self.watcher().on_new_thread(th), "has-log")
        self.assertEqual(self.api.posted, [])

    def test_someone_elses_log_does_not_count(self):
        th = thread()
        self.api.messages[th["id"]] = [{"id": sid(NOW - 10), "author": {"id": "7"},
                                        "attachments": [{"filename": "play.log"}]}, starter(th)]
        self.assertEqual(self.watcher().on_new_thread(th), "replied")

    def test_other_servers_and_channels_are_never_touched(self):
        w = self.watcher()
        self.assertEqual(w.on_new_thread(thread(guild="111111111111111111")), "out-of-scope")
        self.assertEqual(w.on_new_thread(thread(parent=GENERAL)), "out-of-scope")
        self.assertEqual(self.api.calls, [])

    def test_no_reply_without_the_message_content_intent(self):
        th = thread()
        self.api.messages[th["id"]] = [starter(th)]
        self.assertEqual(self.watcher(content_ok=False).on_new_thread(th), "no-content-intent")
        self.assertEqual(self.api.calls, [])

    def test_opt_out(self):
        th = thread()
        self.api.messages[th["id"]] = [starter(th)]
        self.assertEqual(self.watcher(log_reply=False).on_new_thread(th), "reply-off")
        self.assertEqual(self.watcher(threads=False).on_new_thread(th), "out-of-scope")
        self.assertEqual(self.api.calls, [])
        c = bot.ThreadConfig.from_env({"SSBU_BOT_THREADS": "0", "STATE_DIRECTORY": "/var/lib/x"})
        self.assertFalse(c.threads)
        self.assertEqual(c.state_dir, "/var/lib/x")
        c = bot.ThreadConfig.from_env({"SSBU_BOT_LOG_REPLY": "off", "SSBU_BOT_DIGEST_HOURS": "0"})
        self.assertTrue(c.threads)
        self.assertFalse(c.log_reply)
        self.assertEqual(c.digest_hours, 0)
        c = bot.ThreadConfig.from_env({})
        self.assertTrue(c.threads and c.log_reply)
        self.assertEqual(c.digest_hours, 24)

    def test_old_own_and_bot_threads(self):
        self.assertEqual(self.watcher().on_new_thread(thread(t=NOW - 3600)), "old")
        self.assertEqual(self.watcher().on_new_thread(thread(owner=BOT)), "own")
        th = thread(owner="")
        self.api.messages[th["id"]] = [starter(th, author="55", bot=True)]
        self.assertEqual(self.watcher().on_new_thread(th), "bot-author")
        th = thread()   # the bot already answered, its saved ids lost
        self.api.messages[th["id"]] = [{"id": sid(NOW), "author": {"id": BOT}, "attachments": []}, starter(th)]
        self.assertEqual(self.watcher().on_new_thread(th), "already")
        self.assertEqual(self.api.posted, [])

    def test_starter_message_late(self):
        th = thread()
        real, misses = self.api.call, [2]

        def call(method, path, body=None):
            if path.endswith(f"/messages/{th['id']}") and misses[0]:
                misses[0] -= 1
                raise bot.DiscordError(404, "Unknown Message")
            return real(method, path, body)
        self.api.messages[th["id"]] = [starter(th, ["play.log"])]
        self.api.call = call
        self.assertEqual(self.watcher().on_new_thread(th), "has-log")

    def test_digest(self):
        a = thread(t=NOW - 3 * 86400, name="Black screen")
        a["last_message_id"] = sid(NOW - 3600)
        b = thread(t=NOW - 86400, parent=HELP, owner="43", name="How to install mods")
        old = thread(t=NOW - 90 * 86400, name="Old")
        other = thread(parent=GENERAL, name="not a forum")
        self.api.threads = [a, other]
        self.api.archived[HELP] = [b]
        self.api.archived[BUGS] = [old, a]
        self.api.messages[a["id"]] = [{"id": sid(NOW - 3600), "author": {"id": "7"},
                                       "attachments": [{"filename": "play.log"}]}, starter(a)]
        self.api.messages[b["id"]] = [starter(b, author="43", global_name="Mario")]
        d = self.watcher().write_digest()
        with open(os.path.join(self.tmp.name, "threads_digest.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f), d)
        self.assertEqual([r["title"] for r in d["threads"]], ["Black screen", "How to install mods"])
        ra, rb = d["threads"]
        self.assertEqual((ra["channel"], ra["author"], ra["has_log"]), ("bug-reports", "player42", True))
        self.assertEqual((rb["channel"], rb["author"], rb["has_log"]), ("help", "Mario", False))
        self.assertEqual(ra["last_activity"], bot.iso(NOW - 3600))
        self.assertTrue(ra["url"].endswith("/" + bot.GUILD_ID + "/" + a["id"]))
        self.assertTrue(any("2 threads, 1 without a log" in s for s in self.logs))
        for method, path in self.api.calls:   # only reads, and only of the feedback forums and their threads
            self.assertEqual(method, "GET")
            self.assertNotIn(GENERAL, path)
            self.assertNotIn(other["id"], path)
        # without the Message Content intent the log column is unknown, not "no"
        d = self.watcher(content_ok=False).digest()
        self.assertEqual([r["has_log"] for r in d["threads"]], [None, None])

    def test_flags_and_names(self):
        self.assertTrue(bot.message_content_enabled({"flags": 1 << 19}))
        self.assertTrue(bot.message_content_enabled({"flags": (1 << 18) | 8}))
        self.assertFalse(bot.message_content_enabled({"flags": 1 << 23}))
        self.assertFalse(bot.message_content_enabled(None))
        self.assertFalse(bot.is_log_attachment("screenshot.png"))
        self.assertFalse(bot.is_log_attachment("message.txt"))
        self.assertAlmostEqual(bot.snowflake_time(sid(NOW)), NOW, places=2)

    def test_rest_retries_429(self):
        seen = []

        class Resp(io.BytesIO):
            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

        def opener(req):
            seen.append((req.get_method(), req.full_url, req.get_header("Authorization")))
            if len(seen) == 1:
                raise urllib.error.HTTPError(req.full_url, 429, "slow", {}, io.BytesIO(b'{"retry_after": 0.1}'))
            return Resp(b'{"flags": 262144}')
        r = bot.DiscordRest("tok", opener=opener, sleep=lambda s: None)
        self.assertTrue(bot.message_content_enabled(r.call("GET", "/applications/@me")))
        self.assertEqual(seen[-1], ("GET", "https://discord.com/api/v10/applications/@me", "Bot tok"))
        self.assertEqual(len(seen), 2)


if __name__ == "__main__":
    unittest.main()
