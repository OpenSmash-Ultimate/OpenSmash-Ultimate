#!/usr/bin/env python3
"""New characters on the select screen: the entries Skyline plugins add through the CSK
Collection's API (add_chara_db_entry_info, add_chara_layout_db_entry_info), which the runtime writes
to dump/skyline/csk.json as the plugins load (runtime/hle_skyline.c), put into the game's
ui_chara_db.prc and ui_layout_db.prc (tools/mods.py does it when it applies the mods).

Usage:
  csk.py show [--tree <dir>]                       the entries the plugins added
  csk.py apply <ui_chara_db.prc> <csk.json> <out>  (for testing)

An entry with a clone_from id is a copy of that entry with the given fields set; one with the id
of an entry the game has changes it; any other is a copy of the same fighter's entry (or the first)
with the fields set.
A field the entry sets that the database's entries lack is left out, but for the CSK Collection's
color_start_index: the entry's colors are the fighter's costumes from there on (cXX_index). Map fields (c00_index...,
characall_label_c00...) come keyed by their names' hash40s."""
import copy
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prc  # noqa: E402

CHARA_DB = "ui/param/database/ui_chara_db.prc"
LAYOUT_DB = "ui/param/database/ui_layout_db.prc"
KINDS = {"ui_chara_db": ("ui_chara_id", CHARA_DB), "ui_layout_db": ("ui_layout_id", LAYOUT_DB)}
TYPES = {"string": prc.STRING, "hash40": prc.HASH, "short": prc.I16, "int": prc.I32, "sbyte": prc.I8,
         "byte": prc.U8, "bool": prc.BOOL, "float": prc.FLOAT}
DB_ROOT = prc.hash40("db_root")
COLOR_START = prc.hash40("color_start_index")


def load(tree):
    """The entries the plugins added (dump/skyline/csk.json), but those of plugins whose mods are turned
    off now: the json is from the game's last start, so a mod turned off since still has its entries
    there (each entry names the plugin module that added it; tools/skyline.py's plugins.json maps
    modules to mods)."""
    path = os.path.join(tree, "dump", "skyline", "csk.json")
    try:
        with open(path, encoding="utf-8") as f:
            entries = json.load(f)
    except (OSError, ValueError):
        return []
    try:
        with open(os.path.join(tree, "dump", "skyline", "plugins.json"), encoding="utf-8") as f:
            record = json.load(f)
        with open(os.path.join(tree, "mods", "mods.json"), encoding="utf-8") as f:
            state = json.load(f)
    except (OSError, ValueError):
        return entries
    on = {n for n, m in state.get("mods", {}).items() if m.get("enabled")}
    off_modules = {r["module"] for r in record.values() if r.get("mod") not in on and not r.get("mod", "").startswith("(standard)")}
    return [e for e in entries if e.get("plugin") not in off_modules]


def targets(entries):
    """The database files the entries change."""
    return sorted({KINDS[e["kind"]][1] for e in entries if e.get("kind") in KINDS})


def key(name):
    return int(name, 16) if name.startswith("0x") else prc.hash40(name)


def value(kind, v):
    if kind == prc.HASH:
        return int(v, 16) if isinstance(v, str) else v
    return v


