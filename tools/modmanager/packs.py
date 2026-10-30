"""Modpacks: a set of mods, in order, to share and turn on together; and the check that tells,
before mods are applied, which of them clash.

A modpack is a small JSON file (.wsmodpack) or, when every mod in it comes from GameBanana or a
link, a share code (WSPACK1:..., the same JSON, zlib-compressed, base64url) to paste in a chat:

  {"format": "windowsmash-modpack", "version": 1, "name": ..., "author": ..., "description": ...,
   "mods": [{"name": the mod's folder name, "title": ..., "enabled": true,
             "gamebanana": mod id, "file": file id, "file_name": ..., "md5": ...   (GameBanana's)
             or "url": a direct download link
             or "bundled": true   (its files travel with the pack: an installer made with it)}]}

The mods' order is the list's: a later mod wins where two change the same file.
"""
import base64
import json
import os
import re
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import mods  # noqa: E402

FORMAT = "windowsmash-modpack"
CODE_PREFIX = "WSPACK1:"
SLOT = re.compile(r"^(fighter/[^/]+)/(?:.*/)?c(\d\d)/")      # fighter/<name>/.../cNN/
SKIN_FILES = {"numdlb", "numshb", "nutexb", "numatb", "nusktb"}   # a skin's model and textures
# a costume's physics or animation (swing bones, motions): made for the model it shipped with
MOTION_FILE = re.compile(r"^fighter/[^/]+/motion/.+\.(prc|nuanmb|bin)$|\.nuhlpb$", re.I)
KIRBY_COPY = re.compile(r"^fighter/kirby/model/(copy_[^/]+)/")   # Kirby's copy-ability hats: one per fighter copied
UI_SLOT = re.compile(r"^ui/replace(?:_patch)?/chara/chara_\d+/chara_\d+_(.+)_(\d\d)\.bntx$")


class PackError(Exception):
    pass


# ---- modpacks --------------------------------------------------------------------------------
def pack_from_mods(tree, names, name, author="", description=""):
    """A modpack of installed mods `names`, in that order."""
    state = tree.load()
    out = []
    for n in names:
        m = state["mods"].get(n)
        if m is None:
            raise PackError(f"no mod called {n}")
        entry = {"name": n, "title": m.get("title") or n, "enabled": True}
        if m.get("gamebanana"):
            entry["gamebanana"] = int(m["gamebanana"])
            for k in ("file", "file_id", "md5", "author", "category", "image"):
                if m.get(k):
                    entry["file_name" if k == "file" else k] = m[k]
        elif m.get("url"):
            entry["url"] = m["url"]
        else:
            entry["bundled"] = True
        out.append(entry)
    return {"format": FORMAT, "version": 1, "name": name, "author": author, "description": description, "mods": out}


def validate(pack):
    if not isinstance(pack, dict) or pack.get("format") != FORMAT:
        raise PackError("that isn't a WindowSmash modpack")
    if pack.get("version", 1) > 1:
        raise PackError("this modpack was made by a newer WindowSmash Mods: update it first")
    if not isinstance(pack.get("mods"), list) or not pack["mods"]:
        raise PackError("the modpack has no mods")
    for m in pack["mods"]:
        if not isinstance(m, dict) or not m.get("name"):
            raise PackError("the modpack lists a mod with no name")
        m["name"] = mods.safe_name(str(m["name"]))
    return pack


