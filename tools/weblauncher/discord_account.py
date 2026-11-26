"""Sign-in with Discord for the web launcher, the same as OpenSmash.exe's (OpenSmash.Launcher/DiscordAccount.cs) and the
game's (runtime/net_discord.c): config/discord_token.txt holds the token all three share (key=value lines; delete it
to sign out).

OAuth2 with PKCE: the Discord application is a Public Client, so no secret is involved; its id is discord.txt beside
the game's program (a release puts it there: tools/discord_fetch.py), and without it there is no sign-in. The browser
comes back to http://127.0.0.1/callback (port 80) where this computer lets a program listen there (Windows, macOS); on
Linux, where ports under 1024 need root, to http://127.0.0.1:53134/callback - a redirect the Discord application must
list too.

Also what the account box shows: the player's GSP per fighter, kept by the lobbies' own game server (GET /gsp/me,
the lobby's board.py, signed with this install's NSA id from netplay_id.txt), and their global position (the one public
lobby's leaderboard.json: every GSP result goes through its game server). The GSP belongs to the Discord account: this install is linked to it with
POST /gsp/link (the token is sent once, the lobby verifies it with Discord and keeps nothing of it; show on/off is the
choice config/discord_leaderboard.txt remembers). Standard library only."""
import base64
import hashlib
import hmac
import json
import os
import random
import re
import secrets
import socket
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

API = "https://discord.com/api"
REDIRECTS = (("http://127.0.0.1/callback", 80), ("http://127.0.0.1:53134/callback", 53134))
import server_region   # noqa: E402  (beside this file: the official lobbies, one a region, and which one is played on)

# The official lobby, by its DNS name (runtime/net.h's NET_OFFICIAL_LOBBY; never an address in numbers): the first
# region's, where nothing says another (server_region.py). Its read-only pages are plain HTTP on the same port: the
# lobby serves no TLS, and they are public.
OFFICIAL_LOBBY = "eu.lobby.opensmashultimate.com:39421"
OLDER_LOBBIES = ("opensmash-lobby.duckdns.org", "smash.recomps.wtf")   # (replaced by it: their lines mean the official one)


def official_lobby(root=None):
    """As the game reads it (runtime/net_region.c): SSBU_OFFICIAL_LOBBY, else config/official_lobby.txt (an override:
    that server, no regions), else the server region's lobby (the one chosen, or Auto's pick)."""
    return server_region.official_lobby(root)


def means_official(host, root=None):
    h = (host or "").lower()
    return (h in ("auto", "eos", "na", "eu", (server_region.override(root) or "").lower()) and h != "" or server_region.of_lobby(h) is not None
            or h.startswith(OLDER_LOBBIES))


LEADERBOARD_URL = "http://" + OFFICIAL_LOBBY + "/leaderboard.json"   # (the default; leaderboard_url(root) is the install's)
PUBLIC_LOBBIES = tuple(r[2] for r in server_region.REGIONS)   # (every region's: a player's records are asked of each)


def leaderboard_url(root=None):
    return "http://" + official_lobby(root) + "/leaderboard.json"


SCOPES = "identify openid sdk.social_layer_presence"
KEYS = ("access_token", "refresh_token", "expires_at", "scope", "user_id", "username", "display_name", "avatar")
UA = {"User-Agent": "OpenSmash-Launcher"}
NAMES = [
    "Mario", "Donkey Kong", "Link", "Samus", "Dark Samus", "Yoshi", "Kirby", "Fox", "Pikachu", "Luigi",
    "Ness", "Captain Falcon", "Jigglypuff", "Peach", "Daisy", "Bowser", "Ice Climbers", "Sheik", "Zelda", "Dr. Mario",
    "Pichu", "Falco", "Marth", "Lucina", "Young Link", "Ganondorf", "Mewtwo", "Roy", "Chrom", "Mr. Game & Watch",
    "Meta Knight", "Pit", "Dark Pit", "Zero Suit Samus", "Wario", "Snake", "Ike", "Pokemon Trainer", "Diddy Kong", "Lucas",
    "Sonic", "King Dedede", "Olimar", "Lucario", "R.O.B.", "Toon Link", "Wolf", "Villager", "Mega Man", "Wii Fit Trainer",
    "Rosalina & Luma", "Little Mac", "Greninja", "Palutena", "Pac-Man", "Robin", "Shulk", "Bowser Jr.", "Duck Hunt", "Ryu",
    "Ken", "Cloud", "Corrin", "Bayonetta", "Richter", "Inkling", "Ridley", "King K. Rool", "Simon", "Isabelle",
    "Incineroar", "Mii Brawler", "Mii Swordfighter", "Mii Gunner", "Piranha Plant", "Joker", "Hero", "Banjo & Kazooie", "Terry", "Byleth",
    "Min Min", "Steve", "Sephiroth", "Pyra & Mythra", "Kazuya", "Sora"]
# (by the GSP records' fighter number - DataStore 82's u8, the game's save_no in ui_chara_db, not the fighter kind:
# 29 is Mr. Game & Watch, issue #389; the same list as runtime/net_discord_core.c and OpenSmash.Launcher/DiscordAccount.cs)


class SignInError(Exception):
    """Something the player can act on; the message says what."""


def fighter_name(kind):
    """A GSP record's fighter number's name (NAMES above)."""
    return NAMES[kind] if 0 <= kind < len(NAMES) and NAMES[kind] else f"Fighter {kind}"


def token_path(root):
    return os.path.join(root, "config", "discord_token.txt")


