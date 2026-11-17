"""A tiny Discord bot for the OpenSmash server.

1. Its status line: how many people are on the online lobby now ("21 players in the OpenSmash lobbies"), as other
   recompilation communities show theirs. It reads the lobby's own aggregate counts (GET /status.json,
   tools/nex/board.py: counts only, no player data) every minute and sets its Discord custom status.
2. The feedback forums of the OpenSmash server only (guild GUILD_ID; #help, #bug-reports, #smash-modding,
   FEEDBACK_CHANNELS; every other server and channel is ignored before any API call):
   - when a NEW thread is opened there and, after a short wait, neither its first post nor its author's next posts
     carry a play.log (or a crash report), it replies ONCE asking for build\\play.log and the version;
   - a digest of those threads (title, author, has a log, last activity) is written at start and then every day to
     $STATE_DIRECTORY/threads_digest.json on the server (/var/lib/ssbu-discord-bot, root only) and summed up in the
     service's log; `systemctl kill -s USR1 ssbu-discord-bot` (or a restart) writes it now. Nothing is posted
     anywhere else.
   Seeing attachments needs the Message Content intent (Developer Portal, Bot tab). The bot reads its application's
   flags at start: without that intent it never replies (it could not tell whether a log is there) and the digest
   says has_log null. The gateway itself asks for the non-privileged guilds intent only, so a portal change never
   stops the bot from signing in.

Opt-out, in /etc/ssbu-discord-bot/bot.env (optional, read by systemd; restart the service after a change):
    SSBU_BOT_THREADS=0          no forum reading at all (status line only, as before)
    SSBU_BOT_LOG_REPLY=0        no reply asking for play.log (the digest stays)
    SSBU_BOT_DIGEST_HOURS=0     no digest (default 24)
    SSBU_BOT_REPLY_DELAY=60     seconds to wait after a new thread before looking for a log

Installed on the lobby's server by tools/deploy/deploy_discord_bot.sh (updated by ssbu_admin.sh update-bot), which
keeps the bot token in /etc/ssbu-discord-bot/token (never in the repository); the service hands it over as a systemd
credential.

    python3 discord_status_bot.py [--status-url URL] [--every SECONDS]
    token: $CREDENTIALS_DIRECTORY/token (systemd), else $DISCORD_BOT_TOKEN
"""
import argparse
import asyncio
import datetime
import json
import os
import re
import signal
import sys
import time
import urllib.error
import urllib.request

STATUS_URL = "http://127.0.0.1:39421/status.json"
API = "https://discord.com/api/v10"
UA = "DiscordBot (https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate, 1.0)"

GUILD_ID = "1561342604698849280"   # the OpenSmash server: the only one whose threads are read
FEEDBACK_CHANNELS = {
    "1561480589591314594": "help",
    "1561545181965189142": "bug-reports",
    "1566881316610515124": "smash-modding",
}
DISCORD_EPOCH_MS = 1420070400000
FLAG_MESSAGE_CONTENT = 1 << 18          # application flags: GATEWAY_MESSAGE_CONTENT
FLAG_MESSAGE_CONTENT_LIMITED = 1 << 19  # GATEWAY_MESSAGE_CONTENT_LIMITED (under 100 servers, toggled on)
NEW_THREAD_MAX_AGE = 15 * 60            # a "new" thread older than this (a late event) gets no reply

LOG_REPLY = (
    "Hi {mention}, thanks for the report! To find the cause we need your **play.log**:\n"
    "- it is at `<OpenSmash folder>\\build\\play.log`; the **Report a problem** button at the bottom of the "
    "launcher opens that folder with the file selected. Drag it into this thread.\n"
    "- after a crash, please also add the `.txt` report from the `reports\\` folder.\n"
    "- and tell us your **version** (the launcher shows it as GAME ... at the bottom).\n"
    "Read the files first: they can contain folder names and player names. Thank you!"
)


