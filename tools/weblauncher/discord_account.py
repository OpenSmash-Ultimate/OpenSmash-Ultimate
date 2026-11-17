"""Sign-in with Discord for the web launcher, the same as OpenSmash.exe's (OpenSmash.Launcher/DiscordAccount.cs) and the
game's (runtime/net_discord.c): config/discord_token.txt holds the token all three share (key=value lines; delete it
to sign out).

OAuth2 with PKCE: the Discord application is a Public Client, so no secret is involved; its id is discord.txt beside
the game's program (a release puts it there: tools/discord_fetch.py), and without it there is no sign-in. The browser
comes back to http://127.0.0.1/callback (port 80) where this computer lets a program listen there (Windows, macOS); on
Linux, where ports under 1024 need root, to http://127.0.0.1:53134/callback - a redirect the Discord application must
list too.

Also what the account box shows: the player's GSP per fighter, kept by the lobbies' own game server (GET /gsp/me,
tools/nex/board.py, signed with this install's NSA id from netplay_id.txt), and their global position (the one public
lobby's leaderboard.json: every GSP result goes through its game server). The GSP belongs to the Discord account: this install is linked to it with
POST /gsp/link (the token is sent once, the lobby verifies it with Discord and keeps nothing of it; show on/off is the
choice config/discord_leaderboard.txt remembers). Standard library only."""
import base64
import hashlib
import hmac
import json
import os
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
LEADERBOARD_URL = "http://opensmash-lobby.duckdns.org:39421/leaderboard.json"   # (the one public lobby's: every GSP result)
PUBLIC_LOBBIES = ("opensmash-lobby.duckdns.org:39421",)
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


# ---- the player on the lobbies' game server (tools/nex/board.py) ------------------------------------------------
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


def lobbies(server_line=""):
    """The lobbies to ask: the public ones, and online_server.txt's own (its first line given) when it names another."""
    out = list(PUBLIC_LOBBIES)
    words = (server_line or "").split()
    host = words[0] if words else ""
    if host and host.lower() not in ("auto", "na", "eu", "off", "lan") and not host.lower().startswith("smash.recomps.wtf"):
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


def records(root, where, ask=_ask):
    """GET /gsp/me on the lobbies where, merged: [(fighter kind, GSP)], the highest GSP of each fighter, highest first."""
    nsa = nsa_id(root)
    if not nsa:
        return []
    player, best = player_id(nsa), {}
    for lobby in where:
        t = int(time.time())
        me = ask(f"http://{lobby}/gsp/me", {"X-OpenSmash-Player": player, "X-OpenSmash-Time": str(t),
                                             "X-OpenSmash-Sig": sign(nsa, "me", player, t)})
        for f in (me or {}).get("fighters") or []:
            if isinstance(f, dict) and isinstance(f.get("fighter"), int) and isinstance(f.get("gsp"), int):
                best[f["fighter"]] = max(best.get(f["fighter"], 0), f["gsp"])
    return sorted(best.items(), key=lambda r: (-r[1], r[0]))


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

    def client_id(self):
        return app_id(self.program_dir)

    def lobbies(self):
        return lobbies(self.server_line())

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
            self.board = _ask(LEADERBOARD_URL) or self.board
        return self.board

    def _mine(self):
        if time.time() - self.mine_at > 60:
            self.mine_at = time.time()
            self.mine = records(self.root, self.lobbies())
        return self.mine

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
