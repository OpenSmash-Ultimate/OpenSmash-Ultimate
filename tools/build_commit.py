"""The commit a build was made from, written into the build itself (SSBU_BUILD_COMMIT_CHECK, on unless 0).

Before, the commit a report named was tools/source_version.txt's: the commit of the SOURCES, which the updater writes
whether the game was rebuilt after it or not. Two PCs ran a build/ssbu.exe of 17/11 (the single-program layout, the
old watchdog and lobby) and reported v0.5.4's commit: the update brought the sources, the build never followed.

Now:
  - the runtime (runtime/hle_build_commit.c) and the game library (tools/game_dll.py's game_tables.c) carry a marker,
    MARKER and 40 '?', which the build fills in with the commit right after it links (stamp: tools/recomp.py link,
    and tools/runtime_fetch.py for a release's prebuilt runtime). The program says it at start ("[boot] build commit
    ..."), in its crash reports, and the launcher sends it with a report;
  - build/build_commit.txt beside it: the commit, where the marker is in build/ssbu.exe (so the launcher reads it
    without going through the file), and a digest of what the build was made from (inputs: the files
    tools/skyline.py needs_build goes by);
  - reason(): tools/skyline.py needs_build and tools/build_game.py build_is_current build again when the program's
    commit is not the sources' one, unless what it was built from is the same (an update of the launcher only, which
    #637's SSBU_UPDATE_GATE builds nothing for: then the sources' commit is added to build_commit.txt's "current"
    list). A program of before this (no marker at all) is built again once. One with the marker left as it was
    (never stamped) is checked by the files' dates only, as before, so a stamp that failed never builds every time.

SSBU_BUILD_COMMIT_CHECK=0: nothing is stamped or checked, the reports name the sources' commit, as before.
"""
import hashlib
import mmap
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)   # (tools/skyline.py's lists: also under python -P)
ROOT = os.path.dirname(HERE)
MARKER = b"SSBU-BUILD-COMMIT:"
SIZE = 40
UNSET = b"?" * SIZE
RECORD = "build_commit.txt"   # (beside the program)
HEX = re.compile(rb"[0-9a-f]{40}")


def on():
    return os.environ.get("SSBU_BUILD_COMMIT_CHECK", "1").strip() != "0"


def say(text):
    print(f"[build commit] {text}", flush=True)


def source_commit(tree=ROOT):
    """The commit the sources in tree are at: tools/source_version.txt's (a player's copy: the updater and the source
    archives write it), else git's HEAD (a checkout, CI); None when neither says."""
    try:
        with open(os.path.join(tree, "tools", "source_version.txt"), encoding="utf-8", errors="replace") as f:
            word = (f.read().split() or [""])[0].lower()
        if re.fullmatch(r"[0-9a-f]{40}", word):
            return word
    except OSError:
        pass
    if os.path.exists(os.path.join(tree, ".git")):
        try:
            r = subprocess.run(["git", "-C", tree, "rev-parse", "HEAD"], capture_output=True, text=True, timeout=30)
            word = r.stdout.strip().lower()
            if r.returncode == 0 and re.fullmatch(r"[0-9a-f]{40}", word):
                return word
        except (OSError, subprocess.SubprocessError):
            pass
    return None


def _find(data, start=0):
    """(offset of the 40 bytes after a marker, what they are) for each marker in data from start."""
    at = data.find(MARKER, start)
    while at >= 0:
        value = bytes(data[at + len(MARKER):at + len(MARKER) + SIZE])
        if value == UNSET or HEX.fullmatch(value):
            yield at + len(MARKER), value
        at = data.find(MARKER, at + 1)


def read(path, at=None):
    """What path says it was built from: (commit or "" for a marker never filled in, offset of it), or (None, None)
    when it has no marker (a build of before SSBU_BUILD_COMMIT_CHECK) or cannot be read. at: where the record says it
    is (checked; looked for in the whole file when wrong)."""
    try:
        with open(path, "rb") as f:
            if at is not None:
                f.seek(max(0, at - len(MARKER)))
                head = f.read(len(MARKER) + SIZE)
                if head[:len(MARKER)] == MARKER:
                    value = head[len(MARKER):]
                    if value == UNSET:
                        return "", at
                    if HEX.fullmatch(value):
                        return value.decode(), at
            if os.fstat(f.fileno()).st_size == 0:
                return None, None
            with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
                for offset, value in _find(m):
                    return ("" if value == UNSET else value.decode()), offset
    except (OSError, ValueError):
        pass
    return None, None


def stamp(path, commit):
    """commit written into every marker of path (the program, the game library); the offset of the first, or None
    when it has none (a runtime of before this)."""
    first = None
    with open(path, "r+b") as f:
        with mmap.mmap(f.fileno(), 0) as m:
            for offset, _value in list(_find(m)):
                m[offset:offset + SIZE] = commit.encode()
                first = offset if first is None else first
            m.flush()
    return first


def _not_built():
    """What never goes into the program (tools/skyline.py NOT_BUILT and, whatever SSBU_UPDATE_GATE says, its update
    list): an update of only these is the same build."""
    import skyline
    return tuple(skyline.NOT_BUILT) + tuple(skyline.NOT_BUILT_UPDATE)