def read_token():
    cred = os.environ.get("CREDENTIALS_DIRECTORY")
    if cred and os.path.exists(os.path.join(cred, "token")):
        with open(os.path.join(cred, "token"), encoding="utf-8") as f:
            return f.read().strip()
    return os.environ.get("DISCORD_BOT_TOKEN", "").strip()


def status_text(counts):
    """The line shown under the bot's name, from /status.json's counts (None: the lobby did not answer)."""
    if counts is None:
        return "🔴 Lobby offline"
    n, rooms = int(counts.get("online", 0)), int(counts.get("open_rooms", 0))
    line = f"🟢 {n} player{'' if n == 1 else 's'} in the OpenSmash lobbies"
    if rooms:
        line += f" · {rooms} open room{'' if rooms == 1 else 's'}"
    played = counts.get("matches")   # (a lobby from before the count has none: nothing said)
    total = played.get("total") if isinstance(played, dict) else None
    if type(total) is int and total > 0:
        line += f" · {total:,} match{'' if total == 1 else 'es'} played"
    return line[:128]


def fetch(url):
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return json.loads(r.read().decode("utf-8"))
    except Exception:
        return None


# ---- feedback threads ------------------------------------------------------------------------------------------

def _flag(env, name, default):
    v = env.get(name, "").strip().lower()
    if not v:
        return default
    return v not in ("0", "no", "off", "false")


def _num(env, name, default):
    try:
        return float(env.get(name, "").strip() or default)
    except ValueError:
        return default


class ThreadConfig:
    def __init__(self, threads=True, log_reply=True, digest_hours=24.0, reply_delay=60.0, state_dir=".",
                 guild_id=GUILD_ID, channels=None, digest_days=30):
        self.threads = threads
        self.log_reply = log_reply
        self.digest_hours = digest_hours
        self.reply_delay = reply_delay
        self.state_dir = state_dir
        self.guild_id = guild_id
        self.channels = dict(FEEDBACK_CHANNELS if channels is None else channels)
        self.digest_days = digest_days

    @classmethod
    def from_env(cls, env):
        return cls(threads=_flag(env, "SSBU_BOT_THREADS", True),
                   log_reply=_flag(env, "SSBU_BOT_LOG_REPLY", True),
                   digest_hours=max(0.0, _num(env, "SSBU_BOT_DIGEST_HOURS", 24.0)),
                   reply_delay=max(0.0, _num(env, "SSBU_BOT_REPLY_DELAY", 60.0)),
                   state_dir=env.get("STATE_DIRECTORY", "").split(":")[0] or ".")


class DiscordError(Exception):
    def __init__(self, status, what):
        super().__init__(f"HTTP {status}: {what}")
        self.status = status


class DiscordRest:
    """Discord's REST API with the bot token (urllib only). Retries 429s."""

    def __init__(self, token, opener=None, sleep=time.sleep):
        self.token = token
        self.opener = opener or (lambda req: urllib.request.urlopen(req, timeout=30))
        self.sleep = sleep

    def call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        for _ in range(6):
            headers = {"User-Agent": UA, "Authorization": "Bot " + self.token}
            if data is not None:
                headers["Content-Type"] = "application/json"
            req = urllib.request.Request(API + path, data=data, method=method, headers=headers)
            try:
                with self.opener(req) as r:
                    b = r.read()
                    return json.loads(b) if b else None
            except urllib.error.HTTPError as e:
                text = e.read()
                if e.code == 429:
                    try:
                        wait = float(json.loads(text).get("retry_after", 1))
                    except Exception:
                        wait = 2.0
                    self.sleep(min(wait, 60) + 0.2)
                    continue
                raise DiscordError(e.code, f"{method} {path}: {text[:200]!r}")
        raise DiscordError(429, f"{method} {path}: still rate limited")


