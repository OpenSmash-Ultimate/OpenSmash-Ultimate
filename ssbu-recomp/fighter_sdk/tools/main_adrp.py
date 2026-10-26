"""python main_adrp.py <address hex> [<main.elf>] : code sites in main.elf that form the address with adrp + add.

The address is main's (0x71xxxxxxxx, or the offset from main's start). main.elf is the game's main program as
scripts/setup.py converts it from your dump; without the argument it is looked for as sdk_paths.main_elf() says.
The code segment is read from the file's program headers, so either build of the game works."""
import struct
import sys

import sdk_paths

BASE = 0x7100000000


def text_segment(d):
    """(file offset, address, size) of an ELF's executable PT_LOAD segment."""
    if d[:4] != b'\x7fELF' or d[4:5] != b'\x02':
        raise SystemExit('not a 64-bit ELF file (main.elf is the converted main program: scripts/setup.py, step convert)')
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    for i in range(phnum):
        typ, flags, off, vaddr, _paddr, filesz = struct.unpack_from('<IIQQQQ', d, phoff + i * phentsize)
        if typ == 1 and flags & 1:
            return off, vaddr, filesz
    raise SystemExit('the ELF file has no executable segment')


def find(target, path):
    import numpy as np
    with open(path, 'rb') as f:
        d = f.read()
    text_off, text_va, text_size = text_segment(d)
    if target >= BASE:
        target -= BASE
    w = np.frombuffer(d, dtype='<u4', count=text_size // 4, offset=text_off)
    pc = np.arange(len(w), dtype=np.int64) * 4 + text_va
    is_adrp = (w & 0x9f000000) == 0x90000000
    imm = (((w >> 5) & 0x7ffff).astype(np.int64) << 2) | ((w >> 29) & 3)
    imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
    page = (pc & ~0xfff) + (imm << 12)
    hits = np.nonzero(is_adrp & (page == (target & ~0xfff)))[0]
    out = []
    for i in hits:
        rd = int(w[i]) & 31
        for j in range(i + 1, min(i + 8, len(w))):
            x = int(w[j])
            if (x & 0xffc00000) == 0x91000000 and ((x >> 5) & 31) == rd and ((x >> 10) & 0xfff) == (target & 0xfff):
                out.append(int(pc[j]))
                break
    return out


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for a in find(int(sys.argv[1], 16), sdk_paths.main_elf(sys.argv[2] if len(sys.argv) > 2 else None)):
        print(hex(a + BASE))