def parse_app_id(text):
    """discord.txt's id ("ApplicationId=<digits>", or the digits alone); "" none."""
    for line in (text or "").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        k, eq, v = line.partition("=")
        if eq and k.strip() != "ApplicationId":
            continue
        v = (v if eq else k).strip()
        if v.isdigit() and v.isascii() and 15 <= len(v) <= 20:
            return v
    return ""


def app_id(program_dir):
    """The Discord application's id: discord.txt beside the game's program (program_dir), "" when there is none."""
    try:
        with open(os.path.join(program_dir, "discord.txt"), encoding="utf-8") as f:
            return parse_app_id(f.read())
    except OSError:
        return ""


def parse(text):
    t = {}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        if k.strip() in KEYS:
            t[k.strip()] = v.strip()
    try:
        t["expires_at"] = int(t.get("expires_at") or 0)
    except ValueError:
        t["expires_at"] = 0
    return t if t.get("access_token") or t.get("refresh_token") else None


def format_token(t):
    for k in KEYS:
        if any(c in str(t.get(k, "")) for c in "\r\n"):
            raise ValueError("a value with a line break")
    return ("# OpenSmash: the Discord sign-in the game and the launchers share. Delete this file to sign out.\n"
            + "".join(f"{k}={t.get(k, 0 if k == 'expires_at' else '')}\n" for k in KEYS))


def load(root):
    try:
        with open(token_path(root), encoding="utf-8") as f:
            return parse(f.read())
    except OSError:
        return None


def save(root, t):
    path = token_path(root)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(format_token(t))
    os.replace(tmp, path)


def sign_out(root):
    try:
        os.remove(token_path(root))
    except OSError:
        pass


def fresh(t, now, margin):
    return bool(t and t.get("access_token")) and t.get("expires_at", 0) - margin > now


def b64url(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def _post(url, form, opener=urllib.request.urlopen):
    req = urllib.request.Request(url, data=urllib.parse.urlencode(form).encode(), headers={
        "Content-Type": "application/x-www-form-urlencoded", **UA})
    try:
        with opener(req, timeout=20) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        raise SignInError(f"Discord refused the sign-in ({e.code}).") from None
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise SignInError(f"Discord could not be reached ({e}).") from None


def _user(t, opener=urllib.request.urlopen):
    req = urllib.request.Request(API + "/users/@me", headers={"Authorization": "Bearer " + t["access_token"], **UA})
    try:
        with opener(req, timeout=20) as r:
            u = json.loads(r.read().decode())
    except (urllib.error.URLError, OSError, ValueError):
        return
    t["user_id"], t["username"] = str(u.get("id") or ""), u.get("username") or ""
    t["display_name"], t["avatar"] = u.get("global_name") or "", u.get("avatar") or ""


def _took(t, answer):
    t["access_token"] = answer["access_token"]
    t["refresh_token"] = answer.get("refresh_token") or t.get("refresh_token", "")
    t["scope"] = answer.get("scope") or ""
    t["expires_at"] = int(time.time()) + int(answer.get("expires_in") or 3600)
    return t


def current(root, client_id, post=_post, user=_user):
    """A token good for a while (refreshed and saved when it expired), or None."""
    t = load(root)
    if not t:
        return None
    if fresh(t, time.time(), 300):
        return t
    if not t.get("refresh_token") or not client_id:
        return None
    try:
        t = _took(t, post(API + "/oauth2/token", {"client_id": client_id, "grant_type": "refresh_token", "refresh_token": t["refresh_token"]}))
        user(t)
        save(root, t)
        return t
    except (SignInError, KeyError):
        again = load(root)   # (the game may have refreshed it meanwhile: each refresh token works once)
        return again if fresh(again, time.time(), 60) else None


def _listen():
    for redirect, port in REDIRECTS:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.bind(("127.0.0.1", port))
            s.listen(4)
            return s, redirect
        except OSError:
            s.close()
    raise SignInError("Neither port 80 nor 53134 on this computer is free for Discord to send the browser back to.")


def _reply(conn, code, text):
    body = ("<!doctype html><meta charset=utf-8><title>OpenSmash</title><body style=\"font:16px sans-serif;"
            "background:#111;color:#eee;padding:40px\">" + text + "</body>").encode()
    conn.sendall(f"HTTP/1.1 {code} OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {len(body)}\r\n"
                 f"Connection: close\r\n\r\n".encode() + body)


def sign_in(root, client_id, open_url=webbrowser.open, post=_post, user=_user, timeout=300, listen=_listen):
    """The browser's sign-in, start to end: the token (saved), or SignInError."""
    if not client_id:
        raise SignInError("This build has no Discord application (discord.txt beside the game): update OpenSmash.")
    verifier = b64url(secrets.token_bytes(48))
    challenge = b64url(hashlib.sha256(verifier.encode()).digest())
    state = b64url(secrets.token_bytes(16))
    server, redirect = listen()
    try:
        open_url("https://discord.com/oauth2/authorize?" + urllib.parse.urlencode({
            "response_type": "code", "client_id": client_id, "scope": SCOPES, "redirect_uri": redirect,
            "code_challenge": challenge, "code_challenge_method": "S256", "state": state, "prompt": "consent"}))
        server.settimeout(1)
        until = time.time() + timeout
        while time.time() < until:
            try:
                conn, _ = server.accept()
            except socket.timeout:
                continue
            with conn:
                conn.settimeout(5)
                try:
                    first = conn.recv(8192).decode("latin-1").split("\r\n", 1)[0]
                except OSError:
                    continue
                parts = first.split(" ")
                if len(parts) < 2 or not parts[1].startswith("/callback"):
                    _reply(conn, 404, "Not here.")
                    continue
                q = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(parts[1]).query))
                if q.get("state") != state:
                    _reply(conn, 400, "This sign-in was not started by OpenSmash. Close this tab.")
                    continue
                if "code" not in q:
                    _reply(conn, 200, "Sign-in cancelled. You can close this tab.")
                    raise SignInError("Sign-in cancelled in Discord.")
                _reply(conn, 200, "Signed in to OpenSmash with Discord. You can close this tab.")
                t = _took({}, post(API + "/oauth2/token", {"client_id": client_id, "grant_type": "authorization_code",
                                                            "code": q["code"], "redirect_uri": redirect, "code_verifier": verifier}))
                user(t)
                save(root, t)
                return t
        raise SignInError("The sign-in took too long: try again.")
    finally:
        server.close()


