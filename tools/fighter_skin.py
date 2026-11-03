#!/usr/bin/env python3
"""A skin mod made for a game fighter, put on a new fighter that was made from it.

Usage:
  fighter_skin.py <skin mod folder> <fighter folder> [--from <donor>] [--credit "<text>"] [--dry]

A skin mod for, say, Ganondorf has its files at Ganondorf's paths (fighter/ganon/model/body/c00/...,
effect/fighter/ganon/ef_ganon_c00.eff, sound/bank/fighter_voice/vc_ganon_c00.nus3audio,
ui/replace/chara/chara_0/chara_0_ganon_00.bntx). A new fighter made from Ganondorf (SSBU_CHARACTERS\\<name>,
fighter.json "donor": "ganon") plays from its own folder under its own name (runtime/hle_fighter_registry.c,
tools/fighter_mod.py). This copies the skin's files into the fighter's folder with the donor's name
replaced by the fighter's, in every path and file name:

  fighter/<donor>/...                         -> fighter/<name>/...           (all colours c00-c07)
  sound/bank/*/<xx>_<donor>_cNN.*             -> sound/bank/*/<xx>_<name>_cNN.*
  ui/replace/chara/chara_N/chara_N_<donor>_NN -> ui/replace/chara/chara_N/chara_N_<name>_NN
  effect/fighter/<donor>/ef_<donor>_cNN.eff   -> the fighter's own effect file, when the skin's colours
                                                 all have the same one (a colour's own effects need the
                                                 One Slot Effects plugin, which a new fighter's folder
                                                 does not use); different ones: c00's, and a warning.

The mod's own files (config.json, info.toml, preview pictures) are not copied; its info.toml's author goes
into the fighter's fighter.json "credits" (or --credit). The fighter folder's files of the same path are
replaced: back the folder up first if you want them again. Nothing of the mod is ever published: the folder
stays local (the mods' authors' terms). Then: tools/fighter_mod.py <fighter folder> --tree <tree> --apply --shaders.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import zlib

SKIP = {"config.json", "info.toml", "preview.webp", "preview.png", "preview.jpg"}


def hash_name(name):
    """0x<hash40 of the file's name, extension included>.<extension>: how a file named only by its hash is kept."""
    h = (len(name.encode()) << 32) | zlib.crc32(name.encode())
    return "0x%010x%s" % (h, os.path.splitext(name)[1])


def donor_of(folder):
    with open(os.path.join(folder, "fighter.json"), encoding="utf-8") as f:
        j = json.load(f)
    return j["name"], j.get("donor")


def author_of(mod):
    p = os.path.join(mod, "info.toml")
    if not os.path.isfile(p):
        return None
    with open(p, encoding="utf-8", errors="replace") as f:
        text = f.read()
    name = re.search(r'^display_name\s*=\s*"([^"]*)"', text, re.M)
    who = re.search(r'^authors\s*=\s*"([^"]*)"', text, re.M)
    if not who:
        return None
    return (name.group(1) + " by " if name else "") + who.group(1)


def mod_roots(path):
    """The folders that hold a mod's data (fighter/, effect/, sound/, ui/): the path itself or its sub-folders
    (a download often wraps the mod in one more folder)."""
    keys = ("fighter", "effect", "sound", "ui", "camera")
    if any(os.path.isdir(os.path.join(path, k)) for k in keys):
        return [path]
    out = []
    for d in sorted(os.listdir(path)):
        sub = os.path.join(path, d)
        if os.path.isdir(sub) and any(os.path.isdir(os.path.join(sub, k)) for k in keys):
            out.append(sub)
    return out


def effect_file(folder, name):
    """The fighter's own effect file (effect/fighter/<name>/<one .eff>), or None."""
    d = os.path.join(folder, "effect", "fighter", name)
    effs = [f for f in os.listdir(d) if f.lower().endswith(".eff")] if os.path.isdir(d) else []
    return os.path.join(d, effs[0]) if len(effs) == 1 else None


def plan(mod, folder, name, donor, say):
    """(source, destination) pairs."""
    pairs, effects = [], []
    tok = re.compile(r"(?<![a-z0-9])%s(?![a-z0-9])" % re.escape(donor))
    for root in mod_roots(mod):
        for d, _dirs, files in os.walk(root):
            for f in files:
                src = os.path.join(d, f)
                rel = os.path.relpath(src, root).replace("\\", "/")
                if "/" not in rel and f in SKIP:
                    continue
                if re.fullmatch(r"effect/fighter/%s/ef_%s(_c\d+)?\.eff" % (re.escape(donor), re.escape(donor)), rel):
                    effects.append(src)
                    continue
                pairs.append((src, os.path.join(folder, tok.sub(name, rel))))
    if effects:
        own = effect_file(folder, name)
        if not own:
            say(f"warning: {name} has no single effect file in effect/fighter/{name}: the skin's effects are left out")
        else:
            digests = set()
            for p in effects:
                with open(p, "rb") as f:
                    digests.add(hashlib.sha256(f.read()).hexdigest())
            first = sorted(effects)[0]
            if len(digests) > 1:
                say(f"warning: the skin's colours have {len(digests)} different effect files; a new fighter has one: "
                    f"{os.path.basename(first)} is used for every colour")
            pairs.append((first, own))
    return pairs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("mod")
    ap.add_argument("folder")
    ap.add_argument("--from", dest="donor")
    ap.add_argument("--credit")
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args(argv)
    name, donor = donor_of(a.folder)
    donor = a.donor or donor
    if not donor:
        print("error: no donor (fighter.json \"donor\", or --from)", file=sys.stderr)
        return 1
    pairs = plan(a.mod, a.folder, name, donor, print)
    kinds, replaced = {}, 0
    for src, dst in pairs:
        top = os.path.relpath(dst, a.folder).replace("\\", "/").split("/")[0]
        kinds[top] = kinds.get(top, 0) + 1
        # the folder may hold the same file under its name's hash (0x0e79652858.nuanmb = j02win1.nuanmb: the
        # game stores most names so, and tools copy them so): that one goes, or both would claim one path
        hashed = os.path.join(os.path.dirname(dst), hash_name(os.path.basename(dst)))
        if os.path.exists(hashed) and os.path.normcase(hashed) != os.path.normcase(dst):
            replaced += 1
            if not a.dry:
                os.remove(hashed)
        if not a.dry:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
    if replaced:
        print(f"{replaced} files the folder had under their name's hash {'would be ' if a.dry else ''}replaced")
    print(f"{name}: {len(pairs)} files of the skin {'would be ' if a.dry else ''}put at {name}'s paths ("
          + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())) + ")")
    credit = a.credit or author_of(a.mod) or (author_of(mod_roots(a.mod)[0]) if mod_roots(a.mod) else None)
    if credit and not a.dry:
        mf = os.path.join(a.folder, "fighter.json")
        with open(mf, encoding="utf-8") as f:
            j = json.load(f)
        credits = j.setdefault("credits", [])
        if credit not in credits:
            credits.append(credit)
            with open(mf, "w", encoding="utf-8", newline="\n") as f:
                json.dump(j, f, indent=2, ensure_ascii=False)
                f.write("\n")
        print(f"credit in fighter.json: {credit}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
