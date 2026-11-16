"""The GSP leaderboard and the player's own page: what the game server keeps of each player's fighter records
(services.Powers, DataStore 82 / 84) given to the launchers, the game's Discord Rich Presence and the project's site,
over HTTP on the online lobby's TCP port (tools/ssbu_online_lobby.py). Read-only but for one thing: a player may
choose to show a name (their Discord display name) on the leaderboard.

    GET  /leaderboard.json   every ranked player: an opaque id, position, best GSP and GSP per fighter with each
                             fighter's position; a name only for a player who chose to show one. No address, NSA id,
                             PID, in-game name or lobby code.
    GET  /gsp/me             the asking player's own: the same, plus where they play now (Quickplay or a Battle Arena:
                             its ID, players, room) for Discord's Rich Presence. Signed (below).
    POST /gsp/profile        {"player", "t", "name", "discord", "sig"}: the name to show ("" none) and a hash of the
                             Discord account (the site's account page finds the player by it). Signed.

A player is named by player_id(nsa): a hash of their install's NSA id (runtime/hle_env.c), which only their own
game and launchers know. A signed request proves it without sending it: X-OpenSmash-Player (the id),
X-OpenSmash-Time (unix seconds) and X-OpenSmash-Sig, hex HMAC-SHA256 with the NSA id (its 16 hex digits, as text)
as the key over "opensmash1\\n<what>\\n<player>\\n<time>\\n<payload>" (what: "me" or "profile"; payload: "" or
"<name>\\n<discord>"). A time more than MAX_SKEW seconds off is refused."""
import hashlib
import hmac
import json
import os
import time

from .server import JsonStore

MAX_SKEW = 300            # seconds a signed request's time may be off
MAX_SHOWN_NAME = 32       # characters of a shown name
MAX_PROFILES = 200000
MAX_LISTED = 5000         # players in /leaderboard.json at most (the best)
BOARD_EVERY = 30.0        # seconds the leaderboard is kept before it is made again
INDEX_EVERY = 10.0        # seconds between two rebuilds of the player id index (a lookup that misses asks for one)


def player_id(nsa):
    """The id a player is known by outside the game server: 16 hex digits, from the NSA id (not back to it)."""
    return hashlib.sha256(("opensmash-player\n" + nsa).encode()).hexdigest()[:16]


def signature(nsa, what, player, t, payload=""):
    msg = f"opensmash1\n{what}\n{player}\n{int(t)}\n{payload}".encode("utf-8")
    return hmac.new(nsa.encode(), msg, hashlib.sha256).hexdigest()


def shown_name(name):
    """A name as the leaderboard shows it: printable, one line, MAX_SHOWN_NAME characters."""
    name = "".join(c for c in str(name) if c.isprintable()).strip()
    return name[:MAX_SHOWN_NAME]


def record_gsp(raw):
    """A fighter record's GSP: its first u32 after the fighter (DataStore 82's {u8 fighter, u32, u32, u32, DateTime,
    u32}; seen 1460 at a match's set-up, 1540 after a win)."""
    return int.from_bytes(raw[1:5], "little") if len(raw) >= 5 else 0


class Profiles(JsonStore):
    """pid -> {"name": shown name, "discord": hash}: what a player chose to show."""

    def __init__(self, state_dir):
        super().__init__(os.path.join(state_dir, "nex_profiles.json") if state_dir else None)
        self.of_pid = self.load() or {}

    def content(self):
        return self.of_pid

    def set(self, pid, name, discord):
        key = str(pid)
        if not name:
            if self.of_pid.pop(key, None) is not None:
                self.dirty = True
            return True
        if key not in self.of_pid and len(self.of_pid) >= MAX_PROFILES:
            return False
        self.of_pid[key] = {"name": name, "discord": discord}
        self.dirty = True
        return True


