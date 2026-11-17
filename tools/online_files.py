#!/usr/bin/env python3
"""The online files beside the program, made current: Epic Online Services' DLL and eos.txt (tools/eos_fetch.py,
Windows) and Discord's SDK and discord.txt (tools/discord_fetch.py). Run by every update (tools/install_windowsmash.ps1,
even when nothing is built), by tools/build_game.py, and by the launchers before the game starts: issue #389, where an
update left them out and the game went online through the public lobby alone, unseen by the players on EOS.

    python tools/online_files.py [build folder]

When the files are there with the hashes of the releases these tools name (online_files.txt beside them says which,
written once both fetches succeeded) nothing is downloaded, so a start of the game costs a few file reads. Otherwise
the fetches run (each never fatal). Exit code 0: every file is in place; 1: one could not be had (the game still runs,
online through the public lobby and without Discord). Only the standard library is used.
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discord_fetch   # noqa: E402
import eos_fetch       # noqa: E402
import runtime_fetch   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAMP = "online_files.txt"


def wanted(windows=None):
    """[(tag, files)] for this system: EOS on Windows only (runtime/net_eos.c), Discord everywhere (its id alone off
    Windows)."""
    windows = os.name == "nt" if windows is None else windows
    out = []
    if windows and os.environ.get("SSBU_EOS") != "0":
        out.append((eos_fetch.EOS_TAG, eos_fetch.FILES))
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


def ensure(build=os.path.join(ROOT, "build"), log=print, windows=None, eos=None, discord=None):
    """Every online file of this system in build; True when they all are. Never raises. eos and discord: the fetches
    (tests)."""
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
    eos = eos or (lambda b: eos_fetch.ensure(b, log=log))
    discord = discord or (lambda b: discord_fetch.ensure(b, log=log, files=discord_fetch.FILES if windows else discord_fetch.ID_ONLY))
    ok = True
    for tag, _ in want:
        ok = (eos(build) if tag == eos_fetch.EOS_TAG else discord(build)) and ok
    if ok:
        try:
            with open(os.path.join(build, STAMP), "w", encoding="utf-8", newline="\n") as f:
                f.write(_stamp_text(build, want))
        except OSError:
            pass
    else:
        log("  (the online files are not all in place: the game goes online through the public lobby, unseen by the players "
            "on Epic Online Services; this runs again at the next Play or update)")
    return ok


def main(argv):
    build = argv[1] if len(argv) > 1 else os.path.join(ROOT, "build")
    return 0 if ensure(build) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