def snowflake_time(sid):
    """Unix seconds a Discord id was made at."""
    return ((int(sid) >> 22) + DISCORD_EPOCH_MS) / 1000.0


def iso(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


_LOG_NAME = re.compile(r"(\.log|\.dmp)$|^(play|crash|report|ssbu|opensmash)[^/\\]*\.(txt|zip|7z)$", re.I)


def is_log_attachment(name):
    """play.log (also renamed: play (1).log, play_old.log), crash reports from reports\\, zipped reports."""
    return bool(_LOG_NAME.search((name or "").strip()))


def has_log(messages, author_id=None):
    for m in messages:
        if author_id and str(m.get("author", {}).get("id")) != str(author_id):
            continue
        if any(is_log_attachment(a.get("filename")) for a in m.get("attachments") or []):
            return True
    return False


def message_content_enabled(app):
    """From GET /applications/@me: is the Message Content intent on in the Developer Portal?"""
    flags = int((app or {}).get("flags") or 0)
    return bool(flags & (FLAG_MESSAGE_CONTENT | FLAG_MESSAGE_CONTENT_LIMITED))


class ThreadWatcher:
    """New-thread replies and the digest. Blocking (REST); the bot calls it from a worker thread."""

    def __init__(self, api, cfg, bot_id, content_ok, log=print, sleep=time.sleep, now=time.time):
        self.api, self.cfg, self.bot_id, self.content_ok = api, cfg, str(bot_id or ""), content_ok
        self.log, self.sleep, self.now = log, sleep, now
        self.replied_path = os.path.join(cfg.state_dir, "replied_threads.json")
        self.replied = self._load_replied()

    def _load_replied(self):
        try:
            with open(self.replied_path, encoding="utf-8") as f:
                return set(str(x) for x in json.load(f))
        except (OSError, ValueError):
            return set()

    def _save_replied(self):
        _write_json(self.replied_path, sorted(self.replied)[-5000:])

    def in_scope(self, thread):
        return (self.cfg.threads and str(thread.get("guild_id")) == self.cfg.guild_id
                and str(thread.get("parent_id")) in self.cfg.channels)

    def _messages(self, tid, limit=100):
        try:
            return self.api.call("GET", f"/channels/{tid}/messages?limit={limit}") or []
        except DiscordError as e:
            self.log(f"threads: cannot read {tid}: {e}")
            return []

    def _starter(self, tid, tries=1):
        for i in range(tries):
            try:
                return self.api.call("GET", f"/channels/{tid}/messages/{tid}")
            except DiscordError as e:
                if e.status != 404 or i + 1 == tries:
                    return None
                self.sleep(3)
        return None

    def on_new_thread(self, thread):
        """A thread just created. Returns what was done (a word, for the log and the tests)."""
        if not self.in_scope(thread):
            return "out-of-scope"
        tid = str(thread["id"])
        if tid in self.replied:
            return "already"
        if self.now() - snowflake_time(tid) > NEW_THREAD_MAX_AGE:
            return "old"
        owner = str(thread.get("owner_id") or "")
        if self.bot_id and owner == self.bot_id:
            return "own"
        if not self.cfg.log_reply:
            return "reply-off"
        if not self.content_ok:
            return "no-content-intent"
        starter = self._starter(tid, tries=4)
        msgs = self._messages(tid, 50)
        if starter and all(str(m.get("id")) != str(starter.get("id")) for m in msgs):
            msgs.append(starter)
        if any(str(m.get("author", {}).get("id")) == self.bot_id for m in msgs):
            self.replied.add(tid)
            self._save_replied()
            return "already"
        author = (starter or {}).get("author") or {}
        owner = owner or str(author.get("id") or "")
        if author.get("bot"):
            return "bot-author"
        if has_log(msgs, owner):
            return "has-log"
        body = {"content": LOG_REPLY.format(mention=f"<@{owner}>" if owner else "there"),
                "allowed_mentions": {"users": [owner] if owner else []}}
        if starter:
            body["message_reference"] = {"message_id": str(starter["id"]), "fail_if_not_exists": False}
        self.api.call("POST", f"/channels/{tid}/messages", body)
        self.replied.add(tid)
        self._save_replied()
        return "replied"

    def _threads(self):
        seen = {}
        try:
            for t in self.api.call("GET", f"/guilds/{self.cfg.guild_id}/threads/active").get("threads", []):
                if str(t.get("parent_id")) in self.cfg.channels:
                    seen[str(t["id"])] = t
        except DiscordError as e:
            self.log(f"digest: active threads: {e}")
        for cid in self.cfg.channels:
            try:
                r = self.api.call("GET", f"/channels/{cid}/threads/archived/public?limit=50") or {}
            except DiscordError as e:
                self.log(f"digest: archived threads of {cid}: {e}")
                continue
            for t in r.get("threads", []):
                seen.setdefault(str(t["id"]), t)
        return list(seen.values())

    def digest(self):
        now = self.now()
        rows = []
        for t in self._threads():
            tid = str(t["id"])
            last = max(snowflake_time(tid), snowflake_time(t["last_message_id"]) if t.get("last_message_id") else 0)
            if now - last > self.cfg.digest_days * 86400:
                continue
            starter = self._starter(tid)
            author = (starter or {}).get("author") or {}
            owner = str(t.get("owner_id") or author.get("id") or "")
            log = None
            if self.content_ok:
                msgs = self._messages(tid)
                if starter:
                    msgs.append(starter)
                log = has_log(msgs)
            meta = t.get("thread_metadata") or {}
            rows.append({
                "id": tid,
                "channel": self.cfg.channels.get(str(t.get("parent_id")), str(t.get("parent_id"))),
                "title": t.get("name", ""),
                "author": author.get("global_name") or author.get("username") or owner,
                "author_id": owner,
                "has_log": log,
                "created": iso(snowflake_time(tid)),
                "last_activity": iso(last),
                "messages": t.get("message_count"),
                "archived": bool(meta.get("archived")),
                "asked_for_log": tid in self.replied,
                "url": f"https://discord.com/channels/{self.cfg.guild_id}/{tid}",
            })
        rows.sort(key=lambda r: r["last_activity"], reverse=True)
        return {"generated_at": iso(now), "guild_id": self.cfg.guild_id, "message_content_intent": self.content_ok,
                "threads": rows}

    def write_digest(self):
        d = self.digest()
        path = os.path.join(self.cfg.state_dir, "threads_digest.json")
        _write_json(path, d)
        rows = d["threads"]
        no_log = sum(1 for r in rows if r["has_log"] is False)
        unknown = sum(1 for r in rows if r["has_log"] is None)
        self.log(f"digest: {len(rows)} threads, {no_log} without a log, {unknown} unknown -> {path}")
        week = iso(self.now() - 7 * 86400)
        for r in rows:
            if r["last_activity"] >= week:
                log = {True: "yes", False: "no", None: "?"}[r["has_log"]]
                self.log(f"digest  #{r['channel']:<13} log={log:<3} {r['last_activity'][:10]}  "
                         f"{str(r['author'])[:24]:<24}  {r['title'][:70]}")
        return d


def _write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)


