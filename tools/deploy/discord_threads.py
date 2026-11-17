#!/usr/bin/env python3
"""Discord thread tool for the OpenSmash server (stdlib only).

Runs on the lobby VM with the status bot's token, which never leaves the
server: /etc/ssbu-discord-bot/token (root only). Installed root-owned at
/opt/ssbu-discord-tools/discord_threads.py and allowed for one maintenance
user through sudoers (see deploy_discord_threads.sh).

  discord_threads.py guilds
  discord_threads.py channels  [--guild G]
  discord_threads.py list      [--guild G] [--channel C] [--archived]
  discord_threads.py export    THREAD... [--out DIR]
  discord_threads.py archive   THREAD... [--lock]
  discord_threads.py unarchive THREAD...
  discord_threads.py delete    THREAD... --yes

export writes DIR/<thread id>/thread.json, messages.json and every
attachment, then hands DIR to $SUDO_USER.
"""
import argparse
import json
import os
import pwd
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

API = "https://discord.com/api/v10"
TOKEN_FILE = "/etc/ssbu-discord-bot/token"
UA = "DiscordBot (https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate, 1.0)"


def token():
    t = os.environ.get("DISCORD_BOT_TOKEN", "").strip()
    if not t:
        with open(TOKEN_FILE, encoding="utf-8") as f:
            t = f.read().strip()
    return t


def call(method, path, body=None, raw=False):
    url = path if path.startswith("http") else API + path
    data = json.dumps(body).encode() if body is not None else None
    for _ in range(8):
        headers = {"User-Agent": UA}
        if not path.startswith("http") or "discord.com/api" in url:
            headers["Authorization"] = "Bot " + token()
        if data is not None:
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                b = r.read()
                if raw:
                    return b
                return json.loads(b) if b else None
        except urllib.error.HTTPError as e:
            if e.code == 429:
                try:
                    wait = float(json.loads(e.read()).get("retry_after", 1))
                except Exception:
                    wait = 2.0
                time.sleep(wait + 0.2)
                continue
            msg = e.read().decode(errors="replace")[:300]
            raise SystemExit(f"{method} {path}: HTTP {e.code} {msg}")
    raise SystemExit(f"{method} {path}: still rate limited")


def guild_ids(arg):
    if arg:
        return [arg]
    return [g["id"] for g in call("GET", "/users/@me/guilds")]


def thread_row(t, names):
    m = t.get("thread_metadata", {})
    flags = ("A" if m.get("archived") else "-") + ("L" if m.get("locked") else "-")
    return (f"{t['id']}  {flags}  parent={names.get(t.get('parent_id'), t.get('parent_id'))}  "
            f"msgs={t.get('message_count', '?')}  {m.get('create_timestamp', '')[:10]}  {t.get('name', '')}")


def channel_names(gid):
    return {c["id"]: "#" + c["name"] for c in call("GET", f"/guilds/{gid}/channels")}


def cmd_guilds(a):
    for g in call("GET", "/users/@me/guilds"):
        print(g["id"], g["name"])


def cmd_channels(a):
    kinds = {0: "text", 5: "news", 15: "forum", 16: "media"}
    for gid in guild_ids(a.guild):
        for c in sorted(call("GET", f"/guilds/{gid}/channels"), key=lambda c: c.get("position", 0)):
            if c["type"] in kinds:
                print(c["id"], kinds[c["type"]], "#" + c["name"])


def cmd_list(a):
    for gid in guild_ids(a.guild):
        names = channel_names(gid)
        if not a.archived:
            for t in call("GET", f"/guilds/{gid}/threads/active").get("threads", []):
                if not a.channel or t.get("parent_id") == a.channel:
                    print(thread_row(t, names))
            continue
        chans = [a.channel] if a.channel else [
            c for c, _ in names.items()]
        for cid in chans:
            before = None
            while True:
                q = "?limit=100" + (f"&before={urllib.parse.quote(before)}" if before else "")
                try:
                    r = call("GET", f"/channels/{cid}/threads/archived/public{q}")
                except SystemExit:
                    break  # not a thread-capable channel, or no access
                for t in r.get("threads", []):
                    print(thread_row(t, names))
                if not r.get("has_more") or not r.get("threads"):
                    break
                before = r["threads"][-1]["thread_metadata"]["archive_timestamp"]


