#!/usr/bin/env python3
"""HDR's motion list patches, applied ahead of time.

HDR changes moves' frame data and adds new moves (Ridley's aerial down special "pogo", Dark Samus'
new up special motions) with motion_patch.yaml files beside a fighter's motion folders
(fighter/ridley/motion/body/motion_patch.yaml). On the console its plugin's motion patcher
(github.com/WuBoytH/runtime-motion-patcher) registers an ARCropolis file callback for each costume's
motion_list.bin and patches the game's own list as it loads. That path doesn't run here (no callback
of it reaches the game's file reads), so a move the patch adds has no motion: the fighter T-poses.

So this does the same work when HDR is installed or turned on: each motion_patch.yaml is applied to the
game's own motion_list.bin (the vanilla c00 one, as the patcher's cache does for every costume) and the
result written for c00-c07 and any other costume folder HDR's mods have there, into the mods folder
hdr-motion, which goes right after HDR's three mods in the order (and is on exactly when they are).
The rules are motion-patch's (github.com/WuBoytH/motion-patch): a field given replaces the motion's,
flags and extra field by field, animations and scripts whole; a motion not in the list is added; the
list ends sorted by its hash40s. The file format is motion_lib's (github.com/ultimate-research/motion_lib).

Usage:
  hdr_motion.py bake [--tree <dir>]                 (re)write mods/hdr-motion
  hdr_motion.py show <motion_list.bin> [names...]   print a list's motions (or the named ones)
"""
import hashlib
import io
import json
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

FOLDER = "hdr-motion"          # (the mods folder written here)
VERSION = 1                    # (bump when what bake writes changes: an older bake is done again)
KINDS = ("fighter", "assist", "boss", "item")   # (the patcher: fighter/*/motion/*/, the others' motion/body only)
COSTUMES = tuple(f"c{i:02d}" for i in range(8))
FLAGS = ("turn", "loop", "move", "fix_trans", "fix_rot", "fix_scale", "unk_40", "unk_80", "unk_100", "unk_200",
         "unk_400", "unk_800", "unk_1000", "unk_2000")
EXTRA = (("xlu_start", ("intangible_start_frame", "xlu_start")), ("xlu_end", ("intangible_end_frame", "xlu_end")),
         ("cancel_frame", ("cancel_frame",)), ("no_stop_intp", ("freeze_during_hitstop", "no_stop_intp")))


class Error(Exception):
    pass


def hash40(text):
    """hash40 crate's label hash: crc32 of the bytes, the length above them."""
    b = text.encode()
    return zlib.crc32(b) | (len(b) << 32)


MAGIC = hash40("motion")
LOW40 = (1 << 40) - 1


def h40(value):
    """A hash40 as motion-patch's YAML has it: a label, or 0x and its hex."""
    if isinstance(value, bool):
        raise Error(f"{value!r} isn't a hash40")
    if isinstance(value, int):
        return value
    s = str(value)
    if s[:2].lower() == "0x":
        return int(s, 16)
    return hash40(s)


# ---- motion_list.bin --------------------------------------------------------------------------------------

