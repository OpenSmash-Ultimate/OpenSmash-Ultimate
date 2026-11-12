#!/usr/bin/env python3
"""Discord's Social SDK beside the program (runtime/net_discord.c): discord_partner_sdk.dll, from OpenSmash-Ultimate's
release DISCORD_TAG, checked against its SHA256SUMS. It is not in the repository: the DLL is Discord's (the Social
SDK's terms let a game ship it; its third-party notices are in NOTICE.md). Without it the game runs as before - no
sign-in with Discord, no Rich Presence - so a failure here is never fatal.

    python tools/discord_fetch.py [build folder]

tools/build_game.py runs it after a Windows build. Only the standard library is used.

The release (the owner publishes it with the rest of plan/EOS_MIGRATION.md): a tag DISCORD_TAG whose assets are
discord_partner_sdk.dll (bin/release of the SDK's Windows package), discord_partner_sdk_notices.txt (the SDK's
License-Notices.txt: the open-source parts inside the DLL, whose notices travel with it) and a SHA256SUMS listing both.
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runtime_fetch   # noqa: E402  (download: GitHub only, Windows' own HTTPS when Python's is refused)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISCORD_TAG = "discord-1"
FILES = ("discord_partner_sdk.dll", "discord_partner_sdk_notices.txt")   # (the SDK's License-Notices.txt, renamed)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def current(build, want):
    """True: every file is there with the release's SHA-256 (nothing to fetch)."""
    for name in FILES:
        path = os.path.join(build, name)
        if not os.path.isfile(path):
            return False
        with open(path, "rb") as f:
            if sha256(f.read()) != want.get(name):
                return False
    return True


def ensure(build=os.path.join(ROOT, "build"), get=runtime_fetch.download, log=print):
    """Puts the DLL and its notices into build; True when they are there. Never raises."""
    if os.environ.get("SSBU_DISCORD") == "0":
        return False
    url = runtime_fetch.URL
    try:
        want = runtime_fetch.sums(get(url.format(tag=DISCORD_TAG, name="SHA256SUMS")).decode("utf-8", "replace"))
        if all(n in want for n in FILES) and current(build, want):
            return True
        for name in FILES:
            if name not in want:
                raise runtime_fetch.Error(f"the release {DISCORD_TAG} has no {name}")
            data = get(url.format(tag=DISCORD_TAG, name=name))
            if sha256(data) != want[name]:
                raise runtime_fetch.Error(f"{name} does not match the release's SHA-256")
            tmp = os.path.join(build, name + ".part")
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, os.path.join(build, name))
        log(f"  Discord ({DISCORD_TAG}): sign-in with Discord and Rich Presence")
        return True
    except (runtime_fetch.Error, OSError, ValueError) as e:
        log(f"  (Discord's SDK not fetched: {e}; the game runs without Discord sign-in and Rich Presence)")
        return False


def main(argv):
    build = argv[1] if len(argv) > 1 else os.path.join(ROOT, "build")
    return 0 if ensure(build) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
