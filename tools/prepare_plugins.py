#!/usr/bin/env python3
"""Prepare imported Skyline plugins before starting the root runtime.

Like ssbu-recomp/scripts/mods.py (ssbu-recomp#63, migrated by OpenSmash-Ultimate#108),
first discover what the plugins need, then compile those sites and remember the result.
This uses the root runtime's recompiler and mod library; no plugin-specific offsets or
modified NROs are shipped. Run with --force to retry a failed preparation.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import sys
import subprocess

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import skyline


def read_state(path):
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def write_state(path, state):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def plugin_key(tree):
    """Contents and order, including an updated file with an unchanged build id."""
    digest = hashlib.sha256()
    for label, source in skyline.turned_on(str(tree)):
        digest.update((label + "\0" + os.path.relpath(source, tree) + "\0").encode("utf-8"))
        with open(source, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                digest.update(chunk)
    return digest.hexdigest()


def preparation_key(tree, plugins):
    """Source changes, newly discovered sites, deleted outputs and a replaced game invalidate it."""
    digest = hashlib.sha256(plugins.encode("ascii"))
    paths = []
    for top in ("runtime", "tools"):
        paths.extend(p for p in (tree / top).rglob("*") if p.suffix in (".c", ".h", ".py")
                     and "__pycache__" not in p.parts)
    exe = skyline.program(str(tree))
    if exe:
        paths.append(Path(exe))
    paths.extend((tree / "build" / "plugins").glob("*.dll"))
    paths.extend(tree / "deps" / name / "windowsmash.json" for name in ("llvm", "msvc"))
    for name in ("plugins.txt", "plugin_dlls.txt", "hooks.txt", "patches.txt", "skip.txt"):
        paths.append(tree / "dump" / "skyline" / name)
    for path in sorted(paths):
        digest.update(str(path.relative_to(tree)).encode("utf-8"))
        try:
            stat = path.stat()
            digest.update(f":{stat.st_size}:{stat.st_mtime_ns}".encode("ascii"))
        except OSError:
            digest.update(b":missing")
    digest.update(skyline._hooks_sig(str(tree)).encode("utf-8"))
    return digest.hexdigest()


@contextmanager
def preparing(work):
    # OS locks are released even if a build is killed. Keep the lock file in place so
    # another process cannot lock a new inode while an existing waiter still has the old one.
    with (work / "plugin_prepare.lock").open("a+b") as output:
        output.seek(0, os.SEEK_END)
        if output.tell() == 0:
            output.write(b" ")
            output.flush()
        output.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(output.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(output, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise RuntimeError("Another plugin preparation is running") from None
        try:
            yield
        finally:
            if os.name == "nt":
                msvcrt.locking(output.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(output, fcntl.LOCK_UN)


def game_open():
    if os.name == "nt":
        command = ["tasklist", "/FI", "IMAGENAME eq ssbu.exe", "/NH"]
    else:
        command = ["pgrep", "-x", "ssbu"]
    try:
        ran = subprocess.run(command, capture_output=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return False
    return b"ssbu.exe" in ran.stdout.lower() if os.name == "nt" else ran.returncode == 0


def prepare(tree, jobs=None, log=print, force=False):
    tree = Path(tree).resolve()
    work = tree / "build" / "work"
    work.mkdir(parents=True, exist_ok=True)
    state_path = work / "plugin_prepare.json"
    state = read_state(state_path)
    plugins = plugin_key(tree)
    key = preparation_key(tree, plugins)
    if not force and state.get("key") == key:
        if state.get("ok"):
            log("Skyline plugins are ready (cached)")
            return
        if state.get("compiler", "") == os.environ.get("SSBU_CLANG", ""):
            raise RuntimeError("The previous plugin preparation failed: " + state.get("error", "unknown error")
                               + ". See build/work/plugin_prepare.log; retry with --force after fixing it")
    # A clean game with no plugins needs neither a compiler nor a discovery boot.
    patch_file = tree / "dump" / "skyline" / "patches.txt"
    if not state and not skyline.turned_on(str(tree)) and not (patch_file.exists() and patch_file.read_text().strip()):
        return
    if not skyline.program(str(tree)):
        raise RuntimeError("Build the game before preparing plugins")
    if game_open():
        raise RuntimeError("Close the game before preparing changed plugins")
    with preparing(work), (work / "plugin_prepare.log").open("w", encoding="utf-8") as output:
        def say(text):
            output.write(str(text) + "\n")
            output.flush()
            log(text)
        try:
            # Inline call sites without callbacks are harmless. Code patches are not: discover
            # them afresh when the enabled plugin set changes, including when its last mod is off.
            if state.get("plugins") != plugins:
                patch_file.parent.mkdir(parents=True, exist_ok=True)
                if patch_file.exists() and patch_file.read_text(encoding="utf-8"):
                    patch_file.write_text("", encoding="utf-8")
            say("Preparing Skyline plugins; the first start may rebuild parts of the game")
            code = skyline.build(str(tree), jobs or skyline.build_jobs(), say)
            if code:
                raise RuntimeError(f"The plugin build failed (code {code})")
            if skyline.plugin_refresh_reason(str(tree)):
                raise RuntimeError("Some enabled plugins are still not compiled")
            if plugin_key(tree) != plugins:
                raise RuntimeError("Enabled plugins changed during preparation; retry before playing")
        except (Exception, SystemExit) as error:
            message = str(error)
            say("Plugin preparation failed: " + message)
            write_state(state_path, {"key": preparation_key(tree, plugins), "plugins": plugins,
                                    "ok": False, "error": message, "compiler": os.environ.get("SSBU_CLANG", "")})
            raise RuntimeError(message) from None
        write_state(state_path, {"key": preparation_key(tree, plugins), "plugins": plugins, "ok": True})
        say("Skyline plugins are ready")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", default=str(HERE.parent))
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    try:
        prepare(args.tree, args.jobs, force=args.force)
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
