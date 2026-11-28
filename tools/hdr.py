#!/usr/bin/env python3
"""HewDraw Remix (HDR, github.com/HDR-Development): the gameplay overhaul, from its official release,
as a switch the Mods app turns on and off - on, it is applied with the user's own mods.

Usage:
  hdr.py status   [--tree <dir>]     installed or not, on or off
  hdr.py download [--tree <dir>]     download HDR's release package and install it (turned off)
  hdr.py install <folder|zip> [--tree <dir>]   install it from a switch-package already downloaded
                                     (the zip, or a folder it was unpacked to: a newer prerelease, say)
  hdr.py on|off   [--tree <dir>]     turn it on or off (then apply, and build plugins if asked)
  hdr.py only on|off [--tree <dir>]  the launcher's HDR mode: ARCropolis' HDR workspace made active, or the one
                                     before it again
  hdr.py motion   [--tree <dir>]     write HDR's motion lists again (mods/hdr-motion: tools/hdr_motion.py), then apply
  hdr.py remove   [--tree <dir>]     delete it

The package is what a Switch's SD card takes. Of it: ultimate/mods/hdr, hdr-assets and hdr-stages go
in as mods (by those names: HDR reads its own folders), first in the order so the user's mods win
where both change a file; ultimate/stage-alts goes on the plugins' SD card (dump/skyline/sd); its stage
plugins (stage_config, stage_alts) are kept in dump/hdr/plugins and put among the standard plugins
(skyline/plugins) once it is installed. The package's ARCropolis, Smashline and nro_hook are left out unless newer
than the standard plugins' (std_plugins.py), and hdr-launcher (the console's updater) isn't used: left out.
HDR's motion_patch.yaml files (new moves, changed frame data) are applied to the game's motion lists when it is
turned on (tools/hdr_motion.py, into the mods folder hdr-motion, switched with HDR's): on the console its plugin
does that as the lists load, which doesn't happen here - without it Ridley's aerial down special, Dark Samus'
up special and HDR's other new moves T-posed.
HDR isn't shipped with OpenSmash: each user downloads it from HDR's releases.

HDR mode (the launcher's third mode, beside Play and Vanilla): HDR changes every fighter, and a moveset mod on top
of it can crash the game or, online, play a different match than the other players' games. So in this mode HDR's
folders are the only ones ARCropolis loads: once HDR is installed they are always among the folders it is shown
(mods.seen), and ARCropolis' workspace "HDR" holds them alone (tools/arc_workspace.py) - the console's way, its own
menu's ARCropolis > Workspaces. The launcher's HDR mode makes that workspace the active one, Play the one before it:
every mod's switch, and every plugin the game is built with, stays as it is, so the switch needs no apply and no
build (HDR's stage plugins are among the standard ones for good once it is installed; the moveset mods' plugins come
through ARCropolis, from the workspace's mods only). mods.json's "hdr_only" ({"workspace": true}) marks the mode.
The package's ARCropolis, Smashline and nro_hook replace OpenSmash's only when newer (newer_standard)."""
import argparse
import hashlib
import json
import os
import re
import shutil
import ssl
import sys
import time
import urllib.error
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hdr_motion  # noqa: E402
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

# HDR's motion_patch.yaml files applied ahead of time (tools/hdr_motion.py): a mods folder written here, right
# after HDR's three in the order and on exactly when they are. Without it HDR's new moves T-pose
MOTION = hdr_motion.FOLDER


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


def own_mods(tree):
    """HDR's mods in their order: its three, and the motion lists written for them once there."""
    return MODS + ((MOTION,) if os.path.isdir(os.path.join(tree.mods, MOTION)) else ())


def bake_motion(tree, log=print, force=False):
    """mods/hdr-motion written again if HDR's motion patches (or the costume folders beside them) changed
    since, or it's missing; switched like HDR's mods (and allowed in HDR only). Whether anything changed
    (an apply is then due); None if HDR isn't installed."""
    if not installed(tree):
        return None
    if not force and hdr_motion.current(tree, MODS):
        return False
    log("applying HDR's motion patches to the game's motion lists")
    try:
        hdr_motion.bake(tree, MODS, log)
    except hdr_motion.Error as e:
        raise Error(f"HDR's motion patches: {e}") from None
    state = tree.load()   # (picks the folder up, off - switched here)
    on = all(state["mods"].get(m, {}).get("enabled") for m in MODS)
    only = state.get("hdr_only")
    if isinstance(only, dict) and os.path.isdir(os.path.join(tree.mods, MOTION)):
        only["mods"] = list(dict.fromkeys(list(only.get("mods") or MODS) + [MOTION]))
    if MOTION in state["mods"]:
        state["mods"][MOTION]["enabled"] = on
        state["mods"][MOTION].setdefault("title", "HewDraw Remix (motion lists)")
    own = own_mods(tree)
    state["order"] = [m for m in own if m in state["mods"]] + [n for n in state["order"] if n not in own]
    tree.save(state)
    return True


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
            or any(name == d + p for d in PLUGIN_DIRS for p in PLUGINS + tuple(std_plugins.PLUGINS)))


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
    newer_standard(tree, keep, log)
    with open(os.path.join(folder(tree), "installed.json"), "w", encoding="utf-8") as f:
        json.dump(record, f, indent=1)
    set_on(tree, was_on, log)