def read(data):
    """(motion_path, {motion kind: motion}) of a motion_list.bin, in its order. A motion is a dict:
    game_script, flags (int), blend_frames, animations [(name, unk)], scripts [hash], extra (4 values) or None."""
    if len(data) < 24 or struct.unpack_from("<Q", data, 0)[0] != MAGIC:
        raise Error("not a motion_list.bin (its header)")
    path, count = struct.unpack_from("<QQ", data, 8)
    o, out = 24, {}
    try:
        for _ in range(count):
            kind, script, flags, blend, anims, size = struct.unpack_from("<QQHBBI", data, o)
            o += 24
            if anims > 3:
                raise Error("a motion with more than 3 animations")
            names = struct.unpack_from(f"<{anims}Q", data, o)
            o += 8 * anims
            unks = data[o:o + anims]
            o = (o + anims + 3) & ~3
            scripts = list(struct.unpack_from(f"<{size // 8}Q", data, o))
            o += 8 * (size // 8)
            extra = None
            if size % 8 == 4:
                extra = (data[o], data[o + 1], data[o + 2], data[o + 3] > 0)
                o += 4
            out[kind] = {"game_script": script, "flags": flags, "blend_frames": blend,
                         "animations": list(zip(names, unks)), "scripts": scripts, "extra": extra}
    except struct.error:
        raise Error("a motion_list.bin cut short") from None
    return path, out


def write(path, motions):
    f = io.BytesIO()
    f.write(struct.pack("<QQQ", MAGIC, path, len(motions)))
    for kind, m in motions.items():
        anims = m["animations"]
        if len(anims) > 3:
            raise Error(f"motion {kind:#x}: more than 3 animations")
        size = 8 * len(m["scripts"]) + (4 if m["extra"] is not None else 0)
        f.write(struct.pack("<QQHBBI", kind, m["game_script"], m["flags"] & 0xFFFF, m["blend_frames"] & 0xFF,
                            len(anims), size))
        for name, _ in anims:
            f.write(struct.pack("<Q", name))
        f.write(bytes(u & 0xFF for _, u in anims))
        f.write(b"\0" * (-f.tell() % 4))
        for s in m["scripts"]:
            f.write(struct.pack("<Q", s))
        if m["extra"] is not None:
            a, b, c, d = m["extra"]
            f.write(bytes((a & 0xFF, b & 0xFF, c & 0xFF, 1 if d else 0)))
    return f.getvalue()


def default_motion():
    return {"game_script": 0, "flags": 0, "blend_frames": 0, "animations": [], "scripts": [], "extra": None}


# ---- motion_patch.yaml ----------------------------------------------------------------------------------

def _scalar(text):
    t = text.strip()
    if len(t) >= 2 and t[0] == t[-1] and t[0] in "'\"":
        return t[1:-1]
    low = t.lower()
    if low in ("true", "false"):
        return low == "true"
    if low in ("~", "null", ""):
        return None
    if re.fullmatch(r"[-+]?\d+", t):
        return int(t)
    return t


def _strip(line):
    """A line without its comment (a # at the start or after a space, outside quotes)."""
    quote = None
    for i, ch in enumerate(line):
        if quote:
            if ch == quote:
                quote = None
        elif ch in "'\"":
            quote = ch
        elif ch == "#" and (i == 0 or line[i - 1] in " \t"):
            return line[:i].rstrip()
    return line.rstrip()


def parse_yaml(text):
    """The YAML motion-patch files are written in: block maps, block lists (of scalars, or of maps starting
    on the dash's line), plain or quoted scalars, comments, [] and {} for empty ones. Not general YAML."""
    lines = []
    for raw in text.replace("\t", "    ").splitlines():
        s = _strip(raw)
        if s.strip() and s.strip() != "---":
            lines.append((len(s) - len(s.lstrip(" ")), s.strip()))
    pos = [0]

    def value_of(rest, indent):
        if rest == "[]":
            return []
        if rest == "{}":
            return {}
        if rest:
            return _scalar(rest)
        if pos[0] < len(lines) and (lines[pos[0]][0] > indent or (lines[pos[0]][0] == indent and lines[pos[0]][1].startswith("- "))):
            return block(lines[pos[0]][0])
        return None

    def block(indent):
        if lines[pos[0]][1].startswith("-"):
            out = []
            while pos[0] < len(lines) and lines[pos[0]][0] == indent and (lines[pos[0]][1] == "-" or lines[pos[0]][1].startswith("- ")):
                item = lines[pos[0]][1][1:].strip()
                pos[0] += 1
                if not item:
                    out.append(value_of("", indent))
                elif re.match(r"^[^'\"\s][^:]*:(\s|$)", item):   # (a map starting on the dash's line)
                    inner = indent + 2
                    lines.insert(pos[0], (inner, item))
                    out.append(block(inner))
                else:
                    out.append(_scalar(item))
            return out
        out = {}
        while pos[0] < len(lines) and lines[pos[0]][0] == indent and not lines[pos[0]][1].startswith("- "):
            key, sep, rest = lines[pos[0]][1].partition(":")
            if not sep:
                raise Error(f"line {lines[pos[0]][1]!r}: not a key")
            pos[0] += 1
            key = _scalar(key)
            out.pop(key, None)   # (a key twice: the last one, as serde_yaml's maps and PyYAML have it - HDR's files do that)
            out[key] = value_of(rest.strip(), indent)
        return out

    if not lines:
        return {}
    result = block(lines[0][0])
    if pos[0] != len(lines):
        raise Error(f"line {lines[pos[0]][1]!r}: not where it fits")
    return result


def load_patch(text):
    """{motion kind hash: patch dict} of a motion_patch.yaml."""
    doc = parse_yaml(text)
    if not isinstance(doc, dict):
        raise Error("not a map of motions")
    return {h40(k): (v if isinstance(v, dict) else {}) for k, v in doc.items()}


def apply_one(p, m):
    """motion-patch's MotionPatch::apply."""
    if p.get("game_script") is not None:
        m["game_script"] = h40(p["game_script"])
    flags = p.get("flags")
    if isinstance(flags, dict):
        for bit, name in enumerate(FLAGS):
            v = flags.get(name)
            if v is not None:
                m["flags"] = (m["flags"] | (1 << bit)) if v else (m["flags"] & ~(1 << bit))
    if p.get("blend_frames") is not None:
        m["blend_frames"] = int(p["blend_frames"])
    if p.get("animations") is not None:
        m["animations"] = [(h40(a["name"]), int(a.get("unk") or 0)) for a in p["animations"]]
    if p.get("scripts") is not None:
        m["scripts"] = [h40(s) for s in p["scripts"]]
    extra = p.get("extra")
    if isinstance(extra, dict):
        cur = list(m["extra"] or (0, 0, 0, False))
        for i, (_field, names) in enumerate(EXTRA):
            for n in names:
                if extra.get(n) is not None:
                    cur[i] = bool(extra[n]) if i == 3 else int(extra[n])
                    break
        m["extra"] = tuple(cur)


def apply(motions, patch):
    """motion-patch's MotionMapExt::apply: in the patch's hash order; a removed motion comes back as a
    default one unless renamed (as there); the list sorted by hash40 at the end."""
    out = dict(motions)
    for kind in sorted(patch):
        p = patch[kind]
        if p.get("remove"):
            out.pop(kind, None)
        if p.get("rename") is not None:
            m = out.pop(kind, None) or default_motion()
            m = dict(m)
            apply_one(p, m)
            out[h40(p["rename"])] = m
        else:
            m = dict(out.get(kind) or default_motion())
            apply_one(p, m)
            out[kind] = m
    return {k: out[k] for k in sorted(out)}


# ---- baking ----------------------------------------------------------------------------------------------

def patches(mods_dir, names):
    """{motion folder (fighter/ridley/motion/body): path of its motion_patch.yaml} in HDR's mods (a later mod's
    over an earlier one's), and {that folder: costume folders the mods have there}."""
    found, costumes = {}, {}
    for name in names:
        root = os.path.join(mods_dir, name)
        for kind in KINDS:
            base = os.path.join(root, kind)
            if not os.path.isdir(base):
                continue
            for who in sorted(os.listdir(base)):
                motion = os.path.join(base, who, "motion")
                if not os.path.isdir(motion):
                    continue
                for part in sorted(os.listdir(motion)):
                    if kind != "fighter" and part != "body":
                        continue
                    y = os.path.join(motion, part, "motion_patch.yaml")
                    if os.path.isfile(y):
                        found[f"{kind}/{who}/motion/{part}"] = y
    for name in names:   # (costume folders: every mod's, a patch from any of them)
        for rel in found:
            d = os.path.join(mods_dir, name, *rel.split("/"))
            if os.path.isdir(d):
                for c in os.listdir(d):
                    if os.path.isdir(os.path.join(d, c)):
                        costumes.setdefault(rel, set()).add(c.lower())
    return found, costumes


def stamp(mods_dir, names):
    h = hashlib.sha1(f"v{VERSION}".encode())
    found, costumes = patches(mods_dir, names)
    for rel in sorted(found):
        h.update(rel.encode() + b"\0")
        with open(found[rel], "rb") as f:
            h.update(hashlib.sha1(f.read()).digest())
        h.update(",".join(sorted(costumes.get(rel, ()))).encode())
    return h.hexdigest()


def record_path(tree):
    return os.path.join(tree.root, "dump", "hdr", "motion.json")


def current(tree, names):
    """Whether mods/hdr-motion is what bake writes for HDR's mods as they are."""
    try:
        with open(record_path(tree), encoding="utf-8") as f:
            rec = json.load(f)
    except (OSError, ValueError):
        return False
    there = os.path.isdir(os.path.join(tree.mods, FOLDER)) or not rec.get("lists")
    return there and rec.get("stamp") == stamp(tree.mods, names)


def game_files(tree):
    """get(path) -> bytes or None over the game's own data.arc (mods/... romfs_vanilla.cfg, else romfs.cfg)."""
    import mods
    cfg = tree.vanilla_cfg if os.path.exists(tree.vanilla_cfg) else tree.cfg
    if not os.path.exists(cfg):
        raise Error("the game isn't set up here yet (no dump/modules/romfs.cfg): build it first")
    image = mods.RomfsImage(cfg)
    arc_at = next(o for p, o, s in mods.romfs_entries(image) if p == "data.arc")
    game = mods.Arc(lambda off, n: image.read(arc_at + off, n))
    return game.get


def bake(tree, names, log=print, get=None):
    """mods/hdr-motion from HDR's motion_patch.yaml files (`names`: HDR's mods, in their order). Returns
    how many motion lists were written."""
    import shutil
    found, costumes = patches(tree.mods, names)
    out = os.path.join(tree.mods, FOLDER)
    tmp = out + ".new"
    shutil.rmtree(tmp, ignore_errors=True)
    get = (get or game_files(tree)) if found else None
    written, skipped, added = 0, [], 0
    for rel, yaml_path in sorted(found.items()):
        with open(yaml_path, encoding="utf-8-sig", errors="replace") as f:
            try:
                patch = load_patch(f.read())
            except (Error, ValueError, KeyError, TypeError) as e:
                skipped.append(f"{rel} ({e})")
                continue
        base = None
        for name in reversed(names):   # (a list one of HDR's mods ships: that, as the console's ARCropolis serves it)
            p = os.path.join(tree.mods, name, *rel.split("/"), "c00", "motion_list.bin")
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    base = f.read()
                break
        if base is None:
            base = get(f"{rel}/c00/motion_list.bin")
        if base is None:
            skipped.append(f"{rel} (the game has no c00/motion_list.bin there)")
            continue
        try:
            path, motions = read(base)
            new = apply(motions, patch)
            data = write(path, new)
        except (Error, ValueError, KeyError, TypeError) as e:
            skipped.append(f"{rel} ({e})")
            continue
        added += len(set(new) - set(motions))
        for c in sorted(set(COSTUMES) | costumes.get(rel, set())):
            dest = os.path.join(tmp, *rel.split("/"), c, "motion_list.bin")
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(data)
            written += 1
    shutil.rmtree(out, ignore_errors=True)
    if written:
        os.replace(tmp, out)
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(os.path.dirname(record_path(tree)), exist_ok=True)
    with open(record_path(tree), "w", encoding="utf-8") as f:
        json.dump({"stamp": stamp(tree.mods, names), "lists": written, "patches": len(found),
                   "new_motions": added, "skipped": skipped}, f, indent=1)
    log(f"HDR's motion patches: {len(found) - len(skipped)} of {len(found)} applied ahead of time "
        f"({added} new moves), {written} motion lists in mods/{FOLDER}")
    for s in skipped:
        log(f"HDR motion patch left out: {s}")
    return written


def show(data, names=()):
    path, motions = read(data)
    want = {h40(n) for n in names}
    print(f"motion_path {path:#x}, {len(motions)} motions")
    for k, m in motions.items():
        if want and k not in want:
            continue
        print(f"{k:#012x}: script {m['game_script']:#x} flags {m['flags']:#06x} blend {m['blend_frames']} "
              f"anims {[(hex(a), u) for a, u in m['animations']]} scripts {[hex(s) for s in m['scripts']]} extra {m['extra']}")


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("command", choices=("bake", "show"))
    ap.add_argument("args", nargs="*")
    ap.add_argument("--tree", default=os.path.dirname(HERE))
    a = ap.parse_args()
    if a.command == "show":
        with open(a.args[0], "rb") as f:
            show(f.read(), a.args[1:])
    else:
        import mods as _mods
        import hdr
        sys.exit(0 if hdr.bake_motion(_mods.Tree(a.tree), force=True) is not None else 1)
