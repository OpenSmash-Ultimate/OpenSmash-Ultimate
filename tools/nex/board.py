"""The GSP leaderboard and the player's own page: what the game server keeps of each player's fighter records
(services.Powers, DataStore 82 / 84) given to the launchers, the game's Discord Rich Presence and the project's site,
over HTTP on the online lobby's TCP port (tools/ssbu_online_lobby.py). Read-only but for what a player sends: the
Discord account they link, and (from a game that plays over Epic Online Services, whose own NEX
server the lobby never sees) the results it uploads.

A player is "registered" when their install is linked to a Discord account (POST /gsp/link: the lobby asks Discord
once whether the access token is real) and they show their name. GSP and ranking belong to the Discord account, not to
the install: every pid linked to the same Discord id is ONE player (the identity). Merge rule: per fighter, the record
with the HIGHEST GSP among the identity's pids; the merged view is computed, nothing is copied, so an install that
links later joins at once, and a new PC that links gets the records back from /gsp/records. An unlinked pid keeps its
own records, unranked. Only registered identities are on the leaderboard and every position is counted among them; there
are no anonymous rows. The row's name is the latest-linked (or latest show-toggled) pid's Discord name and its choice
to show; the id is the account hash (sha256("opensmash-discord\\n" + Discord id), first 16 hex).

    GET  /leaderboard.json   every registered ranked player: the account hash as id, Discord name, the same hash as
                             "discord", the Discord profile picture's URL as "avatar" (cdn.discordapp.com; it carries
                             the Discord id, so only players who show their name are listed with it), position, best
                             GSP and GSP per fighter with each fighter's position. No address, NSA id, PID, in-game
                             name or lobby code.
    GET  /status.json        aggregate counts only: players online, rooms (gatherings with a player), arenas, rooms
                             with room left, registered players ranked, and "matches": online matches played
                             {"total", "today", "by_mode", "estimated", "since"} (nex/matches.py). No player data at all.
    GET  /gsp/me             the asking player's own: the same, plus where they play now (Quickplay or a Battle Arena:
                             its ID, players, room) for Discord's Rich Presence. Signed (below). A linked player gets
                             the identity's merged GSP and position; an unlinked one their own GSP and position 0.
    GET  /gsp/records        {"records": [hex, ...]}: the asking player's fighter records, 25 bytes each (the
                             identity's merged ones when linked). Signed.
    POST /gsp/link           {"player", "t", "sig", "key"?, "token", "show"}: links the install to the Discord account
                             of access token "token" (what=link, payload "<token>\\n<1|0 show>"). After the signature
                             checks the lobby asks GET discord.com/api/v10/users/@me once (the token is never logged
                             or kept). 200 {"ok", "name", "discord"}; 403 when Discord does not accept the token; 502
                             when Discord is unreachable; 429 when this player linked less than LINK_EVERY s ago.
    POST /gsp/profile        {"player", "t", "name", "discord", "sig", "key"?}: only toggles show (name non-empty: show,
                             "": hide) for an already linked player; the client-sent name and hash are ignored and make
                             nobody registered. Signed. Kept for older launchers and the game.
    POST /gsp/sync           {"player", "t", "data", "sig", "key"?}: results of a game with its own NEX server. data is
                             a JSON string {"records": [hex of a fighter record, ...] (128 at most), "ranked":
                             [{"category", "rating", "win", "loss", "draw", "disconnect"}, ...] (128 at most)}, signed
                             as is. Stored under the pid. 200 {"ok", "records", "ranked"}; 400 for a bad body; 413 for
                             a body over 64 KiB. "m": 1 beside "data" says the game reports its matches (below), so
                             its syncs are not counted as an old game's matches.
    POST /match/played       {"player", "t", "data", "sig", "key"?}: a match this game played online (nex/matches.py
                             says what data holds and how a match is counted once). Signed (what "match"). 200 {"ok",
                             "counted"}; 400 for bad data; 429 when the player reported less than REPORT_EVERY s ago.

A player the lobby does not know is refused (403 {"error":"unknown player"}) unless the request carries "key": the
NSA id (16 lowercase hex digits, whose player_id is the player). That key never makes an account: only the game's
own login on this game server (nex/server.py) does. With a key that is the player's and a good signature, a match
report is counted all the same (nothing is kept of the player) and a sync is answered 200 with nothing kept
("kept": false), so the game does not send it again and again; everything else stays refused.

What a /gsp/sync record counts for on the leaderboard is checked (sync_check); the record itself is kept as sent, so
the game gets back what it sent. A fighter record's GSP (seen 1307..1750 on the live board, +80 for a win) counts up to
RECORD_GSP_MAX; a fighter's first record up to NEW_RECORD_GSP (or the identity's best fighter); a rise above what the
player's identity already has needs credits, one match's worth (RISE_PER_MATCH) each, earned when the lobby hears of a
match the player played from two different games (two players, two addresses: pair_report) or, for an old game that
sends no reports, one every LEGACY_CREDIT_EVERY seconds. What a record may count for is kept (nex_gsp_allowed.json) and
rises as credits come. Every signed request also takes from its address's bucket (IP_RATE, IP_BURST: 429 beyond).

A player is named by player_id(nsa): a hash of their install's NSA id (runtime/hle_env.c), which only their own
game and launchers know. A signed request proves it without sending it: X-OpenSmash-Player (the id),
X-OpenSmash-Time (unix seconds) and X-OpenSmash-Sig, hex HMAC-SHA256 with the NSA id (its 16 hex digits, as text)
as the key over "opensmash1\\n<what>\\n<player>\\n<time>\\n<payload>" (what: "me", "records", "profile", "link",
"sync" or "match"; payload: "", "", "<name>\\n<discord>", "<token>\\n<1|0>" or the data string). A time more than MAX_SKEW seconds off is refused."""
import hashlib
import hmac
import itertools
import json
import os
import re
import time
import urllib.error
import urllib.request