# ---- the player on the lobbies' game server (its board.py) ------------------------------------------------------
def nsa_id(root):
    """The install's NSA id as the game's token writes it (runtime/hle_env.c): netplay_id.txt's two numbers XORed, 16
    hex digits; "" before the game has played online."""
    try:
        with open(os.path.join(root, "netplay_id.txt"), encoding="utf-8") as f:
            words = f.read().split()
        a, b = int(words[0], 16), int(words[1], 16)
    except (OSError, ValueError, IndexError):
        return ""
    if not a and not b:
        return ""
    return f"{(a ^ b) or 1:016x}"


def player_id(nsa):
    return hashlib.sha256(("opensmash-player\n" + nsa).encode()).hexdigest()[:16]


def account_hash(discord_user_id):
    return hashlib.sha256(("opensmash-discord\n" + discord_user_id).encode()).hexdigest()[:16]


def sign(nsa, what, player, t, payload=""):
    return hmac.new(nsa.encode(), f"opensmash1\n{what}\n{player}\n{int(t)}\n{payload}".encode(), hashlib.sha256).hexdigest()


def lobbies(server_line="", root=None):
    """The lobbies to ask: the public ones, and online_server.txt's own (its first line given) when it names another."""
    out = server_region.official_lobbies(root)   # (the one played on first, then the other regions')
    words = (server_line or "").split()
    host = words[0] if words else ""
    if host and host.lower() not in ("off", "lan") and not means_official(host, root):
        if ":" not in host:
            host += ":39420"
        if host.lower() not in (x.lower() for x in out):
            out.append(host)
    return out


def _ask(url, headers=None, body=None, opener=urllib.request.urlopen):
    req = urllib.request.Request(url, data=body, headers={**UA, **(headers or {})})
    try:
        with opener(req, timeout=8) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, OSError, ValueError):
        return None


def ticket_expiry(ticket):
    """A lobby ticket's expiry ("<id>|<name>|<expiry>[|...].<mac>", unix seconds); 0 when it is not one."""
    parts = (ticket or "").rpartition(".")[0].split("|")
    return int(parts[2]) if len(parts) >= 3 and parts[2].isdigit() else 0


def stored_ticket(root, margin=60, now=None):
    """(v0.5.0) The lobby ticket a signed-in launcher last wrote for the game (build/cache/online_pass.txt,
    OpenSmash.Core/OnlinePass.cs) while it has margin seconds left, else None: with it the official lobbies give the
    account's GSP (its records are the account's there, not the install's)."""
    try:
        with open(os.path.join(root, "build", "cache", "online_pass.txt"), encoding="ascii") as f:
            t = f.read().strip()
    except (OSError, UnicodeDecodeError):
        return None
    return t if ticket_expiry(t) > (time.time() if now is None else now) + margin else None


def records(root, where, ask=_ask):
    """GET /gsp/me on the lobbies where, merged: [(fighter kind, GSP)], the highest GSP of each fighter, highest first.
    The official lobbies are asked with the stored lobby ticket when there is one (never another server: it is a
    credential), the others signed with the install's NSA id."""
    nsa, ticket = nsa_id(root), stored_ticket(root)
    official = {x.lower() for x in server_region.official_lobbies(root)} if ticket else set()
    if not nsa and not ticket:
        return []
    player, best = player_id(nsa) if nsa else "", {}
    for lobby in where:
        t = int(time.time())
        if lobby.lower() in official:
            headers = {"X-OpenSmash-Ticket": ticket}
        elif nsa:
            headers = {"X-OpenSmash-Player": player, "X-OpenSmash-Time": str(t), "X-OpenSmash-Sig": sign(nsa, "me", player, t)}
        else:
            continue
        me = ask(f"http://{lobby}/gsp/me", headers)
        for f in (me or {}).get("fighters") or []:
            if isinstance(f, dict) and isinstance(f.get("fighter"), int) and isinstance(f.get("gsp"), int):
                best[f["fighter"]] = max(best.get(f["fighter"], 0), f["gsp"])
    return sorted(best.items(), key=lambda r: (-r[1], r[0]))


