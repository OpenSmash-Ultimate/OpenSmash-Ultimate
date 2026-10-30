#!/usr/bin/env python3
"""Installs WindowSmash Mods (the mod manager) into a WindowSmash folder, from the installer zip
make_installer.py makes - and, when the zip carries a modpack, that pack's mods too, turned on and
applied. Run by the zip's "Install WindowSmash Mods" (.command on macOS, .bat on Windows), with
WindowSmash's own Python:

  install_mods.py --target <WindowSmash folder> [--no-apply] [--no-app] [--no-build]
  install_mods.py --target <WindowSmash folder> --uninstall [--delete-mods | --keep-mods]

Files it replaces are kept beside them as <name>.before-modmanager (the first time), and what it
put where is written to tools/modmanager/installed.json. It never touches the game, the save or
the build's own tools: the mod manager is tools/mods.py, prc.py, msbt.py, tools/modmanager/, the
launchers and (macOS) its app - and the game-code fixes new costume slots need (code_fixes.py:
edits to tools/recomp.py and lift.py, then the game rebuilt by tools/build_game.py).

--uninstall puts it all back: the game reads its own files again (romfs_vanilla.cfg), the modded
data (dump/mods, the layer map) is deleted, the mod manager's files are removed and the ones it
replaced restored, its app or desktop shortcut removed - and, if asked, the installed mods too.
(Run from the installer zip or from the WindowSmash folder's launchers, or the app's button.)
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))   # the unpacked zip's installer/ folder (or tools/modmanager)
PAYLOAD = os.path.join(os.path.dirname(HERE), "payload")
PACK = os.path.join(os.path.dirname(HERE), "modpack")
MANIFEST = os.path.join("tools", "modmanager", "installed.json")
# the mod manager's files, for a WindowSmash set up before installs kept a record of them
KNOWN = ["tools/mods.py", "tools/prc.py", "tools/msbt.py", "tools/modmanager", "tools/mac/ModsShell.swift",
         "tools/mac/build_mods_app.sh", "launchers/WindowSmash Mods.bat", "launchers/WindowSmash Mods.command",
         "launchers/Uninstall WindowSmash Mods.bat", "launchers/Uninstall WindowSmash Mods.command"]
APP = "WindowSmash Mods.app"


def say(text=""):
    print(text, flush=True)


def is_windowsmash(folder):
    return os.path.isfile(os.path.join(folder, "tools", "build_game.py"))


def read_manifest(target):
    try:
        with open(os.path.join(target, MANIFEST), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"files": {}}


def copy_payload(target):
    """The payload's files in place; returns how many changed. The record keeps, for each, whether
    the first install made it ("new"), replaced one ("replaced": kept as .before-modmanager) or
    found the same file there ("there")."""
    changed = 0
    manifest = read_manifest(target)
    record = manifest.setdefault("files", {})
    for dp, _dirs, files in os.walk(PAYLOAD):
        for f in files:
            src = os.path.join(dp, f)
            rel = os.path.relpath(src, PAYLOAD)
            key = rel.replace(os.sep, "/")
            dst = os.path.join(target, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.exists(dst):
                with open(src, "rb") as a, open(dst, "rb") as b:
                    same = a.read() == b.read()
                record.setdefault(key, "there" if same else "replaced")
                if same:
                    continue
                keep = dst + ".before-modmanager"
                if not os.path.exists(keep):
                    shutil.copy2(dst, keep)
            else:
                record.setdefault(key, "new")
            shutil.copy2(src, dst)
            if f.endswith((".command", ".sh")):
                os.chmod(dst, 0o755)
            changed += 1
    with open(os.path.join(target, MANIFEST), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    return changed


def mac_app(target):
    if subprocess.run(["xcrun", "--find", "swiftc"], capture_output=True).returncode:
        say("  (the app needs Apple's Command Line Tools to build: run  xcode-select --install  then open")
        say("   launchers/WindowSmash Mods.command; until then it opens in your browser)")
        return False
    r = subprocess.run(["bash", os.path.join(target, "tools", "mac", "build_mods_app.sh")], capture_output=True, text=True)
    if r.returncode:
        say("  the app didn't build:\n" + (r.stderr or r.stdout)[-800:])
        return False
    say("  WindowSmash Mods is in your Applications folder (~/Applications), and Spotlight finds it.")
    return True


def desktop_shortcut():
    return os.path.join(os.environ.get("USERPROFILE", os.path.expanduser("~")), "Desktop", "WindowSmash Mods.lnk")


def windows_shortcut(target):
    q = lambda path: path.replace("'", "''")   # (PowerShell's single-quoted strings)
    bat = q(os.path.join(target, "launchers", "WindowSmash Mods.bat"))
    ico = os.path.join(target, "tools", "modmanager", "ui", "icon.ico")
    ps = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Desktop')+'\\WindowSmash Mods.lnk');"
          f"$s.TargetPath='{bat}';$s.WorkingDirectory='{q(target)}';$s.WindowStyle=7;"
          + (f"$s.IconLocation='{q(ico)}';" if os.path.exists(ico) else "") + "$s.Save()")
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", ps], capture_output=True, text=True)
    if r.returncode == 0:
        say("  A \"WindowSmash Mods\" shortcut is on your desktop.")


def install_code_fixes(target):
    """The costume-slot game-code fixes in the recompiler (recorded, so uninstalling puts the files back)."""
    sys.path.insert(0, HERE if os.path.exists(os.path.join(HERE, "code_fixes.py")) else os.path.join(target, "tools", "modmanager"))
    import code_fixes
    st = code_fixes.state(target)
    if st == "there":
        return code_fixes
    if st != "ready":
        say(f"  The game-code fixes for new costume slots weren't added ({st}): those slots work in regular")
        say("  matches; Classic Mode, Tourney Mode and Inkling/Terry/Pyra-Mythra with them may not.")
        return None
    manifest = read_manifest(target)
    code_fixes.apply(target, manifest.setdefault("files", {}))
    with open(os.path.join(target, MANIFEST), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    say("  the game-code fixes for new costume slots (Classic, Tourney, Inkling, Terry, Pyra/Mythra) are in the recompiler")
    return code_fixes


def rebuild(target, code_fixes):
    """The game built again, when the recompiler changed since it was built."""
    if not code_fixes or not code_fixes.needs_build(target):
        return
    retry = "launchers/Build WindowSmash" + (".bat" if os.name == "nt" else ".command")
    if game_open():
        say(f"\nThe game is open: close it, then run {retry} to build it with the new fixes.")
        return
    cmd = code_fixes.build_command(target, sys.executable)
    if not cmd:
        say(f"\nTo put the game-code fixes in the game, run {retry} (it builds the game again).")
        return
    say("\nBuilding the game again with the fixes - usually 10 to 30 minutes; it can be left alone, and the")
    say("computer stays usable (few files are compiled at a time). Mods work meanwhile, after it finishes too.")
    r = subprocess.run(cmd, cwd=target)
    if r.returncode:
        say(f"\nThe build stopped (see above). Run {retry} to carry on - mods still work in regular matches.")
    else:
        say("\nThe game is built with the fixes.")


def install_pack(target, apply):
    files = [f for f in os.listdir(PACK) if f.endswith(".wsmodpack")] if os.path.isdir(PACK) else []
    if not files:
        return
    sys.path.insert(0, os.path.join(target, "tools"))
    sys.path.insert(0, os.path.join(target, "tools", "modmanager"))
    import mods
    import packs
    import server
    tree = mods.Tree(target)
    with open(os.path.join(PACK, files[0]), encoding="utf-8") as f:
        pack = packs.validate(json.load(f))
    say(f"\nThe modpack \"{pack['name']}\" ({len(pack['mods'])} mods):")
    bundled = os.path.join(PACK, "mods")
    for m in pack["mods"]:
        src = os.path.join(bundled, m["name"])
        if m.get("bundled") and os.path.isdir(src) and not os.path.exists(os.path.join(tree.mods, m["name"])):
            say(f"  {m['title'] or m['name']}: from the installer")
            shutil.copytree(src, os.path.join(tree.mods, m["name"]))
            state = tree.load()
            state["mods"][m["name"]] = {k: v for k, v in m.items() if k in ("title", "author", "category", "image")}
            state["mods"][m["name"]]["enabled"] = True
            tree.save(state)
    for m in packs.missing(tree, pack):
        title = m.get("title") or m["name"]
        try:
            if m.get("gamebanana"):
                say(f"  {title}: downloading from GameBanana")
                mods.gamebanana_install(tree, int(m["gamebanana"]), m.get("file_id") or server.pack_file(m))
            elif m.get("url"):
                say(f"  {title}: downloading")
                server.install_url(tree, m["url"], say, lambda *a: None)
        except (mods.Error, OSError) as e:
            say(f"    couldn't: {e}")
    left = packs.use_pack(tree, pack, only=False)
    packs.save_pack(tree, pack)
    if left:
        say("  not installed: " + ", ".join(m["name"] for m in left))
    if apply and server.game_running():
        say("  The game is open: the mods are turned on; press Apply in WindowSmash Mods after closing it.")
    elif apply:
        try:
            mods.apply(tree, lambda t: say("  " + t))
        except mods.Error as e:
            say(f"  The mods are installed and turned on, but not applied yet ({e}): open WindowSmash Mods and press Apply.")


# ---- uninstalling ----------------------------------------------------------------------------
def game_open():
    try:
        if sys.platform == "darwin":
            return subprocess.run(["pgrep", "-f", "MacOS/ssbu"], capture_output=True).returncode == 0
        if os.name == "nt":
            r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ssbu.exe", "/NH"], capture_output=True, text=True)
            return "ssbu.exe" in r.stdout.lower()
    except OSError:
        pass
    return False


def remove(path):
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path, ignore_errors=True)
    elif os.path.lexists(path):
        os.remove(path)


def uninstall(target, delete_mods):
    if game_open():
        say("The game is open: close it first, then run this again.")
        return 1
    say(f"Removing WindowSmash Mods from {target}")
    # the game as it is: its own layer map again, the modded data deleted
    modules = os.path.join(target, "dump", "modules")
    cfg, vanilla = os.path.join(modules, "romfs.cfg"), os.path.join(modules, "romfs_vanilla.cfg")
    if os.path.exists(vanilla):
        try:
            with open(cfg, encoding="utf-8") as f:
                modded = "romfs_mods.layers" in f.read()
        except OSError:
            modded = True
        if modded:
            shutil.copyfile(vanilla, cfg)
            say("  the game reads its own files again")
    remove(os.path.join(modules, "romfs_mods.layers"))
    remove(os.path.join(target, "dump", "mods"))
    say("  the modded game data is deleted")
    if delete_mods:
        remove(os.path.join(target, "mods"))
        say("  the installed mods are deleted")
    else:
        say("  your mods are kept in the mods folder (install WindowSmash Mods again to use them)")
    # the mod manager's files: its own removed, the ones it replaced back
    record = read_manifest(target).get("files") or {k: "new" for k in KNOWN}
    restored = 0
    for rel, how in sorted(record.items(), key=lambda kv: -len(kv[0])):
        path = os.path.join(target, *rel.split("/"))
        keep = path + ".before-modmanager"
        if how != "there" and os.path.exists(keep):   # (one it replaced: back)
            os.replace(keep, path)
            restored += 1
        elif how == "new":
            remove(path)
    remove(os.path.join(target, MANIFEST))
    # folders the install made that are empty now (a __pycache__ left by Python doesn't count)
    for rel in sorted(record, key=len, reverse=True):
        d = os.path.dirname(os.path.join(target, *rel.split("/")))
        while os.path.normpath(d) != os.path.normpath(target) and os.path.isdir(d) \
                and not [f for f in os.listdir(d) if f != "__pycache__"]:
            remove(d)
            d = os.path.dirname(d)
    say(f"  the mod manager is removed{f' ({restored} files it had replaced put back)' if restored else ''}")
    if record.get("tools/recomp.py") == "replaced":
        say("  (the game keeps its costume-slot fixes until it's next built: they change nothing without mods)")
    # its app (macOS: the one before it, if it replaced one) or its desktop shortcut
    if sys.platform == "darwin":
        built = os.path.join(target, "build-mac", APP)
        before = built + ".before-modmanager"
        mine = os.path.join(os.path.expanduser("~"), "Applications", APP)
        remove(mine)
        remove(built)
        if os.path.exists(before):
            os.replace(before, built)
            shutil.copytree(built, mine, symlinks=True)
            say("  the mod manager app you had before is back")
        else:
            say("  the WindowSmash Mods app is removed")
    elif os.name == "nt":
        remove(desktop_shortcut())
        say("  the desktop shortcut is removed")
    say("\nDone: WindowSmash is the game as it was.")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True)
    ap.add_argument("--no-apply", action="store_true")
    ap.add_argument("--no-app", action="store_true", help="leave the macOS app / Windows shortcut alone")
    ap.add_argument("--no-build", action="store_true", help="don't rebuild the game (the code fixes wait for the next build)")
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--delete-mods", action="store_true", help="with --uninstall: delete the installed mods too")
    ap.add_argument("--keep-mods", action="store_true", help="with --uninstall: keep them (don't ask)")
    a = ap.parse_args()
    target = os.path.abspath(a.target)
    if not is_windowsmash(target):
        say(f"{target} isn't a WindowSmash folder (it has no tools/build_game.py).")
        return 1
    if a.uninstall:
        delete = a.delete_mods
        if not (a.delete_mods or a.keep_mods):
            try:
                delete = input("Also delete your downloaded mods (the mods folder)? [y/N] ").strip().lower() in ("y", "yes")
            except EOFError:
                delete = False
        return uninstall(target, delete)
    say(f"Installing WindowSmash Mods into {target}")
    n = copy_payload(target)
    say(f"  {n} files put in place (any it replaced are kept as *.before-modmanager)")
    if a.no_app:
        pass
    elif sys.platform == "darwin":
        mac_app(target)
    elif os.name == "nt":
        windows_shortcut(target)
    fixes = install_code_fixes(target)
    install_pack(target, not a.no_apply)
    if not a.no_build:
        rebuild(target, fixes)
    say("\nDone. Open WindowSmash Mods to find, install and share mods.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
