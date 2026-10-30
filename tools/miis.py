#!/usr/bin/env python3
"""Your own Miis in the game: imports them into miis/, where the game reads them (runtime/ipc_mii.c)
as the console's Mii database. They then show up wherever the game lists Miis, such as Games & More
-> Mii Fighters, after the six default ones.

Usage:
  miis.py list
  miis.py import [<source> ...] [--name <name>] [--ask]
  miis.py remove <name> ...

import, without a source: the Mii database of every emulator found on this computer (Ryujinx, yuzu
and its forks: Eden, Citron, Sudachi, Suyu, Torzu). Make Miis there with the Mii editor (Ryujinx's
Mii Edit Applet, yuzu's Open Mii Editor, in their menus), then import them. A source can also be:
  - an emulator's MiiDatabase.dat (a console's Mii database: "NFDB", up to 100 Miis)
  - a Mii Studio link (https://studio.mii.nintendo.com/miis/image.png?data=...: the address of a
    Mii's picture on Mii Studio, or any site that uses the same data), or its data alone (hex)
  - a Switch Mii file: CharInfo (.charinfo, 0x58 bytes), StoreData (.nfsd, 0x44) or CoreData
    (.nfcd, 0x30)
  - a 3DS / Wii U Mii file (.ffsd, .cfsd, .bin: Ver3StoreData, 0x5C or 0x60 bytes; not a QR code)
--name: the name for Miis that have none (Mii Studio's), up to 10 characters (default "Mii").
--ask: also ask for Mii Studio links, one at a time (Import Miis.bat / Import Miis.command).

Each Mii is one file, miis/<its name>.charinfo, which you can give to friends: put it in their
miis folder. Importing a Mii again (the same Mii: its id) replaces its file. The game reads miis/
when it starts, up to 100 Miis, in the files' name order. A Mii Fighter made from one of your Miis
is kept in the game's save, so it stays when the file is removed.

The formats: the Switch's CoreData packs a Mii into seven 32-bit words of bit fields and its name
(StoreData adds the Mii's id and two checksums); CharInfo, what the game reads, has one byte per
part. Their layout and value ranges are those the console's Mii service uses (as the emulators
implement it). The 3DS / Wii U Ver3StoreData has its own bit fields and a smaller palette of
colours (mapped as the Switch's conversion does), and Mii Studio's data is one byte per part,
obfuscated (each byte added to 7 after XOR with the one before)."""
import argparse
import glob
import hashlib
import os
import re
import struct
import sys
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MIIS = os.path.join(ROOT, "miis")
EXT = ".charinfo"
DATABASE_MAX = 100

# CharInfo's parts after the id (16 bytes) and the name (11 UTF-16 characters, the last 0), one byte
# each, in order, with the largest value the console accepts
PARTS = [
    ("font_region", 3), ("favorite_color", 11), ("gender", 1), ("height", 127), ("build", 127),
    ("type", 1), ("region_move", 3), ("faceline_type", 11), ("faceline_color", 9),
    ("faceline_wrinkle", 11), ("faceline_make", 11), ("hair_type", 131), ("hair_color", 99),
    ("hair_flip", 1), ("eye_type", 59), ("eye_color", 99), ("eye_scale", 7), ("eye_aspect", 6),
    ("eye_rotate", 7), ("eye_x", 12), ("eye_y", 18), ("eyebrow_type", 23), ("eyebrow_color", 99),
    ("eyebrow_scale", 8), ("eyebrow_aspect", 6), ("eyebrow_rotate", 11), ("eyebrow_x", 12),
    ("eyebrow_y", 18), ("nose_type", 17), ("nose_scale", 8), ("nose_y", 18), ("mouth_type", 35),
    ("mouth_color", 99), ("mouth_scale", 8), ("mouth_aspect", 6), ("mouth_y", 18),
    ("beard_color", 99), ("beard_type", 5), ("mustache_type", 5), ("mustache_scale", 8),
    ("mustache_y", 16), ("glass_type", 19), ("glass_color", 99), ("glass_scale", 7), ("glass_y", 20),
    ("mole_type", 1), ("mole_scale", 8), ("mole_x", 16), ("mole_y", 30),
]
CHARINFO_SIZE = 0x58
NAME_CHARS = 10

