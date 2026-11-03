#!/usr/bin/env python3
"""Where the big folders live: dump\\ (the game's files, the shader cache) and build\\ (the compiled game).
Together they take tens of GB; a player whose system drive is full puts them on another drive.

    python tools/data_location.py                 where each one is now
    python tools/data_location.py --move D:\\OS    move both into D:\\OS\\dump and D:\\OS\\build

The folders are moved, and a link is left in their place (a directory junction on Windows, a symbolic
link elsewhere), so the build, the game, the launcher and every script find them where they were.
Moving them back is the same command with the repository's own folder. Close the game and the
launcher first: a file in use stops the move, and what was moved so far is put back.
Only the standard library is used.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDERS = ("dump", "build")


def is_link(path):
    """A junction or a symbolic link (os.path.islink misses junctions before Python 3.12)."""
    if os.path.islink(path):
        return True
    isjunction = getattr(os.path, "isjunction", None)
    if isjunction:
        return isjunction(path)
    if os.name == "nt" and os.path.isdir(path):
        try:
            return os.stat(path, follow_symlinks=False).st_file_attributes & 0x400 != 0   # FILE_ATTRIBUTE_REPARSE_POINT
        except (OSError, AttributeError):
            return False
    return False


def where(root, name):
    """The real folder behind root/name (itself, or the link's target)."""
    return os.path.realpath(os.path.join(root, name))


def make_link(link, target):
    if os.name == "nt":
        r = subprocess.run(["cmd", "/c", "mklink", "/J", link, target], capture_output=True, text=True)
        if r.returncode:
            raise OSError(f"mklink /J {link} {target}: {(r.stdout + r.stderr).strip()}")
    else:
        os.symlink(target, link, target_is_directory=True)


def remove_link(link):
    if os.name == "nt":
        os.rmdir(link)   # (a junction is removed as an empty folder; its target is left alone)
    else:
        os.unlink(link)


def same(a, b):
    """One folder written two ways (a short name, a link above it); the last part itself is not followed."""
    def real(p):
        p = os.path.abspath(p)
        return os.path.normcase(os.path.join(os.path.realpath(os.path.dirname(p)), os.path.basename(p)))
    return real(a) == real(b)


def move(root, dest, log=print):
    """Move root's dump and build into dest (dest = root: back into the repository). Returns the moves made."""
    dest = os.path.abspath(dest)
    back = same(dest, root)
    plan = []
    for name in FOLDERS:
        here = os.path.join(root, name)
        now = where(root, name)
        want = os.path.join(dest, name)
        if same(now, want):
            log(f"{name}: already in {want}")
            continue
        if os.path.exists(want) and not back:
            raise SystemExit(f"{want} exists already: choose an empty folder")
        plan.append((name, here, now, want))
    done = []
    try:
        for name, here, now, want in plan:
            linked = is_link(here)
            log(f"{name}: moving {now} to {want} ...")
            if linked:
                remove_link(here)   # (back home: the link's place is where the folder goes)
            os.makedirs(os.path.dirname(want), exist_ok=True)
            if os.path.exists(now):
                shutil.move(now, want)
            else:
                os.makedirs(want)
            if not back:
                make_link(here, want)
            done.append((name, here, now, want, linked))
    except (OSError, shutil.Error) as e:
        log(f"stopped: {e}; putting back what was moved")
        for name, here, now, want, linked in reversed(done):
            if is_link(here):
                remove_link(here)
            shutil.move(want, now)
            if linked:
                make_link(here, now)
        raise SystemExit(1)
    return done


def main(argv):
    if len(argv) > 2 and argv[1] == "--move":
        move(ROOT, argv[2])
        argv = argv[:1]
    if len(argv) == 1:
        for name in FOLDERS:
            here = os.path.join(ROOT, name)
            kind = " (a link)" if is_link(here) else ""
            print(f"{name}: {where(ROOT, name)}{kind}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
