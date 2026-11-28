#!/usr/bin/env python3
"""HewDraw Remix (HDR, github.com/HDR-Development): the gameplay overhaul, from its official release,
as a switch the Mods app turns on and off - on, it is applied with the user's own mods.

Usage:
  hdr.py status   [--tree <dir>]     installed or not, on or off
  hdr.py download [--tree <dir>]     download HDR's release package and install it (turned off)
  hdr.py install <folder|zip> [--tree <dir>]   install it from a switch-package already downloaded
                                     (the zip, or a folder it was unpacked to: a newer prerelease, say)
  hdr.py on|off   [--tree <dir>]     turn it on or off (then apply, and build plugins if asked)
  hdr.py only on|off [--tree <dir>]  HDR only (the launcher's HDR mode): HDR on, every other mod and community
                                     plugin off; off gives back what was on before
  hdr.py remove   [--tree <dir>]     delete it

The package is what a Switch's SD card takes. Of it: ultimate/mods/hdr, hdr-assets and hdr-stages go
in as mods (by those names: HDR reads its own folders), first in the order so the user's mods win
where both change a file; ultimate/stage-alts goes on the plugins' SD card (dump/skyline/sd); its stage
plugins (stage_config, stage_alts) are kept in dump/hdr/plugins and put among the standard plugins
(skyline/plugins) while HDR is on. The package's ARCropolis, Smashline and nro_hook are the standard
plugins' own builds (std_plugins.py), and hdr-launcher (the console's updater) isn't used: left out.
HDR isn't shipped with OpenSmash: each user downloads it from HDR's releases.

HDR only (the launcher's third mode, beside Play and Vanilla): HDR changes every fighter, and a moveset
mod on top of it (or a community plugin it doesn't expect) can crash the game or, online, play a
different match than the other players' games. So in this mode HDR's own mods are the only ones on and
HDR's plugins the only community ones loaded: mods.json's "hdr_only" keeps what was on before (each
mod's switch), mods.Tree.load turns every other mod off whatever its switch says while it is there -
the Mods app, the game's settings menu, an import: none of them turns one on - and skyline.standard
lists only ONLY_PLUGINS of the community plugins (the others stay built into the game, not loaded).
Leaving it puts every switch back as it was."""
import argparse
import hashlib
import json
import os
import shutil
import ssl
import sys
import time
import urllib.error
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mods  # noqa: E402
import std_plugins  # noqa: E402

VERSION = "v0.50.8"
URL = "https://github.com/HDR-Development/HDR-PreReleases/releases/download/v0.50.8/switch-package.zip"
SHA256 = "2504f4dd594d9276ec961ba981ca587b3a10d02970d9ebf140e957feb613c460"
SIZE = 1504417945            # (the download; installed it takes about 3.1 GB)
INSTALLED_SIZE = 3_300_000_000
MODS = ("hdr", "hdr-assets", "hdr-stages")
PLUGINS = ("libstage_config.nro", "libstage_alts.nro")
PLUGIN_DIR = "atmosphere/contents/01006A800016E000/romfs/skyline/plugins/"
SD = ("ultimate/stage-alts/",)   # (the package's paths that go on the plugins' SD card as they are)
# The plugins folder in the package: the console's layout, or that layout's sd-card copy without atmosphere/
# (what a user who unpacked it for a modded Switch has: skyline/plugins at the top)
PLUGIN_DIRS = (PLUGIN_DIR, "skyline/plugins/")
# HDR only: the community plugins (skyline/plugins) that still load - ARCropolis reads HDR's files, Smashline
# runs its fighters' code, nro_hook tells it of their modules - and HDR's stage plugins (PLUGINS)
ONLY_PLUGINS = ("libarcropolis.nro", "libsmashline_plugin.nro", "libnro_hook.nro") + PLUGINS


class Error(Exception):
    pass


def folder(tree):
    return os.path.join(tree.root, "dump", "hdr")


def installed(tree):
    """The installed version (a local package's: its ui/hdr_version.txt), or None."""
    try:
        with open(os.path.join(folder(tree), "installed.json"), encoding="utf-8") as f:
            v = json.load(f).get("version")
    except (OSError, ValueError):
        return None
    return v if all(os.path.isdir(os.path.join(tree.mods, m)) for m in MODS) else None