def inputs(tree):
    """A digest of what goes into the game's program: the files of runtime/ and tools/ that tools/skyline.py
    needs_build goes by, by their content (not their dates), without tools/source_version.txt."""
    not_built = _not_built()
    h = hashlib.sha256()
    for top in ("runtime", "tools"):
        for dp, dirs, files in os.walk(os.path.join(tree, top)):
            dirs[:] = sorted(d for d in dirs if not d.startswith((".", "__pycache__")))
            for n in sorted(files):
                rel = os.path.relpath(os.path.join(dp, n), tree).replace(os.sep, "/")
                if n.startswith(".") or ".before-" in n or n.endswith((".orig", ".pyc")) or rel.startswith(not_built) \
                        or rel == "tools/source_version.txt":
                    continue
                try:
                    with open(os.path.join(dp, n), "rb") as f:
                        data = f.read()
                except OSError:
                    continue
                h.update(rel.encode() + b"\0" + hashlib.sha256(data).digest())
    return h.hexdigest()


def load(build):
    """build/build_commit.txt as a dict (its "current" a list); {} when there is none."""
    out = {}
    try:
        with open(os.path.join(build, RECORD), encoding="utf-8") as f:
            for line in f:
                key, _, value = line.strip().partition(" ")
                if key == "current":
                    out.setdefault("current", []).extend(value.split())
                elif key:
                    out[key] = value.strip()
    except OSError:
        pass
    return out


def save(build, record):
    lines = [f"{k} {record[k]}" for k in ("commit", "inputs", "exe_at", "library", "library_at") if record.get(k) not in (None, "")]
    if record.get("current"):
        lines.append("current " + " ".join(record["current"][-8:]))
    tmp = os.path.join(build, RECORD + ".tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("# what build/ssbu.exe was built from: tools/build_commit.py\n" + "\n".join(lines) + "\n")
    os.replace(tmp, os.path.join(build, RECORD))


def record(exe, library=None, tree=ROOT, commit=None):
    """After a link: the sources' commit written into exe (and into library, when it was linked now) and into
    build_commit.txt with what they were built from. A failure is said, never a reason to stop the build: the program
    then says "not recorded" and the dates decide, as before."""
    if not on():
        return None
    commit = commit or source_commit(tree)
    if not commit:
        say("the sources do not say which commit they are (no tools/source_version.txt, no git): nothing written")
        return None
    build = os.path.dirname(os.path.abspath(exe))
    try:
        rec = load(build)
        at = stamp(exe, commit)
        if at is None:
            say(f"{os.path.basename(exe)} has no place for it (a runtime of before this): nothing written")
            return None
        rec.update(commit=commit, inputs=inputs(tree), exe_at=str(at), current=[])
        if library and os.path.isfile(library):
            lat = stamp(library, commit)
            if lat is not None:
                rec.update(library=commit, library_at=str(lat))
        save(build, rec)
        say(f"{os.path.basename(exe)}{' and ' + os.path.basename(library) if library else ''}: commit {commit}")
        return commit
    except (OSError, ValueError) as e:
        say(f"could not be written ({e}): the program says \"not recorded\"")
        return None


def reason(tree, exe=None, log=say):
    """Why the program at exe (build/ssbu.exe) has to be built again because of the commit it was built from, or
    None: see the top of this file."""
    if not on():
        return None
    own = os.path.join(tree, "build", "ssbu.exe")
    exe = exe or own
    # (only the tree's own Windows program: the sources' commit says nothing of another; Linux's and macOS's programs
    # are not written into, their dates decide as before)
    if os.path.normcase(os.path.abspath(exe)) != os.path.normcase(os.path.abspath(own)) or not os.path.isfile(exe):
        return None
    now = source_commit(tree)
    if not now:
        return None
    build = os.path.dirname(os.path.abspath(exe))
    rec = load(build)
    try:
        hint = int(rec.get("exe_at", ""))
    except ValueError:
        hint = None
    built, _at = read(exe, hint)
    if built is None:
        log(f"build/ssbu.exe does not say which commit it was built from (a build of before v0.5.5); the sources are at {now[:10]}")
        return "build/ssbu.exe does not say which commit it was built from"
    if built == "":   # (a program that was never stamped: the dates decide, as before)
        return None
    if built == now or now in rec.get("current", []):
        return None
    same = rec.get("commit") == built and rec.get("inputs") and rec.get("inputs") == inputs(tree)
    if same:   # (an update of what is not built: the launcher, the Mods app, source_version.txt)
        rec.setdefault("current", []).append(now)
        try:
            save(build, rec)
        except OSError:
            pass
        log(f"build/ssbu.exe is commit {built[:10]}, the sources {now[:10]}: what it is built from is the same, no build")
        return None
    log(f"build/ssbu.exe is commit {built[:10]} but the sources are at {now[:10]} and differ: it was not built again "
        "after an update, so it is now")
    return f"build/ssbu.exe is commit {built[:10]}, the sources are at {now[:10]}"


if __name__ == "__main__":   # (python tools/build_commit.py [<tree>]: what the build says, and whether it is current)
    tree = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ROOT)
    exe = os.path.join(tree, "build", "ssbu.exe")
    built, at = read(exe, None)
    print(f"build/ssbu.exe: {'not there' if not os.path.isfile(exe) else 'no marker' if built is None else built or 'not recorded'}")
    print(f"sources: {source_commit(tree) or 'unknown'}")
    why = reason(tree, exe, log=lambda s: None)
    print(f"build needed: {why}" if why else "no build needed for the commit")
    sys.exit(1 if why else 0)