def apply(data, entries, db_kind):
    """A database's bytes with the entries of db_kind ("ui_chara_db", "ui_layout_db") in it; and
    notes on what was done."""
    id_name, _ = KINDS[db_kind]
    id_key = prc.hash40(id_name)
    root = prc.read(data)
    rows = dict(root[1]).get(DB_ROOT)
    if not rows or rows[0] != prc.LIST:
        raise prc.Error("no db_root list")
    rows = rows[1]
    notes = []
    last = last_place(rows)

    def find(h):
        for i, (_kind, st) in enumerate(rows):
            if dict(st).get(id_key, (None, None))[1] == h:
                return i
        return None

    for e in entries:
        if e.get("kind") != db_kind:
            continue
        new_id = int(e[id_name], 16)
        at = find(new_id)
        if at is not None:
            row = rows[at]
        else:
            base = find(int(e["clone_from"], 16)) if e.get("clone_from") else None
            if base is None:   # (else the same fighter's entry, or the same character's for a layout)
                like = e.get("set", {}).get("fighter_kind" if db_kind == "ui_chara_db" else "ui_chara_id")
                like_key = prc.hash40("fighter_kind" if db_kind == "ui_chara_db" else "ui_chara_id")
                if like and db_kind == "ui_layout_db":   # (the new character's own layout is not there: its fighter's)
                    like = None
                for i, (_kind, st) in enumerate(rows):
                    if like and dict(st).get(like_key, (None, None))[1] == int(like[1], 16):
                        base = i
                        break
            row = copy.deepcopy(rows[base if base is not None else 0])
            fields = row[1]
            for i, (k, p) in enumerate(fields):
                if k == id_key:
                    fields[i] = (k, (prc.HASH, new_id))
            rows.append(row)
        fields = row[1]
        index = {k: i for i, (k, _p) in enumerate(fields)}
        sets = {key(name): (TYPES[tname], v) for name, (tname, v) in e.get("set", {}).items()}
        # CSK's color_start_index (not a field of the game's): the entry's colors are that fighter's
        # costumes from there on (c120...), which the game's cXX_index fields say per color
        start = sets.pop(COLOR_START, None)
        if start is not None and start[1]:
            count = sets.get(prc.hash40("color_num"), (None, None))[1] or dict(fields).get(prc.hash40("color_num"), (0, 8))[1]
            for c in range(count):
                sets[prc.hash40(f"c{c:02d}_index")] = (prc.U8, start[1] + c)
        for k, (t, v) in sets.items():
            if k in index:
                t = fields[index[k]][1][0]   # (the database's own type for that field)
                fields[index[k]] = (k, (t, value(t, v)))
            else:
                notes.append(f"{db_kind}: field {k:#x} left out (the game's entries have no such field)")
        notes.append(f"{db_kind}: {'changed' if at is not None else 'added'} {e[id_name]}"
                     + (f" (a copy of {e['clone_from']})" if at is None and e.get("clone_from") else ""))
    if db_kind == "ui_chara_db":
        notes += place_beyond(rows, last)
    return prc.write(root), notes


DISP_ORDER, RANDOM_PLACE = prc.hash40("disp_order"), 99   # (99: the Random tile)


def last_place(rows):
    """The last fighter's place on the select screen (86 in 13.0.5), before any entry is added."""
    return max((dict(st).get(DISP_ORDER, (None, -1))[1] for _k, st in rows
                if -1 < dict(st).get(DISP_ORDER, (None, -1))[1] < RANDOM_PLACE), default=0)


def place_beyond(rows, last):
    """Entries asking for a place past the last fighter's go right after it. The grid takes as many
    tiles as there are entries (two at one place sit side by side), but a place between the last
    fighter's and Random's (87-98) or past them collapses it to one column (and with more, the game
    stops)."""
    notes = []
    for _kind, st in rows:
        for i, (k, p) in enumerate(st):
            if k == DISP_ORDER and p[1] is not None and p[1] > last and p[1] != RANDOM_PLACE:
                notes.append(f"ui_chara_db: select-screen place {p[1]} is past the last fighter's ({last}): {last} instead")
                st[i] = (k, (p[0], last))
    return notes


NEW_TILES = 4   # (the select screen's layout: a fifth new tile takes a 14th column, and the game aborts while loading the menus)


def limit_new(entries, log=print):
    """The entries with at most NEW_TILES new characters (the first ones, in the plugins' order); the
    others are left off the select screen (their costumes still work, by number)."""
    keep, dropped = [], []
    new_ids = [e["ui_chara_id"] for e in entries if e.get("kind") == "ui_chara_db" and int(e["ui_chara_id"], 16) not in KNOWN]
    allowed = set(new_ids[:NEW_TILES])
    for e in entries:
        if e.get("kind") == "ui_chara_db" and e["ui_chara_id"] in new_ids and e["ui_chara_id"] not in allowed:
            dropped.append(e.get("set", {}).get("name_id", ["", e["ui_chara_id"]])[1])
            continue
        if e.get("kind") == "ui_layout_db" and e.get("set", {}).get("ui_chara_id", ["", ""])[1] in set(new_ids) - allowed:
            continue
        keep.append(e)
    if dropped:
        log(f"Skyline plugins: only {NEW_TILES} new characters fit the select screen: left off - " + ", ".join(dropped) +
            " (turn others off to show them)")
    return keep


KNOWN = set()   # (ui_chara ids the game has: none of the plugins' new ones are; csk.apply changes those in place)


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "show":
        tree = sys.argv[sys.argv.index("--tree") + 1] if "--tree" in sys.argv else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        print(json.dumps(load(tree), indent=1))
    elif len(sys.argv) == 5 and sys.argv[1] == "apply":
        with open(sys.argv[2], "rb") as f:
            data = f.read()
        with open(sys.argv[3], encoding="utf-8") as f:
            entries = json.load(f)
        kind = "ui_layout_db" if "layout" in os.path.basename(sys.argv[2]) else "ui_chara_db"
        out, notes = apply(data, entries, kind)
        with open(sys.argv[4], "wb") as f:
            f.write(out)
        print("\n".join(notes))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
