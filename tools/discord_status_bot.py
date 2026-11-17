"""A tiny Discord bot whose only job is its status line: how many people are on the online lobby now
("21 players in the OpenSmash lobbies"), as other recompilation communities show theirs.

It reads the lobby's own aggregate counts (GET /status.json, tools/nex/board.py: counts only, no player data)
every minute and sets its Discord custom status. It joins no voice, reads no messages and needs no privileged
intents. Installed on the lobby's server by tools/deploy/deploy_discord_bot.sh, which keeps the bot token in
/etc/ssbu-discord-bot/token (never in the repository); the service hands it over as a systemd credential.

    python3 discord_status_bot.py [--status-url URL] [--every SECONDS]
    token: $CREDENTIALS_DIRECTORY/token (systemd), else $DISCORD_BOT_TOKEN
"""
import argparse
import asyncio
import json
import os
import sys
import urllib.request

STATUS_URL = "http://127.0.0.1:39421/status.json"


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
    return line[:128]


def fetch(url):
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return json.loads(r.read().decode("utf-8"))
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--status-url", default=STATUS_URL)
    ap.add_argument("--every", type=int, default=60)
    o = ap.parse_args()
    token = read_token()
    if not token:
        sys.exit("no bot token: put it in /etc/ssbu-discord-bot/token (deploy_discord_bot.sh) or DISCORD_BOT_TOKEN")
    import discord   # (only here: the tests import this file without discord.py)

    client = discord.Client(intents=discord.Intents.none())

    async def loop():
        await client.wait_until_ready()
        last = None
        while not client.is_closed():
            text = status_text(await asyncio.to_thread(fetch, o.status_url))
            if text != last:
                await client.change_presence(status=discord.Status.online, activity=discord.CustomActivity(name=text))
                print(f"status: {text}", flush=True)
                last = text
            await asyncio.sleep(max(15, o.every))

    @client.event
    async def on_ready():
        print(f"signed in as {client.user}", flush=True)

    async def run():
        async with client:
            client.loop.create_task(loop())
            await client.start(token)

    asyncio.run(run())


if __name__ == "__main__":
    main()