# Where a standard plugin keeps its version (its name, then the version): how the package's copy is told newer or not
VERSION_MARKS = {"libarcropolis.nro": (b"arcropolis",), "libsmashline_plugin.nro": (b"smashline",),
                 "libnro_hook.nro": (b"nro_hook", b"nro-hook", b"nro hook")}


def _version(text):
    try:
        return tuple(int(x) for x in re.findall(r"\d+", text)[:3])
    except ValueError:
        return ()


def nro_version(data, name):
    """The version a plugin names itself with (e.g. "ARCropolis v4.1.1"), or () when it names none."""
    best = ()
    for mark in VERSION_MARKS.get(name, ()):
        for m in re.finditer(re.escape(mark) + rb"[ _\-/:]{0,3}v?(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?![\d.])", data, re.I):
            best = max(best, tuple(int(g) for g in m.groups()))
    return best


def newer_standard(tree, keep, log=print):
    """The package's copies of OpenSmash's standard plugins (ARCropolis, Smashline, nro_hook): one that is a newer
    version than OpenSmash's replaces it (OpenSmash's kept in dump/hdr/replaced, put back when HDR is removed); the
    same or older stays out - OpenSmash's own runs. Whether one was put in."""
    std = std_plugins.folder(tree.root)
    put = False
    for name in sorted(std_plugins.PLUGINS):
        theirs = os.path.join(keep, name)
        if not os.path.isfile(theirs):
            continue
        ours = os.path.join(std, name)
        with open(theirs, "rb") as f:
            data = f.read()
        mine = b""
        if os.path.isfile(ours):
            with open(ours, "rb") as f:
                mine = f.read()
        if data == mine:
            log(f"HDR's {name}: the same as OpenSmash's")
            continue
        new, old = nro_version(data, name), (nro_version(mine, name) if mine else ()) or _version(std_plugins.PLUGINS[name][0])
        if mine and not (new and old and new > old):
            log(f"HDR's {name} ({'.'.join(map(str, new)) or 'no version in it'}): not newer than OpenSmash's "
                f"({'.'.join(map(str, old)) or '?'}): OpenSmash's kept")
            continue
        backup = os.path.join(folder(tree), "replaced")
        if mine and not os.path.isfile(os.path.join(backup, name)):
            os.makedirs(backup, exist_ok=True)
            shutil.copyfile(ours, os.path.join(backup, name))
        os.makedirs(std, exist_ok=True)
        shutil.copyfile(theirs, ours)
        put = True
        log(f"HDR's {name} {'.'.join(map(str, new))}: newer than OpenSmash's ({'.'.join(map(str, old)) or 'none'}): put in its place")
    for name in std_plugins.PLUGINS:   # (only HDR's stage plugins are kept with it)
        if os.path.isfile(os.path.join(keep, name)):
            os.remove(os.path.join(keep, name))
    return put


def stage_plugins(tree):
    """HDR's stage plugins among the standard ones - always, once it is installed: the plugins the game is built with
    then stay the same in Play and HDR mode, so switching needs no build. Whether one was copied."""
    plugins = std_plugins.folder(tree.root)
    os.makedirs(plugins, exist_ok=True)
    copied = False
    for p in PLUGINS:
        src, dest = os.path.join(folder(tree), "plugins", p), os.path.join(plugins, p)
        if not os.path.isfile(src):
            continue
        with open(src, "rb") as f:
            want = f.read()
        try:
            with open(dest, "rb") as f:
                same = f.read() == want
        except OSError:
            same = False
        if not same:
            shutil.copyfile(src, dest)
            copied = True
    return copied


def set_on(tree, on, log=print):
    """HDR's mods turned on or off (first in the order: the user's own win where both change a file),
    and its stage plugins put among the standard ones or taken out. Apply (and Build plugins, for a
    plugin not compiled into the game yet) makes it so in the game."""
    if on and not installed(tree):
        raise Error("HDR isn't downloaded")
    if on:
        bake_motion(tree, log)
    state = tree.load()
    own = own_mods(tree)
    for m in own:
        if m in state["mods"]:
            state["mods"][m]["enabled"] = bool(on)
            state["mods"][m].setdefault("title", "HewDraw Remix" if m in MODS else "HewDraw Remix (motion lists)")
    state["order"] = [m for m in own if m in state["mods"]] + [n for n in state["order"] if n not in own]
    tree.save(state)
    if installed(tree):
        stage_plugins(tree)
    log(f"HewDraw Remix turned {'on' if on else 'off'}")
    return status(tree)


def _restore_legacy(tree, state, log):
    """mods.json's "hdr_only" as it was before the workspaces (every other mod's switch turned off, kept there): the
    switches as they were."""
    saved = state.pop("hdr_only")
    for n, m in state["mods"].items():   # (a mod installed meanwhile: off, as a new one is)
        m["enabled"] = bool(saved.get("enabled", {}).get(n, False))
    tree.save(state)
    if installed(tree):
        set_on(tree, bool(saved.get("hdr_was_on")), log)
    log("HDR mode as it was before ARCropolis' workspaces: every mod's switch put back")
    return tree.load()


