"""Online matches played, in every mode, kept by the public lobby for its /status.json (the site, the Discord status
bot): totals of all time, of today (UTC) and per mode. Nothing about any player is kept: a match is counted, not who
played it. Kept in nex_matches.json beside the board's files; tools/nex/reset_board.py never empties it.

Where a match is learnt, one source a match:
  - POST /match/played from the game (v0.4.1 on, runtime/net_results.c), each game of the match sending the same
    match id: a match played over Epic Online Services (whose game server is each game's own, never the lobby's) and a
    Local Wireless room's. "exact": the id is the rollback handshake's match seed (hle_rb_core.c), the same on every
    game of that one match: counted once, whoever sends it first. Not exact (the game's own netcode, no seed): the id is
    the players' addresses and the mode, the same for the next match of the same players, so a report of it within
    WINDOW seconds of the first is the same match.
    A report of a Quickplay match played on the lobby's own game server ("via": "lobby") is not counted: that match
    is counted below, for every version of the game. A Battle Arena match is counted from the reports alone, over
    the lobby as over EOS: its games post no DataStore 21 (none in the v0.4.1 and #458 arena pairs, two in every
    Quickplay pair), so refusing them left the arenas' count at 0.
  - The lobby's own game server: DataStore method 21 (PostMetaBinary) is posted by every game of a Quickplay match
    as it is set up (seen on both games of every Quickplay match of the v0.4.0 regression runs); the posts of one
    gathering within LOBBY_WINDOW seconds are one match. One in an arena's gathering is not counted (the reports
    count the arenas: one source a match).
  - Estimated, v0.4.0 over EOS (which sends no report): a /gsp/sync from a game that sends no reports (its body has no
    "m") follows each fighter record the game posts - one of every player as a match is set up, and after the fight
    from some of them - so every second such sync (one a player at most every LEGACY_EVERY seconds) is counted as a
    match, kept apart as "estimated"."""
import json
import os
import re
import time

from .server import JsonStore

MODES = ("quickplay", "arena", "room")
WINDOW = 60.0            # seconds: a report not exact with the id of one counted this recently is the same match
EXACT_KEEP = 24 * 3600   # seconds an id is remembered
MAX_SEEN = 200000        # ids remembered at most (the oldest go first)
DAYS_KEPT = 62           # days of "today" counts kept
LOBBY_WINDOW = 45.0      # seconds: DataStore 21 posts of one gathering within this are one match
REPORT_EVERY = 15.0      # seconds between two reports of one player (429 sooner)
LEGACY_EVERY = 60.0      # seconds: an old game's syncs closer than this are one
MAX_SECONDS = 3600       # a match's length kept at most
HEX16 = re.compile(r"[0-9a-f]{16}")


def day_of(when):
    return time.strftime("%Y-%m-%d", time.gmtime(when))