def status(tree):
    state = tree.load()
    have = installed(tree)
    return {"installed": have, "release": VERSION, "size": SIZE, "installed_size": INSTALLED_SIZE, "mods": list(MODS),
            "on": bool(have) and all(state["mods"].get(m, {}).get("enabled") for m in MODS),
            "only": bool(state.get("hdr_only"))}


def ssl_context():
    try:
        import certifi   # (python.org's Python has no certificates of its own until it is told)
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


def fetch(target, progress=None, log=print, tries=6):
    """The package into `target`, carried on from where an earlier try stopped (a big download on a
    slow or busy connection stops short), then checked."""
    ctx = ssl_context()
    for attempt in range(1, tries + 1):
        have = os.path.getsize(target) if os.path.exists(target) else 0
        if have < SIZE:
            headers = {"User-Agent": "OpenSmash mod manager", **({"Range": f"bytes={have}-"} if have else {})}
            try:
                with urllib.request.urlopen(urllib.request.Request(URL, headers=headers), timeout=60, context=ctx) as r:
                    resumed = have and r.status == 206
                    done = have if resumed else 0
                    with open(target, "ab" if resumed else "wb") as out:
                        while chunk := r.read(1 << 20):
                            out.write(chunk)
                            done += len(chunk)
                            if progress:
                                progress(done * 0.9, SIZE)   # (the last tenth: unpacking)
            except urllib.error.HTTPError as e:
                if not (have and e.code == 416):
                    raise Error(f"HDR didn't download (GitHub said {e.code} {e.reason}): try again later")
            except (OSError, urllib.error.URLError) as e:
                if attempt == tries:
                    raise Error(f"HDR didn't download ({e}): try again - it carries on from where it stopped")
                log(f"the download stopped ({e}); trying again ({attempt + 1} of {tries})")
                time.sleep(3 * attempt)
                continue
        sha = hashlib.sha256()
        with open(target, "rb") as f:
            while chunk := f.read(1 << 22):
                sha.update(chunk)
        if sha.hexdigest() == SHA256:
            return
        os.remove(target)   # (not the release checked here: downloaded again from the start)
        if attempt == tries:
            raise Error("HDR's download isn't the release OpenSmash checked (its checksum differs): try again later")
        log("the download isn't whole; downloading it again")


def download(tree, progress=None, log=print):
    """Download HDR's package and install it, turned off."""
    os.makedirs(folder(tree), exist_ok=True)
    target = os.path.join(folder(tree), "switch-package.zip")
    have = os.path.getsize(target) if os.path.exists(target) else 0
    free = shutil.disk_usage(folder(tree)).free
    if free < SIZE - have + INSTALLED_SIZE + (1 << 30):
        raise Error(f"not enough disk space for HDR: it needs about {(SIZE - have + INSTALLED_SIZE) / (1 << 30):.1f} GB, "
                    f"{free / (1 << 30):.1f} GB is free")
    log(f"downloading HewDraw Remix {VERSION} from HDR's releases on GitHub ({SIZE / (1 << 30):.1f} GB)")
    fetch(target, progress, log)
    log("unpacking")
    stage = unpack(tree, target, progress)
    place(tree, stage, {"version": VERSION, "sha256": SHA256, "when": time.strftime("%Y-%m-%d %H:%M")}, log)
    os.remove(target)
    log(f"HewDraw Remix {VERSION} installed")
    return status(tree)


def wanted(name):
    """A package file OpenSmash takes: HDR's three mods, the stage-alts list, its two stage plugins."""
    return (any(name.startswith(f"ultimate/mods/{m}/") for m in MODS) or name.startswith(SD)
            or any(name == d + p for d in PLUGIN_DIRS for p in PLUGINS))