def to_code(pack):
    """A share code for the pack (None when a mod in it can't be downloaded: bundled files)."""
    if any(m.get("bundled") for m in pack["mods"]):
        return None
    raw = json.dumps(pack, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return CODE_PREFIX + base64.urlsafe_b64encode(zlib.compress(raw, 9)).decode("ascii").rstrip("=")


def from_code(code):
    code = "".join(code.split())
    if not code.startswith(CODE_PREFIX):
        raise PackError("that isn't a modpack code (it starts with WSPACK1:)")
    body = code[len(CODE_PREFIX):]
    try:
        raw = zlib.decompress(base64.urlsafe_b64decode(body + "=" * (-len(body) % 4)))
        return validate(json.loads(raw.decode("utf-8")))
    except (ValueError, zlib.error) as e:
        raise PackError(f"the code is cut short or damaged ({e})")


def packs_dir(tree):
    return os.path.join(tree.mods, ".packs")


def saved_packs(tree):
    d = packs_dir(tree)
    out = []
    for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        if f.endswith(".wsmodpack"):
            try:
                with open(os.path.join(d, f), encoding="utf-8") as fh:
                    p = validate(json.load(fh))
                p["id"] = f[:-len(".wsmodpack")]
                p["code"] = to_code({k: v for k, v in p.items() if k != "id"})
                out.append(p)
            except (OSError, ValueError, PackError):
                continue
    return out


def save_pack(tree, pack):
    pack = validate(dict(pack))
    pack.pop("id", None)
    pack.pop("code", None)
    os.makedirs(packs_dir(tree), exist_ok=True)
    pid = mods.safe_name(pack.get("name") or "modpack")
    with open(os.path.join(packs_dir(tree), pid + ".wsmodpack"), "w", encoding="utf-8") as f:
        json.dump(pack, f, indent=1, ensure_ascii=False)
    return pid


def delete_pack(tree, pid):
    p = os.path.join(packs_dir(tree), mods.safe_name(pid) + ".wsmodpack")
    if os.path.exists(p):
        os.remove(p)


def missing(tree, pack):
    """The pack's mods that aren't installed (by GameBanana id, else by folder name)."""
    state = tree.load()
    by_gb = {int(m["gamebanana"]): n for n, m in state["mods"].items() if m.get("gamebanana")}
    out = []
    for m in pack["mods"]:
        if m.get("gamebanana") and int(m["gamebanana"]) in by_gb:
            continue
        if m["name"] in state["mods"]:
            continue
        out.append(m)
    return out


def local_name(tree, entry):
    """The installed folder of a pack's mod (by GameBanana id first), or None."""
    state = tree.load()
    if entry.get("gamebanana"):
        for n, m in state["mods"].items():
            if m.get("gamebanana") and int(m["gamebanana"]) == int(entry["gamebanana"]):
                return n
    return entry["name"] if entry["name"] in state["mods"] else None


def use_pack(tree, pack, only=True):
    """Turn the pack's mods on, in its order, before the rest; with `only`, every other mod off.
    Returns the pack's mods that aren't installed (left out)."""
    state = tree.load()
    names, left = [], []
    for m in pack["mods"]:
        n = local_name(tree, m)
        if n is None:
            left.append(m)
            continue
        names.append(n)
        state["mods"][n]["enabled"] = bool(m.get("enabled", True))
    if only:
        for n in state["mods"]:
            if n not in names:
                state["mods"][n]["enabled"] = False
    state["order"] = names + [n for n in state["order"] if n not in names]
    tree.save(state)
    return left


# ---- online safety ---------------------------------------------------------------------------
# What makes a mod unsafe in public online lobbies ("not Wi-Fi safe"): anything that changes how
# the game plays - each player's game simulates the match, so one that differs desyncs, or gives
# its player an unfair edge - and costume slots the other player's game doesn't have.
UNSAFE = [
    (re.compile(r"^fighter/[^/]+/param/|^fighter/common/param/|^param/|^common/param/"),
     "changes fighter or battle params (damage, speed, hurtboxes)"),
    (re.compile(r"^fighter/[^/]+/motion/[^/]+/c\d\d/motion_list\.bin$"), "changes move timings (motion_list)"),
    (re.compile(r"^stage/(?!resultstage)[^/]+/.+(\.lvd|/param/.+\.(prc|stprm|stdat))$"), "changes stage collision or stage params"),
    (re.compile(r"^item/.+\.prc$|^item/[^/]+/param/"), "changes item params"),
    (re.compile(r"^assist/.+\.prc$|^pokemon/.+\.prc$"), "changes assist trophy or Pokémon params"),
]
CAUTION = [
    (re.compile(r"^fighter/[^/]+/model/body/c\d\d/model\.nusktb$"), "has its own skeleton (can shift hurtboxes slightly)"),
]
# More than a look: what plays out in the match or around it. Every player's game in an online room
# runs the same match and the same results screen, step by step, so all of them need these files
# the same - a player without them sees a different results screen or match, and it can hang
# (seen: a Joker mod's victory poses, camera and results stage left an online results screen stuck).
SAME = [
    (re.compile(r"^fighter/[^/]+/motion/[^/]+/c\d\d/j0\d[^/]*\.nuanmb$|^camera/fighter/"), "changes the victory poses or camera on the results screen"),
    (re.compile(r"^stage/resultstage"), "changes the results screen's stage"),
    (re.compile(r"^stage/(?!resultstage)"), "changes a stage"),
    (re.compile(r"^(prebuilt;/)?movie/"), "changes movies (a Final Smash or victory movie)"),
    (re.compile(r"^stream;/sound/bgm/bgm_z9"), "changes a victory theme"),
    (re.compile(r"^fighter/[^/]+/motion/body/c\d\d/.+\.nuanmb$"), "changes body animations (hurtboxes follow them)"),
]
# (animations that don't move hurtboxes in a match: faces, victory poses, entrances)
FACE_ANIM = re.compile(r"/(a00default|a00eyelid|a00face|j0\d|entry|.*(eye|mouth|face|blink).*)[^/]*\.nuanmb$")
_safety_cache = {}


def online_safety(folder):
    """{"level": "unsafe" | "same" | "caution" | "safe", "reasons": [...], "same": [...]} for an installed mod, from
    its files. "same": fine in public lobbies' terms (no gameplay edge) but more than a look, so every player in an
    online room needs it too; "same" also lists that for an unsafe mod."""
    try:
        stamp = max(os.path.getmtime(folder), os.path.getmtime(os.path.join(folder, "config.json"))
                    if os.path.exists(os.path.join(folder, "config.json")) else 0)
    except OSError:
        return {"level": "safe", "reasons": []}
    hit = _safety_cache.get(folder)
    if hit and hit[0] == stamp:
        return hit[1]
    unsafe, same, caution = [], [], []
    for path, _src in mods.mod_files(folder):
        stem, ext = os.path.splitext(path)
        if ext.lower() in (".prcx", ".prcxml") and not path.startswith("ui/"):
            unsafe.append("patches game params (" + os.path.basename(stem) + ")")
        for rx, why in UNSAFE:
            if rx.search(path):
                unsafe.append(why)
        for rx, why in SAME:
            if rx.search(path) and (why != SAME[-1][1] or not FACE_ANIM.search(path)):
                same.append(why)
                break
        for rx, why in CAUTION:
            if rx.search(path) and not FACE_ANIM.search(path):
                caution.append(why)
    for dp, _dirs, fs in os.walk(folder):
        if any(f.lower().endswith(".nro") for f in fs):
            unsafe.append("has a code plugin (movesets and gameplay changes)")
            break
    cfg = os.path.join(folder, "config.json")
    if os.path.exists(cfg):
        try:
            with open(cfg, encoding="utf-8-sig") as f:
                if json.load(f).get("new-dir-infos"):
                    unsafe.append("adds costume slots (c08 and up): a player who doesn't have them can crash")
        except (OSError, ValueError):
            pass
    reasons = list(dict.fromkeys(unsafe or same or caution))
    out = {"level": "unsafe" if unsafe else "same" if same else "caution" if caution else "safe", "reasons": reasons,
           "same": list(dict.fromkeys(same))}
    _safety_cache[folder] = (stamp, out)
    return out


# what a GameBanana page says about it (no files to look at before downloading)
SAYS_UNSAFE = re.compile(r"(not|isn'?t|non)[\s-]*(wi-?fi|online)[\s-]*(safe|friendly)|(wi-?fi|online)[\s-]*(unsafe|incompatible)"
                         r"|not\s+safe\s+(for|to\s+use|to\s+play)\s+(on\s*line|on\s+wi-?fi|wi-?fi)|desync", re.I)
SAYS_SAFE = re.compile(r"\b(wi-?fi|online)[\s-]*(safe|friendly|compatible)\b", re.I)
UNSAFE_CATEGORIES = {"Gameplay", "Skyline", "Custom Moveset Skins", "Movesets", "Moveset"}
CAUTION_CATEGORIES = {"Animation", "Final Smash", "Stages", "Modpacks"}


def listing_works(category="", text="", root_category=""):
    """Whether a GameBanana mod can work in WindowSmash at all (what's known before downloading it):
    None, or {"level": "no" | "partly", "text"}. Movesets and new characters do: their Skyline
    plugins are recompiled into the game when they're applied (tools/skyline.py), and once one is
    installed the clash check says if it isn't built in yet."""
    return None


def listing_safety(category="", text="", root_category=""):
    """The same verdict for a GameBanana listing: its category and what its author wrote."""
    if SAYS_UNSAFE.search(text or ""):
        return {"level": "unsafe", "reasons": ["its author says it isn't online safe"]}
    hit = next((c for c in (category, root_category) if c in UNSAFE_CATEGORIES), None)
    if hit:
        return {"level": "unsafe", "reasons": [f"it's in GameBanana's {hit} mods: it changes how the game plays"]}
    if re.search(r"\bmoveset\b", text or "", re.I) and not SAYS_SAFE.search(text or ""):
        return {"level": "unsafe", "reasons": ["a moveset mod: it changes how the game plays"]}
    if re.search(r"\b(c0?8|c09|c1\d|extra slots?|added slots?|new slots?)\b", text or "", re.I) and not SAYS_SAFE.search(text or ""):
        return {"level": "caution", "reasons": ["may add costume slots (c08 and up), which aren't online safe"]}
    if SAYS_SAFE.search(text or ""):
        return {"level": "safe", "reasons": ["its author says it's online safe"]}
    hit = next((c for c in (category, root_category) if c in CAUTION_CATEGORIES), None)
    if hit:
        return {"level": "caution", "reasons": [f"{hit} mods can change gameplay: check its page"]}
    return {"level": "unknown", "reasons": []}


# ---- the clash check -------------------------------------------------------------------------
def mod_summary(folder):
    """What a mod changes: arc paths, the costume slots it touches, its new slots, code plugins."""
    files = mods.mod_files(folder)
    slots, skin_slots, ui_slots, motion_slots, plugins = set(), set(), set(), set(), []
    size = 0
    for path, src in files:
        try:
            size += os.path.getsize(src)
        except OSError:
            pass
        m = SLOT.match(path)
        if m:
            k = KIRBY_COPY.match(path)   # (two mods' different copy hats don't mix: each is its own model)
            slot = ("kirby " + k.group(1) if k else m.group(1).split("/")[1], int(m.group(2)))
            slots.add(slot)
            if path.rsplit(".", 1)[-1].lower() in SKIN_FILES:
                skin_slots.add(slot)
            if MOTION_FILE.search(path):
                motion_slots.add(slot)
        m = UI_SLOT.match(path)
        if m:
            ui_slots.add((m.group(1), int(m.group(2))))
    plugins = mods.plugins(folder)
    new_dirs = []
    cfg = os.path.join(folder, "config.json")
    if os.path.exists(cfg):
        try:
            with open(cfg, encoding="utf-8-sig") as f:
                c = json.load(f)
            new_dirs = list(c.get("new-dir-infos", [])) + list(c.get("new-dir-infos-base", {}).keys())
        except (OSError, ValueError):
            new_dirs = None
    return {"files": [p for p, _ in files], "slots": slots, "skin_slots": skin_slots, "ui_slots": ui_slots,
            "motion_slots": motion_slots, "plugins": plugins,
            "new_dirs": new_dirs, "size": size}


def check(tree, names):
    """Clashes among mods `names` (in order: a later one wins), as a list of
    {"level": "error"|"warn"|"info", "title", "detail", "mods": [...]} - errors are what can crash
    the game or make a mod not load at all."""
    out = []
    sums = {}
    for n in names:
        folder = os.path.join(tree.mods, n)
        if not os.path.isdir(folder):
            out.append({"level": "error", "title": f"{n} isn't installed", "detail": "Install it first.", "mods": [n]})
            continue
        sums[n] = mod_summary(folder)
    # code plugins (Skyline .nro): recompiled into the game when applied; only one that isn't built in
    # yet doesn't run (a new plugin needs the game rebuilt: the Build plugins button)
    for n, s in sums.items():
        if not s["plugins"]:
            continue
        import skyline
        waiting = [p["file"] for p in skyline.plugin_state(tree.root, n, s["plugins"]) if not p["built"]]
        if waiting:
            out.append({"level": "warn", "title": f"{n}: its plugin isn't built into the game yet",
                        "detail": "Its " + ", ".join(waiting[:3]) + " (the new moves, or the new character) runs once it's "
                                  "compiled into the game: press Build plugins into the game (a few minutes). Until then only the mod's "
                                  "models, sounds and art load.",
                        "mods": [n]})
        if s["new_dirs"] is None:
            out.append({"level": "error", "title": f"{n}: its config.json can't be read",
                        "detail": "Its new costume slots won't load. Reinstall the mod.", "mods": [n]})
    # the select screen has room for 4 new characters (91 tiles): more need a bigger layout mod
    new, arranged = mods.new_characters(tree, list(sums))
    if len(new) > 4 and not arranged:
        out.append({"level": "info", "title": f"{len(new)} new characters, and the select screen has room for 4",
                    "detail": "Apply to game installs the CSS 91+ Layout Fix from GameBanana and turns it on: it makes room "
                              "for them all (without it the select screen breaks). New characters: " + ", ".join(new) + ".",
                    "mods": new, "fix": {"label": "Install the CSS 91+ Layout Fix", "gamebanana": mods.LAYOUT_FIX_ID,
                                                 "file": mods.LAYOUT_FIX_FILE}})
    # the same costume slot from two mods: a skin's model and textures mixed with another's can look
    # broken or crash a match; a mod that only changes physics or animation goes with any skin
    by_slot = {}
    for n, s in sums.items():
        for slot in s["slots"]:
            by_slot.setdefault(slot, []).append(n)
    clashes = {}
    for (fighter, c), ns in sorted(by_slot.items()):
        skins = [n for n in ns if (fighter, c) in sums[n]["skin_slots"]]
        if len(skins) > 1:
            clashes.setdefault((fighter, tuple(skins)), []).append(c)
        elif len(ns) > 1:
            clashes.setdefault((fighter, tuple(ns), "mix"), []).append(c)
    overlaps = {}
    for key, cs in clashes.items():
        fighter, ns = key[0], key[1]
        slots = ", ".join(f"c{c:02d}" for c in cs)
        moving = [n for n in ns if any((fighter, c) in sums[n]["motion_slots"] for c in cs)]
        dressing = [n for n in ns if n not in moving and any((fighter, c) in sums[n]["skin_slots"] for c in cs)]
        if len(key) == 3 and moving and dressing:   # (one warning for each pair of mods, every slot they share)
            pair = overlaps.setdefault((tuple(moving), tuple(dressing)), [])
            pair += [f"{fighter} c{c:02d}" for c in cs]
            continue
        if len(key) == 2:
            out.append({"level": "warn", "title": f"{' and '.join(ns)} are both skins for {fighter} {slots}",
                        "detail": f"Their models and textures would mix ({ns[-1]} wins where both have a file), which can "
                                  f"look broken or crash a match. Turn one off, or get a version of one for other costume slots.",
                        "mods": list(ns), "fighter": fighter, "slots": cs})
        else:
            out.append({"level": "info", "title": f"{' and '.join(ns)} both change {fighter} {slots}",
                        "detail": "One is a skin, the other changes physics, animation or effects: these usually go together. "
                                  f"{ns[-1]} wins where both have the same file.",
                        "mods": list(ns), "fighter": fighter, "slots": cs})
    for (moving, dressing), where in overlaps.items():
        out.append({"level": "warn", "title": f"{' and '.join(moving)} changes the physics or animations of costumes "
                                              f"{' and '.join(dressing)} reskin{'s' if len(dressing) == 1 else ''} ({', '.join(where)})",
                    "detail": f"{', '.join(moving)} was made for the game's own models. On a reskinned costume the bones it "
                              "moves may not be there or may be shaped differently: hair or clothes can stretch or clip, and "
                              "every overlapping file makes the fighter's load bigger. If a match with one of these costumes looks "
                              f"wrong or won't finish loading, turn {', '.join(moving)} off first.",
                    "mods": list(moving + dressing)})
    # two mods making the same new slot
    by_dir = {}
    for n, s in sums.items():
        for d in s["new_dirs"] or []:
            by_dir.setdefault(d.strip("/"), []).append(n)
    seen = set()
    for d, ns in sorted(by_dir.items()):
        if len(ns) > 1 and tuple(ns) not in seen:
            seen.add(tuple(ns))
            out.append({"level": "error", "title": f"{', '.join(ns)} both add {d}",
                        "detail": "Two mods made for the same new costume slot: only one can have it.", "mods": ns})
    # the same file from two mods (not a costume: UI art, sound, params); patches (.prcxml, .prcx, .xmsbt)
    # change a few entries each and all of them apply, one on top of the other: not a clash
    by_file = {}
    for n, s in sums.items():
        for p in s["files"]:
            if not SLOT.match(p) and os.path.splitext(p)[1].lower() not in mods.PATCHES:
                by_file.setdefault(p, []).append(n)
    pairs = {}
    for p, ns in by_file.items():
        if len(ns) > 1:
            pairs.setdefault(tuple(ns), []).append(p)
    for ns, ps in sorted(pairs.items()):
        out.append({"level": "info", "title": f"{', '.join(ns)} change the same {len(ps)} file{'s' if len(ps) > 1 else ''}",
                    "detail": f"{ns[-1]} wins (it's later in the order): " + ", ".join(ps[:4]) + (" ..." if len(ps) > 4 else ""),
                    "mods": list(ns)})
    # mods that aren't online safe: fine offline and with friends who have them, not in public lobbies
    for n in sums:
        o = online_safety(os.path.join(tree.mods, n))
        if o["level"] == "unsafe":
            out.append({"level": "online", "title": f"{n} isn't online safe",
                        "detail": "Don't use it in public online lobbies: " + "; ".join(o["reasons"]) +
                                  ". It's fine offline, and online with friends who have the same mods.", "mods": [n]})
        elif o["level"] == "same":
            out.append({"level": "online", "title": f"Online, everyone in the room needs {n} too",
                        "detail": "It changes more than the fighter's look: " + "; ".join(o["same"]) + ". Every player's game "
                                  "plays the same match and results screen in step, so a player without the same mod (same "
                                  "version, turned on) sees something else and the match or the results screen can hang. "
                                  "Offline it's fine.", "mods": [n]})
    total = sum(s["size"] for s in sums.values())
    if total > 6 * 1024 ** 3:
        out.append({"level": "warn", "title": f"{total / 1024 ** 3:.1f} GB of mods",
                    "detail": "Applying writes about as much again to the disk; make sure there's room.", "mods": list(sums)})
    order = {"error": 0, "online": 1, "warn": 2, "info": 3}
    out.sort(key=lambda x: order[x["level"]])
    return out
