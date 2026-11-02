#!/usr/bin/env python3
"""Compressed packages (.nsz) made plain (.nsp) again, so the other tools can read them.

Usage:
  nsz.py <package.nsz> <out.nsp>

An .nsz is a PFS0 (as an .nsp) whose .ncz entries are NCAs with their body compressed: the first
0x4000 bytes are the NCA header as it is, then 'NCZSECTN' (the sections: offset, size, crypto
type, key, counter), then one zstd stream or 'NCZBLOCK' (blocks of 2^exp bytes, each zstd or
stored), holding the decrypted body from 0x4000 on. The NCA is rebuilt by decompressing and
encrypting each section again (AES-128-CTR for crypto types 3 and 4: the counter is the section's
upper 8 bytes and the offset / 16). No keys are needed: the section keys travel in the NCZ header.
Other entries (tickets, certificates, .cnmt) are copied.

tools/gamefiles.py takes an .nsz in a game folder through plain_nsp: the plain copy is made once
in dump/nsz/ (several GB, minutes), and made again only when the .nsz changes.
(Ported from ssbu-recomp's tools/extract/nsz_to_nsp.py.)
"""
import os
import struct
import sys

from Cryptodome.Cipher import AES
from Cryptodome.Util import Counter

try:
    from compression.zstd import ZstdDecompressor, decompress as zstd_decompress   # Python 3.14
except ImportError:                                                                  # 3.10 to 3.13
    import zstandard

    def zstd_decompress(data):
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)

    class ZstdDecompressor:   # (only what ncz_to_nca uses: decompress() of the stream piece by piece)
        def __init__(self):
            self._d = zstandard.ZstdDecompressor().decompressobj()

        def decompress(self, data):
            return self._d.decompress(data)

def _log(line):   # (stderr: gamefiles.py --json keeps stdout for its answer)
    print(line, file=sys.stderr, flush=True)


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "dump", "nsz")
CHUNK = 16 << 20


def pfs0(f):
    """[(name, absolute offset, size)] of a PFS0's entries."""
    f.seek(0)
    magic, count, strsize, _ = struct.unpack("<4sIII", f.read(16))
    if magic != b"PFS0":
        raise ValueError("not a PFS0 package")
    ents = [struct.unpack("<QQII", f.read(24)) for _ in range(count)]
    strings = f.read(strsize)
    base = 16 + 24 * count + strsize
    return [(strings[n:strings.index(b"\0", n)].decode(), base + o, s) for o, s, n, _ in ents]


def encrypt(secs, off, data):
    """The decrypted bytes of the NCA at off (0x4000 or more), encrypted where a section says so."""
    out = bytearray(data)
    for s_off, s_size, crypto, key, ctr in secs:
        lo, hi = max(off, s_off), min(off + len(data), s_off + s_size)
        if lo >= hi or crypto not in (3, 4):
            continue
        a = lo & ~0xF                          # (CTR works in 16-byte steps)
        pad = lo - a
        c = Counter.new(64, prefix=ctr[:8], initial_value=a >> 4)
        enc = AES.new(key, AES.MODE_CTR, counter=c).encrypt(bytes(pad) + bytes(out[lo - off:hi - off]))
        out[lo - off:hi - off] = enc[pad:]
    return bytes(out)


def ncz_to_nca(src, start, size, dst):
    """The NCA of the NCZ at src[start:start+size], written to dst; its size."""
    src.seek(start)
    dst.write(src.read(0x4000))
    magic, n = struct.unpack("<8sQ", src.read(16))
    if magic != b"NCZSECTN":
        raise ValueError("no NCZSECTN after the NCA header")
    secs = []
    for _ in range(n):
        o, s, crypto, _pad = struct.unpack("<QQQQ", src.read(32))
        secs.append((o, s, crypto, src.read(16), src.read(16)))
    pos = src.tell()
    off = 0x4000
    if src.read(8) == b"NCZBLOCK":
        _ver, _typ, _u, exp, nblk, total = struct.unpack("<BBBBIQ", src.read(16))
        sizes = struct.unpack("<%dI" % nblk, src.read(4 * nblk))
        bsize = 1 << exp
        for i, cs in enumerate(sizes):
            raw = src.read(cs)
            want = min(bsize, 0x4000 + total - off) if i == nblk - 1 else bsize
            data = raw if cs >= want else zstd_decompress(raw)
            dst.write(encrypt(secs, off, data))
            off += len(data)
    else:
        src.seek(pos)
        left = start + size - pos
        d = ZstdDecompressor()
        while left > 0:
            piece = src.read(min(CHUNK, left))
            if not piece:
                break
            left -= len(piece)
            data = d.decompress(piece)
            if data:
                dst.write(encrypt(secs, off, data))
                off += len(data)
    return off


def convert(src_path, out_path, log=_log):
    """A plain .nsp of the .nsz at src_path, at out_path (written as out_path.partial first)."""
    tmp = out_path + ".body"
    out_ents = []
    with open(src_path, "rb") as src, open(tmp, "wb") as body:
        for name, off, size in pfs0(src):
            at = body.tell()
            if name.lower().endswith(".ncz"):
                n = ncz_to_nca(src, off, size, body)
                name = name[:-4] + ".nca"
            else:
                src.seek(off)
                left = size
                while left:
                    b = src.read(min(CHUNK, left))
                    body.write(b)
                    left -= len(b)
                n = size
            out_ents.append((name, at, n))
            log("    %-48s %14s bytes" % (name, format(n, ",")))
    strings = b"".join(n.encode() + b"\0" for n, _, _ in out_ents)
    strings += b"\0" * ((-(16 + 24 * len(out_ents) + len(strings))) % 0x20)
    name_off, table = 0, b""
    for n, at, size in out_ents:
        table += struct.pack("<QQII", at, size, name_off, 0)
        name_off += len(n) + 1
    partial = out_path + ".partial"
    with open(partial, "wb") as out, open(tmp, "rb") as body:
        out.write(struct.pack("<4sIII", b"PFS0", len(out_ents), len(strings), 0) + table + strings)
        while True:
            b = body.read(CHUNK)
            if not b:
                break
            out.write(b)
    os.remove(tmp)
    os.replace(partial, out_path)


def _stamp(path):
    st = os.stat(path)
    return "%d %d" % (st.st_size, int(st.st_mtime))


def plain_nsp(nsz_path, cache=CACHE, log=_log):
    """The plain .nsp for an .nsz: dump/nsz/<name>.nsp, made now unless it is there from this
    very .nsz (its size and time, in <name>.nsp.src)."""
    os.makedirs(cache, exist_ok=True)
    out = os.path.join(cache, os.path.splitext(os.path.basename(nsz_path))[0] + ".nsp")
    stamp = out + ".src"
    want = _stamp(nsz_path)
    try:
        with open(stamp, encoding="utf-8") as f:
            if f.read().strip() == want and os.path.isfile(out):
                return out
    except OSError:
        pass
    log("  %s: compressed (.nsz); making a plain copy in dump\\nsz (once, a few minutes)" % os.path.basename(nsz_path))
    convert(nsz_path, out, log)
    with open(stamp, "w", encoding="utf-8") as f:
        f.write(want + "\n")
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    convert(sys.argv[1], sys.argv[2])
    print("written %s" % sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
