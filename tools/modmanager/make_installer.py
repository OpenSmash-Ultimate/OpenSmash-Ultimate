#!/usr/bin/env python3
"""Makes the WindowSmash Mods installer: one .zip for Windows and macOS that adds the mod manager
to a WindowSmash folder - and, with a modpack, that pack's mods too (downloaded from GameBanana as
it installs, or carried in the zip).

Usage:
  make_installer.py [--tree <port folder>] [--pack <pack name or .wsmodpack>] [--bundle-all] [--out <dir>]

The zip: "Install WindowSmash Mods.command" (macOS) and ".bat" (Windows), which find the
WindowSmash folder and its Python and run installer/install_mods.py; payload/ (the mod manager's
files, at their places in a WindowSmash folder); modpack/ (the pack, and the files of its mods
that can't be downloaded - or, with --bundle-all, of every mod in it); READ ME.txt.
"""
import argparse
import json
import os
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)
import mods  # noqa: E402
import packs  # noqa: E402

# the mod manager's files, from the port's folder
PAYLOAD = ["tools/mods.py", "tools/prc.py", "tools/msbt.py",
           "tools/modmanager/server.py", "tools/modmanager/packs.py", "tools/modmanager/arc_mods.py",
           "tools/modmanager/install_mods.py", "tools/modmanager/make_installer.py", "tools/modmanager/code_fixes.py",
           "tools/mac/ModsShell.swift", "tools/mac/build_mods_app.sh",
           "launchers/WindowSmash Mods.bat", "launchers/WindowSmash Mods.command",
           "launchers/Uninstall WindowSmash Mods.bat", "launchers/Uninstall WindowSmash Mods.command"]

MAC = r'''#!/bin/bash
# Installs WindowSmash Mods (the mod manager) into your WindowSmash folder, and the modpack this
# installer carries, if it has one. Double-click it (the first time macOS may ask: right-click > Open).
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
here="$PWD"
target=""
for d in "$here/.." "$here/../.." "$HOME/WindowSmash" "$HOME/Desktop/WindowSmash" "$HOME/Downloads/WindowSmash" "$HOME/Documents/WindowSmash"; do
    [ -f "$d/tools/build_game.py" ] && { target="$(cd "$d" && pwd -P)"; break; }
done
if [ -z "$target" ]; then
    target=$(osascript -e 'POSIX path of (choose folder with prompt "Where is your WindowSmash folder?")' 2>/dev/null) || exit 1
fi
target="${target%/}"
py=""
for p in "$target/deps/venv/bin/python" "$target/deps/python/bin/python3" python3; do
    if command -v "$p" >/dev/null 2>&1 && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then py="$p"; break; fi
done
if [ -z "$py" ]; then
    echo "WindowSmash's Python wasn't found: install WindowSmash first (Install WindowSmash.command)."
    read -r -p "Press Return to close." _; exit 1
fi
"$py" "$here/installer/install_mods.py" --target "$target"
status=$?
[ $status -eq 0 ] && [ -d "$HOME/Applications/WindowSmash Mods.app" ] && open "$HOME/Applications/WindowSmash Mods.app"
read -r -p "Press Return to close." _
exit $status
'''

WIN = r'''@echo off
rem Installs WindowSmash Mods (the mod manager) into your WindowSmash folder, and the modpack this
rem installer carries, if it has one. Double-click it.
setlocal
cd /d "%~dp0"
set "HERE=%~dp0"
set "TARGET="
for %%D in ("%~dp0.." "%~dp0..\.." "%USERPROFILE%\WindowSmash" "%USERPROFILE%\Desktop\WindowSmash" "%USERPROFILE%\Downloads\WindowSmash" "%USERPROFILE%\Documents\WindowSmash" "C:\WindowSmash") do (
    if not defined TARGET if exist "%%~D\tools\build_game.py" set "TARGET=%%~fD"
)
if not defined TARGET (
    for /f "usebackq delims=" %%P in (`powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; $d=New-Object System.Windows.Forms.FolderBrowserDialog; $d.Description='Where is your WindowSmash folder?'; if($d.ShowDialog() -eq 'OK'){$d.SelectedPath}"`) do set "TARGET=%%P"
)
if not defined TARGET (echo No WindowSmash folder chosen. & pause & exit /b 1)
pushd "%TARGET%"
call tools\find_python.bat || (popd & pause & exit /b 1)
popd
pushd "%TARGET%"
%PY% "%HERE%installer\install_mods.py" --target "%TARGET%"
set "RESULT=%ERRORLEVEL%"
popd
if "%RESULT%"=="0" start "" "%TARGET%\launchers\WindowSmash Mods.bat"
echo.
pause
exit /b %RESULT%
'''

# the same, the other way: WindowSmash as it was (install_mods.py --uninstall)
MAC_UNINSTALL = (MAC.replace("# Installs WindowSmash Mods (the mod manager) into your WindowSmash folder, and the modpack this\n"
                             "# installer carries, if it has one.", "# Removes WindowSmash Mods from your WindowSmash folder and puts the game back as it was.")
                 .replace('--target "$target"\n', '--target "$target" --uninstall\n')
                 .replace('[ $status -eq 0 ] && [ -d "$HOME/Applications/WindowSmash Mods.app" ] && open "$HOME/Applications/WindowSmash Mods.app"\n', ""))
WIN_UNINSTALL = (WIN.replace("rem Installs WindowSmash Mods (the mod manager) into your WindowSmash folder, and the modpack this\n"
                             "rem installer carries, if it has one.", "rem Removes WindowSmash Mods from your WindowSmash folder and puts the game back as it was.")
                 .replace('--target "%TARGET%"\n', '--target "%TARGET%" --uninstall\n')
                 .replace('if "%RESULT%"=="0" start "" "%TARGET%\\launchers\\WindowSmash Mods.bat"\n', ""))