# ---- mastery points (as OpenSmash.Core/Mastery.cs): per account and fighter, points that only go up; no levels, no
# tiers. Each official lobby counts only its own matches, so the regions are SUMMED; the one-time veteran bonus is the
# largest of the regions', not their sum. GET /mastery/me (with the stored lobby ticket, official lobbies only) and the
# public GET /mastery.json; an older lobby answers 404 and is skipped. OPENSMASH_MASTERY_UI=0: the GSP view as before;
# OPENSMASH_ARENA_CARD=0 hides the SSBU Arena card, OPENSMASH_TOP_MASTERY=0 its top list.
ARENA_DOWNLOAD = "https://www.ssbuarena.com/download"   # (Arena is Windows only: on Linux and macOS a link, nothing to start)
FIGHTER_KEYS = {
    "mario": "Mario", "donkey": "Donkey Kong", "link": "Link", "samus": "Samus", "samusd": "Dark Samus",
    "yoshi": "Yoshi", "kirby": "Kirby", "fox": "Fox", "pikachu": "Pikachu", "luigi": "Luigi", "ness": "Ness",
    "captain": "Captain Falcon", "purin": "Jigglypuff", "peach": "Peach", "daisy": "Daisy", "koopa": "Bowser",
    "ice_climber": "Ice Climbers", "popo": "Ice Climbers", "nana": "Ice Climbers", "sheik": "Sheik", "zelda": "Zelda",
    "mariod": "Dr. Mario", "pichu": "Pichu", "falco": "Falco", "marth": "Marth", "lucina": "Lucina",
    "younglink": "Young Link", "ganon": "Ganondorf", "mewtwo": "Mewtwo", "roy": "Roy", "chrom": "Chrom",
    "gamewatch": "Mr. Game & Watch", "metaknight": "Meta Knight", "pit": "Pit", "pitb": "Dark Pit",
    "szerosuit": "Zero Suit Samus", "wario": "Wario", "snake": "Snake", "ike": "Ike", "ptrainer": "Pokemon Trainer",
    "pzenigame": "Squirtle", "pfushigisou": "Ivysaur", "plizardon": "Charizard", "diddy": "Diddy Kong",
    "lucas": "Lucas", "sonic": "Sonic", "dedede": "King Dedede", "pikmin": "Olimar", "lucario": "Lucario",
    "robot": "R.O.B.", "toonlink": "Toon Link", "wolf": "Wolf", "murabito": "Villager", "rockman": "Mega Man",
    "wiifit": "Wii Fit Trainer", "rosetta": "Rosalina & Luma", "littlemac": "Little Mac", "gekkouga": "Greninja",
    "palutena": "Palutena", "pacman": "Pac-Man", "reflet": "Robin", "shulk": "Shulk", "koopajr": "Bowser Jr.",
    "duckhunt": "Duck Hunt", "ryu": "Ryu", "ken": "Ken", "cloud": "Cloud", "kamui": "Corrin", "bayonetta": "Bayonetta",
    "inkling": "Inkling", "ridley": "Ridley", "simon": "Simon", "richter": "Richter", "krool": "King K. Rool",
    "shizue": "Isabelle", "gaogaen": "Incineroar", "packun": "Piranha Plant", "jack": "Joker", "brave": "Hero",
    "buddy": "Banjo & Kazooie", "dolly": "Terry", "master": "Byleth", "tantan": "Min Min", "pickel": "Steve",
    "edge": "Sephiroth", "eflame": "Pyra", "elight": "Mythra", "element": "Pyra & Mythra",
    "eflame_first": "Pyra", "elight_first": "Mythra", "eflame_only": "Pyra", "elight_only": "Mythra",
    "demon": "Kazuya", "trail": "Sora",
    "miifighter": "Mii Brawler", "miiswordsman": "Mii Swordfighter", "miigunner": "Mii Gunner"}


def switch_on(name):
    """A switch that is on unless the variable says 0 / off / false."""
    return (os.environ.get(name) or "").strip().lower() not in ("0", "off", "false")


# How often the accounts service (Supabase) is asked for what the page shows of everyone (the top mastery, SSBU Arena's
# ranked board): at most every 5 minutes, with a random 0..30 s more so the launchers do not ask in the same second
# (OpenSmash.Core/CommunityPoll.cs). OPENSMASH_COMMUNITY_POLL_S=<seconds> (30..3600, default 300); 60 is the old pace.
POLL_DEFAULT_S, POLL_MIN_S, POLL_MAX_S, POLL_JITTER_S = 300, 30, 3600, 30


def poll_seconds(value=None):
    """The interval OPENSMASH_COMMUNITY_POLL_S gives (none or not a number: 300 s), within 30..3600."""
    raw = os.environ.get("OPENSMASH_COMMUNITY_POLL_S") if value is None else value
    try:
        s = int(str(raw).strip())
    except (TypeError, ValueError):
        s = POLL_DEFAULT_S
    if s <= 0:
        s = POLL_DEFAULT_S
    return max(POLL_MIN_S, min(POLL_MAX_S, s))


def poll_next(interval=None, rnd=random.random):
    """The wait before the accounts service is asked again: the interval and a random 0..30 s (none at 60 s or less)."""
    interval = poll_seconds() if interval is None else interval
    return interval if interval <= 60 else interval + POLL_JITTER_S * min(1.0, max(0.0, rnd()))


def fighter_key(k):
    """A fighter key as the lobby sends it ("ui_chara_" cut), or "" when it is not one."""
    k = (k if isinstance(k, str) else "").strip().lower()
    if k.startswith("ui_chara_"):
        k = k[9:]
    return k if 0 < len(k) <= 32 and all(c.isascii() and (c.isalnum() or c == "_") for c in k) else ""


def fighter_key_name(k):
    k = fighter_key(k)
    return FIGHTER_KEYS.get(k) or (k[:1].upper() + k[1:].replace("_", " ") if k else "?")


