"""Sign-in with Discord for the web launcher, the same as OpenSmash.exe's (OpenSmash.Launcher/DiscordAccount.cs) and the
game's (runtime/net_discord.c): config/discord_token.txt holds the token all three share (key=value lines; delete it
to sign out); the game signs in to EOS with it.

OAuth2 with PKCE: the Discord app "OpenSmash" is a Public Client, so no secret is involved. The browser comes back to
http://127.0.0.1/callback (port 80) where this computer lets a program listen there (Windows, macOS); on Linux, where
ports under 1024 need root, to http://127.0.0.1:53134/callback - a redirect the Discord application must list too.

Also what the account box shows: the player's GSP per fighter (nex_powers.bin beside the save folder: the records the
game posts) and their position in the leaderboard (the site's leaderboard.json, made from EOS's leaderboard).
Standard library only."""
import base64
import hashlib
import json
import os
import secrets
import socket
import struct
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

APP_ID = "1069217215540371466"
API = "https://discord.com/api"
REDIRECTS = (("http://127.0.0.1/callback", 80), ("http://127.0.0.1:53134/callback", 53134))
LEADERBOARD_URL = "https://denkishi.github.io/opensmash-site/leaderboard.json"
SCOPES = "identify openid sdk.social_layer_presence"
KEYS = ("access_token", "refresh_token", "expires_at", "scope", "user_id", "username", "display_name", "avatar")
ENTRY = 33   # (nex_powers.bin: u64 pid, then the record: u8 fighter, u32 GSP, u32, u32, u64 date, u32)
NAMES = [
    "Mario", "Donkey Kong", "Link", "Samus", "Dark Samus", "Yoshi", "Kirby", "Fox", "Pikachu", "Luigi",
    "Ness", "Captain Falcon", "Jigglypuff", "Peach", "Daisy", "Bowser", "Sheik", "Zelda", "Dr. Mario", "Pichu",
    "Falco", "Marth", "Lucina", "Young Link", "Ganondorf", "Mewtwo", "Roy", "Chrom", "Mr. Game & Watch", "Meta Knight",
    "Pit", "Dark Pit", "Zero Suit Samus", "Wario", "Snake", "Ike", "Squirtle", "Ivysaur", "Charizard", "Diddy Kong",
    "Lucas", "Sonic", "King Dedede", "Olimar", "Lucario", "R.O.B.", "Toon Link", "Wolf", "Villager", "Mega Man",
    "Wii Fit Trainer", "Rosalina & Luma", "Little Mac", "Greninja", "Palutena", "Pac-Man", "Robin", "Shulk", "Bowser Jr.", "Duck Hunt",
    "Ryu", "Ken", "Cloud", "Corrin", "Bayonetta", "Inkling", "Ridley", "Simon", "Richter", "King K. Rool",
    "Isabelle", "Incineroar", "Mii Brawler", "Mii Swordfighter", "Mii Gunner", "Ice Climbers", None, None, None, None,
    None, "Piranha Plant", "Joker", "Hero", "Banjo & Kazooie", "Terry", "Byleth", "Min Min", "Steve", "Sephiroth",
    "Pyra", "Mythra", "Kazuya", "Sora"]   # (by fighter kind, as runtime/net_discord_core.c)


class SignInError(Exception):
    """Something the player can act on; the message says what."""


def fighter_name(kind):
    if kind == 114:
        return "Pokemon Trainer"
    return NAMES[kind] if 0 <= kind < len(NAMES) and NAMES[kind] else f"Fighter {kind}"


def token_path(root):
    return os.path.join(root, "config", "discord_token.txt")


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
        "Content-Type": "application/x-www-form-urlencoded", "User-Agent": "OpenSmash-Launcher"})
    try:
        with opener(req, timeout=20) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        raise SignInError(f"Discord refused the sign-in ({e.code}).") from None
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise SignInError(f"Discord could not be reached ({e}).") from None


def _user(t, opener=urllib.request.urlopen):
    req = urllib.request.Request(API + "/users/@me", headers={"Authorization": "Bearer " + t["access_token"], "User-Agent": "OpenSmash-Launcher"})
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