def unpack(tree, package, progress=None):
    """The package's files OpenSmash takes, into dump/hdr/new (returned)."""
    stage = os.path.join(folder(tree), "new")
    shutil.rmtree(stage, ignore_errors=True)
    with zipfile.ZipFile(package) as z:
        names = [i for i in z.infolist() if not i.is_dir() and wanted(i.filename)]
        total = sum(i.file_size for i in names) or 1
        done = 0
        for i in names:
            if ".." in i.filename.split("/") or i.filename.startswith("/"):
                continue
            z.extract(i, stage)
            done += i.file_size
            if progress:
                progress(0.9 + 0.1 * done / total)
    return stage


def install(tree, source, log=print):
    """HDR from a switch-package already downloaded: its zip, or the folder it was unpacked to (ultimate/
    and skyline/ or atmosphere/ at its top, or one folder down). Copied (the folder stays as it is)."""
    source = os.path.abspath(source)
    if os.path.isfile(source):
        if not zipfile.is_zipfile(source):
            raise Error(f"{source} isn't a zip: pick HDR's switch-package.zip or the folder it was unpacked to")
        log(f"unpacking {os.path.basename(source)}")
        stage = unpack(tree, source)
    else:
        top = source
        if not os.path.isdir(os.path.join(top, "ultimate", "mods", "hdr")):   # (one folder down: switch-package/...)
            inner = [os.path.join(source, n) for n in sorted(os.listdir(source)) if os.path.isdir(os.path.join(source, n, "ultimate", "mods", "hdr"))] \
                if os.path.isdir(source) else []
            if not inner:
                raise Error(f"no HDR in {source}: it needs ultimate/mods/hdr, hdr-assets and hdr-stages (HDR's switch-package, unpacked)")
            top = inner[0]
        log(f"copying HDR from {top}")
        stage = os.path.join(folder(tree), "new")
        shutil.rmtree(stage, ignore_errors=True)
        for dp, dirs, files in os.walk(top):
            for n in files:
                rel = os.path.relpath(os.path.join(dp, n), top).replace(os.sep, "/")
                if wanted(rel):
                    dest = os.path.join(stage, *rel.split("/"))
                    os.makedirs(os.path.dirname(dest), exist_ok=True)
                    shutil.copy2(os.path.join(dp, n), dest)
    version = ""
    try:
        with open(os.path.join(stage, "ultimate", "mods", "hdr", "ui", "hdr_version.txt"), encoding="utf-8-sig") as f:
            version = f.read().strip()
    except OSError:
        pass
    place(tree, stage, {"version": version or "local", "source": source, "when": time.strftime("%Y-%m-%d %H:%M")}, log)
    log(f"HewDraw Remix {version or '(no version file)'} installed")
    return status(tree)


def place(tree, stage, record, log=print):
    """dump/hdr/new's files where they go (a version there already: replaced), and installed.json."""
    was_on = status(tree)["on"]
    for m in MODS:   # (a version there already: replaced)
        src = os.path.join(stage, "ultimate", "mods", m)
        if not os.path.isdir(src):
            raise Error(f"HDR's package has no {m} folder: not the layout OpenSmash knows")
        shutil.rmtree(os.path.join(tree.mods, m), ignore_errors=True)
        shutil.move(src, os.path.join(tree.mods, m))
    sd = os.path.join(tree.root, "dump", "skyline", "sd")
    for p in SD:
        dest = os.path.join(sd, *p.strip("/").split("/"))
        shutil.rmtree(dest, ignore_errors=True)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.move(os.path.join(stage, *p.strip("/").split("/")), dest)
    keep = os.path.join(folder(tree), "plugins")
    shutil.rmtree(keep, ignore_errors=True)
    found = next((os.path.join(stage, *d.strip("/").split("/")) for d in PLUGIN_DIRS
                  if all(os.path.isfile(os.path.join(stage, *d.strip("/").split("/"), p)) for p in PLUGINS)), None)
    if not found:
        raise Error("HDR's package has no " + " or ".join(PLUGINS) + ": not the layout OpenSmash knows")
    shutil.move(found, keep)
    shutil.rmtree(stage, ignore_errors=True)
    with open(os.path.join(folder(tree), "installed.json"), "w", encoding="utf-8") as f:
        json.dump(record, f, indent=1)
    set_on(tree, was_on, log)