def _num(v):
    return max(0, v) if isinstance(v, int) and not isinstance(v, bool) else 0


def merge_mastery(answers):
    """Several /mastery/me answers (None skipped) as one: [{"fighter", "name", "points" (summed + the largest veteran),
    "veteran", "games", "wins"}], highest first; None when no answer was one."""
    best, any_answer = {}, False
    for a in answers:
        if not isinstance(a, dict) or not isinstance(a.get("fighters"), list):
            continue
        any_answer = True
        for f in a["fighters"]:
            key = fighter_key(f.get("fighter")) if isinstance(f, dict) else ""
            if not key:
                continue
            s = best.setdefault(key, {"points": 0, "veteran": 0, "games": 0, "wins": 0})
            s["points"] += _num(f.get("points"))
            s["games"] += _num(f.get("games"))
            s["wins"] += _num(f.get("wins"))
            s["veteran"] = max(s["veteran"], min(1000, _num(f.get("veteran"))))
    if not any_answer:
        return None
    rows = [{"fighter": k, "name": fighter_key_name(k), "points": v["points"] + v["veteran"], "veteran": v["veteran"],
             "games": v["games"], "wins": v["wins"], "icon": fighter_icon(k)["icon"]} for k, v in best.items() if v["points"] + v["veteran"] or v["games"]]
    return sorted(rows, key=lambda r: (-r["points"], -r["games"], r["fighter"]))


def merge_mastery_board(boards, take=5):
    """/mastery.json "total" lists of several lobbies as one, by the opaque id (the same on every lobby): points summed,
    the largest veteran; [{"id", "name", "points"}] (points with the veteran), highest first."""
    rows = {}
    for b in boards:
        total = b.get("total") if isinstance(b, dict) else None
        for r in total if isinstance(total, list) else []:
            if not isinstance(r, dict) or not isinstance(r.get("id"), str) or not r["id"] or len(r["id"]) > 64:
                continue
            s = rows.setdefault(r["id"], {"id": r["id"], "name": "", "p": 0, "v": 0})
            s["p"] += _num(r.get("points"))
            s["v"] = max(s["v"], _num(r.get("veteran")))
            name = r.get("name") if isinstance(r.get("name"), str) else ""
            if not s["name"] and name:
                s["name"] = "".join(c for c in name if c.isprintable()).strip()[:32]
    out = [{"id": s["id"], "name": s["name"] or "Player", "points": s["p"] + s["v"]} for s in rows.values() if s["p"] + s["v"] > 0]
    return sorted(out, key=lambda r: (-r["points"], r["id"]))[:take]


def mastery_me(root, ask=_ask):
    """The account's mastery from every official lobby (the stored lobby ticket as the proof), summed; None when there is
    no ticket or no lobby has /mastery/me yet."""
    ticket = stored_ticket(root)
    if not ticket:
        return None
    return merge_mastery([ask(f"http://{lobby}/mastery/me", {"X-OpenSmash-Ticket": ticket})
                          for lobby in server_region.official_lobbies(root)])


def mastery_board(root, ask=_ask, take=5):
    """The official lobbies' top mastery (public), merged; None when no lobby has /mastery.json yet."""
    got = [ask(f"http://{lobby}/mastery.json") for lobby in server_region.official_lobbies(root)]
    got = [b for b in got if isinstance(b, dict) and isinstance(b.get("total"), list)]
    return merge_mastery_board(got, take) if got else None


# The accounts service (Supabase) is the one source of mastery points: the lobbies push their counts to it every hour.
# Its public board, public.opensmash_mastery_board(p_fighter, p_limit, p_offset) -> [{pos, id, name (null: "Anonymous"),
# tag, avatar, main_fighter, total, games, total_count}], is asked first with the publishable key (anyone may call it); the
# lobbies' /mastery.json is only the fallback. The player's own points and place (my_opensmash_mastery(),
# my_opensmash_mastery_rank()) need the account's session, which only OpenSmash.exe / the Avalonia launcher keep: this page
# shows the player's own points from the lobbies with the stored lobby ticket, as before.
def _service_file(path):
    try:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError):
        return {}
    return d if isinstance(d, dict) else {}


def account_service(root=None):
    """The accounts service as OpenSmash.Core/Account.cs reads it: OpenSmash.Core/account_service.json of the tree, the
    install's config/account_service.json over it, OPENSMASH_SUPABASE_URL / _KEY over both. (url, key), or None when there
    is none (an address that is not https, or not this PC, counts as none)."""
    url = key = ""
    for path in ([os.path.join(root, "OpenSmash.Core", "account_service.json"), os.path.join(root, "config", "account_service.json")] if root else []):
        d = _service_file(path)
        if isinstance(d.get("url"), str) and d["url"].strip():
            url = d["url"]
        if isinstance(d.get("publishable_key"), str) and d["publishable_key"].strip():
            key = d["publishable_key"]
    url = (os.environ.get("OPENSMASH_SUPABASE_URL") or url).strip().rstrip("/")
    key = (os.environ.get("OPENSMASH_SUPABASE_KEY") or key).strip()
    p = urllib.parse.urlparse(url)
    if not key or not p.hostname or not (p.scheme == "https" or (p.scheme == "http" and p.hostname in ("127.0.0.1", "localhost"))):
        return None
    return url, key


# ---- fighters by any spelling and their icons (OpenSmash.Core/Fighters.cs, opensmash-site's js/fighters.js) -------------
def _squash(v):
    return "".join(c for c in str(v or "").lower() if c.isascii() and c.isalnum())


