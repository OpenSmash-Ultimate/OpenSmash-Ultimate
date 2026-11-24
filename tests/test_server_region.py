"""The server region (tools/weblauncher/server_region.py, the web launcher's copy of runtime/net_region.c and
OpenSmash.Core/ServerRegion.cs): the table, the saved choice, Auto's pick from made-up answer times, the pick kept
for the game, overrides, and the question itself against a lobby on this machine. Nothing is sent to a real lobby."""
import os
import socket
import sys
import tempfile
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "weblauncher"))
import server_region as sr  # noqa: E402

EU, NA = "eu.lobby.opensmashultimate.com:39421", "na.lobby.opensmashultimate.com:39421"


class Region(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.was = os.environ.pop("SSBU_OFFICIAL_LOBBY", None)

    def tearDown(self):
        os.environ.pop("SSBU_OFFICIAL_LOBBY", None)
        if self.was is not None:
            os.environ["SSBU_OFFICIAL_LOBBY"] = self.was
        self.tmp.cleanup()

    def config(self, name, text=None):
        path = os.path.join(self.root, "config", name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if text is None:
            if os.path.exists(path):
                os.remove(path)
        else:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
        return path

    def test_table(self):
        self.assertEqual(sr.REGIONS, (("eu", "EU", EU), ("na-east", "NA East", NA)))
        self.assertEqual((sr.by_id("NA-East"), sr.by_id(" eu "), sr.by_id("auto"), sr.by_id("na"), sr.by_id(None)), ("na-east", "eu", None, None, None))
        self.assertEqual((sr.of_lobby(NA), sr.of_lobby("NA.lobby.opensmashultimate.com"), sr.of_lobby(EU)), ("na-east", "na-east", "eu"))
        for other in ("", None, "na.lobby.opensmashultimate.com:1", "na.lobby.opensmashultimate.com.evil.example:39421", "auto"):
            self.assertIsNone(sr.of_lobby(other), other)
        self.assertEqual((sr.label("na-east"), sr.lobby("na-east")), ("NA East", NA))

    def test_pick(self):
        self.assertEqual((sr.median([30, 10, 20]), sr.median([40, 10]), sr.median([-1, 12, -1]), sr.median([]), sr.median([-1])), (20, 25, 12, -1, -1))
        self.assertEqual(sr.pick({"eu": 20, "na-east": 90}), "eu")
        self.assertEqual(sr.pick({"eu": 90, "na-east": 20}), "na-east")
        self.assertEqual(sr.pick({"eu": -1, "na-east": 140}), "na-east")       # (the only one that answers)
        self.assertEqual(sr.pick({"na-east": 140}), "na-east")
        self.assertIsNone(sr.pick({"eu": -1, "na-east": -1}))                  # (none answers: no pick)
        self.assertIsNone(sr.pick({}))
        self.assertEqual(sr.pick({"eu": 40, "na-east": 40}), "eu")             # (the same time: the first region)
        self.assertEqual(sr.pick({"eu": 52, "na-east": 60}, "na-east"), "na-east")   # (a little sooner: the last pick stays)
        self.assertEqual(sr.pick({"eu": 50, "na-east": 60}, "na-east"), "eu")        # (clearly sooner)
        self.assertEqual(sr.pick({"eu": 20, "na-east": -1}, "na-east"), "eu")        # (the last pick does not answer)
        self.assertEqual(sr.pick({"eu": 20, "na-east": 90}, "moon"), "eu")

    def test_kept_line(self):
        self.assertEqual(sr.parse_kept("na-east 31 1700000000"), ("na-east", 31))
        self.assertEqual(sr.parse_kept("eu"), ("eu", -1))
        self.assertEqual(sr.parse_kept("eu -4 1"), ("eu", -1))
        for bad in ("mars 5 1", "", None, "auto 3 1"):
            self.assertIsNone(sr.parse_kept(bad)[0], bad)

    def test_auto(self):
        self.assertEqual((sr.setting(self.root), sr.current(self.root), sr.official_lobby(self.root), sr.shown(self.root)), ("auto", "eu", EU, "Auto"))
        self.assertEqual((sr.setting(None), sr.official_lobby(None), sr.official_lobby()), ("auto", EU, EU))
        asked = []

        def times(ms):
            return lambda region: asked.append(region) or ms[region]
        self.assertEqual(sr.measure(self.root, asker=times({"eu": [95, 90, 99], "na-east": [21, 25, 23]})), ("na-east", 23))
        self.assertEqual(sorted(asked), ["eu", "na-east"])
        with open(os.path.join(self.root, "config", "server_region_auto.txt")) as f:
            self.assertRegex(f.read(), r"^na-east 23 \d+\n$")   # (kept for the game: runtime/net_region.c starts from it)
        self.assertEqual((sr.current(self.root), sr.official_lobby(self.root), sr.shown(self.root)), ("na-east", NA, "Auto (NA East, 23 ms)"))
        self.assertEqual(sr.official_lobbies(self.root), [NA, EU])
        self.assertEqual(sr.measure(self.root, asker=times({"eu": [55], "na-east": [60]})), ("na-east", 60))     # (stays)
        self.assertEqual(sr.measure(self.root, asker=times({"eu": [30, 34], "na-east": [120, 130]})), ("eu", 32))
        self.assertEqual(sr.measure(self.root, asker=times({"eu": [], "na-east": []})), (None, -1))               # (no answer)
        self.assertEqual(sr.kept(self.root), ("eu", 32))                                                          # (the pick before stays)

        def broken(region):
            if region == "eu":
                raise OSError("no network")
            return [140]
        self.assertEqual(sr.measure(self.root, asker=broken), ("na-east", 140))

    def test_slow_lobby_not_waited_for(self):
        def slow(region):
            if region == "eu":
                time.sleep(2)
            return [50]
        start = time.monotonic()
        self.assertEqual(sr.measure_all(0.2, slow), {"eu": -1, "na-east": 50})
        self.assertLess(time.monotonic() - start, 1.5)

    def test_choice(self):
        self.config("server_region_auto.txt", "na-east 23 1700000000\n")
        sr.set_setting(self.root, "eu")
        self.assertEqual((sr.setting(self.root), sr.official_lobby(self.root), sr.shown(self.root)), ("eu", EU, "EU"))
        path = self.config("server_region.txt", "# the server region\nNA-East   # mine\n")
        self.assertEqual((sr.setting(self.root), sr.official_lobby(self.root), sr.shown(self.root)), ("na-east", NA, "NA East"))
        sr.set_setting(self.root, "auto")
        with open(path) as f:
            self.assertEqual(f.read(), "# the server region\nauto\n")   # (the comment kept)
        self.assertEqual((sr.setting(self.root), sr.official_lobby(self.root)), ("auto", NA))
        sr.set_setting(self.root, "moon")
        self.assertEqual(sr.setting(self.root), "auto")
        self.config("server_region.txt", "moon\n")
        self.assertEqual(sr.setting(self.root), "auto")

    def test_override(self):
        self.config("server_region.txt", "na-east\n")
        self.config("official_lobby.txt", "lobby.example:1\n")
        self.assertEqual((sr.override(self.root), sr.official_lobby(self.root), sr.current(self.root), sr.shown(self.root)),
                         ("lobby.example:1", "lobby.example:1", None, "lobby.example:1"))
        self.assertEqual(sr.official_lobbies(self.root), ["lobby.example:1"])
        os.environ["SSBU_OFFICIAL_LOBBY"] = EU   # (a region's lobby as an override: that server, no Auto)
        self.assertEqual((sr.official_lobby(self.root), sr.current(self.root)), (EU, None))
        os.environ["SSBU_OFFICIAL_LOBBY"] = "http://x/ y"   # (not host:port: not used)
        self.assertEqual(sr.official_lobby(self.root), "lobby.example:1")
        del os.environ["SSBU_OFFICIAL_LOBBY"]
        self.config("official_lobby.txt")
        self.assertEqual((sr.override(self.root), sr.official_lobby(self.root)), (None, NA))


class Question(unittest.TestCase):
    """ask() against a lobby on 127.0.0.1 that answers as an official one does: REJECT with the JOIN's nonce."""

    def lobby(self, answer):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        s.settimeout(0.1)
        seen = []
        stop = threading.Event()

        def run():
            while not stop.is_set():
                try:
                    p, at = s.recvfrom(2048)
                except socket.timeout:
                    continue
                except OSError:
                    return
                seen.append(p)
                reply = answer(p, len(seen))
                if reply:
                    s.sendto(reply, at)
        t = threading.Thread(target=run, daemon=True)
        t.start()
        self.addCleanup(lambda: (stop.set(), t.join(1), s.close()))
        return s.getsockname()[1], seen

    def ask(self, port, samples=sr.SAMPLES):
        regions, sr.REGIONS = sr.REGIONS, (("eu", "EU", f"127.0.0.1:{port}"),) + sr.REGIONS[1:]
        try:
            return sr.ask("eu", samples)
        finally:
            sr.REGIONS = regions

    def test_reject_is_the_answer(self):
        port, seen = self.lobby(lambda p, n: b"SN\x02\x03" + p[4:12] + b"\x08" + b"update OpenSmash".ljust(64, b"\0"))
        got = self.ask(port)
        self.assertEqual(len(got), sr.SAMPLES)
        self.assertTrue(all(0 <= ms < 200 for ms in got), got)
        self.assertEqual(len(seen), sr.SAMPLES)
        join = seen[0]   # (a JOIN of version 2 as runtime/net_relay.c's: nothing of the player in it, no ticket)
        self.assertEqual((join[:4], len(join), join[12:17], join[44:48], join[76:]), (b"SN\x02\x01", 4 + 8 + 32 + 32 + 40, b"smash", b"ping", bytes(40)))

    def test_welcome_is_left(self):
        port, seen = self.lobby(lambda p, n: b"SN\x02\x02" + p[4:12] + bytes(13) if p[3] == 1 else None)
        got = self.ask(port)
        self.assertEqual(len(got), 1)   # (one time is enough there)
        deadline = time.monotonic() + 1
        while len(seen) < 2 and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual((seen[1][:4], seen[1][4:12]), (b"SN\x02\x08", seen[0][4:12]))   # (LEAVE with its nonce)

    def test_another_nonce_is_not_an_answer(self):
        port, _seen = self.lobby(lambda p, n: b"SN\x02\x03" + bytes(8) + b"\x08" + bytes(64))
        self.assertEqual(self.ask(port), [])

    def test_one_lost_question(self):
        port, _seen = self.lobby(lambda p, n: None if n == 1 else b"SN\x02\x03" + p[4:12] + b"\x08" + bytes(64))
        self.assertEqual(len(self.ask(port)), sr.SAMPLES - 1)

    def test_nothing_there(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        start = time.monotonic()
        self.assertEqual(self.ask(port), [])
        self.assertLess(time.monotonic() - start, 2 * sr.SAMPLE_WAIT + 0.5)   # (asked twice at most)


if __name__ == "__main__":
    unittest.main()