class Matches(JsonStore):
    def __init__(self, state_dir, clock=time.time):
        super().__init__(os.path.join(state_dir, "nex_matches.json") if state_dir else None)
        self.clock = clock
        self.data = {"total": 0, "by_mode": {}, "by_via": {}, "ranked": 0, "seconds": 0, "days": {},
                     "estimated": 0, "estimated_days": {}, "since": None, "seen": {}}
        loaded = self.load()
        if isinstance(loaded, dict):
            self.data.update(loaded)
        self.report_at = {}      # pid -> when it last reported a match
        self.legacy_at = {}      # pid -> when its last sync counted
        self.lobby_at = {}       # gathering (or pid) -> when its match was counted

    def content(self):
        return self.data

    # ---- counting -----------------------------------------------------------------------------
    def count(self, mode, via, when, ranked=False, seconds=0):
        d = self.data
        d["total"] += 1
        d["by_mode"][mode] = d["by_mode"].get(mode, 0) + 1
        d["by_via"][via] = d["by_via"].get(via, 0) + 1
        d["ranked"] += 1 if ranked else 0
        d["seconds"] += max(0, min(int(seconds), MAX_SECONDS))
        day = day_of(when)
        d["days"][day] = d["days"].get(day, 0) + 1
        if d["since"] is None:
            d["since"] = day
        self.prune(when)
        self.dirty = True

    def prune(self, when):
        d = self.data
        for key in ("days", "estimated_days"):
            if len(d[key]) > DAYS_KEPT:
                for day in sorted(d[key])[:-DAYS_KEPT]:
                    del d[key][day]
        seen = d["seen"]
        if len(seen) > MAX_SEEN or d["total"] % 256 == 0:   # (now and then: the old ids out, at most MAX_SEEN / 2 kept)
            keep = sorted(((t, i) for i, t in seen.items() if when - t <= EXACT_KEEP), reverse=True)[:MAX_SEEN // 2]
            d["seen"] = {i: t for t, i in keep}
        for m in (self.report_at, self.legacy_at, self.lobby_at):
            if len(m) > 4096:
                for k in [k for k, t in m.items() if when - t > max(REPORT_EVERY, LEGACY_EVERY, LOBBY_WINDOW)]:
                    del m[k]

    @staticmethod
    def parse(data):
        """The report of a data string, or None: {"id": 16 lowercase hex, "mode": one of MODES, "players": 2-8,
        "seconds": >= 0, "ranked": bool, "net": "rollback"|"game", "via": "eos"|"lobby", "exact": bool}."""
        try:
            d = json.loads(data)
        except ValueError:
            return None
        if not isinstance(d, dict):
            return None
        ok = (isinstance(d.get("id"), str) and HEX16.fullmatch(d["id"]) and d.get("mode") in MODES
              and type(d.get("players")) is int and 2 <= d["players"] <= 8
              and type(d.get("seconds")) is int and d["seconds"] >= 0
              and isinstance(d.get("ranked"), bool) and isinstance(d.get("exact"), bool)
              and d.get("net") in ("rollback", "game") and d.get("via") in ("eos", "lobby"))
        return d if ok else None

    def report(self, pid, data):
        """(status, body) of a game's report of a match it played (pid: the signed player)."""
        when = self.clock()
        d = self.parse(data)
        if d is None:
            return 400, b'{"error":"bad data"}'
        last = self.report_at.get(pid)
        if last is not None and when - last < REPORT_EVERY:
            return 429, b'{"error":"too many reports"}'
        self.report_at[pid] = when
        if d["via"] == "lobby" and d["mode"] == "quickplay":   # (DataStore 21 counts it; an arena posts none)
            return 200, b'{"ok":true,"counted":false,"why":"the lobby counts it"}'
        key = ("x" if d["exact"] else "w") + d["id"]
        seen = self.data["seen"].get(key)
        if seen is not None and (d["exact"] or when - seen < WINDOW):
            return 200, b'{"ok":true,"counted":false,"why":"already counted"}'
        self.data["seen"][key] = when
        self.count(d["mode"], d["via"], when, d["ranked"], d["seconds"])
        return 200, b'{"ok":true,"counted":true}'

    def lobby_post(self, key, mode, when=None):
        """A DataStore 21 post on the lobby's own game server, key its gathering: True when it is a new match."""
        when = self.clock() if when is None else when
        last = self.lobby_at.get(key)
        if last is not None and when - last < LOBBY_WINDOW:
            return False
        self.lobby_at[key] = when
        self.count(mode, "lobby", when, ranked=mode == "quickplay")
        return True

    def legacy_sync(self, pid, when=None):
        """A /gsp/sync with records from a game that sends no reports: True when it counted (half a match)."""
        when = self.clock() if when is None else when
        last = self.legacy_at.get(pid)
        if last is not None and when - last < LEGACY_EVERY:
            return False
        self.legacy_at[pid] = when
        d = self.data
        d["estimated"] += 1
        day = day_of(when)
        d["estimated_days"][day] = d["estimated_days"].get(day, 0) + 1
        if d["since"] is None:
            d["since"] = day
        self.prune(when)
        self.dirty = True
        return True

    # ---- what /status.json shows ------------------------------------------------------------------
    def summary(self, when=None):
        """{"total", "today", "by_mode", "estimated", "since"}: total and today include the estimate (old games' syncs,
        two of them a match); by_mode is what was counted exactly."""
        when = self.clock() if when is None else when
        d, day = self.data, day_of(when)
        est, est_today = d["estimated"] // 2, d["estimated_days"].get(day, 0) // 2
        return {"total": d["total"] + est, "today": d["days"].get(day, 0) + est_today,
                "by_mode": {m: d["by_mode"].get(m, 0) for m in MODES}, "estimated": est, "since": d["since"]}