_ALIAS = {}
for _k in FIGHTER_KEYS:
    _ALIAS[_squash(_k)] = _k
for _k, _n in FIGHTER_KEYS.items():
    _ALIAS.setdefault(_squash(_n), _k)
for _pair in ("joker:jack hero:brave banjo:buddy banjokazooie:buddy terry:dolly terrybogard:dolly byleth:master minmin:tantan steve:pickel "
              "alex:pickel sephiroth:edge pyra:eflame mythra:elight pyramythra:element aegis:element kazuya:demon sora:trail "
              "pt:ptrainer pokemontrainer:ptrainer pokmontrainer:ptrainer trainer:ptrainer squirtle:pzenigame ivysaur:pfushigisou charizard:plizardon "
              "iceclimbers:ice_climber iceclimber:ice_climber ics:ice_climber icies:ice_climber "
              "mii:miifighter miibrawler:miifighter brawler:miifighter miisword:miiswordsman miiswordfighter:miiswordsman swordfighter:miiswordsman "
              "gunner:miigunner dk:donkey donkeykong:donkey diddykong:diddy kingkrool:krool kingdedede:dedede falcon:captain captainfalcon:captain "
              "cf:captain jigglypuff:purin puff:purin bowser:koopa bowserjr:koopajr drmario:mariod doc:mariod yink:younglink tink:toonlink "
              "ganondorf:ganon gameandwatch:gamewatch mrgameandwatch:gamewatch mrgamewatch:gamewatch gnw:gamewatch gw:gamewatch mk:metaknight "
              "darkpit:pitb darksamus:samusd zss:szerosuit zerosuitsamus:szerosuit olimar:pikmin alph:pikmin rob:robot villager:murabito "
              "megaman:rockman wiifittrainer:wiifit wft:wiifit rosalina:rosetta rosalinaluma:rosetta luma:rosetta mac:littlemac greninja:gekkouga "
              "robin:reflet corrin:kamui isabelle:shizue incineroar:gaogaen piranhaplant:packun plant:packun dh:duckhunt belmont:simon").split():
    _a, _b = _pair.split(":")
    _ALIAS[_squash(_a)] = _b
ICON_BASE = "https://www.ssbuarena.com/fighters/"
_ICON_FOLDERS = set(("bayonetta brave buddy captain chrom cloud daisy dedede demon diddy dolly donkey duckhunt edge eflame elight falco fox "
                     "gamewatch ganon gaogaen gekkouga ice_climber ike inkling jack kamui ken kirby koopa koopajr krool link littlemac lucario "
                     "lucas lucina luigi mario mariod marth master metaknight mewtwo miifighter miigunner miiswordsman murabito ness packun "
                     "pacman palutena peach pfushigisou pichu pickel pikachu pikmin pit pitb plizardon ptrainer purin pzenigame reflet richter "
                     "ridley robot rockman rosetta roy ryu samus samusd sheik shizue shulk simon snake sonic szerosuit tantan toonlink trail "
                     "wario wiifit wolf yoshi younglink zelda").split())
_ICON_FOLDER_OF = {"popo": "ice_climber", "nana": "ice_climber", "ptrainer_low": "ptrainer", "element": "eflame", "eflame_first": "eflame",
                   "eflame_only": "eflame", "elight_first": "elight", "elight_only": "elight"}
_COSTUME = {"ridley": 5}   # (the site's .webp: Ridley has no costume 1 there)


def fighter_key_any(v):
    """The game key of a fighter given any spelling (a key, "ui_chara_" id, a name like "Joker", a nickname); None."""
    s = str(v or "").strip()
    if s.lower().startswith("ui_chara_"):
        s = s[9:]
    return _ALIAS.get(_squash(s)) or None


def fighter_icon(v):
    """{"name", "icon"} of a fighter (any spelling): its stock icon as SSBU Arena serves it, "" when there is none."""
    k = fighter_key_any(v)
    if not k:
        return {"name": str(v or "").strip()[:40], "icon": ""}
    folder = _ICON_FOLDER_OF.get(k, k)
    icon = f"{ICON_BASE}{folder}/{_COSTUME.get(folder, 1)}.webp" if folder in _ICON_FOLDERS else ""
    return {"name": fighter_key_name(k), "icon": icon}


# ---- SSBU Arena's ranked board (OpenSmash.Core/RankedBoard.cs): public.ranked_board, the publishable key alone ----------
RANKED_REGIONS = ("eu", "na-east", "sa")
ARENA_LEADERBOARD = "https://www.ssbuarena.com/leaderboard"


