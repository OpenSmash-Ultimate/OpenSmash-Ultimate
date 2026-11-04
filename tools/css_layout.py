#!/usr/bin/env python3
"""The character select screen for any number of fighters: the game's screen holds 91 tiles; this
makes its two files hold as many as are asked for, the tiles smaller the more there are.

Usage:
  css_layout.py make --tiles N --out <folder> [--romfs-cfg <romfs.cfg>] [--layout <layout.arc>]
      writes <folder>/ui/layout/menu/chara_select/chara_select/layout.arc and
      <folder>/ui/param/menu/chara_icon_arrangement.prc (a mod's folder: tools/mods.py does it by itself
      when the turned-on mods add more fighters than the screen holds)
  css_layout.py plan N        the rows and the tile size the screen gets for N fighters

How the game lays the screen out (main 13.0.5 0x71019eb840, plan/phases/N2_real_slots.md 3.2): for N
tiles it reads row N of a table in chara_icon_arrangement.prc (normal_layout_param, and
compe_layout_param for the tournament's screen): how many tiles each of up to 13 rows has, and a tile
size, which is an animation of the tile's own layout (window_size_00 .. _08) that sizes its panes; then
it places the tiles, one part pane "set_parts_chr_s_<k>" and one picture "set_parts_chr_bg_s_<k>"
each, in rows centred in the pane set_chr_area, a tile apart (the tile's pane "hit"). The game has rows
for N up to 91 and 91 tiles in its layout. Here:
  - the table gets rows 92 .. N, as many rows (13 at most) of as many tiles as make them biggest;
  - the tile's layout gets smaller sizes, window_size_09 .. _99 (SIZES of them): copies of window_size_07
    (the 91-tile size) with every pane's place, scale or size times a factor (FACTOR ** k), so a tile
    looks the same, smaller; layout.info lists them;
  - the screen's layout gets the tiles 91 .. N-1: copies of tile 0's two panes, each picture with a
    copy of its material (the game colours each tile's own).
The game's own code only walks tiles 0..90 when it sets the tiles up and hides the ones not used:
runtime/hle_select_screen.c does that for the others.

Nothing of the game is in this file: the files are made from the player's own copies of them."""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prc  # noqa: E402

LAYOUT = "ui/layout/menu/chara_select/chara_select/layout.arc"
ARRANGEMENT = "ui/param/menu/chara_icon_arrangement.prc"
MAIN = "blyt/chara_select.bflyt"
INFO = "info/layout.info"
PART = "chara_select_btn0_00"                   # the tile's own layout
SIZE_ANIM = "anim/" + PART + "_window_size_{:02d}.bflan"
GAME_TILES = 91
BASE_SIZE, FIRST_SIZE, SIZES, FACTOR = 7, 9, 91, 0.97   # sizes 09..99: 0.97 .. 0.97**91 (0.06) of size 07
CELL_W, CELL_H = 144.0, 82.0                    # size 07's tile ("hit"): 13 x 7 of them fill set_chr_area
NORMAL_COLS, NORMAL_ROWS = 13, 7                # what fits at size 07 (1868 x 572)
COMPE_W = 7 * 138.0                             # the tournament screen: 13 rows of 7 at size 08 (138 x 82)
MAX_ROWS = 13                                   # the game keeps 13 row counts (object + 0x1d4)
MAX_TILES = 1000                                # (they fit at the smallest size, the tournament screen too; chara_num a u16 past 255)
VERSION = 1                                     # (tools/mods.py makes the files again when this changes)
CHR, CHR_BG = "set_parts_chr_s_{:02d}", "set_parts_chr_bg_s_{:02d}"


class Error(Exception):
    pass


# ---- SARC (the layout archive) -------------------------------------------------------------------
def sarc_read(data):
    """{name: bytes} in the archive's data order, and each file's alignment."""
    if data[:4] != b"SARC":
        raise Error("not a SARC archive")
    hs, _bom, _size, doff, _ver = struct.unpack_from("<HHIIH", data, 4)
    if data[hs:hs + 4] != b"SFAT":
        raise Error("no SFAT section")
    _fh, n, _key = struct.unpack_from("<HHI", data, hs + 4)
    nodes = [struct.unpack_from("<IIII", data, hs + 12 + 16 * i) for i in range(n)]
    names_at = hs + 12 + 16 * n + 8
    files = []
    for _h, attr, s, e in nodes:
        no = names_at + (attr & 0xFFFFFF) * 4
        name = data[no:data.index(b"\0", no)].decode()
        at = doff + s
        align = 4
        while align < 0x2000 and at % (align * 2) == 0:
            align *= 2
        files.append((s, name, data[at:doff + e], align))
    files.sort()
    return [(name, body, align) for _s, name, body, align in files]