# CoreData's bit fields: part -> (word, first bit, bits). eyebrow_y is stored 3 lower than CharInfo's.
CORE_BITS = {
    "hair_type": (0, 0, 8), "height": (0, 8, 7), "mole_type": (0, 15, 1), "build": (0, 16, 7),
    "hair_flip": (0, 23, 1), "hair_color": (0, 24, 7), "type": (0, 31, 1),
    "eye_color": (1, 0, 7), "gender": (1, 7, 1), "eyebrow_color": (1, 8, 7), "mouth_color": (1, 16, 7),
    "beard_color": (1, 24, 7),
    "glass_color": (2, 0, 7), "eye_type": (2, 8, 6), "region_move": (2, 14, 2), "mouth_type": (2, 16, 6),
    "font_region": (2, 22, 2), "eye_y": (2, 24, 5), "glass_scale": (2, 29, 3),
    "eyebrow_type": (3, 0, 5), "mustache_type": (3, 5, 3), "nose_type": (3, 8, 5), "beard_type": (3, 13, 3),
    "nose_y": (3, 16, 5), "mouth_aspect": (3, 21, 3), "mouth_y": (3, 24, 5), "eyebrow_aspect": (3, 29, 3),
    "mustache_y": (4, 0, 5), "eye_rotate": (4, 5, 3), "glass_y": (4, 8, 5), "eye_aspect": (4, 13, 3),
    "mole_x": (4, 16, 5), "eye_scale": (4, 21, 3), "mole_y": (4, 24, 5),
    "glass_type": (5, 0, 5), "favorite_color": (5, 8, 4), "faceline_type": (5, 12, 4),
    "faceline_color": (5, 16, 4), "faceline_wrinkle": (5, 20, 4), "faceline_make": (5, 24, 4), "eye_x": (5, 28, 4),
    "eyebrow_scale": (6, 0, 4), "eyebrow_rotate": (6, 4, 4), "eyebrow_x": (6, 8, 4), "eyebrow_y": (6, 12, 4),
    "nose_scale": (6, 16, 4), "mouth_scale": (6, 20, 4), "mustache_scale": (6, 24, 4), "mole_scale": (6, 28, 4),
}
CORE_SIZE, STORE_SIZE, STORE_ELEMENT_SIZE = 0x30, 0x44, 0x48
NFDB_MAGIC, NFDB_SIZE = b"NFDB", 0x1A98

# Mii Studio's data: its parts in order (46 bytes), with CharInfo's values
STUDIO_PARTS = [
    "beard_color", "beard_type", "build", "eye_aspect", "eye_color", "eye_rotate", "eye_scale",
    "eye_type", "eye_x", "eye_y", "eyebrow_aspect", "eyebrow_color", "eyebrow_rotate", "eyebrow_scale",
    "eyebrow_type", "eyebrow_x", "eyebrow_y", "faceline_color", "faceline_make", "faceline_type",
    "faceline_wrinkle", "favorite_color", "gender", "glass_color", "glass_scale", "glass_type", "glass_y",
    "hair_color", "hair_flip", "hair_type", "height", "mole_scale", "mole_type", "mole_x", "mole_y",
    "mouth_aspect", "mouth_color", "mouth_scale", "mouth_type", "mouth_y", "mustache_scale",
    "mustache_type", "mustache_y", "nose_scale", "nose_type", "nose_y",
]

# Ver3StoreData (3DS / Wii U): colours by the Switch's conversion tables
VER3_FACELINE_COLOR = [0, 1, 2, 3, 4, 5]
VER3_HAIR_COLOR = [8, 1, 2, 3, 4, 5, 6, 7]
VER3_EYE_COLOR = [8, 9, 10, 11, 12, 13]
VER3_MOUTH_COLOR = [19, 20, 21, 22, 23]
VER3_GLASS_COLOR = [8, 14, 15, 16, 17, 18, 0]


class Bad(Exception):
    pass