# ---- the bot ---------------------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--status-url", default=STATUS_URL)
    ap.add_argument("--every", type=int, default=60)
    o = ap.parse_args()
    token = read_token()
    if not token:
        sys.exit("no bot token: put it in /etc/ssbu-discord-bot/token (deploy_discord_bot.sh) or DISCORD_BOT_TOKEN")
    import discord   # (only here: the tests import this file without discord.py)

    cfg = ThreadConfig.from_env(os.environ)
    rest = DiscordRest(token)
    watcher = None
    if cfg.threads:
        try:
            content_ok = message_content_enabled(rest.call("GET", "/applications/@me"))
        except Exception as e:
            print(f"threads: cannot read the application's flags ({e}): taken as no Message Content intent", flush=True)
            content_ok = False
        print("threads: on (" + ", ".join("#" + n for n in cfg.channels.values()) + "); Message Content intent "
              + ("on" if content_ok else "OFF in the Developer Portal: no log replies, digest has_log unknown")
              + ("" if cfg.log_reply else "; log replies off (SSBU_BOT_LOG_REPLY=0)"), flush=True)
        watcher = ThreadWatcher(rest, cfg, "", content_ok, log=lambda s: print(s, flush=True))
    else:
        print("threads: off (SSBU_BOT_THREADS=0)", flush=True)

    intents = discord.Intents.none()
    intents.guilds = watcher is not None   # THREAD_CREATE; not privileged
    client = discord.Client(intents=intents)
    digest_now = asyncio.Event()

    async def status_loop():
        await client.wait_until_ready()
        last = None
        while not client.is_closed():
            text = status_text(await asyncio.to_thread(fetch, o.status_url))
            if text != last:
                await client.change_presence(status=discord.Status.online, activity=discord.CustomActivity(name=text))
                print(f"status: {text}", flush=True)
                last = text
            await asyncio.sleep(max(15, o.every))

    async def digest_loop():
        await client.wait_until_ready()
        await asyncio.sleep(20)
        while not client.is_closed():
            digest_now.clear()
            try:
                await asyncio.to_thread(watcher.write_digest)
            except Exception as e:
                print(f"digest failed: {e!r}", flush=True)
            try:
                await asyncio.wait_for(digest_now.wait(), timeout=cfg.digest_hours * 3600)
            except asyncio.TimeoutError:
                pass

    @client.event
    async def on_ready():
        print(f"signed in as {client.user}", flush=True)
        if watcher is None:
            return
        watcher.bot_id = str(client.user.id)
        guild = client.get_guild(int(cfg.guild_id))
        if guild is None:
            print(f"threads: the bot is not in guild {cfg.guild_id}", flush=True)
            return
        for cid, name in cfg.channels.items():
            ch = guild.get_channel(int(cid))
            if ch is None:
                print(f"threads: #{name} ({cid}) not visible to the bot", flush=True)
                continue
            try:
                p = ch.permissions_for(guild.me)
                need = ["view_channel", "read_message_history"] + (["send_messages_in_threads"] if cfg.log_reply else [])
                missing = [n for n in need if not getattr(p, n)]
                if missing:
                    print(f"threads: #{name} missing permissions: {', '.join(missing)}", flush=True)
            except Exception as e:
                print(f"threads: #{name} permissions unknown ({e!r})", flush=True)

    @client.event
    async def on_thread_create(thread):
        # (discord.py calls this only for newly created threads; joining an old one is on_thread_join)
        if watcher is None:
            return
        d = {"id": str(thread.id), "guild_id": str(thread.guild.id), "parent_id": str(thread.parent_id),
             "owner_id": str(thread.owner_id or ""), "name": thread.name}
        if not watcher.in_scope(d):   # other servers and channels: nothing read, nothing logged
            return
        await asyncio.sleep(cfg.reply_delay)
        try:
            what = await asyncio.to_thread(watcher.on_new_thread, d)
            print(f"threads: new {d['id']} in #{cfg.channels[d['parent_id']]}: {what}", flush=True)
        except Exception as e:
            print(f"threads: new {d['id']} failed: {e!r}", flush=True)

    async def run():
        async with client:
            client.loop.create_task(status_loop())
            if watcher is not None and cfg.digest_hours > 0:
                client.loop.create_task(digest_loop())
                try:
                    client.loop.add_signal_handler(signal.SIGUSR1, digest_now.set)
                except (NotImplementedError, AttributeError, RuntimeError):
                    pass
            await client.start(token)

    asyncio.run(run())


if __name__ == "__main__":
    main()
