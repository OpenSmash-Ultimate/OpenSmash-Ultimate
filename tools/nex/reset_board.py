#!/usr/bin/env python3
"""Empties the leaderboard for a release: the fighter records (nex_powers.json) and the ratings (nex_ratings.json) of the
online lobby's state directory, after a copy of them in DIR/backup-board-<UTC YYYYmmdd-HHMMSS>/. The Discord links
(nex_profiles.json) are kept, so registered players stay registered, unless --profiles; the accounts
(nex_accounts.json) are never touched. Without --yes it only says what it would do.

Run it while the lobby is STOPPED: the lobby keeps all of this in memory and writes it back.

    python3 reset_board.py --state-dir /var/lib/ssbu-online [--profiles] [--yes]"""
import argparse
import os
import shutil
import sys
import time

RESET = ("nex_powers.json", "nex_ratings.json")
PROFILES = "nex_profiles.json"


def empty_file(path, like):
    """path made an empty JSON object (atomically), owned as `like` was."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("{}\n")
    try:
        st = os.stat(like)
        os.chown(tmp, st.st_uid, st.st_gid)
        os.chmod(tmp, st.st_mode & 0o7777)
    except (AttributeError, OSError):
        pass
    os.replace(tmp, path)


def reset(state_dir, profiles=False, yes=False, now=None, say=print):
    """The backup folder made (None without yes or when there was nothing to reset)."""
    names = [n for n in RESET + ((PROFILES,) if profiles else ()) if os.path.exists(os.path.join(state_dir, n))]
    backup = [n for n in RESET + (PROFILES,) if os.path.exists(os.path.join(state_dir, n))]
    if not names:
        say(f"nothing to reset in {state_dir}")
        return None
    folder = os.path.join(state_dir, time.strftime("backup-board-%Y%m%d-%H%M%S", time.gmtime(now)))
    say(f"{'would copy' if not yes else 'copying'} {', '.join(backup)} to {folder}")
    say(f"{'would empty' if not yes else 'emptying'} {', '.join(names)}"
        + ("" if profiles else f" (keeping {PROFILES}: the Discord links)") + "; nex_accounts.json is left alone")
    if not yes:
        say("nothing done: add --yes (with the lobby stopped)")
        return None
    os.makedirs(folder)
    for n in backup:
        shutil.copy2(os.path.join(state_dir, n), folder)
    for n in names:
        empty_file(os.path.join(state_dir, n), os.path.join(state_dir, n))
    say(f"backup: {folder}")
    return folder


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--state-dir", required=True, help="the lobby's --state directory")
    ap.add_argument("--profiles", action="store_true", help="also empty the Discord links (nex_profiles.json)")
    ap.add_argument("--yes", action="store_true", help="do it (the lobby must be stopped); else only say what it would do")
    o = ap.parse_args(argv)
    if not os.path.isdir(o.state_dir):
        print(f"no such directory: {o.state_dir}", file=sys.stderr)
        return 2
    reset(o.state_dir, o.profiles, o.yes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
