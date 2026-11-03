#!/usr/bin/env python3
"""New fighter kinds: the patches of main's code that make room for them (plan/phases/R9_fighter_registry.md,
step 1; ssbu-recomp's N3, plan/phases/N3_new_kind.md).

Usage:
  fighter_kinds.py [--tree DIR] [--check] [--remove]
      write DIR/dump/patches/main.txt (DIR: this checkout) from DIR/dump/exefs/main: tools/recomp.py then lifts
      main with them (local_patches), and main's C is generated and compiled again where they fall
  --check   say whether the dump's main takes them (every site holds the instruction expected), write nothing
  --remove  delete the file: main is lifted as the game has it

The game keeps the fighters' parameters in one object with 0x5E rows a kind (array A, rows of 0x38 bytes, then
array B, rows of 0xC, then 0x50 bytes of members). With these patches it has 0x72 rows (0x5E-0x71); the engine's name and hash tables have 16 spare rows (0x5E-0x6D: 0x6E.. are special kinds), so at most 16 new fighters:
array A grows in place, array B moves behind the members (+0x14F0 -> +0x19A8), the object is made bigger, the
bound checks `kind < 0x5E` become `< 0x72`, and a second per-kind container grows likewise. Every patch changes
only an instruction's immediate (an offset, a bound, a size): nothing else of the game changes, and with no new
fighter the game plays exactly as before (the fight hash: plan/phases/N3_new_kind.md 3.4).

SITES below holds no game code: each site's address in 13.0.5's main, the kind of instruction, and its
immediate before and after. The instruction words themselves are read from the player's own dump when this runs
and written only into the dump's folder (never committed).
"""
import argparse
import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

KINDS, NEW_KINDS = 0x5E, 0x72