def sarc_hash(name, key=0x65):
    h = 0
    for ch in name.encode():
        h = (h * key + ch) & 0xFFFFFFFF
    return h


def sarc_write(files):
    """An archive of [(name, bytes, alignment)], the data in that order."""
    names = bytearray()
    name_at = {}
    for name, _b, _a in files:
        name_at[name] = len(names)
        names += name.encode() + b"\0"
        while len(names) % 4:
            names.append(0)
    nodes = sorted(files, key=lambda f: sarc_hash(f[0]))
    head = 0x14 + 12 + 16 * len(files) + 8 + len(names)
    big = max(a for _n, _b, a in files)
    doff = (head + big - 1) // big * big
    data, place = bytearray(), {}
    for name, body, align in files:
        while (doff + len(data)) % align:
            data.append(0)
        place[name] = (len(data), len(data) + len(body))
        data += body
    out = bytearray(b"SARC" + struct.pack("<HHIIHH", 0x14, 0xFEFF, doff + len(data), doff, 0x100, 0))
    out += b"SFAT" + struct.pack("<HHI", 12, len(files), 0x65)
    for name, _b, _a in nodes:
        s, e = place[name]
        out += struct.pack("<IIII", sarc_hash(name), 0x01000000 | name_at[name] // 4, s, e)
    out += b"SFNT" + struct.pack("<HH", 8, 0) + names
    out += b"\0" * (doff - len(out))
    return bytes(out + data)


# ---- BFLYT (the screen's layout) -----------------------------------------------------------------
PANES = (b"pan1", b"pic1", b"txt1", b"wnd1", b"bnd1", b"prt1", b"scr1", b"ali1")


def sections(lyt):
    if lyt[:4] != b"FLYT":
        raise Error("not a layout (FLYT)")
    hs, n = struct.unpack_from("<H", lyt, 6)[0], struct.unpack_from("<H", lyt, 0x10)[0]
    out, o = [], hs
    for _ in range(n):
        size = struct.unpack_from("<I", lyt, o + 4)[0]
        out.append(bytearray(lyt[o:o + size]))
        o += size
    return lyt[:hs], out


def pane_name(sec):
    return bytes(sec[12:36]).split(b"\0")[0].decode() if bytes(sec[:4]) in PANES else None


def set_name(sec, at, name, room):
    raw = name.encode()
    if len(raw) >= room:
        raise Error(f"name too long: {name}")
    sec[at:at + room] = raw + b"\0" * (room - len(raw))


def add_tiles(lyt, tiles):
    """The screen's layout with tiles 0..tiles-1 (copies of tile 0 after the last one it has)."""
    head, secs = sections(lyt)
    names = [pane_name(s) for s in secs]
    have = 0
    while CHR.format(have) in names and CHR_BG.format(have) in names:
        have += 1
    if have < 1:
        raise Error("the layout has no tile panes (set_parts_chr_s_00)")
    if tiles <= have:
        return lyt, have
    mat_i = next(i for i, s in enumerate(secs) if bytes(s[:4]) == b"mat1")
    mat = secs[mat_i]
    count = struct.unpack_from("<H", mat, 8)[0]
    offs = list(struct.unpack_from(f"<{count}I", mat, 12)) + [len(mat)]
    bodies = [bytes(mat[offs[i]:offs[i + 1]]) for i in range(count)]
    part = secs[names.index(CHR.format(0))]
    pic = secs[names.index(CHR_BG.format(0))]
    pic_mat = struct.unpack_from("<H", pic, 0x54 + 16)[0]
    new_pics, new_parts = [], []
    for k in range(have, tiles):
        p = bytearray(part)
        set_name(p, 12, CHR.format(k), 24)
        new_parts.append(p)
        q = bytearray(pic)
        set_name(q, 12, CHR_BG.format(k), 24)
        struct.pack_into("<H", q, 0x54 + 16, len(bodies))
        new_pics.append(q)
        m = bytearray(bodies[pic_mat])
        set_name(m, 0, CHR_BG.format(k), 28)
        bodies.append(bytes(m))
    if len(bodies) > 0xFFFF:
        raise Error("too many materials")
    table = 12 + 4 * len(bodies)
    new_mat = bytearray(mat[:8]) + struct.pack("<HH", len(bodies), 0)
    pos, offsets = table, []
    for b in bodies:
        offsets.append(pos)
        pos += len(b)
    new_mat += struct.pack(f"<{len(bodies)}I", *offsets) + b"".join(bodies)
    struct.pack_into("<I", new_mat, 4, len(new_mat))
    secs[mat_i] = new_mat
    last_part, last_pic = names.index(CHR.format(have - 1)), names.index(CHR_BG.format(have - 1))
    out = []
    for i, s in enumerate(secs):
        out.append(s)
        if i == last_part:
            out += new_parts
        if i == last_pic:
            out += new_pics
    body = b"".join(bytes(s) for s in out)
    hdr = bytearray(head)
    struct.pack_into("<I", hdr, 0x0C, len(hdr) + len(body))
    struct.pack_into("<H", hdr, 0x10, len(out))
    return bytes(hdr + body), have


# ---- BFLAN (the tile sizes) ----------------------------------------------------------------------
def scaled_size(anim, old, new, factor):
    """The size animation `old` (window_size_07) as `new`: every pane's place times `factor`, and its
    scale when the animation sets one, else its size."""
    a = bytearray(anim.replace(old.encode() + b"\0", new.encode() + b"\0"))
    if a[:4] != b"FLAN":
        raise Error("not an animation (FLAN)")
    hs, n = struct.unpack_from("<H", a, 6)[0], struct.unpack_from("<H", a, 0x10)[0]
    o = hs
    for _ in range(n):
        size = struct.unpack_from("<I", a, o + 4)[0]
        if a[o:o + 4] == b"pai1":
            ntex, ncont, contoff = struct.unpack_from("<HHI", a, o + 0x0C)
            for co in struct.unpack_from(f"<{ncont}I", a, o + contoff):
                c = o + co
                tags = struct.unpack_from(f"<{a[c + 28]}I", a, c + 32)
                curves = []
                for t in tags:
                    to = c + t
                    if a[to:to + 4] != b"FLPA":
                        continue
                    for tt in struct.unpack_from(f"<{a[to + 4]}I", a, to + 8):
                        curves.append(to + tt)
                scaled = {a[k + 1] for k in curves} & {6, 7}
                for k in curves:
                    target, kind = a[k + 1], a[k + 2]
                    if kind != 2 or not (target in (0, 1, 6, 7) or (target in (8, 9) and not scaled)):
                        continue
                    keys, at = struct.unpack_from("<H", a, k + 4)[0], k + struct.unpack_from("<I", a, k + 8)[0]
                    for j in range(keys):
                        f, v, s = struct.unpack_from("<3f", a, at + 12 * j)
                        struct.pack_into("<3f", a, at + 12 * j, f, v * factor, s * factor)
        o += size
    return bytes(a)


def add_info_names(info, names):
    """layout.info (the layouts and their animations, as names) with `names` added to the tile's."""
    parts = info.split(b"\0")
    at = parts.index(PART.encode()) + 1
    have = set(parts)
    new = [n.encode() for n in names if n.encode() not in have]
    return b"\0".join(parts[:at] + new + parts[at:])


def size_factor(size):
    return FACTOR ** (size - FIRST_SIZE + 1) if size >= FIRST_SIZE else 1.0


def pick_size(factor):
    """The biggest size of ours no bigger than `factor` of size 07."""
    for size in range(FIRST_SIZE, FIRST_SIZE + SIZES):
        if size_factor(size) <= factor + 1e-9:
            return size
    return FIRST_SIZE + SIZES - 1


# ---- the table ---------------------------------------------------------------------------------
def plan(n, compe=False):
    """(tiles per row, size) for n tiles past 91: the rows that make the tiles biggest."""
    best = None
    for rows in ([MAX_ROWS] if compe else range(1, MAX_ROWS + 1)):
        cols = -(-n // rows)
        used = -(-n // cols)
        if compe:
            factor = min(COMPE_W / (CELL_W * cols), 1.0)
        else:
            factor = min(NORMAL_COLS / cols, NORMAL_ROWS / used)
        if best is None or factor > best[0] + 1e-9:
            best = (factor, cols, used)
    factor, cols, used = best
    counts = [cols] * (used - 1) + [n - cols * (used - 1)]
    return counts, pick_size(factor)


KEYS = {name: prc.hash40(name) for name in
        ["chara_num", "icon_size", "margin_x", "margin_y", "offset"] + [f"row{r:02d}_num" for r in range(MAX_ROWS)]}


def add_rows(data, tiles):
    """chara_icon_arrangement.prc with rows for 1..tiles in both tables (the game's kept as they are)."""
    kind, root = prc.read(data)
    out = prc.Struct()
    for key, (lkind, rows) in root:
        if lkind != prc.LIST or not rows:
            out.append((key, (lkind, rows)))
            continue
        compe = key == prc.hash40("compe_layout_param")
        rows = prc.List(rows)
        template = rows[-1][1]
        fields = {k: t for k, (t, _v) in template}
        for n in range(len(rows) + 1, tiles + 1):
            counts, size = plan(n, compe)
            value = {KEYS["chara_num"]: n, KEYS["icon_size"]: size, KEYS["margin_x"]: 0, KEYS["margin_y"]: 0,
                     KEYS["offset"]: 1}
            for r in range(MAX_ROWS):
                value[KEYS[f"row{r:02d}_num"]] = counts[r] if r < len(counts) else 0
            row = prc.Struct()
            for k, t in fields.items():
                v = value.get(k, 0)
                if t == prc.U8 and v > 0xFF:
                    t = prc.U16
                row.append((k, (t, v)))
            rows.append((prc.STRUCT, row))
        out.append((key, (prc.LIST, rows)))
    return prc.write((kind, out))


# ---- the two files -----------------------------------------------------------------------------
def make_layout(arc, tiles):
    files = sarc_read(arc)
    by = {name: i for i, (name, _b, _a) in enumerate(files)}
    for need in (MAIN, INFO, SIZE_ANIM.format(BASE_SIZE)):
        if need not in by:
            raise Error(f"the layout archive has no {need}")
    name, body, align = files[by[MAIN]]
    files[by[MAIN]] = (name, add_tiles(body, tiles)[0], align)
    base = files[by[SIZE_ANIM.format(BASE_SIZE)]][1]
    names = []
    for size in range(FIRST_SIZE, FIRST_SIZE + SIZES):
        anim = SIZE_ANIM.format(size)
        tag = f"window_size_{size:02d}"
        names.append(tag)
        body = scaled_size(base, f"window_size_{BASE_SIZE:02d}", tag, size_factor(size))
        if anim in by:
            files[by[anim]] = (anim, body, files[by[anim]][2])
        else:
            files.append((anim, body, 16))
    name, body, align = files[by[INFO]]
    files[by[INFO]] = (name, add_info_names(body, names), align)
    return sarc_write(files)


def make(tiles, layout, arrangement):
    """(layout.arc, chara_icon_arrangement.prc) for `tiles` tiles, from the game's (or a mod's) two."""
    if not GAME_TILES < tiles <= MAX_TILES:
        raise Error(f"{tiles} tiles: {GAME_TILES + 1} to {MAX_TILES}")
    return make_layout(layout, tiles), add_rows(arrangement, tiles)


def write_mod(folder, tiles, layout, arrangement):
    lay, arr = make(tiles, layout, arrangement)
    for path, data in ((LAYOUT, lay), (ARRANGEMENT, arr)):
        out = os.path.join(folder, *path.split("/"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out + ".tmp", "wb") as f:
            f.write(data)
        os.replace(out + ".tmp", out)
    return [LAYOUT, ARRANGEMENT]


def game_files(romfs_cfg):
    import arc as arcmod
    from nca import RomfsImage, romfs_entries
    image = RomfsImage(romfs_cfg)
    try:
        at = next(o for p, o, s in romfs_entries(image) if p == "data.arc")
        a = arcmod.Arc(lambda off, n: image.read(at + off, n))
        return a.get(LAYOUT), a.get(ARRANGEMENT)
    finally:
        getattr(image, "close", lambda: None)()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("--tiles", type=int, required=True)
    m.add_argument("--out", required=True)
    m.add_argument("--romfs-cfg", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                       "dump", "modules", "romfs.cfg"))
    m.add_argument("--layout", help="a layout.arc to start from (a mod's), instead of the game's")
    p = sub.add_parser("plan")
    p.add_argument("n", type=int)
    args = ap.parse_args()
    if args.cmd == "plan":
        for compe in (False, True):
            counts, size = plan(args.n, compe)
            print(f"{'tournament' if compe else 'normal'}: rows {counts}, size {size} ({size_factor(size):.2f} of the 91-tile size)")
        return
    layout, arrangement = game_files(args.romfs_cfg)
    if args.layout:
        with open(args.layout, "rb") as f:
            layout = f.read()
    for path in write_mod(args.out, args.tiles, layout, arrangement):
        print(os.path.join(args.out, *path.split("/")))


if __name__ == "__main__":
    try:
        main()
    except Error as e:
        sys.exit(f"css_layout: {e}")