def _shown(tree):
    """The mods folders ARCropolis is shown now (dump/skyline/mods.txt's lines)."""
    try:
        with open(os.path.join(tree.root, "dump", "skyline", "mods.txt"), encoding="utf-8") as f:
            return [ln.strip() for ln in f if ln.strip()]
    except OSError:
        return None


def set_only(tree, on, log=print):
    """The launcher's HDR mode, on or off: ARCropolis' "HDR" workspace (HDR's folders alone, tools/arc_workspace.py)
    made the active one, or the one before it again - every mod's switch stays as it is, and so do the plugins the
    game is built with, so neither an apply nor a build is due. mods.json's "hdr_only" ({"workspace": true}) marks
    the mode (the launcher reads it; the online mod set is HDR's then, mods.write_applied). Returns whether an apply
    is due after all: the first time (HDR's folders not shown to ARCropolis yet) or once HDR's files changed."""
    import arc_workspace
    state = tree.load()
    only = state.get("hdr_only")
    need = False
    if isinstance(only, dict) and not only.get("workspace"):
        state = _restore_legacy(tree, state, log)
        need = True
    if on:
        if not installed(tree):
            raise Error("HDR isn't installed")
        shown = _shown(tree) or []
        hdr_roots = set(own_mods(tree)) | {mods.HDR_SHARES}
        if not arc_workspace.ready(tree.root, [r for r in shown if r.split("/")[0] not in hdr_roots]):
            raise Error("HDR mode is an ARCropolis workspace, and ARCropolis has no settings to keep one in yet (or ones "
                        "OpenSmash can't read): start the game once with Play, then pick HDR")
        need = bool(bake_motion(tree, log)) or need
        need = stage_plugins(tree) or need
        state = tree.load()
        state["hdr_only"] = {"workspace": True, "mods": list(own_mods(tree))}
        tree.save(state)
        shown = _shown(tree)
        need = need or shown is None or not all(m in {x.split("/")[0] for x in shown} for m in own_mods(tree))
        if not need:
            arc_workspace.update(tree, shown, state, log)
        if not arc_workspace.activate(tree.root, True, log) and not arc_workspace.active_is(tree.root, arc_workspace.WORKSPACE):
            if need:
                log("ARCropolis' HDR workspace is made as the mods are applied: it is made the active one then")
            else:
                log("ARCropolis has no HDR workspace yet (no settings of its own until the game first starts with it): "
                    "start the game once in Play mode, then pick HDR")
        log("HDR mode: ARCropolis' HDR workspace - HewDraw Remix alone; every other mod stays switched as it was")
    else:
        if isinstance(state.get("hdr_only"), dict):
            state.pop("hdr_only")
            tree.save(state)
        arc_workspace.activate(tree.root, False, log)
        log("HDR mode: off - ARCropolis' workspace from before it")
    if not need:
        on_now = [n for n in state["order"] if state["mods"].get(n, {}).get("enabled")]
        mods.write_applied(tree, tree.load(), on_now)
    return need


def remove(tree, log=print):
    if tree.load().get("hdr_only"):
        set_only(tree, False, log)
    if installed(tree):
        set_on(tree, False, log)
    for m in MODS + (MOTION,):
        shutil.rmtree(os.path.join(tree.mods, m), ignore_errors=True)
    for p in SD:
        shutil.rmtree(os.path.join(tree.root, "dump", "skyline", "sd", *p.strip("/").split("/")), ignore_errors=True)
    for p in PLUGINS:
        dest = os.path.join(std_plugins.folder(tree.root), p)
        if os.path.exists(dest):
            os.remove(dest)
    replaced = os.path.join(folder(tree), "replaced")   # (OpenSmash's standard plugins HDR's newer ones replaced)
    for name in (os.listdir(replaced) if os.path.isdir(replaced) else ()):
        shutil.copyfile(os.path.join(replaced, name), os.path.join(std_plugins.folder(tree.root), name))
        log(f"{name}: OpenSmash's own put back")
    shutil.rmtree(os.path.join(tree.mods, mods.HDR_SHARES), ignore_errors=True)
    shutil.rmtree(folder(tree), ignore_errors=True)
    tree.save(tree.load())
    log("HewDraw Remix removed")
    return status(tree)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("command", choices=("status", "download", "install", "on", "off", "only", "motion", "remove"))
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
            if set_only(tree, a.what == "on"):
                mods.apply(tree)
                import arc_workspace
                arc_workspace.activate(tree.root, a.what == "on")
            out = status(tree)
        elif a.command in ("on", "off"):
            out = set_on(tree, a.command == "on")
        elif a.command == "motion":
            if bake_motion(tree, force=True) is None:
                raise Error("HDR isn't installed")
            out = status(tree)
        else:
            out = remove(tree)
    except Error as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