assert MAC_UNINSTALL.count("--uninstall") == 1 and WIN_UNINSTALL.count("--uninstall") == 1 and "start" not in WIN_UNINSTALL.split("--uninstall")[1]

README = """WindowSmash Mods {what}
==================

WindowSmash Mods finds Smash Ultimate mods on GameBanana, installs them, turns them on and off,
checks that they go together, and shares them as modpacks.{pack}

It also adds the game-code fixes mods with new costume slots (c08 and up) need in Classic Mode,
Tourney Mode and for Inkling, Terry and Pyra/Mythra, and builds the game again with them (10 to 30
minutes the first time; mods work meanwhile).

To install (WindowSmash must already be installed and set up):
  Windows: unzip this, then double-click "Install WindowSmash Mods.bat".
  macOS:   unzip this, then double-click "Install WindowSmash Mods.command"
           (if macOS says it can't be opened: right-click it, choose Open, then Open).

It finds your WindowSmash folder (or asks where it is), adds the mod manager to it and opens it.
Nothing of the game or your save is changed; files it replaces are kept as *.before-modmanager.

To go back to the game as it was: "Uninstall WindowSmash Mods" (here, in WindowSmash's launchers
folder, or the button in the app). It turns the mods off in the game, deletes the modded data and
removes the mod manager; it asks whether to delete your downloaded mods too.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tree", default=os.path.dirname(TOOLS))
    ap.add_argument("--pack", help="a saved modpack's name, or a .wsmodpack file")
    ap.add_argument("--bundle-all", action="store_true", help="carry every mod's files (no downloads while installing)")
    ap.add_argument("--out", default=os.path.join(os.path.expanduser("~"), "Downloads"))
    a = ap.parse_args()
    print(json.dumps({"result": build(mods.Tree(a.tree), a.pack, a.bundle_all, a.out, print)}))


def build(tree, pack_ref=None, bundle_all=False, out_dir=None, log=print):
    """The installer zip; returns its path."""
    pack = None
    if pack_ref:
        if os.path.isfile(pack_ref):
            with open(pack_ref, encoding="utf-8") as f:
                pack = packs.validate(json.load(f))
        else:
            found = [p for p in packs.saved_packs(tree) if p["id"] == pack_ref or p["name"] == pack_ref]
            if not found:
                raise packs.PackError(f"no saved modpack called {pack_ref}")
            pack = {k: v for k, v in found[0].items() if k not in ("id", "code")}
    name = "WindowSmash Mods Installer" + (f" - {mods.safe_name(pack['name'])}" if pack else "") + ".zip"
    out = os.path.join(out_dir or tree.root, name)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    tmp = out + ".partial"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        def text(arc, body, exe=False):
            info = zipfile.ZipInfo(arc, time.localtime()[:6])
            info.external_attr = (0o100755 if exe else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, body.replace("\n", "\r\n") if arc.endswith(".bat") else body)

        def file(src, arc):
            info = zipfile.ZipInfo.from_file(src, arc)
            info.compress_type = zipfile.ZIP_DEFLATED
            if src.endswith((".command", ".sh")):
                info.external_attr = 0o100755 << 16
            with open(src, "rb") as f:
                z.writestr(info, f.read())
        text("Install WindowSmash Mods.command", MAC, exe=True)
        text("Install WindowSmash Mods.bat", WIN)
        text("Uninstall WindowSmash Mods.command", MAC_UNINSTALL, exe=True)
        text("Uninstall WindowSmash Mods.bat", WIN_UNINSTALL)
        what = f"- modpack \"{pack['name']}\"" if pack else ""
        about = ""
        if pack:
            about = "\n\nThis installer also adds the modpack \"" + pack["name"] + "\" (" + ", ".join(m.get("title") or m["name"] for m in pack["mods"]) + \
                    "), turned on and ready to play."
        text("READ ME.txt", README.format(what=what, pack=about))
        root = os.path.dirname(TOOLS)   # (the mod manager's own files: this copy's)
        file(os.path.join(HERE, "install_mods.py"), "installer/install_mods.py")
        file(os.path.join(HERE, "code_fixes.py"), "installer/code_fixes.py")
        for rel in PAYLOAD:
            file(os.path.join(root, rel), "payload/" + rel)
        ui = os.path.join(HERE, "ui")
        for f in sorted(os.listdir(ui)):
            file(os.path.join(ui, f), "payload/tools/modmanager/ui/" + f)
        if pack:
            carried = []
            for m in pack["mods"]:
                local = packs.local_name(tree, m)
                if not (bundle_all or m.get("bundled")) or local is None:
                    continue
                folder = os.path.join(tree.mods, local)
                for dp, _dirs, files in os.walk(folder):
                    for f in files:
                        src = os.path.join(dp, f)
                        file(src, "modpack/mods/" + m["name"] + "/" + os.path.relpath(src, folder).replace(os.sep, "/"))
                m["bundled"] = True
                carried.append(m["name"])
                log(f"carrying {m['name']}")
            text(f"modpack/{mods.safe_name(pack['name'])}.wsmodpack", json.dumps(pack, indent=1, ensure_ascii=False))
    os.replace(tmp, out)
    log(f"made {out} ({os.path.getsize(out) / 1048576:.1f} MB)")
    return out


if __name__ == "__main__":
    try:
        main()
    except (mods.Error, packs.PackError) as e:
        print(f"error: {e}")
        sys.exit(1)