# <main offset> <instruction kind> <immediate in the game> <immediate patched> <what>
#   ls: a load or store with an unsigned offset (the immediate in bytes); imm12: add / sub / cmp with a 12-bit
#   immediate; movz / movn: a 16-bit move.
SITES = """
034b0c0 imm12 5e 72 bound 0x5e -> NK
034b0e8 ls 14f0 19a8 B +0x14f0 -> +0x19a8
034b1a4 imm12 5e 72 bound 0x5e -> NK
034b1d4 ls 14f4 19ac B +0x14f4 -> +0x19ac
060cf44 imm12 5e 72 bound 0x5e -> NK
060cf74 ls 14f0 19a8 B +0x14f0 -> +0x19a8
06306b4 imm12 5e 72 bound 0x5e -> NK
06306e0 ls 14f4 19ac B +0x14f4 -> +0x19ac
06370d8 imm12 5e 72 bound 0x5e -> NK
0637104 ls 14f4 19ac B +0x14f4 -> +0x19ac
0637958 imm12 5e 72 bound 0x5e -> NK
0637984 ls 14f4 19ac B +0x14f4 -> +0x19ac
0637a24 imm12 5e 72 bound 0x5e -> NK
0637a60 ls 14f0 19a8 B +0x14f0 -> +0x19a8
0638288 imm12 5e 72 bound 0x5e -> NK
06382b4 ls 14f4 19ac B +0x14f4 -> +0x19ac
0665214 imm12 5e 72 bound 0x5e -> NK
0665238 ls 14f0 19a8 B +0x14f0 -> +0x19a8
066ee70 movz 19a8 1f00 ctor: object size
066ee90 movz 19a8 1f00 ctor: object size
066eeb4 movz 19a8 1f00 ctor: object size
066eecc movz 14f0 1950 ctor: end of A = start of the zero fill
066eee4 movn 148f 18ef ctor: -(size of A)
066ef08 ls 14f8 1958 ctor: row store
066ef0c ls 1508 1968 ctor: row store
066ef10 ls 1500 1960 ctor: row store
066ef14 ls 1520 1980 ctor: row store
066ef18 ls 1518 1978 ctor: row store
066ef1c ls 1510 1970 ctor: row store
066ef20 ls 1530 1990 ctor: row store
066ef24 ls 1540 19a0 ctor: row store
066ef28 ls 1538 1998 ctor: row store
066ef2c ls 1558 19b8 ctor: row store
066ef30 ls 1550 19b0 ctor: row store
066ef34 ls 1548 19a8 ctor: row store
066ef40 movz 47c 5b0 ctor: zero fill length (members behind A + B)
066f384 movz 8f0 ad0 container 2 ctor: size (allocation / zero fill)
066f3a4 movz 8f0 ad0 container 2 ctor: size (allocation / zero fill)
066f3c8 movz 8f0 ad0 container 2 ctor: size (allocation / zero fill)
066f3e0 movz 8f0 ad0 container 2 ctor: size (allocation / zero fill)
066f534 ls 8d1 ab1 container 2 ctor: lock byte
066f538 ls 8d2 ab2 container 2 ctor: lock byte
066f964 ls 8d0 ab0 container 2 ctor: lock byte
06797b0 imm12 5e 72 bound 0x5e -> NK
06797d0 ls 14f0 19a8 B +0x14f0 -> +0x19a8
0686d54 imm12 5e 72 bound 0x5e -> NK
0686d70 imm12 5e 72 bound 0x5e -> NK
0686d84 ls 14f4 19ac B +0x14f4 -> +0x19ac
0686dc0 ls 14f0 19a8 B +0x14f0 -> +0x19a8
06871b8 imm12 5e 72 bound 0x5e -> NK
06871d0 ls 14f0 19a8 B +0x14f0 -> +0x19a8
0690534 imm12 5e 72 bound 0x5e -> NK
069054c ls 14f0 19a8 B +0x14f0 -> +0x19a8
06a6618 imm12 5e 72 bound 0x5e -> NK
06a6638 ls 14f0 19a8 B +0x14f0 -> +0x19a8
06a6cb4 imm12 5e 72 bound 0x5e -> NK
06a6cd4 ls 14f0 19a8 B +0x14f0 -> +0x19a8
06a9b0c imm12 5e 72 bound 0x5e -> NK
06a9b28 ls 14f0 19a8 B +0x14f0 -> +0x19a8
06aaf38 imm12 5e 72 bound 0x5e -> NK
06aaf58 ls 14f0 19a8 B +0x14f0 -> +0x19a8
06bdde0 imm12 5e 72 bound 0x5e -> NK
06bddfc ls 14f0 19a8 B +0x14f0 -> +0x19a8
06c93bc imm12 5e 72 bound 0x5e -> NK
06c93f8 ls 14f0 19a8 B +0x14f0 -> +0x19a8
06dece4 imm12 8d0 ab0 container 2: lock address
06df68c imm12 8d0 ab0 container 2: lock address
06e1efc imm12 5e 72 bound 0x5e -> NK
06e1f28 ls 14f4 19ac B +0x14f4 -> +0x19ac
06e22d4 imm12 5e 72 bound 0x5e -> NK
06e2300 ls 14f4 19ac B +0x14f4 -> +0x19ac
06e2514 imm12 5e 72 bound 0x5e -> NK
06e2540 ls 14f4 19ac B +0x14f4 -> +0x19ac
06e5018 imm12 8d0 ab0 container 2: lock address
0709b20 movz 14f0 1950 end of A
0709d7c movz 14f0 1950 end of A
070a4a8 ls 14f0 19a8 B +0x14f0 -> +0x19a8
070a4f4 ls 14f8 19b0 B +0x14f8 -> +0x19b0
070a540 ls 14f4 19ac B +0x14f4 -> +0x19ac
070c350 movz 14f0 1950 end of A
0720fb4 imm12 5e 72 bound 0x5e -> NK
0720fe0 ls 14f8 19b0 B +0x14f8 -> +0x19b0
0721044 imm12 5e 72 bound 0x5e -> NK
0721054 ls 14f8 19b0 B +0x14f8 -> +0x19b0
0721124 imm12 5e 72 bound 0x5e -> NK
0721134 ls 14f8 19b0 B +0x14f8 -> +0x19b0
07211cc imm12 5e 72 bound 0x5e -> NK
07211dc ls 14f8 19b0 B +0x14f8 -> +0x19b0
0721254 imm12 5e 72 bound 0x5e -> NK
072126c ls 14f8 19b0 B +0x14f8 -> +0x19b0
07212f4 imm12 5e 72 bound 0x5e -> NK
072130c ls 14f8 19b0 B +0x14f8 -> +0x19b0
07213b8 imm12 5e 72 bound 0x5e -> NK
07213c8 ls 14f8 19b0 B +0x14f8 -> +0x19b0
0721444 imm12 5e 72 bound 0x5e -> NK
072145c ls 14f8 19b0 B +0x14f8 -> +0x19b0
07214e4 imm12 5e 72 bound 0x5e -> NK
07214fc ls 14f8 19b0 B +0x14f8 -> +0x19b0
077d11c imm12 5e 72 bound 0x5e -> NK
077d134 ls 14f0 19a8 B +0x14f0 -> +0x19a8
077d7e4 imm12 5e 72 bound 0x5e -> NK
077d810 ls 14f4 19ac B +0x14f4 -> +0x19ac
077daa0 imm12 5e 72 bound 0x5e -> NK
077dacc ls 14f4 19ac B +0x14f4 -> +0x19ac
077dde8 imm12 5e 72 bound 0x5e -> NK
077de14 ls 14f4 19ac B +0x14f4 -> +0x19ac
085ae24 ls 18d4 1d8c Bconst kind 0x53 field 0
090e924 imm12 5e 72 bound 0x5e -> NK
090e948 ls 14f0 19a8 B +0x14f0 -> +0x19a8
0b9f9d0 imm12 5e 72 bound 0x5e -> NK
0b9f9f4 ls 14f0 19a8 B +0x14f0 -> +0x19a8
0be8dbc ls 17a8 1c60 Bconst kind 0x3a field 0
0c44a30 ls 1760 1c18 Bconst kind 0x34 field 0
0e62430 imm12 5e 72 bound 0x5e -> NK
0e62454 ls 14f0 19a8 B +0x14f0 -> +0x19a8
0f02978 imm12 5e 72 bound 0x5e -> NK
0f029a4 ls 14f4 19ac B +0x14f4 -> +0x19ac
0f126dc ls 1910 1dc8 Bconst kind 0x58 field 0
1083724 imm12 5e 72 bound 0x5e -> NK
1083750 ls 14f4 19ac B +0x14f4 -> +0x19ac
116be3c imm12 5e 72 bound 0x5e -> NK
116be80 ls 14f0 19a8 B +0x14f0 -> +0x19a8
11b639c imm12 5e 72 bound 0x5e -> NK
11b63c8 ls 14f4 19ac B +0x14f4 -> +0x19ac
1266d40 ls 194c 1e04 Bconst kind 0x5d field 0
12ae578 imm12 5e 72 bound 0x5e -> NK
12ae5a4 ls 14f4 19ac B +0x14f4 -> +0x19ac
1315ee4 imm12 5e 72 bound 0x5e -> NK
1315f24 ls 14f0 19a8 B +0x14f0 -> +0x19a8
14f2510 imm12 5e 72 container 2: loop over all kinds 0x5e -> NK
14f2520 imm12 8d0 ab0 container 2: lock address
153e4c8 imm12 5e 72 bound 0x5e -> NK
153e4e0 ls 14f0 19a8 B +0x14f0 -> +0x19a8
167aa8c imm12 5e 72 bound 0x5e -> NK
167ac7c imm12 5e 72 bound 0x5e -> NK
167ac90 imm12 5e 72 bound 0x5e -> NK
167acb8 ls 14f0 19a8 B +0x14f0 -> +0x19a8
167ace0 imm12 5e 72 bound 0x5e -> NK
199b828 imm12 5e 72 bound 0x5e -> NK
199b840 ls 14f0 19a8 B +0x14f0 -> +0x19a8
1f9a658 imm12 5e 72 bound 0x5e -> NK
1f9a67c ls 14f0 19a8 B +0x14f0 -> +0x19a8
1f9a6c8 imm12 5e 72 bound 0x5e -> NK
1f9a6ec ls 14f0 19a8 B +0x14f0 -> +0x19a8
1f9a738 imm12 5e 72 bound 0x5e -> NK
1f9a75c ls 14f0 19a8 B +0x14f0 -> +0x19a8
1f9a7a8 imm12 5e 72 bound 0x5e -> NK
1f9a7cc ls 14f0 19a8 B +0x14f0 -> +0x19a8
1f9a858 imm12 5e 72 bound 0x5e -> NK
1f9a87c ls 14f0 19a8 B +0x14f0 -> +0x19a8
20adb98 imm12 5e 72 bound 0x5e -> NK
20adbc4 ls 14f4 19ac B +0x14f4 -> +0x19ac
20ae094 imm12 5e 72 bound 0x5e -> NK
20ae0c0 ls 14f4 19ac B +0x14f4 -> +0x19ac
21cb8cc ls 1910 1dc8 Bconst kind 0x58 field 0
22822b8 imm12 5e 72 bound 0x5e -> NK
22822dc ls 14f0 19a8 B +0x14f0 -> +0x19a8
2282308 imm12 5e 72 bound 0x5e -> NK
228232c ls 14f0 19a8 B +0x14f0 -> +0x19a8
2282378 imm12 5e 72 bound 0x5e -> NK
228239c ls 14f0 19a8 B +0x14f0 -> +0x19a8
22e7970 imm12 5e 72 bound 0x5e -> NK
22e7994 ls 14f0 19a8 B +0x14f0 -> +0x19a8
33df52c imm12 5e 72 bound 0x5e -> NK
33df570 ls 14f0 19a8 B +0x14f0 -> +0x19a8
3406724 ls 176c 1c24 Bconst kind 0x35 field 0
34c9cbc ls 16ac 1b64 Bconst kind 0x25 field 0
34c9cec ls 16b8 1b70 Bconst kind 0x26 field 0
34d0bac ls 16a0 1b58 Bconst kind 0x24 field 0
35001c0 imm12 5e 72 bound 0x5e -> NK
3500204 ls 14f0 19a8 B +0x14f0 -> +0x19a8
3500a74 imm12 5e 72 bound 0x5e -> NK
3500ab8 ls 14f0 19a8 B +0x14f0 -> +0x19a8
3516d94 imm12 5e 72 bound 0x5e -> NK
3516db8 ls 14f0 19a8 B +0x14f0 -> +0x19a8
"""


