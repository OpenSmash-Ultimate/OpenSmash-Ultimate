#!/usr/bin/env python3
"""Port a Skyline plugin built for an older game version to 13.0.5: its addresses in the game's `main`
(hook targets, code patches, functions it calls and tables it reads, all `text base + offset`) are
rewritten where the plugin's code builds them, from an address map made for that plugin's build
(tools/ports/<module>-<version>.txt).

The CSK Collection 5.0.6 is built for 13.0.4 and has no 13.0.5 release. Between the two, main's code
moved by different amounts in different places (0, -0x40, -0x1A0, +0x1BC, +0x3D8, +0x450, +0x490,
+0x5B0) and its read-only data and data by +0x1000, so no single shift works: the map gives each
address the plugin uses its own 13.0.5 address, with the evidence for it (docs/PLUGINS-REPORT.md).

Usage:
  port_plugin.py <plugin.nro> <map.txt> <out.nro>    rewrite; lists what it changed and what it couldn't

A plugin builds an offset with `movz` and `movk` into one register (`mov w9, #0x5070; movk w9, #0x454,
lsl #16`): each such chain whose value is in the map gets the new value's halves. A map entry no
chain builds is an error (the plugin builds it some other way: look before trusting the port)."""
import hashlib
import os
import struct
import sys


MOVZ, MOVK = 0x52800000, 0x72800000


def load_map(path):
    """{old: new}, and the plugin's SHA-256 the map was made for ("sha256 <hex>" line)."""
    table, sha = {}, None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if not line:
                continue
            if line[0] == "sha256":
                sha = line[1].lower()
                continue
            table[int(line[0], 16)] = int(line[1], 16)
    return table, sha


def chains(data, lo, hi):
    """[(value, [instruction offsets])] for each movz (+ movk) chain in [lo, hi)."""
    out, regs = [], {}
    for at in range(lo, hi, 4):
        w = struct.unpack_from("<I", data, at)[0]
        rd = w & 31
        if (w & 0x7F800000) == MOVZ:
            if rd in regs:
                out.append(regs.pop(rd))
            hw = (w >> 21) & 3
            regs[rd] = [((w >> 5) & 0xFFFF) << (16 * hw), [at]]
        elif (w & 0x7F800000) == MOVK and rd in regs and at - regs[rd][1][-1] <= 32:
            hw = (w >> 21) & 3
            r = regs[rd]
            r[0] = r[0] & ~(0xFFFF << (16 * hw)) | (((w >> 5) & 0xFFFF) << (16 * hw))
            r[1].append(at)
    out.extend(regs.values())
    return out


def port(data, table):
    """(new bytes, [(old, new, offsets)], [map entries no chain builds])"""
    data = bytearray(data)
    done, seen = [], set()
    for value, at in chains(bytes(data), *_text(data)):
        if value not in table:
            continue
        new = table[value]
        halves = {(struct.unpack_from("<I", data, a)[0] >> 21) & 3 for a in at}
        if any((new >> (16 * h)) & 0xFFFF for h in range(4) if h not in halves):
            raise SystemExit(f"{value:#x} -> {new:#x}: needs a half the plugin's code doesn't set (chain at {at[0]:#x})")
        for a in at:
            w = struct.unpack_from("<I", data, a)[0]
            half = (new >> (16 * ((w >> 21) & 3))) & 0xFFFF
            struct.pack_into("<I", data, a, (w & ~(0xFFFF << 5)) | (half << 5))
        done.append((value, new, at))
        seen.add(value)
    return bytes(data), done, sorted(set(table) - seen)


def _text(data):
    """The NRO's text segment, [start, end) (its header: 'NRO0' at 0x10, segments from 0x20)."""
    if data[0x10:0x14] != b"NRO0":
        raise SystemExit("not an NRO")
    off, size = struct.unpack_from("<II", data, 0x20)
    return off, off + size


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    src, map_path, out = sys.argv[1:]
    with open(src, "rb") as f:
        data = f.read()
    table, sha = load_map(map_path)
    have = hashlib.sha256(data).hexdigest()
    if sha and have != sha:
        raise SystemExit(f"{src} is not the build the map was made for ({have}, not {sha})")
    new, done, missing = port(data, table)
    for old, to, at in done:
        print(f"{old:#x} -> {to:#x} at {', '.join(hex(a) for a in at)}")
    if missing:
        raise SystemExit("not built by a movz/movk chain (left alone): " + ", ".join(hex(v) for v in missing))
    with open(out, "wb") as f:
        f.write(new)
    print(f"{len(done)} address(es) moved: {out}")


if __name__ == "__main__":
    main()
