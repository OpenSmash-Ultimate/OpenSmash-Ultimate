"""The GSP leaderboard and the player's own page: what the game server keeps of each player's fighter records
(services.Powers, DataStore 82 / 84) given to the launchers, the game's Discord Rich Presence and the project's site,
over HTTP on the online lobby's TCP port (tools/ssbu_online_lobby.py). Read-only but for what a player sends: the
name and Discord account they register with, and (from a game that plays over Epic Online Services, whose own NEX
server the lobby never sees) the results it uploads.

A player is "registered" when their profile has both a name (their Discord display name) and a hash of their Discord
account. Only registered players are on the leaderboard and every position is counted among them; there are no
anonymous rows.

    GET  /leaderboard.json   every registered ranked player: an opaque id, name, Discord hash, position, best GSP and
                             GSP per fighter with each fighter's position. No address, NSA id, PID, in-game name or
                             lobby code.
    GET  /status.json        aggregate counts only: players online, rooms (gatherings with a player), arenas, rooms
                             with room left, registered players ranked. No player data at all.
    GET  /gsp/me             the asking player's own: the same, plus where they play now (Quickplay or a Battle Arena:
                             its ID, players, room) for Discord's Rich Presence. Signed (below). Position 0 (and
                             fighter positions 0) while not registered; their own GSP is still shown.
    GET  /gsp/records        {"records": [hex, ...]}: the asking player's fighter records, 25 bytes each. Signed.
    POST /gsp/profile        {"player", "t", "name", "discord", "sig", "key"?}: the name to show ("" none) and a hash
                             of the Discord account (the site's account page finds the player by it). Signed. The
                             profile is dropped only when both are empty.
    POST /gsp/sync           {"player", "t", "data", "sig", "key"?}: results of a game with its own NEX server. data is
                             a JSON string {"records": [hex of a fighter record, ...] (128 at most), "ranked":
                             [{"category", "rating", "win", "loss", "draw", "disconnect"}, ...] (128 at most)}, signed
                             as is. 200 {"ok", "records", "ranked"}; 400 for a bad body; 413 for a body over 64 KiB.

A player the lobby does not know is refused (403 {"error":"unknown player"}) unless the request carries "key": the
NSA id (16 lowercase hex digits, whose player_id is the player), with which the account is made first (503 when no
new one may be made now); the signature is checked as usual.

A player is named by player_id(nsa): a hash of their install's NSA id (runtime/hle_env.c), which only their own
game and launchers know. A signed request proves it without sending it: X-OpenSmash-Player (the id),
X-OpenSmash-Time (unix seconds) and X-OpenSmash-Sig, hex HMAC-SHA256 with the NSA id (its 16 hex digits, as text)
as the key over "opensmash1\\n<what>\\n<player>\\n<time>\\n<payload>" (what: "me", "records", "profile" or "sync";
payload: "", "", "<name>\\n<discord>" or the data string). A time more than MAX_SKEW seconds off is refused."""
import hashlib
import hmac
import json
import os
import time

from .ranked import clamp_rating
from .server import JsonStore
from .services import POWER_RECORD

