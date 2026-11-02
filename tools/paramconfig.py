#!/usr/bin/env python3
"""Native lib_paramconfig toml path: each enabled mod's config_param.toml -> dump/skyline/paramconfig.json.

The runtime (hle_paramconfig.c) loads that dump and hooks the game's get_param getters.
The standalone libparam_config.nro is not run (its main hangs here)."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import prc
from paramconfig_kinds import FIGHTERS, WEAPONS

try:
    import tomllib
except ImportError:
    tomllib = None
    try:
        import tomli as tomllib  # noqa: F401
    except ImportError:
        tomllib = None

# entry type tags in the dump (hle_paramconfig.c)
T_INT, T_FLOAT, T_ATTR_MUL, T_INT_MUL = 0, 1, 2, 3


def hash_param(s):
    if s is None:
        return 0
    s = str(s).strip()
    if not s or s == " ":
        return 0
    if s.startswith("0x") or s.startswith("0X"):
        return int(s, 16)
    return prc.hash40(s)


def kind_of(name):
    """Fighter kind id, or negated weapon kind id. None if unknown."""
    key = name.lower().strip()
    if key in FIGHTERS:
        return FIGHTERS[key]
    # weapons: names with '_' (except ice_climber) are weapons in the reference plugin
    if key == "ice_climber" and key in FIGHTERS:
        return FIGHTERS[key]
    if key in WEAPONS:
        return -WEAPONS[key]
    if "_" in key and key in WEAPONS:
        return -WEAPONS[key]
    # try as fighter anyway
    if key in FIGHTERS:
        return FIGHTERS[key]
    return None


def is_fighter_name(name):
    key = name.lower().strip()
    if key == "ice_climber":
        return True
    return "_" not in key


def resolve_kind(name):
    key = name.lower().strip()
    if is_fighter_name(key):
        return FIGHTERS.get(key)
    w = WEAPONS.get(key)
    return -w if w is not None else None


def read_toml(path):
    if not tomllib:
        raise RuntimeError("tomllib is needed to read config_param.toml (Python 3.11+)")
    with open(path, "rb") as f:
        return tomllib.load(f)


def entries_from_toml(data):
    """Flat [kind, slot, type, param_hash, sub_hash, value] rows."""
    main_kind = (data.get("kind") or "") or ""
    main_slots = list(data.get("slots") or [])
    out = []

    def add_block(items, typ):
        if not items:
            return
        for item in items:
            param = item.get("param") or ""
            sub = item.get("subparam")
            if sub is None:
                sub = ""
            # special params clear subparam like the reference plugin
            if typ == T_INT and param in ("article_use_type", "kirby_cant_copy"):
                sub = ""
            kinds = item.get("kinds")
            if kinds is None:
                kinds = [main_kind] if main_kind else []
            slots = item.get("slots")
            if slots is None:
                slots = list(main_slots)
            if not kinds or not slots:
                continue
            ph = hash_param(param)
            # villager/rosetta: subparam is a weapon kind id, not a hash
            if param in ("villager_pocket_behavior", "rosetta_cant_pocket", "rosetta_pull_behavior",
                         "kirby_inhale_behavior"):
                if sub:
                    wk = WEAPONS.get(str(sub).lower())
                    sh = wk if wk is not None else 0
                else:
                    sh = 0
            else:
                sh = hash_param(sub)
            if typ == T_INT:
                val = int(item.get("value", 0))
            else:
                val = float(item.get("value", 0.0))
            for kn in kinds:
                kind = resolve_kind(kn)
                if kind is None:
                    continue
                for slot in slots:
                    out.append([int(kind), int(slot), typ, ph, sh, val])

    add_block(data.get("param_int"), T_INT)
    add_block(data.get("param_float"), T_FLOAT)
    add_block(data.get("attribute_mul"), T_ATTR_MUL)
    add_block(data.get("param_int_mul"), T_INT_MUL)
    return out


def write(tree, log=print):
    """Scan enabled mods for config_param.toml; write dump/skyline/paramconfig.json."""
    import mods
    t = mods.Tree(tree) if not isinstance(tree, mods.Tree) else tree
    root = t.root
    state = t.load()
    on = [n for n in state["order"] if state["mods"].get(n, {}).get("enabled")]
    rows = []
    for name in on:
        path = os.path.join(t.mods, name, "config_param.toml")
        if not os.path.isfile(path):
            continue
        try:
            data = read_toml(path)
            part = entries_from_toml(data)
        except Exception as e:
            log(f"paramconfig: {name}: {e}")
            continue
        if part:
            log(f"paramconfig: {name}: {len(part)} override(s) from config_param.toml")
            rows.extend(part)
    sky = os.path.join(root, "dump", "skyline")
    os.makedirs(sky, exist_ok=True)
    out = os.path.join(sky, "paramconfig.json")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"entries": rows}, f, separators=(",", ":"))
        f.write("\n")
    if not rows:
        log("paramconfig: no config_param.toml overrides")
    return len(rows)


if __name__ == "__main__":
    tree = sys.argv[sys.argv.index("--tree") + 1] if "--tree" in sys.argv else os.path.dirname(HERE)
    write(tree)