def set_on(tree, on, log=print):
    """HDR's mods turned on or off (first in the order: the user's own win where both change a file),
    and its stage plugins put among the standard ones or taken out. Apply (and Build plugins, for a
    plugin not compiled into the game yet) makes it so in the game."""
    if on and not installed(tree):
        raise Error("HDR isn't downloaded")
    state = tree.load()
    for m in MODS:
        if m in state["mods"]:
            state["mods"][m]["enabled"] = bool(on)
            state["mods"][m].setdefault("title", "HewDraw Remix")
    state["order"] = [m for m in MODS if m in state["mods"]] + [n for n in state["order"] if n not in MODS]
    tree.save(state)
    plugins = std_plugins.folder(tree.root)
    os.makedirs(plugins, exist_ok=True)
    for p in PLUGINS:
        dest = os.path.join(plugins, p)
        if on:
            shutil.copyfile(os.path.join(folder(tree), "plugins", p), dest)
        elif os.path.exists(dest):
            os.remove(dest)
    log(f"HewDraw Remix turned {'on' if on else 'off'}")
    return status(tree)


def set_only(tree, on, log=print):
    """HDR only (the launcher's HDR mode), on or off. On: what each mod's switch was (and whether HDR was on)
    kept in mods.json's "hdr_only", HDR turned on and every other mod off - mods.Tree.load keeps them off while
    it is there, and skyline.standard loads only ONLY_PLUGINS of the community plugins. Off: every switch as
    it was. Returns whether anything changed (an apply is then due)."""
    state = tree.load()
    if bool(state.get("hdr_only")) == bool(on):
        return False
    if on:
        if not installed(tree):
            raise Error("HDR isn't installed")
        kept = {n: bool(m.get("enabled")) for n, m in state["mods"].items()}
        set_on(tree, True, log)
        state = tree.load()
        state["hdr_only"] = {"enabled": kept, "hdr_was_on": all(kept.get(m) for m in MODS), "mods": list(MODS),
                             "plugins": list(ONLY_PLUGINS)}
        for n, m in state["mods"].items():
            m["enabled"] = n in MODS
        tree.save(state)
        log("HDR only: HewDraw Remix on, every other mod and community plugin off")
        return True
    saved = state.pop("hdr_only")
    for n, m in state["mods"].items():   # (a mod installed meanwhile: off, as a new one is)
        m["enabled"] = bool(saved.get("enabled", {}).get(n, False))
    tree.save(state)
    if installed(tree):
        set_on(tree, bool(saved.get("hdr_was_on")), log)
    log("HDR only: off, the mods as they were")
    return True


def remove(tree, log=print):
    if tree.load().get("hdr_only"):
        set_only(tree, False, log)
    if installed(tree):
        set_on(tree, False, log)
    for m in MODS:
        shutil.rmtree(os.path.join(tree.mods, m), ignore_errors=True)
    for p in SD:
        shutil.rmtree(os.path.join(tree.root, "dump", "skyline", "sd", *p.strip("/").split("/")), ignore_errors=True)
    for p in PLUGINS:
        dest = os.path.join(std_plugins.folder(tree.root), p)
        if os.path.exists(dest):
            os.remove(dest)
    shutil.rmtree(folder(tree), ignore_errors=True)
    tree.save(tree.load())
    log("HewDraw Remix removed")
    return status(tree)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("command", choices=("status", "download", "install", "on", "off", "only", "remove"))
    ap.add_argument("what", nargs="?", help="install: the package (zip or folder); only: on or off")
    ap.add_argument("--tree", default=os.path.dirname(HERE))
    a = ap.parse_args()
    tree = mods.Tree(a.tree)
    try:
        if a.command == "status":
            out = status(tree)
        elif a.command == "download":
            out = download(tree)
        elif a.command == "install":
            if not a.what:
                raise Error("name the package: HDR's switch-package.zip or the folder it was unpacked to")
            out = install(tree, a.what)
        elif a.command == "only":
            if a.what not in ("on", "off"):
                raise Error("only on|off")
            set_only(tree, a.what == "on")
            out = status(tree)
        elif a.command in ("on", "off"):
            out = set_on(tree, a.command == "on")
        else:
            out = remove(tree)
    except Error as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