class Board:
    def __init__(self, nex):
        self.nex = nex
        self.profiles = Profiles(getattr(nex, "state_dir", None))
        self.by_player = {}      # player id -> (pid, NSA id)
        self.player_of = {}      # pid -> player id
        self.indexed_users = -1
        self.indexed_at = None
        self.cache = None        # (when, leaderboard bytes, {pid: (rank, {fighter: rank})}, ranked)

    # ---- who is who -----------------------------------------------------------------------------
    def index(self, now, force=False):
        users = self.nex.accounts.data["users"]
        if not force and len(users) == self.indexed_users:
            return
        if not force and self.indexed_at is not None and now - self.indexed_at < INDEX_EVERY:
            return
        self.indexed_at, self.indexed_users = now, len(users)
        self.by_player, self.player_of = {}, {}
        for nsa, u in users.items():
            p = player_id(nsa)
            self.by_player[p] = (u["pid"], nsa)
            self.player_of[u["pid"]] = p

    def verify(self, player, t, sig, what, payload="", now=None):
        """The pid of a signed request, or None (an unknown player, a time too far off, a wrong signature)."""
        now = time.time() if now is None else now
        try:
            t = int(t)
        except (TypeError, ValueError):
            return None
        if not isinstance(player, str) or not isinstance(sig, str) or abs(now - t) > MAX_SKEW:
            return None
        if player not in self.by_player:
            self.index(time.monotonic())
        known = self.by_player.get(player)
        if known is None:
            return None
        pid, nsa = known
        return pid if hmac.compare_digest(signature(nsa, what, player, t, payload), sig.lower()) else None

    # ---- the leaderboard ------------------------------------------------------------------------
    def records(self, pid):
        """{fighter: GSP} of a player's records (GSP 0 left out)."""
        out = {}
        for fighter, raw in self.nex.services.powers.records(pid):
            g = record_gsp(raw)
            if g > 0:
                out[fighter] = g
        return out

    def ranks(self, now):
        """(leaderboard JSON bytes, {pid: (position, {fighter: position})}, players ranked), made again every
        BOARD_EVERY seconds."""
        if self.cache is not None and now - self.cache[0] < BOARD_EVERY:
            return self.cache[1:]
        self.index(now, force=True)
        of = {}
        for key in list(self.nex.services.powers.of_pid):
            try:
                pid = int(key)
            except ValueError:
                continue
            recs = self.records(pid)
            if recs:
                of[pid] = recs
        overall = sorted(of, key=lambda pid: (-max(of[pid].values()), pid))
        by_fighter = {}
        for pid, recs in of.items():
            for fighter, g in recs.items():
                by_fighter.setdefault(fighter, []).append((g, pid))
        fighter_rank = {}
        for fighter, rows in by_fighter.items():
            rows.sort(key=lambda r: (-r[0], r[1]))
            for i, (_g, pid) in enumerate(rows):
                fighter_rank[(fighter, pid)] = i + 1
        positions, players = {}, []
        for i, pid in enumerate(overall):
            recs = of[pid]
            mine = {f: fighter_rank[(f, pid)] for f in recs}
            positions[pid] = (i + 1, mine)
            if i >= MAX_LISTED or pid not in self.player_of:
                continue
            best = max(recs, key=lambda f: (recs[f], -f))
            row = {"id": self.player_of[pid], "rank": i + 1, "gsp": recs[best], "fighter": best,
                   "fighters": {str(f): {"gsp": g, "rank": mine[f]} for f, g in sorted(recs.items())}}
            shown = self.profiles.of_pid.get(str(pid))
            if shown:
                row["name"] = shown.get("name", "")
                if shown.get("discord"):
                    row["discord"] = shown["discord"]
            players.append(row)
        body = json.dumps({"updated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "ranked": len(overall),
                           "players": players}, separators=(",", ":")).encode()
        self.cache = (now, body, positions, len(overall))
        return self.cache[1:]

    def leaderboard(self, now):
        return self.ranks(now)[0]

    # ---- a player's own --------------------------------------------------------------------------
    def where(self, pid):
        """Where the player plays now, as the game server knows it: a gathering they are in (a Battle Arena when it
        has an arena ID, else Quickplay), or None."""
        mm, arenas = getattr(self.nex, "matchmaking", None), getattr(self.nex, "arenas", None)
        if mm is None:
            return None
        for ms in mm.sessions.values():
            if pid in ms.participants:
                arena = arenas.id_of.get(ms.gid, "") if arenas is not None else ""
                return {"mode": "arena" if arena else "quickplay", "arena": arena, "players": len(ms.participants),
                        "max": ms.max_participants, "open": bool(ms.open)}
        return None

    def me(self, pid, now):
        _body, positions, ranked = self.ranks(now)
        rank, mine = positions.get(pid, (0, {}))
        recs = self.records(pid)   # (newer than the leaderboard's, which is up to BOARD_EVERY old)
        last = self.nex.services.powers.last_of.get(pid)
        shown = self.profiles.of_pid.get(str(pid)) or {}
        return {"id": self.player_of.get(pid, ""), "rank": rank, "ranked": ranked,
                "fighters": [{"fighter": f, "gsp": g, "rank": mine.get(f, 0)}
                             for f, g in sorted(recs.items(), key=lambda r: (-r[1], r[0]))],
                "fighter": last if last is not None else -1, "gsp": recs.get(last, 0) if last is not None else 0,
                "name": shown.get("name", ""), "where": self.where(pid)}

    def set_profile(self, pid, name, discord):
        name = shown_name(name)
        discord = discord if isinstance(discord, str) and len(discord) <= 64 and all(c in "0123456789abcdef" for c in discord) else ""
        ok = self.profiles.set(pid, name, discord)
        if ok:
            self.cache = None
            self.nex.info(f"{self.nex.accounts.name(pid)} {'shows a name on' if name else 'hides from'} the leaderboard")
        return ok

    def flush(self, now, force=False):
        self.profiles.flush(now, force)

    # ---- HTTP (tools/ssbu_online_lobby.py hands each request here) --------------------------------
    def http(self, method, path, headers, body, now=None):
        """(status, JSON bytes) for one request. headers: lower-case names."""
        now = time.monotonic() if now is None else now
        if method == "GET" and path == "/leaderboard.json":
            return 200, self.leaderboard(now)
        if method == "GET" and path == "/gsp/me":
            pid = self.verify(headers.get("x-opensmash-player"), headers.get("x-opensmash-time"),
                              headers.get("x-opensmash-sig"), "me")
            if pid is None:
                return 403, b'{"error":"not signed by a player this server knows"}'
            return 200, json.dumps(self.me(pid, now), separators=(",", ":")).encode()
        if method == "POST" and path == "/gsp/profile":
            try:
                d = json.loads(body.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                return 400, b'{"error":"not JSON"}'
            if not isinstance(d, dict) or not isinstance(d.get("name", ""), str) or not isinstance(d.get("discord", ""), str):
                return 400, b'{"error":"not a profile"}'
            name, discord = d.get("name", ""), d.get("discord", "")
            pid = self.verify(d.get("player"), d.get("t"), d.get("sig"), "profile", f"{name}\n{discord}")
            if pid is None:
                return 403, b'{"error":"not signed by a player this server knows"}'
            if not self.set_profile(pid, name, discord):
                return 503, b'{"error":"too many profiles"}'
            return 200, json.dumps({"ok": True, "name": shown_name(name)}).encode()
        if path in ("/leaderboard.json", "/gsp/me", "/gsp/profile"):
            return 405, b'{"error":"method not allowed"}'
        return 404, b'{"error":"not found"}'