MAX_SKEW = 300            # seconds a signed request's time may be off
MAX_SHOWN_NAME = 32       # characters of a shown name
MAX_PROFILES = 200000
MAX_LISTED = 5000         # players in /leaderboard.json at most (the best)
BOARD_EVERY = 30.0        # seconds the leaderboard is kept before it is made again
MAX_SYNC_ITEMS = 128      # records, and ranked entries, in one /gsp/sync
MAX_SYNC_BODY = 65536     # bytes of a /gsp/sync body
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
        if not name and not discord:
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

    def registered(self, pid):
        """A player who shows a name and has linked a Discord account."""
        p = self.profiles.of_pid.get(str(pid)) or {}
        return bool(p.get("name") and p.get("discord"))

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
            if recs and self.registered(pid):
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
            shown = self.profiles.of_pid[str(pid)]
            row["name"], row["discord"] = shown["name"], shown["discord"]
            players.append(row)
        body = json.dumps({"updated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "ranked": len(overall),
                           "players": players}, separators=(",", ":")).encode()
        self.cache = (now, body, positions, len(overall))
        return self.cache[1:]

    def leaderboard(self, now):
        return self.ranks(now)[0]

    # ---- the lobby's activity -----------------------------------------------------------------------
    def status(self, now):
        """Aggregate counts for the project's site: nothing about any player."""
        mm, arenas = getattr(self.nex, "matchmaking", None), getattr(self.nex, "arenas", None)
        rooms = arena_rooms = open_rooms = in_matches = 0
        for ms in (mm.sessions.values() if mm is not None else ()):
            n = len(ms.participants)
            if n == 0:
                continue
            rooms += 1
            if arenas is not None and arenas.id_of.get(ms.gid, ""):
                arena_rooms += 1
            if ms.open and n < ms.max_participants:
                open_rooms += 1
            if n >= 2:
                in_matches += n
        return {"updated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "online": len(self.nex.by_pid),
                "rooms": rooms, "arenas": arena_rooms, "open_rooms": open_rooms, "playing": in_matches,
                "ranked": self.ranks(now)[2]}

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

    # ---- results uploaded by a game with its own NEX server --------------------------------------------
    def register(self, player, key, now):
        """Makes the account of NSA id key when it is player's: True when player is known after it, False when key is
        not that id, None when no new account may be made now."""
        if not isinstance(key, str) or len(key) != 16 or any(c not in "0123456789abcdef" for c in key):
            return False
        if player_id(key) != player:
            return False
        if self.nex.accounts.login(key, "", now) is None:
            return None
        self.index(now, force=True)
        return True

    def refusal(self, player, key, now):
        """None when player is known (or has just been registered from key), else the (status, body) of the refusal."""
        if not isinstance(player, str):
            return 403, b'{"error":"not signed by a player this server knows"}'
        if player not in self.by_player:
            self.index(time.monotonic())
        if player in self.by_player:
            return None
        if key is None:
            return 403, b'{"error":"unknown player"}'
        got = self.register(player, key, now)
        if got is None:
            return 503, b'{"error":"too many new accounts"}'
        if not got:
            return 403, b'{"error":"unknown player"}'
        return None

    @staticmethod
    def parse_sync(data):
        """([fighter record bytes], [(category, rating, win, loss, draw, disconnect)]) of a /gsp/sync data string, or
        None when it is not what it should be."""
        try:
            d = json.loads(data)
        except ValueError:
            return None
        if not isinstance(d, dict):
            return None
        recs, ranked = d.get("records", []), d.get("ranked", [])
        if not isinstance(recs, list) or not isinstance(ranked, list) or len(recs) > MAX_SYNC_ITEMS or len(ranked) > MAX_SYNC_ITEMS:
            return None
        out_recs, out_ranked = [], []
        for h in recs:
            try:
                raw = bytes.fromhex(h)
            except (TypeError, ValueError):
                return None
            if len(raw) != POWER_RECORD:
                return None
            out_recs.append(raw)
        for e in ranked:
            if not isinstance(e, dict):
                return None
            vals = []
            for f in ("category", "rating", "win", "loss", "draw", "disconnect"):
                v = e.get(f)
                if type(v) is not int or v < 0:
                    return None
                vals.append(min(v, 0xFFFFFFFF))
            out_ranked.append(tuple(vals))
        return out_recs, out_ranked

    def sync(self, pid, recs, ranked):
        """Keeps uploaded results: (records stored, ranked entries stored)."""
        stored = sum(1 for raw in recs if self.nex.services.powers.put(pid, raw))
        stats, kept = self.nex.ranked, 0
        for category, rating, win, loss, draw, disconnect in ranked:
            e = stats.entry(pid, category)
            if e is None:
                continue
            e.update(ratingValue=clamp_rating(rating), totalWin=win, totalLoss=loss, totalDraw=draw,
                     totalDisconnect=disconnect)
            kept += 1
        if kept:
            stats.dirty = True
        self.cache = None
        return stored, kept

    def flush(self, now, force=False):
        self.profiles.flush(now, force)

    # ---- HTTP (tools/ssbu_online_lobby.py hands each request here) --------------------------------
    def http(self, method, path, headers, body, now=None):
        """(status, JSON bytes) for one request. headers: lower-case names."""
        now = time.monotonic() if now is None else now
        if method == "GET" and path == "/leaderboard.json":
            return 200, self.leaderboard(now)
        if method == "GET" and path == "/status.json":
            return 200, json.dumps(self.status(now), separators=(",", ":")).encode()
        if method == "GET" and path == "/gsp/me":
            pid = self.verify(headers.get("x-opensmash-player"), headers.get("x-opensmash-time"),
                              headers.get("x-opensmash-sig"), "me")
            if pid is None:
                return 403, b'{"error":"not signed by a player this server knows"}'
            return 200, json.dumps(self.me(pid, now), separators=(",", ":")).encode()
        if method == "GET" and path == "/gsp/records":
            pid = self.verify(headers.get("x-opensmash-player"), headers.get("x-opensmash-time"),
                              headers.get("x-opensmash-sig"), "records")
            if pid is None:
                return 403, b'{"error":"not signed by a player this server knows"}'
            return 200, json.dumps({"records": [raw.hex() for _f, raw in self.nex.services.powers.records(pid)]},
                                   separators=(",", ":")).encode()
        if method == "POST" and path == "/gsp/sync":
            if len(body) > MAX_SYNC_BODY:
                return 413, b'{"error":"too large"}'
            try:
                d = json.loads(body.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                return 400, b'{"error":"not JSON"}'
            if not isinstance(d, dict) or not isinstance(d.get("data"), str):
                return 400, b'{"error":"not a sync"}'
            refused = self.refusal(d.get("player"), d.get("key"), now)
            if refused:
                return refused
            pid = self.verify(d.get("player"), d.get("t"), d.get("sig"), "sync", d["data"])
            if pid is None:
                return 403, b'{"error":"not signed by a player this server knows"}'
            got = self.parse_sync(d["data"])
            if got is None:
                return 400, b'{"error":"bad data"}'
            stored, kept = self.sync(pid, *got)
            return 200, json.dumps({"ok": True, "records": stored, "ranked": kept}).encode()
        if method == "POST" and path == "/gsp/profile":
            try:
                d = json.loads(body.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                return 400, b'{"error":"not JSON"}'
            if not isinstance(d, dict) or not isinstance(d.get("name", ""), str) or not isinstance(d.get("discord", ""), str):
                return 400, b'{"error":"not a profile"}'
            name, discord = d.get("name", ""), d.get("discord", "")
            refused = self.refusal(d.get("player"), d.get("key"), now)
            if refused:
                return refused
            pid = self.verify(d.get("player"), d.get("t"), d.get("sig"), "profile", f"{name}\n{discord}")
            if pid is None:
                return 403, b'{"error":"not signed by a player this server knows"}'
            if not self.set_profile(pid, name, discord):
                return 503, b'{"error":"too many profiles"}'
            return 200, json.dumps({"ok": True, "name": shown_name(name)}).encode()
        if path in ("/leaderboard.json", "/status.json", "/gsp/me", "/gsp/records", "/gsp/profile", "/gsp/sync"):
            return 405, b'{"error":"method not allowed"}'
        return 404, b'{"error":"not found"}'