def give_to_sudo_user(path):
    user = os.environ.get("SUDO_USER")
    if not user or os.geteuid() != 0:
        return
    pw = pwd.getpwnam(user)
    for root, dirs, files in os.walk(path):
        for n in [root] + [os.path.join(root, x) for x in dirs + files]:
            os.chown(n, pw.pw_uid, pw.pw_gid)


def safe_name(s):
    keep = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in s)
    return keep[:120] or "file"


def cmd_export(a):
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    for tid in a.threads:
        th = call("GET", f"/channels/{tid}")
        d = os.path.join(out, tid)
        os.makedirs(d, exist_ok=True)
        msgs, before = [], None
        while True:
            q = "?limit=100" + (f"&before={before}" if before else "")
            page = call("GET", f"/channels/{tid}/messages{q}")
            if not page:
                break
            msgs.extend(page)
            before = page[-1]["id"]
        msgs.reverse()
        # a forum post's first message has the thread's id
        try:
            starter = call("GET", f"/channels/{tid}/messages/{tid}")
            if starter and all(m["id"] != starter["id"] for m in msgs):
                msgs.insert(0, starter)
        except SystemExit:
            pass
        n_att = 0
        for m in msgs:
            for att in m.get("attachments", []):
                fn = os.path.join(d, f"{m['id']}_{safe_name(att.get('filename', 'file'))}")
                if not os.path.exists(fn):
                    with open(fn, "wb") as f:
                        f.write(call("GET", att["url"], raw=True))
                att["saved_as"] = os.path.basename(fn)
                n_att += 1
        with open(os.path.join(d, "thread.json"), "w", encoding="utf-8") as f:
            json.dump(th, f, ensure_ascii=False, indent=1)
        with open(os.path.join(d, "messages.json"), "w", encoding="utf-8") as f:
            json.dump(msgs, f, ensure_ascii=False, indent=1)
        print(f"{tid}  {len(msgs)} messages  {n_att} attachments  {th.get('name', '')}")
    give_to_sudo_user(out)


def cmd_archive(a, archived=True):
    for tid in a.threads:
        body = {"archived": archived}
        if archived and a.lock:
            body["locked"] = True
        if not archived:
            body["locked"] = False
        t = call("PATCH", f"/channels/{tid}", body)
        print(tid, "archived" if archived else "unarchived", "locked" if body.get("locked") else "", t.get("name", ""))


def cmd_delete(a):
    if not a.yes:
        raise SystemExit("delete is permanent: add --yes")
    for tid in a.threads:
        call("DELETE", f"/channels/{tid}")
        print(tid, "deleted")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = p.add_subparsers(dest="cmd", required=True)
    s.add_parser("guilds")
    c = s.add_parser("channels"); c.add_argument("--guild")
    l = s.add_parser("list"); l.add_argument("--guild"); l.add_argument("--channel"); l.add_argument("--archived", action="store_true")
    e = s.add_parser("export"); e.add_argument("threads", nargs="+"); e.add_argument("--out", default="/home/" + os.environ.get("SUDO_USER", "claude") + "/discord_export")
    ar = s.add_parser("archive"); ar.add_argument("threads", nargs="+"); ar.add_argument("--lock", action="store_true")
    u = s.add_parser("unarchive"); u.add_argument("threads", nargs="+")
    d = s.add_parser("delete"); d.add_argument("threads", nargs="+"); d.add_argument("--yes", action="store_true")
    a = p.parse_args()
    for x in getattr(a, "threads", []) or []:
        if not x.isdigit():
            raise SystemExit(f"not a thread id: {x}")
    {"guilds": cmd_guilds, "channels": cmd_channels, "list": cmd_list, "export": cmd_export,
     "archive": cmd_archive, "unarchive": lambda a: cmd_archive(a, False), "delete": cmd_delete}[a.cmd](a)


if __name__ == "__main__":
    main()