class Mii:
    """A CharInfo: create_id (16 bytes), name (str) and the parts (dict)."""

    def __init__(self, create_id, name, parts):
        self.create_id, self.name, self.parts = create_id, name, parts

    @classmethod
    def from_charinfo(cls, b):
        if len(b) != CHARINFO_SIZE:
            raise Bad(f"{len(b)} bytes, not a CharInfo's {CHARINFO_SIZE}")
        units = struct.unpack_from("<11H", b, 16)
        return cls(b[:16], name_from_units(units), {k: b[0x26 + i] for i, (k, _) in enumerate(PARTS)})

    def charinfo(self):
        units = [ord(c) for c in self.name[:NAME_CHARS]]
        units += [0] * (11 - len(units))
        return (self.create_id + struct.pack("<11H", *units)
                + bytes(self.parts[k] for k, _ in PARTS) + b"\0")

    def check(self):
        """Why the game can't use it, or None."""
        if not any(self.create_id):
            return "it has no id"
        if not self.name:
            return "it has no name"
        if any(ord(c) > 0xFFFF for c in self.name):
            return "its name has characters the Switch can't show"
        for k, top in PARTS:
            if not 0 <= self.parts.get(k, -1) <= top:
                return f"its {k.replace('_', ' ')} ({self.parts.get(k)}) is out of range (0-{top})"
        if self.parts["eyebrow_y"] < 3:
            return f"its eyebrow y ({self.parts['eyebrow_y']}) is out of range (3-18)"
        return None


def name_from_units(units):
    out = []
    for u in units:
        if not u:
            break
        out.append(chr(u))
    return "".join(out)


def made_id(*data):
    """An id for a Mii that came without one: the same Mii gets the same id (a version-4 UUID's form)."""
    h = bytearray(hashlib.sha256(b"WindowSmash Mii".join(data)).digest()[:16])
    h[6] = (h[6] & 0x0F) | 0x40
    h[8] = (h[8] & 0x3F) | 0x80
    return bytes(h)


# ---- the formats ---------------------------------------------------------------------------------

def from_core(b, create_id=None):
    words = struct.unpack_from("<7I", b, 0)
    parts = {k: (words[w] >> shift) & ((1 << n) - 1) for k, (w, shift, n) in CORE_BITS.items()}
    parts["eyebrow_y"] += 3
    name = name_from_units(struct.unpack_from("<10H", b, 0x1C))
    return Mii(create_id or made_id(bytes(b[:CORE_SIZE])), name, parts)


def from_store(b):
    return from_core(b[:CORE_SIZE], bytes(b[CORE_SIZE:CORE_SIZE + 16]) or None)


def from_nfdb(b):
    """A console's Mii database (MiiDatabase.dat): 'NFDB', 100 StoreData, version, count, CRC."""
    count = b[4 + DATABASE_MAX * STORE_SIZE + 1]
    if count > DATABASE_MAX:
        raise Bad(f"it says it holds {count} Miis (at most {DATABASE_MAX})")
    return [from_store(b[4 + i * STORE_SIZE:4 + (i + 1) * STORE_SIZE]) for i in range(count)]


