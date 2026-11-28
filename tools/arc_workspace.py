#!/usr/bin/env python3
"""ARCropolis' workspaces, for the launcher's HDR mode.

ARCropolis (the real one, among the standard plugins) loads only the mods of its active workspace's preset: the mods
folders it sees (sd:/ultimate/mods: dump/skyline/mods.txt's) whose hash is in it. Its in-game menu
(ARCropolis > Workspaces) switches between them, and the next start loads the other set at once - nothing applied,
nothing built. HDR mode uses that: HewDraw Remix's folders (hdr, hdr-assets, hdr-stages, hdr-motion) are always among
the folders ARCropolis sees once HDR is installed (tools/skyline.py), in a workspace of their own, "HDR", and left out
of every other one; the launcher's HDR mode makes "HDR" the active workspace, Play makes the one before active again.
Moveset mods' plugins come through ARCropolis too (a mod's plugin.nro is chainloaded from the mods it loads), so they
stay off in HDR mode as their files do.

ARCropolis' settings (crates/config in its source, skyline_config's storage) are files in
sd:/ultimate/arcropolis/config/<user id>/<user id>/ - here dump/skyline/sd/... - one a setting:
  workspace        the active workspace's name (plain text)
  workspace_list   JSON {workspace name: the setting its preset is kept in}; Default's is "presets"
  <preset>         JSON [hash40 of "sd:/ultimate/mods/<folder>", ...] - the mods turned on in that workspace
  mod_cache        JSON [hash40, ...] - the mods folders seen at the last start: one not in it (and not in the preset)
                   is new, and ARCropolis turns it on in the active workspace
  version          (a folder without it: ARCropolis makes its settings again from scratch at its next start)
hash40: the length of the text in the top 8 bits of 40, its CRC-32 below (smash-arc's hash40).

Usage: arc_workspace.py status [--tree <dir>]"""
import json
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
WORKSPACE = "HDR"
PRESET = "hdr_preset"
DEFAULT = "Default"


def hash40(text):
    b = text.encode()
    return (len(b) << 32) | zlib.crc32(b)


def flat(root):
    """A mods.txt line as ARCropolis sees the folder (runtime/hle_fs.c mod_folder_name): a pack's "Pack/X" is
    "Pack - X", mods/.characters is "WindowSmash characters"."""
    if root.startswith("."):
        return "WindowSmash " + root[1:]
    return root.replace("/", " - ")


# How ARCropolis spells a mods folder's path before hashing it (discover.rs: the mods path joined with read_dir's
# entry): the first is the console's; the others are what a joined path can come out as. The one its mod cache
# holds is used (form_of), and nothing is touched when none matches.
FORMS = ("sd:/ultimate/mods/{}", "sd:/ultimate/mods/sd:/ultimate/mods/{}", "sd:/ultimate/mods//{}",
         "sd://ultimate/mods/{}", "/ultimate/mods/{}", "ultimate/mods/{}", "sd:/ultimate/mods\\{}")


def mod_hash(root, form=FORMS[0]):
    """The hash ARCropolis knows a mods folder by: its path on the SD card, hash40."""
    return hash40(form.format(flat(root)))


def form_of(d, roots):
    """The spelling (FORMS) under which the mod cache holds these folders, or None: the one with the most of them."""
    cache = set(_read_json(d, "mod_cache", []))
    best, most = None, 0
    for form in FORMS:
        n = sum(1 for r in roots if mod_hash(r, form) in cache)
        if n > most:
            best, most = form, n
    return best


def ready(tree_root, roots):
    """Whether HDR's workspace can be kept for every ARCropolis settings folder: there is one, and its mod cache
    holds these folders (the ones shown to it now, HDR's aside) under a spelling known here."""
    dirs = config_dirs(tree_root)
    return bool(dirs) and bool(roots) and all(form_of(d, roots) for d in dirs)


def config_dirs(tree_root):
    """ARCropolis' settings folders (one each user that started the game): those with a version and a workspace list."""
    base = os.path.join(tree_root, "dump", "skyline", "sd", "ultimate", "arcropolis", "config")
    out = []
    if os.path.isdir(base):
        for a in sorted(os.listdir(base)):
            pa = os.path.join(base, a)
            if not os.path.isdir(pa):
                continue
            for b in sorted(os.listdir(pa)):
                d = os.path.join(pa, b)
                if os.path.isfile(os.path.join(d, "version")) and os.path.isfile(os.path.join(d, "workspace_list")):
                    out.append(d)
    return out


def _read(d, name, default=None):
    try:
        with open(os.path.join(d, name), encoding="utf-8") as f:
            return f.read()
    except OSError:
        return default


def _read_json(d, name, default):
    text = _read(d, name)
    if text is None:
        return default
    try:
        return json.loads(text)
    except ValueError:
        return default


def _write(d, name, text):
    path = os.path.join(d, name)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    os.replace(tmp, path)


def _write_json(d, name, value):
    _write(d, name, json.dumps(value, separators=(",", ":")))


