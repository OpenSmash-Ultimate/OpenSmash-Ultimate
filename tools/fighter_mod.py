#!/usr/bin/env python3
"""A new fighter's folder as a mod: its files at their own paths, under its own name.

Usage:
  fighter_mod.py <fighter folder> [--tree <dir>] [--apply]

A fighter folder (SSBU_CHARACTERS\\<name>: fighter.json with "name" and "donor", the script library,
the data files at their data.arc paths - fighter/<name>/..., sound/..., effect/fighter/<name>/...,
camera/fighter/<name>/..., ui/...) is played by runtime/hle_fighter_registry.c, which makes the game
build every path of the fighter with its own name. Those paths have to be in the game's data. This puts
the folder's data files into <tree>/mods/<name> (hard links, else copies) with a config.json that says
{"new-fighters": {"<name>": "<donor>"}}, and turns the mod on; tools/mods.py apply then copies the
donor's directories under the new name (mods.Table.clone_fighter) with the folder's files in place of
the donor's. A file named by its name's hash (0x17a1e0db16.nuanmb, as the game stores most names) keeps
that name: mods.py takes it as the hash.

The tree is the one mods.py applies to (its dump/modules/romfs.cfg is rewritten): --tree, default this
checkout. --apply runs mods.py apply after.
"""
import argparse
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mods  # noqa: E402

# the folder's parts that are not data files (the manifest, the scripts, the library, notes)
NOT_DATA = {"src", "fighter.json", "clone_files.json", "build.cmd", "readme.txt"}


def load_manifest(folder):
    path = os.path.join(folder, "fighter.json")
    try:
        with open(path, encoding="utf-8-sig") as f:
            manifest = json.load(f)
    except (OSError, ValueError) as e:
        raise mods.Error(f"{path}: can't be read ({e})")
    name, donor = str(manifest.get("name", "")).strip().lower(), str(manifest.get("donor", "")).strip().lower()
    if not name or not donor or "/" in name or "/" in donor:
        raise mods.Error(f"{path}: needs a \"name\" and a \"donor\" (a fighter of the game's)")
    return manifest, name, donor


def data_files(folder):
    """(data.arc path, file) of the folder's data files: those under the data's top folders."""
    out = []
    for top in sorted(os.listdir(folder)):
        full = os.path.join(folder, top)
        if top.lower() in NOT_DATA or not os.path.isdir(full) or top.lower() not in mods.TOP:
            continue
        for dp, dirs, files in os.walk(full):
            dirs.sort()
            for n in sorted(files):
                src = os.path.join(dp, n)
                out.append((mods.arc_path(os.path.relpath(src, folder)), src))
    return out


def link_or_copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def make_mod(folder, tree):
    """The fighter folder as the mod <tree>/mods/<name>, turned on; returns (name, donor, files)."""
    _manifest, name, donor = load_manifest(folder)
    files = data_files(folder)
    if not files:
        raise mods.Error(f"{folder}: no data files (fighter/{name}/..., sound/..., ui/...)")
    out = os.path.join(tree.mods, name)
    if os.path.isdir(out):
        mods.remove_folder(out)
    for path, src in files:
        link_or_copy(src, os.path.join(out, *path.split("/")))
    with open(os.path.join(out, "config.json"), "w", encoding="utf-8") as f:
        json.dump({"new-fighters": {name: donor}}, f, indent=1)
    state = tree.load()
    state["mods"][name] = dict(state["mods"].get(name, {}), enabled=True, title=f"{name} (a new fighter, made of {donor})")
    tree.save(state)
    return name, donor, len(files)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("folder")
    ap.add_argument("--tree", default=os.path.dirname(HERE))
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args(argv)
    tree = mods.Tree(a.tree)
    try:
        name, donor, n = make_mod(a.folder, tree)
        print(f"{name}: {n} data files in {os.path.join(tree.mods, name)} (new fighter made of {donor}), turned on")
        if a.apply:
            mods.apply(tree, print)
    except mods.Error as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