def from_ver3(b):
    """A 3DS / Wii U Mii (Ver3StoreData, little-endian), with the Switch's conversion."""
    if b[0] != 3:
        raise Bad(f"version {b[0]}, not a 3DS / Wii U Mii's 3")
    info, = struct.unpack_from("<H", b, 0x18)
    b1, b2, hair_type, b3 = b[0x30], b[0x31], b[0x32], b[0x33]
    eye, brow = struct.unpack_from("<II", b, 0x34)
    nose, mouth = struct.unpack_from("<HH", b, 0x3C)
    b8 = b[0x40]
    beard, glass, mole = struct.unpack_from("<HHH", b, 0x42)

    def bits(v, first, n):
        return (v >> first) & ((1 << n) - 1)

    def table(t, i, what):
        if i >= len(t):
            raise Bad(f"its {what} colour ({i}) is not one a 3DS / Wii U Mii has")
        return t[i]

    parts = {
        "font_region": bits(b[1], 4, 2), "favorite_color": bits(info, 10, 4), "gender": bits(info, 0, 1),
        "height": b[0x2E], "build": b[0x2F], "type": 0, "region_move": 0,
        "faceline_type": bits(b1, 1, 4), "faceline_color": table(VER3_FACELINE_COLOR, bits(b1, 5, 3), "skin"),
        "faceline_wrinkle": bits(b2, 0, 4), "faceline_make": bits(b2, 4, 4),
        "hair_type": hair_type, "hair_color": table(VER3_HAIR_COLOR, bits(b3, 0, 3), "hair"),
        "hair_flip": bits(b3, 3, 1),
        "eye_type": bits(eye, 0, 6), "eye_color": table(VER3_EYE_COLOR, bits(eye, 6, 3), "eye"),
        "eye_scale": bits(eye, 9, 4), "eye_aspect": bits(eye, 13, 3), "eye_rotate": bits(eye, 16, 5),
        "eye_x": bits(eye, 21, 4), "eye_y": bits(eye, 25, 5),
        "eyebrow_type": bits(brow, 0, 5), "eyebrow_color": table(VER3_HAIR_COLOR, bits(brow, 5, 3), "eyebrow"),
        "eyebrow_scale": bits(brow, 8, 4), "eyebrow_aspect": bits(brow, 12, 3),
        "eyebrow_rotate": bits(brow, 16, 4), "eyebrow_x": bits(brow, 21, 4), "eyebrow_y": bits(brow, 25, 5),
        "nose_type": bits(nose, 0, 5), "nose_scale": bits(nose, 5, 4), "nose_y": bits(nose, 9, 5),
        "mouth_type": bits(mouth, 0, 6), "mouth_color": table(VER3_MOUTH_COLOR, bits(mouth, 6, 3), "mouth"),
        "mouth_scale": bits(mouth, 9, 4), "mouth_aspect": bits(mouth, 13, 3), "mouth_y": bits(b8, 0, 5),
        "mustache_type": bits(b8, 5, 3),
        "beard_type": bits(beard, 0, 3), "beard_color": table(VER3_HAIR_COLOR, bits(beard, 3, 3), "beard"),
        "mustache_scale": bits(beard, 6, 4), "mustache_y": bits(beard, 10, 5),
        "glass_type": bits(glass, 0, 4), "glass_color": table(VER3_GLASS_COLOR, bits(glass, 4, 3), "glasses"),
        "glass_scale": bits(glass, 7, 4), "glass_y": bits(glass, 11, 5),
        "mole_type": bits(mole, 0, 1), "mole_scale": bits(mole, 1, 4), "mole_x": bits(mole, 5, 5),
        "mole_y": bits(mole, 10, 5),
    }
    name = name_from_units(struct.unpack_from("<10H", b, 0x1A))
    return Mii(made_id(bytes(b[:0x5C])), name, parts)


def from_studio(data, name):
    """Mii Studio's data: 47 bytes (a random first byte, then each part obfuscated), or the 46 parts."""
    if len(data) == len(STUDIO_PARTS) + 1:
        prev, plain = data[0], []
        for e in data[1:]:
            plain.append(((e - 7) & 0xFF) ^ prev)
            prev = e
        data = bytes(plain)
    if len(data) != len(STUDIO_PARTS):
        raise Bad(f"{len(data)} bytes of Mii Studio data, not {len(STUDIO_PARTS)} or {len(STUDIO_PARTS) + 1}")
    parts = dict(zip(STUDIO_PARTS, data))
    parts.update(font_region=0, type=0, region_move=0)
    return Mii(made_id(data, name.encode("utf-16-le")), name, parts)


def studio_data(text):
    """The Mii Studio data in a link (its data= parameter) or a code, or None."""
    text = text.strip()
    if "://" in text or "?" in text:
        query = urllib.parse.urlparse(text).query or text.split("?", 1)[-1]
        text = (urllib.parse.parse_qs(query).get("data") or [""])[0]
    if re.fullmatch(r"(?i)[0-9a-f]{92}|[0-9a-f]{94}", text):
        return bytes.fromhex(text)
    return None