def ranked_board(root, region=None, take=10, ask=_ask):
    """The board's first rows for a region (None: every region): [{"rank", "name", "tag", "rating", "wins", "losses",
    "tier", "tier_color", "fighters": [{"name", "icon"}]}]; None when there is no service or it did not answer with a list."""
    s = account_service(root)
    if not s:
        return None
    url, key = s
    region = region if region in RANKED_REGIONS else None
    got = ask(url + "/rest/v1/rpc/ranked_board", {"apikey": key, "Authorization": "Bearer " + key, "Content-Type": "application/json"},
              json.dumps({"p_region": region, "p_search": None, "p_fighter": None, "p_limit": take, "p_offset": 0}).encode())
    if not isinstance(got, list):
        return None
    out = []
    for r in got:
        name = r.get("username") if isinstance(r, dict) and isinstance(r.get("username"), str) else ""
        name = "".join(c for c in name if c.isprintable()).strip()[:32]
        if not name:
            continue
        rating = r.get("rating")
        try:
            rating = round(float(rating))
        except (TypeError, ValueError):
            rating = 0
        color = r.get("tier_color") if isinstance(r.get("tier_color"), str) and re.fullmatch(r"#[0-9A-Fa-f]{6}", r.get("tier_color")) else "#8C9AAD"
        fighters = [fighter_icon(f.get("fighter")) for f in (r.get("fighters") if isinstance(r.get("fighters"), list) else [])
                    if isinstance(f, dict) and isinstance(f.get("fighter"), str)][:3]
        out.append({"rank": _num(r.get("rank")), "name": name, "tag": f"{_num(r.get('tag')):04d}", "rating": rating,
                    "wins": _num(r.get("wins")), "losses": _num(r.get("losses")),
                    "tier": (r.get("tier") if isinstance(r.get("tier"), str) else "")[:24] or "Unranked", "tier_color": color,
                    "fighters": fighters})
    return out[:take]


def service_board(root, ask=_ask, take=5):
    """The top mastery from the accounts service: [{"id", "name", "points" (its total), "pos"}], in its order; None when
    there is no service or it did not answer with a list (then the lobbies are asked)."""
    s = account_service(root)
    if not s:
        return None
    url, key = s
    got = ask(url + "/rest/v1/rpc/opensmash_mastery_board",
              {"apikey": key, "Authorization": "Bearer " + key, "Content-Type": "application/json"},
              json.dumps({"p_fighter": None, "p_limit": take, "p_offset": 0}).encode())
    if not isinstance(got, list):
        return None
    out = []
    for r in got:
        if not isinstance(r, dict) or not isinstance(r.get("id"), str) or not r["id"] or len(r["id"]) > 64:
            continue
        name = r.get("name") if isinstance(r.get("name"), str) else ""
        name = "".join(c for c in name if c.isprintable()).strip()[:32] or "Anonymous"
        out.append({"id": r["id"], "name": name, "points": _num(r.get("total")), "pos": _num(r.get("pos")), "fighter": fighter_icon(r.get("main_fighter")) if r.get("main_fighter") else None})
    return out[:take]


def ranks_in(board, pid):
    """The player's (global position, {fighter: position}) in leaderboard.json; (0, {}) when not there."""
    for p in (board or {}).get("players") or []:
        if pid and isinstance(p, dict) and p.get("id") == pid:
            fighters = {}
            for k, v in (p.get("fighters") or {}).items():
                if isinstance(v, dict) and isinstance(v.get("rank"), int) and k.isdigit():
                    fighters[int(k)] = v["rank"]
            return (p.get("rank") if isinstance(p.get("rank"), int) else 0), fighters
    return 0, {}


def shown_path(root):
    return os.path.join(root, "config", "discord_leaderboard.txt")


def shows_name(root):
    """On unless config/discord_leaderboard.txt says "off" (a missing file is on)."""
    try:
        with open(shown_path(root), encoding="utf-8") as f:
            return f.read().strip().lower() != "off"
    except OSError:
        return True


def _send(url, body, opener=urllib.request.urlopen):
    """POST body (JSON bytes): (HTTP status, JSON answer or {}); (0, {}) when the lobby can not be reached."""
    req = urllib.request.Request(url, data=body, headers={**UA, "Content-Type": "application/json"})
    try:
        with opener(req, timeout=8) as r:
            code, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        code, raw = e.code, e.read()
        e.close()
    except (urllib.error.URLError, OSError):
        return 0, {}
    try:
        d = json.loads(raw.decode())
    except ValueError:
        return code, {}
    return code, d if isinstance(d, dict) else {}


def link_body(nsa, token, shown, key=False):
    player, now = player_id(nsa), int(time.time())
    d = {"player": player, "t": now, "token": token, "show": bool(shown),
         "sig": sign(nsa, "link", player, now, token + "\n" + ("1" if shown else "0"))}
    if key:
        d["key"] = nsa
    return json.dumps(d).encode()


def hide_body(nsa):
    """The old /gsp/profile with no name: hides an already linked player (no Discord token needed)."""
    player, now = player_id(nsa), int(time.time())
    return json.dumps({"player": player, "t": now, "name": "", "discord": "", "sig": sign(nsa, "profile", player, now, "\n")}).encode()


def set_shown(root, t, shown, where, send=_send):
    """Remembers the choice and tells the lobbies where: POST /gsp/link with the token's access token (the lobby verifies
    it with Discord), retried once with the NSA id as "key" when the lobby does not know this install yet. Without a
    token (or when the lobby refuses it) a hide is sent as the old profile call. How many lobbies took it."""
    os.makedirs(os.path.dirname(shown_path(root)), exist_ok=True)
    with open(shown_path(root), "w", encoding="utf-8") as f:
        f.write("on\n" if shown else "off\n")
    nsa = nsa_id(root)
    if not nsa:
        return 0
    token = (t or {}).get("access_token") or ""
    took = 0
    for lobby in where:
        ok = False
        if token:
            url = f"http://{lobby}/gsp/link"
            code, answer = send(url, link_body(nsa, token, shown))
            if code == 403 and answer.get("error") == "unknown player":
                code, answer = send(url, link_body(nsa, token, shown, key=True))
            ok = code == 200 and bool(answer.get("ok"))
        if not ok and not shown:
            code, answer = send(f"http://{lobby}/gsp/profile", hide_body(nsa))
            ok = code == 200 and bool(answer.get("ok"))
        took += ok
    return took