def _preset_name(lst, workspace):
    return lst.get(workspace) or ("presets" if workspace == DEFAULT else None)




def prepare(tree_root, hdr_folders, visible, hdr_in_play, log=print):
    """Every settings folder: the HDR workspace made (its preset: HDR's folders), HDR's folders left out of the
    Play workspace (the one before HDR's, else the active one) unless hdr_in_play (HDR turned on with the user's mods, the
    Mods app's switch: then in it), and in the mod cache, so
    ARCropolis does not take them for new mods and turn them on in the active workspace. visible: the folders
    ARCropolis sees (mods.txt): those new since its last start are turned on in the Play workspace, as ARCropolis would
    have had it been the active one, and put in the cache. Returns the settings folders done."""
    dirs = config_dirs(tree_root)
    done = []
    for d in dirs:
        lst = _read_json(d, "workspace_list", {})
        if not isinstance(lst, dict):
            log(f"ARCropolis settings {d}: its workspace list is not readable: left as it is")
            continue
        changed_list = False
        if lst.get(WORKSPACE) != PRESET:
            lst[WORKSPACE] = PRESET
            changed_list = True
        if DEFAULT not in lst:
            lst[DEFAULT] = "presets"
            changed_list = True
        cache = set(_read_json(d, "mod_cache", []))
        form = form_of(d, [r for r in visible if r not in hdr_folders])
        if not form:   # (none of the folders it saw last time: not the hashes it makes, so hands off)
            log(f"ARCropolis settings {d}: its mod cache has none of the mods folders' hashes as made here: left as it is")
            continue
        hdr = {mod_hash(f, form) for f in hdr_folders}
        if changed_list:
            _write_json(d, "workspace_list", lst)
        _write_json(d, PRESET, sorted(hdr))
        new = {mod_hash(f, form) for f in visible} - cache - hdr
        active = (_read(d, "workspace") or DEFAULT).strip()
        back = (_read(d, "workspace_before_hdr") or "").strip()
        if active != WORKSPACE:
            play = active if active in lst else DEFAULT
        else:
            play = back if back in lst and back != WORKSPACE else DEFAULT
        preset = _preset_name(lst, play)   # (the Play workspace's: a workspace the user made stays theirs to set)
        items = set(_read_json(d, preset, []))
        was = set(items)
        items = (items | new | hdr) if hdr_in_play else ((items | new) - hdr)
        if items != was:
            _write_json(d, preset, sorted(items))
        if (cache | hdr | new) != cache:
            _write_json(d, "mod_cache", sorted(cache | hdr | new))
        done.append(d)
    if not dirs:
        log("ARCropolis has no settings yet (it makes them at the game's first start with it): HDR's workspace is made after that")
    return done


def update(tree, roots, state, log=print):
    """prepare for what ARCropolis is shown (roots: dump/skyline/mods.txt's lines) and mods.json's state (a mods.Tree's):
    HDR's folders and its shares folder in HDR's workspace; in the Play one too when HDR's switch is on there."""
    import mods
    hdr = set(mods.hdr_seen(tree))
    if not hdr:
        return []
    mine = [r for r in roots if r.split("/")[0] in hdr or r == mods.HDR_SHARES]
    in_play = all(state["mods"].get(m, {}).get("enabled") for m in mods.HDR_FOLDERS[:3])
    return prepare(tree.root, mine, list(roots), in_play, log)


def active_is(tree_root, name):
    names = [ws for _d, ws in active_names(tree_root)]
    return bool(names) and all(ws == name for ws in names)


def activate(tree_root, on, log=print):
    """HDR's workspace made the active one (on), keeping the one before; or the one before made active again. Whether any
    settings folder changed."""
    changed = False
    for d in config_dirs(tree_root):
        active = (_read(d, "workspace") or DEFAULT).strip()
        lst = _read_json(d, "workspace_list", {})
        if not isinstance(lst, dict) or lst.get(WORKSPACE) != PRESET:   # (prepare left it alone: so is it here)
            continue
        if on:
            if active != WORKSPACE:
                _write(d, "workspace_before_hdr", active)
                _write(d, "workspace", WORKSPACE)
                changed = True
        else:
            if active == WORKSPACE:
                back = (_read(d, "workspace_before_hdr") or DEFAULT).strip()
                if back not in lst or back == WORKSPACE:
                    back = DEFAULT
                _write(d, "workspace", back)
                changed = True
    return changed


def active_names(tree_root):
    return [(d, (_read(d, "workspace") or DEFAULT).strip()) for d in config_dirs(tree_root)]


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("command", choices=("status",))
    ap.add_argument("--tree", default=os.path.dirname(HERE))
    a = ap.parse_args()
    for d, ws in active_names(a.tree):
        lst = _read_json(d, "workspace_list", {})
        print(f"{d}: active {ws}; workspaces: " + ", ".join(f"{k} ({len(_read_json(d, v, []))} mods)" for k, v in lst.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