def read_source(src, name):
    """The Miis in a file, link or code, and what to call it in messages."""
    if os.path.isfile(src):
        with open(src, "rb") as f:
            b = f.read()
        if b[:4] == NFDB_MAGIC and len(b) == NFDB_SIZE:
            return from_nfdb(b)
        if len(b) == CHARINFO_SIZE:
            return [Mii.from_charinfo(b)]
        if len(b) in (STORE_SIZE, STORE_ELEMENT_SIZE):
            return [from_store(b)]
        if len(b) == CORE_SIZE:
            return [from_core(b)]
        if len(b) in (0x5C, 0x60):
            return [from_ver3(b)]
        data = studio_data(b.decode("ascii", "replace"))
        if data:
            return [from_studio(data, name)]
        raise Bad(f"{len(b)} bytes: not a Mii file this tool knows")
    data = studio_data(src)
    if data:
        return [from_studio(data, name)]
    raise Bad("not a file, nor a Mii Studio link or code")


# ---- the emulators' Mii databases ----------------------------------------------------------------

YUZU_FAMILY = ("yuzu", "eden", "citron", "sudachi", "suyu", "torzu")


def emulator_databases():
    """(emulator, MiiDatabase.dat) for every emulator's Mii database on this computer."""
    home = os.path.expanduser("~")
    if sys.platform == "win32":
        tops = [os.environ.get("APPDATA", os.path.join(home, "AppData", "Roaming"))]
    elif sys.platform == "darwin":
        tops = [os.path.join(home, "Library", "Application Support")]
    else:
        tops = [os.environ.get("XDG_CONFIG_HOME", os.path.join(home, ".config")),
                os.environ.get("XDG_DATA_HOME", os.path.join(home, ".local", "share"))]
    found = []
    for top in tops:
        # Ryujinx keeps a save's committed copy and its working copy (0 and 1): the newer one
        copies = glob.glob(os.path.join(top, "Ryujinx", "bis", "system", "save", "8000000000000030", "*", "MiiDatabase.dat"))
        if copies:
            found.append(("Ryujinx", max(copies, key=os.path.getmtime)))
        for name in YUZU_FAMILY:
            for folder in os.listdir(top) if os.path.isdir(top) else []:
                if folder.lower() == name:
                    p = os.path.join(top, folder, "nand", "system", "save", "8000000000000030", "MiiDatabase.dat")
                    if os.path.isfile(p):
                        found.append((folder, p))
    return found


# ---- miis/ -----------------------------------------------------------------------------------

def installed():
    """{file name: Mii or the reason it can't be read} in miis/."""
    out = {}
    for path in sorted(glob.glob(os.path.join(MIIS, "*" + EXT))):
        try:
            with open(path, "rb") as f:
                out[os.path.basename(path)] = Mii.from_charinfo(f.read())
        except (OSError, Bad) as e:
            out[os.path.basename(path)] = str(e)
    return out


def file_name(name, taken):
    # (plain ASCII: the game lists the folder with Windows' ANSI calls, which mangle other letters)
    base = re.sub(r'[^A-Za-z0-9 _\-()\[\]!\'+,;=@#$%&~]', "_", name).strip(". _") or "Mii"
    candidate, n = base + EXT, 2
    while candidate.lower() in taken:
        candidate, n = f"{base} ({n}){EXT}", n + 1
    return candidate


def add(miis, where):
    """Writes each Mii to miis/: a Mii already there (the same id) is replaced. Returns how many."""
    os.makedirs(MIIS, exist_ok=True)
    have = installed()
    by_id = {m.create_id: f for f, m in have.items() if isinstance(m, Mii)}
    taken = {f.lower() for f in have}
    done = 0
    for m in miis:
        why = m.check()
        if why:
            print(f"  skipped {m.name or '(no name)'!r} from {where}: {why}")
            continue
        f = by_id.get(m.create_id)
        verb = "updated"
        if f is None:
            if len(taken) >= DATABASE_MAX:
                print(f"  skipped {m.name!r}: miis/ already has {DATABASE_MAX} Miis, as many as the game reads")
                continue
            f, verb = file_name(m.name, taken), "added"
            taken.add(f.lower())
            by_id[m.create_id] = f
        with open(os.path.join(MIIS, f), "wb") as out:
            out.write(m.charinfo())
        print(f"  {verb} {m.name!r} -> miis/{f}")
        done += 1
    return done