def avatar_url(t):
    if not t or not t.get("user_id"):
        return ""
    if t.get("avatar"):
        return f"https://cdn.discordapp.com/avatars/{t['user_id']}/{t['avatar']}.png?size=64"
    try:
        return f"https://cdn.discordapp.com/embed/avatars/{(int(t['user_id']) >> 22) % 6}.png"
    except ValueError:
        return ""


class Account:
    """The launcher's view of it: sign-in in a thread, the account as the page shows it."""

    def __init__(self, root, program_dir, server_line=lambda: ""):
        self.root, self.program_dir, self.server_line = root, program_dir, server_line
        self.busy = False
        self.error = ""
        self.board = None
        self.board_at = 0
        self.mine = []
        self.mine_at = 0
        self.lock = threading.Lock()
        self.told = False
        self.mastery_at = 0
        self.mastery_mine = None   # (mastery_view: the last answers kept while a lobby is down)
        self.mastery_top = None
        self.top_next = 0   # (when the top mastery may be asked again: the accounts service, poll_next())

    def client_id(self):
        return app_id(self.program_dir)

    def lobbies(self):
        return lobbies(self.server_line(), self.root)

    def start_sign_in(self):
        with self.lock:
            if self.busy:
                return
            self.busy, self.error = True, ""

        def run():
            try:
                sign_in(self.root, self.client_id())
                self.board_at = self.mine_at = 0
            except SignInError as e:
                self.error = str(e)
            finally:
                self.busy = False
        threading.Thread(target=run, daemon=True).start()

    def sign_out(self):
        if shows_name(self.root):
            set_shown(self.root, load(self.root), False, self.lobbies())
        sign_out(self.root)

    def set_shown(self, shown):
        took = set_shown(self.root, current(self.root, self.client_id()), shown, self.lobbies())
        self.error = "Play online once first: then your name can be shown." if shown and not took and not nsa_id(self.root) else ""
        return {"ok": True, "lobbies": took}

    def _board(self):
        if time.time() - self.board_at > 600:
            self.board_at = time.time()
            self.board = _ask(leaderboard_url(self.root)) or self.board
        return self.board

    def _mine(self):
        if time.time() - self.mine_at > 60:
            self.mine_at = time.time()
            self.mine = records(self.root, self.lobbies())
        return self.mine

    def mastery_view(self, me=mastery_me, board=mastery_board, service=service_board):
        """What the page shows of the mastery points and the SSBU Arena card. The player's own points (the lobbies) are
        asked again at most once a minute; the top list (the accounts service's board, the lobbies' lists only when it
        fails) at most every 5 minutes (poll_seconds)."""
        out = {"on": switch_on("OPENSMASH_MASTERY_UI"), "arena_card": switch_on("OPENSMASH_ARENA_CARD"),
               "top_on": switch_on("OPENSMASH_TOP_MASTERY"), "arena_url": ARENA_DOWNLOAD}
        if not out["on"]:
            return out
        if time.time() - self.mastery_at > 60:
            self.mastery_at = time.time()
            got = me(self.root)
            self.mastery_mine = got if got is not None else self.mastery_mine
        if out["top_on"] and time.time() >= self.top_next:
            self.top_next = time.time() + poll_next()
            top = service(self.root)
            if top is None:
                top = board(self.root)
            self.mastery_top = top if top is not None else self.mastery_top
        mine = self.mastery_mine
        out.update({"ticket": stored_ticket(self.root) is not None, "available": mine is not None, "fighters": mine or [],
                    "total": sum(f["points"] for f in mine or []), "games": sum(f["games"] for f in mine or []),
                    "wins": sum(f["wins"] for f in mine or []), "top": self.mastery_top})
        return out

    def ranked_view(self, region=None, board=ranked_board):
        """The Competitive page: SSBU Arena's ranked top 10 for a region (None: all), asked again at most every 5 minutes a
        region (the accounts service: poll_seconds);
        rows None while nothing answered (the last answer kept while the service is down)."""
        region = region if region in RANKED_REGIONS else None
        cache = self.__dict__.setdefault("ranked", {})
        at, rows = cache.get(region, (0, None))
        failed = False
        if time.time() - at > poll_seconds() - 5:
            got = board(self.root, region)
            failed = got is None
            rows = got if got is not None else rows
            cache[region] = (time.time(), rows)
        return {"region": region or "", "rows": rows, "failed": failed and rows is None, "arena_url": ARENA_DOWNLOAD,
                "leaderboard_url": ARENA_LEADERBOARD}

    def view(self):
        out = {"available": bool(self.client_id()), "busy": self.busy, "error": self.error}
        t = current(self.root, self.client_id()) if out["available"] else None
        out["signed_in"] = bool(t)
        if not t:
            return out
        if shows_name(self.root) and not self.told:   # (the name the lobbies show follows the account's: once a start)
            self.told = True
            threading.Thread(target=set_shown, args=(self.root, t, True, self.lobbies()), daemon=True).start()
        nsa = nsa_id(self.root)
        rank, fighter_ranks = ranks_in(self._board(), account_hash(t["user_id"]) if t.get("user_id") else "")
        out.update({"name": t.get("display_name") or t.get("username") or "", "avatar": avatar_url(t), "rank": rank,
                    "shown": shows_name(self.root),
                    "fighters": [{"fighter": k, "name": fighter_name(k), "gsp": g, "rank": fighter_ranks.get(k, 0)}
                                 for k, g in self._mine()]})
        return out