def sites():
    out = []
    for line in SITES.strip().splitlines():
        off, kind, old, new, what = line.split(" ", 4)
        out.append((int(off, 16), kind, int(old, 16), int(new, 16), what))
    return out


def decode(word, kind):
    """The immediate of an instruction of this kind, or None if the word is not one."""
    if kind == "ls" and (word & 0x3B000000) == 0x39000000 and (word >> 24) & 1:
        return ((word >> 10) & 0xFFF) << (word >> 30)
    if kind == "imm12" and (word & 0x1F000000) == 0x11000000 and not (word >> 22) & 1:
        return (word >> 10) & 0xFFF
    if kind in ("movz", "movn") and (word & 0x7F800000) == (0x52800000 if kind == "movz" else 0x12800000) and not (word >> 21) & 3:
        return (word >> 5) & 0xFFFF
    return None


def encode(word, kind, value):
    if kind == "ls":
        size = word >> 30
        if value % (1 << size) or value >> size > 0xFFF:
            raise ValueError(f"{value:#x} is not a {1 << size}-byte offset")
        return (word & ~(0xFFF << 10)) | ((value >> size) << 10)
    if kind == "imm12":
        return (word & ~(0xFFF << 10)) | (value << 10)
    return (word & ~(0xFFFF << 5)) | (value << 5)