from .matches import Matches
from .ranked import clamp_rating
from .server import Bucket, JsonStore
from .services import POWER_RECORD

MAX_SKEW = 300            # seconds a signed request's time may be off
MAX_SHOWN_NAME = 32       # characters of a shown name
MAX_PROFILES = 200000
MAX_LISTED = 5000         # players in /leaderboard.json at most (the best)
BOARD_EVERY = 30.0        # seconds the leaderboard is kept before it is made again
MAX_SYNC_ITEMS = 128      # records, and ranked entries, in one /gsp/sync
MAX_SYNC_BODY = 65536     # bytes of a /gsp/sync body
LINK_EVERY = 30.0         # seconds between two Discord verifications of one player
MAX_LINK_BODY = 4096
DISCORD_ME = "https://discord.com/api/v10/users/@me"
DISCORD_TIMEOUT = 5       # seconds the lobby waits for Discord
INDEX_EVERY = 10.0        # seconds between two rebuilds of the player id index (a lookup that misses asks for one)
IP_RATE, IP_BURST = 1.0, 30.0   # signed requests a second from one address, and at once (429 beyond)
MAX_IPS = 8192            # addresses whose buckets are kept (in memory only; full ones are dropped first)
RECORD_GSP_MAX = 9999     # a fighter record's GSP above this is not the game's
NEW_RECORD_GSP = 1600     # the most a fighter's first record may say (the game starts a fighter near 1460)
RISE_PER_MATCH = 100      # GSP one fighter may rise per match credited
MAX_CREDITS = 5           # matches' credits a player may hold
CREDIT_EVERY = 60.0       # seconds: one credit at most this often to one player
LEGACY_CREDIT_EVERY = 600.0   # seconds: a game that sends no reports earns one credit this often at most
PAIR_WINDOW = 900.0       # seconds the reports of one match are matched up
MAX_PAIRS = 20000         # matches whose reports are kept to match up (in memory)
ALLOWED_FILE = "nex_gsp_allowed.json"


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


def account_hash(discord_id):
    """The hash of a Discord account id the site and the launchers know it by (tools/weblauncher/discord_account.py)."""
    return hashlib.sha256(("opensmash-discord\n" + discord_id).encode()).hexdigest()[:16]


AVATAR_HASH = re.compile(r"(a_)?[0-9a-f]{32}")
DISCORD_CDN = "https://cdn.discordapp.com/"


def avatar_of(me):
    """What the profile keeps of the profile picture in a /users/@me answer: its hash (lowercase hex, "a_" for an
    animated one) when it has one, else None (Discord's default avatar)."""
    a = me.get("avatar") if isinstance(me, dict) else None
    return a if isinstance(a, str) and AVATAR_HASH.fullmatch(a) else None


