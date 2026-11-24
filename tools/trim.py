#!/usr/bin/env python3
"""Give back the disk a finished Windows build no longer needs.

Usage:
  trim.py [--tree <dir>] [--dry] <what>...      what: c, nsp, map, or all

  c     the game's generated C (build\\<module>\\g*.c, about 6 GB): each module's is removed once every object of
        it is compiled and recorded (tools/objcache.py keeps each source's digest, so the objects stay current and
        a runtime-only update still only links). A later build that has to compile from it (another compiler, flag
        or runtime header; a changed lifter writes it again anyway) writes that module's C again first
        (tools/recomp.py c_needed): the price is the time to write it, minutes, on those updates only. A file the
        compile ladder compiled another way keeps its C (its key is made from the C: tools/ladder.py).
  nsp   the plain copy of a compressed package (dump\\nsz\\*.nsp, about 4 GB with a .nsz update): only read while
        the game's files are set up, so removed once they are (the program, data and DLC all in place). Setting
        the folder up again from the game's files makes it again (a minute or two).
  map   build\\ssbu.map (0.8 GB): only the profiler reads it. SSBU_LINK_MAP=0 stops the link writing a new one.

Nothing of the game's data, the objects or the program is touched. tools/build_game.py runs this after a build
that worked when SSBU_TRIM names what (SSBU_TRIM=c,nsp,map); without it nothing is removed, as before."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import objcache   # noqa: E402

WHAT = ("c", "nsp", "map")


def module_folders(build):
    """The module build folders of a Windows build folder: build\\<module> and build\\nro\\<module>, each
    with a manifest (tools/recomp.py)."""
    out = []
    for base in (build, os.path.join(build, "nro")):
        if os.path.isdir(base):
            out += [os.path.join(base, n) for n in sorted(os.listdir(base))
                    if os.path.isfile(os.path.join(base, n, "manifest.tsv"))]
    return out


def trim_c(folder, dry=False):
    """Remove a module folder's generated C that is compiled and recorded: (bytes freed, files removed, why
    not when it is left whole). The record gets every source's digest first, and is read back before
    anything goes."""
    if not os.path.isfile(os.path.join(folder, "module.txt")):
        return 0, 0, "not compiled to the end (no module.txt)"
    with open(os.path.join(folder, "manifest.tsv")) as f:
        names = [line.split("\t")[0] for line in f.read().splitlines() if line.strip()]
    record = objcache.Record(folder, scan=(objcache.HOOK_SLOTS,), log=lambda _m: None)
    go = []
    for name in names:
        src = os.path.join(folder, name)
        obj = src[:-2] + ".obj"
        if not os.path.isfile(src):
            continue   # (removed by an earlier run)
        entry = record.recorded(obj)
        if not os.path.isfile(obj) or not entry:
            return 0, 0, f"{name} has no recorded object"
        if entry.get("ladder"):
            continue   # (compiled another way: its key is made from its C, which stays)
        record.source(src)
        go.append(src)
    if not go:
        return 0, 0, None
    freed = sum(os.path.getsize(p) for p in go)
    if dry:
        return freed, len(go), None
    record.recent.clear()   # (their digests are kept, however new the files: they are not written again)
    record.keep_all()
    record.save()
    marker = os.path.join(folder, objcache.TRIMMED)
    with open(marker, "w", newline="\n") as f:
        f.write("this module's generated C was removed after it compiled (tools/trim.py); "
                "a build that needs it writes it again\n")
    back = objcache.Record(folder, scan=(objcache.HOOK_SLOTS,), log=lambda _m: None)
    try:
        for src in go:
            back.sources[os.path.abspath(src)][2]
    except (KeyError, IndexError, TypeError):
        os.remove(marker)
        return 0, 0, "the record did not keep the sources' digests"
    for src in go:
        os.remove(src)
    return freed, len(go), None


def trim_nsp(tree, dry=False):
    """dump\\nsz's plain copies, once the game's files are all set up (build_game.py --check-ready)."""
    cache = os.path.join(tree, "dump", "nsz")
    if not os.path.isdir(cache):
        return 0, 0, None
    ready = subprocess.run([sys.executable, os.path.join(tree, "tools", "build_game.py"), "--check-ready"],
                           cwd=tree, capture_output=True).returncode == 0
    if not ready:
        return 0, 0, "the game's files are not all set up yet"
    freed = count = 0
    for n in os.listdir(cache):
        p = os.path.join(cache, n)
        if os.path.isfile(p) and n.lower().endswith((".nsp", ".nsp.src", ".nsp.partial")):
            freed += os.path.getsize(p)
            count += 1
            if not dry:
                os.remove(p)
    return freed, count, None


def trim_map(tree, dry=False):
    freed = count = 0
    for n in ("ssbu.map", "ssbu_game.map"):
        p = os.path.join(tree, "build", n)
        if os.path.isfile(p):
            freed += os.path.getsize(p)
            count += 1
            if not dry:
                os.remove(p)
    return freed, count, None


def trim(tree, what, dry=False, log=print):
    """Trim `what` (names of WHAT) of a tree; the bytes freed."""
    total = 0
    would = "would be " if dry else ""
    if "c" in what:
        freed = files = 0
        for folder in module_folders(os.path.join(tree, "build")):
            b, n, why = trim_c(folder, dry)
            freed, files = freed + b, files + n
            if why:
                log(f"  {os.path.relpath(folder, tree)}: its C stays ({why})")
        log(f"  generated C: {files:,} files, {freed / 2**30:.2f} GB {would}freed")
        total += freed
    for name, fn, label in (("nsp", trim_nsp, "plain copies of compressed packages"), ("map", trim_map, "the program's map")):
        if name in what:
            b, n, why = fn(tree, dry)
            log(f"  {label}: " + (f"left ({why})" if why else f"{n} files, {b / 2**30:.2f} GB {would}freed"))
            total += b
    return total


def main():
    args = sys.argv[1:]
    tree = ROOT
    if "--tree" in args:
        k = args.index("--tree")
        tree = os.path.abspath(args[k + 1])
        del args[k:k + 2]
    dry = "--dry" in args
    what = {w for a in args if not a.startswith("--") for w in a.replace(",", " ").split()}
    if "all" in what:
        what = set(WHAT)
    if not what or what - set(WHAT):
        print(__doc__)
        return 1
    total = trim(tree, what, dry)
    print(f"{total / 2**30:.2f} GB {'would be ' if dry else ''}freed in {tree}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