def cmd_list(_args):
    have = installed()
    if not have:
        print("No Miis of your own yet (miis/ is empty): the game shows its six default Miis.")
        return 0
    print(f"{len(have)} in miis/ (the game reads them when it starts):")
    for f, m in have.items():
        print(f"  {f}: " + (f"{m.name!r}" + (f" - can't be used: {m.check()}" if m.check() else "")
                          if isinstance(m, Mii) else f"can't be read: {m}"))
    return 0


def cmd_import(args):
    sources = list(args.sources)
    done = 0
    if not sources:
        dbs = emulator_databases()
        if not dbs:
            print("No emulator's Mii database was found on this computer (Ryujinx, yuzu, Eden, Citron, ...).")
        for emu, path in dbs:
            try:
                miis = read_source(path, args.name)
            except (OSError, Bad) as e:
                print(f"{emu}'s Mii database ({path}) can't be read: {e}")
                continue
            print(f"{emu}: {len(miis)} Mii{'s' if len(miis) != 1 else ''} ({path})")
            done += add(miis, emu)
    failed = 0
    for src in sources:
        try:
            miis = read_source(src, args.name)
        except (OSError, Bad) as e:
            print(f"{src}: {e}")
            failed += 1
            continue
        print(f"{os.path.basename(src) if os.path.isfile(src) else 'Mii Studio data'}: "
              f"{len(miis)} Mii{'s' if len(miis) != 1 else ''}")
        done += add(miis, src)
    while args.ask:
        try:
            text = input("\nPaste a Mii Studio link or code to import it too (or press Enter to finish): ").strip()
        except EOFError:
            break
        if not text:
            break
        data = studio_data(text)
        if not data:
            print("  That's not a Mii Studio link or code (it has data=<94 hex digits>).")
            continue
        try:
            name = input("  Its name (up to 10 characters, Enter for \"Mii\"): ").strip()[:NAME_CHARS] or "Mii"
        except EOFError:
            name = "Mii"
        try:
            done += add([from_studio(data, name)], "Mii Studio")
        except Bad as e:
            print(f"  {e}")
    print(f"\n{done} Mii{'s' if done != 1 else ''} imported. Restart the game to see "
          f"{'them' if done != 1 else 'it'}: Games & More -> Mii Fighters." if done else "\nNothing imported.")
    return 1 if failed and not done else 0


def cmd_remove(args):
    have = installed()
    status = 0
    for want in args.names:
        hits = [f for f, m in have.items()
                if f.lower() in (want.lower(), (want + EXT).lower()) or (isinstance(m, Mii) and m.name == want)]
        if not hits:
            print(f"{want}: no such Mii in miis/")
            status = 1
        for f in hits:
            os.remove(os.path.join(MIIS, f))
            print(f"removed miis/{f}")
    return status


def main(argv=None):
    ap = argparse.ArgumentParser(description="Your own Miis in the game (miis/).")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="the Miis in miis/").set_defaults(run=cmd_list)
    p_import = sub.add_parser("import", help="import Miis (no source: every emulator's Mii database found)")
    p_import.add_argument("sources", nargs="*", help="Mii files, Mii databases, Mii Studio links or codes")
    p_import.add_argument("--name", default="Mii", help="the name for Miis that have none (Mii Studio's)")
    p_import.add_argument("--ask", action="store_true", help="also ask for Mii Studio links")
    p_import.set_defaults(run=cmd_import)
    p = sub.add_parser("remove", help="remove Miis from miis/ (by name or file name)")
    p.add_argument("names", nargs="+")
    p.set_defaults(run=cmd_remove)
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["import"]:   # (--name anywhere among the sources)
        args = p_import.parse_intermixed_args(argv[1:])
        args.run = cmd_import
    else:
        args = ap.parse_args(argv)
    if getattr(args, "name", None):
        args.name = args.name[:NAME_CHARS]
    return args.run(args)


if __name__ == "__main__":
    sys.exit(main())