def avatar_url(discord_id, avatar, discriminator=None):
    """The URL of a Discord account's profile picture, 64 px: the uploaded one (PNG, also for an animated one) when
    avatar is its hash, else the default avatar Discord shows (index (id >> 22) % 6; discriminator % 5 for a legacy
    account with a discriminator other than "0")."""
    if isinstance(avatar, str) and AVATAR_HASH.fullmatch(avatar):
        return f"{DISCORD_CDN}avatars/{discord_id}/{avatar}.png?size=64"
    if isinstance(discriminator, str) and discriminator.isdigit() and int(discriminator) != 0:
        n = int(discriminator) % 5
    else:
        n = (int(discord_id) >> 22) % 6
    return f"{DISCORD_CDN}embed/avatars/{n}.png"


def discord_lookup(token):
    """GET /users/@me with an access token: the account's JSON, None when Discord does not accept the token, OSError
    when it can not be asked (unreachable, rate limited, down)."""
    req = urllib.request.Request(DISCORD_ME, headers={"Authorization": "Bearer " + token, "User-Agent": "OpenSmash-Lobby"})
    try:
        with urllib.request.urlopen(req, timeout=DISCORD_TIMEOUT) as r:
            d = json.loads(r.read(65536).decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            return None
        raise OSError(f"discord answered {e.code}")
    except (urllib.error.URLError, ValueError) as e:
        raise OSError(str(e))
    return d if isinstance(d, dict) else None


class Profiles(JsonStore):
    """pid -> {"name": Discord name, "discord": account hash, "discord_id": verified Discord id, "avatar": profile
    picture hash or None, "discriminator": legacy discriminator or None, "show": bool, "at": unix time of the last link
    or toggle}: what a player linked and chose to show (a profile from before avatars has neither key: the default
    avatar until the player links again). An entry without discord_id (older
    versions kept a client-sent name and hash) is unverified: not registered."""

    def __init__(self, state_dir):
        super().__init__(os.path.join(state_dir, "nex_profiles.json") if state_dir else None)
        self.of_pid = self.load() or {}
        self.pids_of = {}        # discord id -> [pids linked to it]
        for key in self.of_pid:
            self.index(key)

    def content(self):
        return self.of_pid

    def index(self, key):
        did = (self.of_pid.get(key) or {}).get("discord_id")
        if did and key not in self.pids_of.setdefault(did, []):
            self.pids_of[did].append(key)

    def discord_id(self, pid):
        did = (self.of_pid.get(str(pid)) or {}).get("discord_id")
        return did if isinstance(did, str) and did else None

    def linked_pids(self, pid):
        """Every pid of the same Discord account as pid (pid itself when unlinked)."""
        did = self.discord_id(pid)
        return [int(k) for k in self.pids_of.get(did, ())] if did else [pid]

    def link(self, pid, discord_id, name, show, at, avatar=None, discriminator=None):
        key = str(pid)
        if key not in self.of_pid and len(self.of_pid) >= MAX_PROFILES:
            return False
        old = self.discord_id(pid)
        if old and old != discord_id and key in self.pids_of.get(old, []):
            self.pids_of[old].remove(key)
        self.of_pid[key] = {"name": name, "discord": account_hash(discord_id), "discord_id": discord_id,
                            "avatar": avatar, "discriminator": discriminator, "show": bool(show), "at": at}
        self.index(key)
        for other in self.pids_of.get(discord_id, ()):   # (the account's picture is the newest on every pid of it)
            self.of_pid[other]["avatar"], self.of_pid[other]["discriminator"] = avatar, discriminator
        self.dirty = True
        return True

    def set_show(self, pid, show, at):
        p = self.of_pid.get(str(pid))
        if not p or not self.discord_id(pid):
            return False
        p["show"], p["at"] = bool(show), at
        self.dirty = True
        return True

    def identity(self, did):
        """The profile that speaks for Discord account did: the pid linked or toggled last."""
        best = None
        for key in self.pids_of.get(did, ()):
            p = self.of_pid[key]
            if best is None or (p.get("at", 0), key) > (best.get("at", 0), best_key):
                best, best_key = p, key
        return best


class Allowed(JsonStore):
    """pid -> {fighter: the most GSP that fighter's record counts for}, for records a sync sent above what its credits
    allowed (nothing kept for a record that counts as sent): the record is kept as sent, the leaderboard counts this."""

    def __init__(self, state_dir):
        super().__init__(os.path.join(state_dir, ALLOWED_FILE) if state_dir else None)
        self.of_pid = self.load() or {}

    def content(self):
        return self.of_pid

    def cap(self, pid, fighter):
        v = (self.of_pid.get(str(pid)) or {}).get(str(fighter))
        return v if isinstance(v, int) else RECORD_GSP_MAX

    def set(self, pid, fighter, value):
        """value None: the record counts as sent."""
        key, f = str(pid), str(fighter)
        mine = self.of_pid.get(key)
        if value is None:
            if mine is not None and mine.pop(f, None) is not None:
                if not mine:
                    del self.of_pid[key]
                self.dirty = True
            return
        if mine is None:
            mine = self.of_pid[key] = {}
        if mine.get(f) != value:
            mine[f] = int(value)
            self.dirty = True


class Board:
    def __init__(self, nex, discord_me=discord_lookup):
        self.nex = nex
        self.discord_me = discord_me     # fn(token) -> account JSON | None | raises OSError (tests inject their own)
        self.link_at = {}                # pid -> when it last had a token verified
        self.profiles = Profiles(getattr(nex, "state_dir", None))
        self.matches = Matches(getattr(nex, "state_dir", None))
        self.by_player = {}      # player id -> (pid, NSA id)
        self.player_of = {}      # pid -> player id
        self.indexed_users = -1
        self.indexed_at = None
        self.cache = None        # (when, leaderboard bytes, {pid: (rank, {fighter: rank})}, ranked)
        self.ip_buckets = {}     # address -> Bucket of its signed requests (memory only, never logged)
        self.credits = {}        # pid -> [credits, when one was last given]
        self.allowed = Allowed(getattr(nex, "state_dir", None))
        self.legacy_credit_at = {}   # pid -> when its last no-report sync gave a credit
        self.pairs = {}          # match key -> {"at", "by": {player id: (address, pid or None)}, "paid": set}

    # ---- who is who -----------------------------------------------------------------------------
    def index(self, now, force=False):
        users = self.nex.accounts.data["users"]
        if not force and len(users) == self.indexed_users:
            return
        if not force and 0 <= self.indexed_users < len(users):
            # (accounts are only added, at the end: index the new ones at once, so a game that has just logged in is
            # known to its next request)
            for nsa, u in itertools.islice(users.items(), self.indexed_users, None):
                p = player_id(nsa)
                self.by_player[p] = (u["pid"], nsa)
                self.player_of[u["pid"]] = p
            self.indexed_users = len(users)
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
    def gsp(self, pid, raw):
        """A record's GSP as the leaderboard counts it: what a sync sent, but no more than it was allowed (Allowed)."""
        return min(record_gsp(raw), self.allowed.cap(pid, raw[0]))

    def merged(self, pid, sent=False):
        """{fighter: (GSP, raw record)} of pid's identity: per fighter the record with the highest GSP among the pids
        linked to the same Discord account (pid's own when unlinked); the GSP as counted (sent: as the games sent it).
        Computed each time; nothing is copied between pids."""
        out = {}
        for p in sorted(self.profiles.linked_pids(pid), key=lambda p: p != pid):   # (on a tie the asking pid's own)
            for fighter, raw in self.nex.services.powers.records(p):
                g = record_gsp(raw) if sent else self.gsp(p, raw)
                if fighter not in out or g > out[fighter][0]:
                    out[fighter] = (g, raw)
        return out

    def records(self, pid):
        """{fighter: GSP} of a player's (merged) records as counted (GSP 0 left out)."""
        return {f: g for f, (g, _raw) in self.merged(pid).items() if g > 0}

    def registered(self, pid):
        """A player whose install is linked to a Discord account and who shows their name."""
        return self.profiles.discord_id(pid) is not None and bool((self.profiles.of_pid.get(str(pid)) or {}).get("show"))

    def listed(self, did):
        """The profile that speaks for Discord account did when it shows its name, else None."""
        p = self.profiles.identity(did)
        return p if p is not None and p.get("show") else None

    def ranks(self, now):
        """(leaderboard JSON bytes, {discord id: (position, {fighter: position})}, players ranked), made again every
        BOARD_EVERY seconds. One entry per Discord account that shows its name."""
        if self.cache is not None and now - self.cache[0] < BOARD_EVERY:
            return self.cache[1:]
        self.index(now, force=True)
        of, seen = {}, set()
        for key in list(self.nex.services.powers.of_pid):
            try:
                pid = int(key)
            except ValueError:
                continue
            did = self.profiles.discord_id(pid)
            if did is None or did in seen:
                continue
            seen.add(did)
            recs = self.records(pid)
            if recs and self.listed(did) is not None:
                of[did] = recs
        overall = sorted(of, key=lambda d: (-max(of[d].values()), d))
        by_fighter = {}
        for did, recs in of.items():
            for fighter, g in recs.items():
                by_fighter.setdefault(fighter, []).append((g, did))
        fighter_rank = {}
        for fighter, rows in by_fighter.items():
            rows.sort(key=lambda r: (-r[0], r[1]))
            for i, (_g, did) in enumerate(rows):
                fighter_rank[(fighter, did)] = i + 1
        positions, players = {}, []
        for i, did in enumerate(overall):
            recs = of[did]
            mine = {f: fighter_rank[(f, did)] for f in recs}
            positions[did] = (i + 1, mine)
            if i >= MAX_LISTED:
                continue
            best = max(recs, key=lambda f: (recs[f], -f))
            shown = self.listed(did)
            row = {"id": account_hash(did), "rank": i + 1, "gsp": recs[best], "fighter": best,
                   "fighters": {str(f): {"gsp": g, "rank": mine[f]} for f, g in sorted(recs.items())}}
            row["name"], row["discord"] = shown["name"], shown["discord"]
            row["avatar"] = avatar_url(did, shown.get("avatar"), shown.get("discriminator"))
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
        # (every game connected to the lobby, Local Wireless rooms and the relay too, not only the Online mode's
        # signed-in players: the lobby sets self.lobby; its Local Wireless rooms count as rooms)
        online, lobby = len(self.nex.by_pid), getattr(self, "lobby", None)
        if lobby is not None:
            online = max(online, len(lobby.by_addr) - lobby.pending)
            rooms += len(lobby.listings)
            open_rooms += len(lobby.listings)
        return {"updated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "online": online,
                "rooms": rooms, "arenas": arena_rooms, "open_rooms": open_rooms, "playing": in_matches,
                "ranked": self.ranks(now)[2], "matches": self.matches.summary()}

    def meta_posted(self, pid):
        """(services: DataStore 21, posted by every game of a Quickplay match on this game server as it is set up)
        counted once a gathering a match (nex/matches.py). Not in a Battle Arena's gathering: arenas are counted from
        the games' reports (POST /match/played), and a post there too would count its match twice."""
        mm, arenas = getattr(self.nex, "matchmaking", None), getattr(self.nex, "arenas", None)
        for ms in (mm.sessions.values() if mm is not None else ()):
            if pid in ms.participants:
                if arenas is not None and arenas.id_of.get(ms.gid, ""):
                    return False
                return self.matches.lobby_post(("g", ms.gid), "quickplay")
        return self.matches.lobby_post(("p", pid), "quickplay")

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
        did = self.profiles.discord_id(pid)
        rank, mine = positions.get(did, (0, {})) if did else (0, {})
        recs = self.records(pid)   # (newer than the leaderboard's, which is up to BOARD_EVERY old)
        last = self.nex.services.powers.last_of.get(pid)
        shown = self.profiles.of_pid.get(str(pid)) or {}
        return {"id": account_hash(did) if did else self.player_of.get(pid, ""), "rank": rank, "ranked": ranked,
                "fighters": [{"fighter": f, "gsp": g, "rank": mine.get(f, 0)}
                             for f, g in sorted(recs.items(), key=lambda r: (-r[1], r[0]))],
                "fighter": last if last is not None else -1, "gsp": recs.get(last, 0) if last is not None else 0,
                "name": shown.get("name", "") if did else "", "where": self.where(pid)}

    def set_profile(self, pid, name):
        """The old /gsp/profile: only the choice to show (a name) or hide ("") of an already linked player. The name and
        hash a client sends are ignored: nobody becomes registered by sending them."""
        show = bool(shown_name(name))
        if self.profiles.set_show(pid, show, time.time()):
            self.cache = None
            self.nex.info(f"{self.nex.accounts.name(pid)} {'shows a name on' if show else 'hides from'} the leaderboard")

    # ---- linking an install to a Discord account ---------------------------------------------------
    def link_prepare(self, d, now):
        """(job, None) for a /gsp/link body d that is signed by a known player and may verify now, else (None, (status,
        body)). job: (pid, token, show). Nothing here waits for Discord."""
        token, show = d.get("token"), d.get("show")
        if (not isinstance(token, str) or not 0 < len(token) <= 512 or not token.isprintable()
                or any(c.isspace() for c in token) or not isinstance(show, bool)):
            return None, (400, b'{"error":"not a link"}')
        refused = self.refusal(d.get("player"))
        if refused:
            return None, refused
        pid = self.verify(d.get("player"), d.get("t"), d.get("sig"), "link", token + "\n" + ("1" if show else "0"))
        if pid is None:
            return None, (403, b'{"error":"not signed by a player this server knows"}')
        last = self.link_at.get(pid)
        if last is not None and now - last < LINK_EVERY:
            return None, (429, b'{"error":"too many links"}')
        self.link_at[pid] = now
        if len(self.link_at) > 4096:
            self.link_at = {p: t for p, t in self.link_at.items() if now - t < LINK_EVERY}
        return (pid, token, show), None

    def link_ask(self, token):
        """What Discord says of token: its account JSON, None (refused) or an OSError (not reachable). Blocks: the lobby
        runs it in a thread. The token goes nowhere else."""
        try:
            return self.discord_me(token)
        except Exception as e:
            return e if isinstance(e, OSError) else OSError(type(e).__name__)

    def link_finish(self, job, me):
        """(status, body) of a prepared link once Discord answered me (see link_ask)."""
        pid, _token, show = job
        if isinstance(me, OSError):
            return 502, b'{"error":"discord not reachable"}'
        did = me.get("id") if isinstance(me, dict) else None
        if not isinstance(did, str) or not did.isdigit() or len(did) > 32:
            return 403, b'{"error":"discord did not accept the token"}'
        name = shown_name(me.get("global_name") or me.get("username") or "") or "Player"
        disc = me.get("discriminator")
        disc = disc if isinstance(disc, str) and disc.isdigit() and len(disc) <= 4 else None
        if not self.profiles.link(pid, did, name, show, time.time(), avatar_of(me), disc):
            return 503, b'{"error":"too many profiles"}'
        self.cache = None
        self.nex.info(f"{self.nex.accounts.name(pid)} linked a Discord account")
        return 200, json.dumps({"ok": True, "name": name, "discord": account_hash(did)}).encode()

    def link_request(self, body, now, peer=None):
        """(job, None) or (None, (status, body)) for a raw /gsp/link request: the part before Discord is asked."""
        if not self.ip_allowed(peer, now):
            return None, (429, b'{"error":"too many requests"}')
        if len(body) > MAX_LINK_BODY:
            return None, (413, b'{"error":"too large"}')
        try:
            d = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return None, (400, b'{"error":"not JSON"}')
        if not isinstance(d, dict):
            return None, (400, b'{"error":"not a link"}')
        return self.link_prepare(d, now)

    # ---- results uploaded by a game with its own NEX server --------------------------------------------
    @staticmethod
    def key_signed(player, key, t, sig, what, payload="", now=None):
        """True when key is the NSA id of player and signed this request: proves the game holds that id, makes
        nothing (an unknown player's key never becomes an account here)."""
        now = time.time() if now is None else now
        if not isinstance(key, str) or len(key) != 16 or any(c not in "0123456789abcdef" for c in key):
            return False
        if not isinstance(player, str) or player_id(key) != player or not isinstance(sig, str):
            return False
        try:
            t = int(t)
        except (TypeError, ValueError):
            return False
        if abs(now - t) > MAX_SKEW:
            return False
        return hmac.compare_digest(signature(key, what, player, t, payload), sig.lower())

    def refusal(self, player):
        """None when player is known (its account made by the game's own login here), else the (status, body) of the
        refusal. Nothing is made for an unknown player, whatever key it sends."""
        if not isinstance(player, str):
            return 403, b'{"error":"not signed by a player this server knows"}'
        if player not in self.by_player:
            self.index(time.monotonic())
        if player in self.by_player:
            return None
        return 403, b'{"error":"unknown player"}'

    def ip_allowed(self, peer, now):
        """Whether the address peer may make one more signed request now (no address: a test, or a caller that has
        none - allowed)."""
        if not peer:
            return True
        b = self.ip_buckets.get(peer)
        if b is None:
            if len(self.ip_buckets) >= MAX_IPS:
                self.ip_buckets = {a: k for a, k in self.ip_buckets.items()
                                   if k.at is not None and now - k.at < IP_BURST / IP_RATE}
                if len(self.ip_buckets) >= MAX_IPS:
                    return False
            b = self.ip_buckets[peer] = Bucket(IP_RATE, IP_BURST)
        return b.take(now)

    # ---- what a sync may keep: credits from matches heard of ---------------------------------------------
    def credit(self, pid, now, every=CREDIT_EVERY):
        """One more match's credit to pid (at most one every `every` seconds), spent at once on what its records still
        wait for."""
        c = self.credits.setdefault(pid, [0.0, None])
        if c[1] is not None and now - c[1] < every:
            return False
        c[0], c[1] = min(MAX_CREDITS, c[0] + 1.0), now
        if len(self.credits) > 50000:
            self.credits = {p: v for p, v in self.credits.items() if v[0] > 0 or str(p) in self.allowed.of_pid}
        waiting = self.allowed.of_pid.get(str(pid))
        if waiting:
            sent = dict(self.nex.services.powers.records(pid))
            for fighter in [int(f) for f in waiting]:
                raw, cap = sent.get(fighter), self.allowed.cap(pid, fighter)
                if raw is None:
                    self.allowed.set(pid, fighter, None)
                elif c[0] > 0 and min(record_gsp(raw), RECORD_GSP_MAX) > cap:
                    self.raise_cap(pid, fighter, min(record_gsp(raw), RECORD_GSP_MAX), cap, c)
            self.cache = None
        return True

    def raise_cap(self, pid, fighter, want, base, c):
        """Spends pid's credits c on raising fighter's counted GSP from base toward want."""
        need = (want - base) / RISE_PER_MATCH
        if c[0] >= need:
            c[0] -= need
            self.allowed.set(pid, fighter, None)
        else:
            self.allowed.set(pid, fighter, max(base + int(c[0] * RISE_PER_MATCH + 1e-6), 0))
            c[0] = 0.0

    def sync_check(self, pid, raw, merged, now):
        """What fighter record raw of a sync may count for (the record itself is kept as sent, so the game that sent it
        gets it back unchanged): within RECORD_GSP_MAX, and above what the identity has (merged: as counted) only by
        pid's credits, spent; a record above them waits for later credits. A fighter the identity has no record of starts
        from NEW_RECORD_GSP or its best fighter's GSP."""
        fighter, want = raw[0], min(record_gsp(raw), RECORD_GSP_MAX)
        have = merged.get(fighter)
        base = have[0] if have is not None else max([NEW_RECORD_GSP] + [g for g, _r in merged.values()])
        if want <= base:
            self.allowed.set(pid, fighter, None if want == record_gsp(raw) else want)
            return
        self.raise_cap(pid, fighter, want, base, self.credits.setdefault(pid, [0.0, None]))
        if want < record_gsp(raw) and self.allowed.cap(pid, fighter) > want:
            self.allowed.set(pid, fighter, want)

    def pair_report(self, key, player, peer, pid, now):
        """A report of match key from player (pid None: unknown here) at address peer: credits every reporter of it
        once two different players at two different addresses reported it."""
        if len(self.pairs) >= MAX_PAIRS:
            self.pairs = {k: v for k, v in self.pairs.items() if now - v["at"] < PAIR_WINDOW}
            while len(self.pairs) >= MAX_PAIRS:
                self.pairs.pop(next(iter(self.pairs)))
        e = self.pairs.get(key)
        if e is None or now - e["at"] >= PAIR_WINDOW or player in e["by"]:
            # (a player who reported it already: the next match of the same players, whose id is not exact)
            e = self.pairs[key] = {"at": now, "by": {}, "paid": set()}
        if player not in e["by"] and len(e["by"]) < 8:
            e["by"][player] = (peer, pid)
        by = e["by"]
        for who, (addr, p) in by.items():
            if who in e["paid"]:
                continue
            if any(o != who and a != addr for o, (a, _p) in by.items()):
                e["paid"].add(who)
                if p is not None:
                    self.credit(p, now)

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

    def sync(self, pid, recs, ranked, now):
        """Keeps uploaded results as sent, each record counting as sync_check lets it: (records stored, ranked entries
        stored)."""
        merged = self.merged(pid) if recs else {}
        stored = 0
        for raw in recs:
            self.sync_check(pid, raw, merged, now)
            if self.nex.services.powers.put(pid, raw):
                stored += 1
                g = self.gsp(pid, raw)
                if raw[0] not in merged or g > merged[raw[0]][0]:
                    merged[raw[0]] = (g, raw)
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
        self.allowed.flush(now, force)
        self.matches.flush(now, force)

    # ---- HTTP (tools/ssbu_online_lobby.py hands each request here) --------------------------------
    def http(self, method, path, headers, body, now=None, peer=None):
        """(status, JSON bytes) for one request. headers: lower-case names; peer: the caller's address (for its
        bucket: every signed request takes from it)."""
        now = time.monotonic() if now is None else now
        if method == "GET" and path == "/leaderboard.json":
            return 200, self.leaderboard(now)
        if method == "GET" and path == "/status.json":
            return 200, json.dumps(self.status(now), separators=(",", ":")).encode()
        signed = (method == "GET" and path in ("/gsp/me", "/gsp/records")) or (
            method == "POST" and path in ("/gsp/sync", "/match/played", "/gsp/profile", "/gsp/link"))
        if signed and path != "/gsp/link" and not self.ip_allowed(peer, now):   # (link_request takes its own)
            return 429, b'{"error":"too many requests"}'
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
            return 200, json.dumps({"records": [raw.hex() for _f, (_g, raw) in sorted(self.merged(pid, sent=True).items())]},
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
            refused = self.refusal(d.get("player"))
            if refused:
                if "key" in d and self.key_signed(d.get("player"), d["key"], d.get("t"), d.get("sig"), "sync", d["data"]):
                    # (a game this server never saw log in: nothing kept, but answered so it does not send it again)
                    return 200, b'{"ok":true,"records":0,"ranked":0,"kept":false}'
                return refused
            pid = self.verify(d.get("player"), d.get("t"), d.get("sig"), "sync", d["data"])
            if pid is None:
                return 403, b'{"error":"not signed by a player this server knows"}'
            got = self.parse_sync(d["data"])
            if got is None:
                return 400, b'{"error":"bad data"}'
            if got[0] and not d.get("m"):   # (an old game, which reports no matches: its syncs estimate them)
                self.matches.legacy_sync(pid)
                last = self.legacy_credit_at.get(pid)
                if last is None or now - last >= LEGACY_CREDIT_EVERY:
                    self.legacy_credit_at[pid] = now
                    self.credit(pid, now, LEGACY_CREDIT_EVERY)
            stored, kept = self.sync(pid, *got, now)
            return 200, json.dumps({"ok": True, "records": stored, "ranked": kept}).encode()
        if method == "POST" and path == "/match/played":
            if len(body) > MAX_LINK_BODY:
                return 413, b'{"error":"too large"}'
            try:
                d = json.loads(body.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                return 400, b'{"error":"not JSON"}'
            if not isinstance(d, dict) or not isinstance(d.get("data"), str):
                return 400, b'{"error":"not a match"}'
            player = d.get("player")
            if self.refusal(player):
                # (a game this server never saw log in: its match counts when its own key signed it; nothing is made)
                if "key" not in d:
                    return 403, b'{"error":"unknown player"}'
                if not self.key_signed(player, d["key"], d.get("t"), d.get("sig"), "match", d["data"]):
                    return 403, b'{"error":"unknown player"}'
                pid, who = None, "p" + player
            else:
                pid = self.verify(player, d.get("t"), d.get("sig"), "match", d["data"])
                if pid is None:
                    return 403, b'{"error":"not signed by a player this server knows"}'
                who = pid
            code, answer = self.matches.report(who, d["data"])
            if code == 200:
                m = Matches.parse(d["data"])
                self.pair_report(("x" if m["exact"] else "w") + m["id"], player, peer, pid, now)
            return code, answer
        if method == "POST" and path == "/gsp/profile":
            try:
                d = json.loads(body.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                return 400, b'{"error":"not JSON"}'
            if not isinstance(d, dict) or not isinstance(d.get("name", ""), str) or not isinstance(d.get("discord", ""), str):
                return 400, b'{"error":"not a profile"}'
            name, discord = d.get("name", ""), d.get("discord", "")
            refused = self.refusal(d.get("player"))
            if refused:
                return refused
            pid = self.verify(d.get("player"), d.get("t"), d.get("sig"), "profile", f"{name}\n{discord}")
            if pid is None:
                return 403, b'{"error":"not signed by a player this server knows"}'
            self.set_profile(pid, name)
            return 200, json.dumps({"ok": True, "name": shown_name(name)}).encode()
        if method == "POST" and path == "/gsp/link":
            job, refused = self.link_request(body, now, peer)
            if refused:
                return refused
            return self.link_finish(job, self.link_ask(job[1]))
        if path in ("/leaderboard.json", "/status.json", "/gsp/me", "/gsp/records", "/gsp/profile", "/gsp/link", "/gsp/sync",
                    "/match/played"):
            return 405, b'{"error":"method not allowed"}'
        return 404, b'{"error":"not found"}'

