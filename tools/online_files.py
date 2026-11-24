#!/usr/bin/env python3
"""The online files beside the program, made current: Discord's SDK and discord.txt (tools/discord_fetch.py). Run by
every update (tools/install_opensmash.ps1, even when nothing is built), by tools/build_game.py, and by the launchers
before the game starts: issue #389, where an update left them out.

    python tools/online_files.py [build folder]

When the files are there with the hashes of the release this tool names (online_files.txt beside them says which,
written once the fetch succeeded) nothing is downloaded, so a start of the game costs a few file reads. Otherwise the
fetch runs (never fatal). Exit code 0: every file is in place; 1: one could not be had (the game still runs, without
Discord). Only the standard library is used.

(OpenSmash 0.5.0) Epic Online Services are gone: their DLL and eos.txt are no longer fetched (an older stamp that names
their release is simply not current any more: Discord's files are checked again once and the stamp written anew).
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discord_fetch   # noqa: E402
import runtime_fetch   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAMP = "online_files.txt"


def wanted(windows=None):
    """[(tag, files)] for this system: Discord everywhere (its id alone off Windows), none with SSBU_DISCORD=0."""
    windows = os.name == "nt" if windows is None else windows
    out = []
    if os.environ.get("SSBU_DISCORD") != "0":
        out.append((discord_fetch.DISCORD_TAG, discord_fetch.FILES if windows else discord_fetch.ID_ONLY))
    return out


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _stamp_text(build, want):
    lines = [f"# {tag}" for tag, _ in want]
    for _, files in want:
        lines += [f"{_sha256(os.path.join(build, n))}  {n}" for n in files]
    return "\n".join(lines) + "\n"


def current(build, want):
    """True: the stamp names these releases and every file is there with its hash (no network needed)."""
    try:
        with open(os.path.join(build, STAMP), encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return False
    tags = [line[2:].strip() for line in text.splitlines() if line.startswith("# ")]
    if tags != [tag for tag, _ in want]:
        return False
    sums = runtime_fetch.sums(text)
    try:
        return all(sums.get(n) == _sha256(os.path.join(build, n)) for _, files in want for n in files)
    except OSError:
        return False


def ensure(build=os.path.join(ROOT, "build"), log=print, windows=None, discord=None):
    """Every online file of this system in build; True when they all are. Never raises. discord: the fetch (tests)."""
    windows = os.name == "nt" if windows is None else windows
    want = wanted(windows)
    if not want:
        return True
    try:
        os.makedirs(build, exist_ok=True)
        if current(build, want):
            return True
    except OSError:
        pass
    discord = discord or (lambda b: discord_fetch.ensure(b, log=log, files=discord_fetch.FILES if windows else discord_fetch.ID_ONLY))
    ok = True
    for _ in want:
        ok = discord(build) and ok
    if ok:
        try:
            with open(os.path.join(build, STAMP), "w", encoding="utf-8", newline="\n") as f:
                f.write(_stamp_text(build, want))
        except OSError:
            pass
    else:
        log("  (the online files are not all in place: no Discord sign-in in the game; this runs again at the next Play "
            "or update)")
    return ok


def main(argv):
    build = argv[1] if len(argv) > 1 else os.path.join(ROOT, "build")
    return 0 if ensure(build) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