def patches(u32):
    """([(offset, old word, new word, what)], [problems]) for a main whose words u32(offset) gives."""
    out, bad = [], []
    for off, kind, old, new, what in sites():
        w = u32(off)
        if decode(w, kind) != old:
            bad.append(f"{off:#x}: not the instruction expected ({what})")
            continue
        out.append((off, w, encode(w, kind, new), what))
    return out, bad


def main_words(tree):
    import recomp
    path = os.path.join(tree, "dump", "exefs", "main")
    if not os.path.exists(path):
        sys.exit(f"no {path}: the game's main is needed (tools/build_game.py puts it there)")
    mod = recomp.Module(path)
    return mod.u32


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tree", default=os.path.dirname(TOOLS))
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--remove", action="store_true")
    a = ap.parse_args(argv)
    out = os.path.join(a.tree, "dump", "patches", "main.txt")
    if a.remove:
        if os.path.exists(out):
            os.remove(out)
        print(f"{out}: removed")
        return 0
    found, bad = patches(main_words(a.tree))
    for b in bad:
        print(b)
    if bad:
        print(f"{len(bad)} of {len(found) + len(bad)} sites differ: this main is not 13.0.5's, or patched already; nothing written")
        return 1
    if a.check:
        print(f"main takes the {len(found)} patches for {NEW_KINDS - KINDS} new fighter kinds")
        return 0
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(f"# tools/fighter_kinds.py: room for {NEW_KINDS - KINDS} new fighter kinds ({KINDS:#x} -> {NEW_KINDS:#x}). "
                "From this PC's dump: do not share.\n")
        for off, old, new, what in found:
            f.write(f"{off:07x} {old:08x} {new:08x}   # {what}\n")
    print(f"{out}: {len(found)} patches for {NEW_KINDS - KINDS} new fighter kinds (main's C is made again where they fall)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