def current(root, post=_post, user=_user):
    """A token good for a while (refreshed and saved when it expired), or None."""
    t = load(root)
    if not t:
        return None
    if fresh(t, time.time(), 300):
        return t
    if not t.get("refresh_token"):
        return None
    try:
        t = _took(t, post(API + "/oauth2/token", {"client_id": APP_ID, "grant_type": "refresh_token", "refresh_token": t["refresh_token"]}))
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


def sign_in(root, open_url=webbrowser.open, post=_post, user=_user, timeout=300, listen=_listen):
    """The browser's sign-in, start to end: the token (saved), or SignInError."""
    verifier = b64url(secrets.token_bytes(48))
    challenge = b64url(hashlib.sha256(verifier.encode()).digest())
    state = b64url(secrets.token_bytes(16))
    server, redirect = listen()
    try:
        open_url("https://discord.com/oauth2/authorize?" + urllib.parse.urlencode({
            "response_type": "code", "client_id": APP_ID, "scope": SCOPES, "redirect_uri": redirect,
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
                t = _took({}, post(API + "/oauth2/token", {"client_id": APP_ID, "grant_type": "authorization_code",
                                                            "code": q["code"], "redirect_uri": redirect, "code_verifier": verifier}))
                user(t)
                save(root, t)
                return t
        raise SignInError("The sign-in took too long: try again.")
    finally:
        server.close()


def records(root):
    """The player's GSP per fighter, highest first: [(fighter kind, GSP)] of the newest player in nex_powers.bin."""
    try:
        with open(os.path.join(root, "nex_powers.bin"), "rb") as f:
            data = f.read()
    except OSError:
        return []
    return records_of(data)


def records_of(data):
    entries = [data[i:i + ENTRY] for i in range(0, len(data) - ENTRY + 1, ENTRY)]
    if not entries:
        return []
    newest = max(entries, key=lambda e: struct.unpack_from("<Q", e, 8 + 13)[0])
    pid = newest[:8]
    mine = [(e[8], struct.unpack_from("<I", e, 9)[0]) for e in entries if e[:8] == pid]
    return sorted(mine, key=lambda r: -r[1])


def ranks_in(board, discord_id):
    """The player's (overall position, {fighter: position}) in leaderboard.json; (0, {}) when not there."""
    for p in (board or {}).get("players") or []:
        if discord_id and isinstance(p, dict) and p.get("discord_id") == discord_id:
            fighters = {}
            for k, v in (p.get("fighters") or {}).items():
                if isinstance(v, dict) and isinstance(v.get("rank"), int) and k.isdigit():
                    fighters[int(k)] = v["rank"]
            return (p.get("rank") if isinstance(p.get("rank"), int) else 0), fighters
    return 0, {}


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

    def __init__(self, root):
        self.root = root
        self.busy = False
        self.error = ""
        self.board = None
        self.board_at = 0
        self.lock = threading.Lock()

    def start_sign_in(self):
        with self.lock:
            if self.busy:
                return
            self.busy, self.error = True, ""

        def run():
            try:
                sign_in(self.root)
                self.board_at = 0
            except SignInError as e:
                self.error = str(e)
            finally:
                self.busy = False
        threading.Thread(target=run, daemon=True).start()

    def sign_out(self):
        sign_out(self.root)

    def _board(self):
        if time.time() - self.board_at > 600:
            self.board_at = time.time()
            try:
                req = urllib.request.Request(LEADERBOARD_URL, headers={"User-Agent": "OpenSmash-Launcher"})
                with urllib.request.urlopen(req, timeout=10) as r:
                    self.board = json.loads(r.read().decode())
            except (urllib.error.URLError, OSError, ValueError):
                pass
        return self.board

    def view(self):
        t = current(self.root)
        out = {"signed_in": bool(t), "busy": self.busy, "error": self.error}
        if not t:
            return out
        rank, fighter_ranks = ranks_in(self._board(), t.get("user_id"))
        out.update({"name": t.get("display_name") or t.get("username") or "", "avatar": avatar_url(t), "rank": rank,
                    "fighters": [{"fighter": k, "name": fighter_name(k), "gsp": g, "rank": fighter_ranks.get(k, 0)}
                                 for k, g in records(self.root)]})
        return out
