"""Reader for the game's lua2cpp script modules (NRO files): symbols, PLT stubs, functions, disassembly.

Used by census.py (facts table) and acmd_decomp.py (ACMD decompiler). Only the game's own files are read.
"""
import bisect
import os
import struct

import sdk_paths

HERE = sdk_paths.HERE
STUDY = sdk_paths.WORK
_md = None


def disassembler():
    """capstone's arm64 disassembler (imported when first needed: the rest reads the file without it)."""
    global _md
    if _md is None:
        import capstone
        _md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        _md.detail = True
    return _md


_map = None


def nro_map():
    """The game's script modules: sdk_paths.nro_modules() (where they are looked for)."""
    global _map
    if _map is None:
        _map = sdk_paths.nro_modules()
    return _map


class Nro:
    def __init__(self, name):
        if not name.startswith('lua2cpp_'):
            name = 'lua2cpp_' + name
        self.name = name
        self.short = name[len('lua2cpp_'):]
        entry = next((e for e in nro_map() if e['name'] == name), None)
        if entry is None:
            raise sdk_paths.Missing('%s is not among the game\'s script modules (%d found: %s)'
                                    % (name, len(nro_map()), os.path.dirname(nro_map()[0]['file']) if nro_map() else 'none'))
        self.base = int(entry['base'], 16)
        with open(entry['file'], 'rb') as f:
            d = self.d = f.read()
        (self.text_off, self.text_size, self.ro_off, self.ro_size, self.data_off, self.data_size,
         self.bss_size) = struct.unpack_from('<7I', d, 0x20)
        m = struct.unpack_from('<I', d, 4)[0]
        dyn_off = m + struct.unpack_from('<i', d, m + 4)[0]
        self.eh_hdr = m + struct.unpack_from('<i', d, m + 0x10)[0]
        dyn = {}
        o = dyn_off
        while True:
            tag, val = struct.unpack_from('<qQ', d, o)
            o += 16
            if tag == 0:
                break
            dyn[tag] = val
        self.dyn = dyn
        symtab, strtab = dyn[6], dyn[5]
        nsyms = struct.unpack_from('<I', d, dyn[4] + 4)[0]          # DT_HASH nchain
        self.syms = []
        for i in range(nsyms):
            no, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', d, symtab + i * 24)
            nm = d[strtab + no:d.index(b'\0', strtab + no)].decode('latin-1')
            self.syms.append((nm, shndx != 0, value, size))
        self.exports = {v: n for n, defined, v, s in self.syms if defined and n}
        # relocations: GOT slot -> symbol name, and data pointers
        self.got = {}
        self.rel_ptr = {}          # offset -> image offset it points to (RELATIVE)
        self.abs_ptr = {}          # offset -> symbol name (ABS64 / GLOB_DAT)
        for tab, size in ((dyn.get(7, 0), dyn.get(8, 0)), (dyn.get(23, 0), dyn.get(2, 0))):
            for i in range(size // 24):
                off, info, addend = struct.unpack_from('<QQq', d, tab + i * 24)
                t = info & 0xffffffff
                if t == 0x403:
                    self.rel_ptr[off] = addend
                elif t in (0x101, 0x401, 0x402):
                    s = self.syms[info >> 32]
                    if t == 0x402:
                        self.got[off] = s[0]
                    elif s[1] and t == 0x101:
                        self.rel_ptr[off] = s[2] + addend
                    else:
                        self.abs_ptr[off] = s[0]
                        self.got[off] = s[0]
        self._plt = None
        self._fde = None

    # ---- raw access (offsets are image offsets)
    def u32(self, off):
        return struct.unpack_from('<I', self.d, off)[0]

    def u64(self, off):
        return struct.unpack_from('<Q', self.d, off)[0]

    def f32(self, off):
        return struct.unpack_from('<f', self.d, off)[0]

    def cstr(self, off):
        return self.d[off:self.d.index(b'\0', off)].decode('latin-1')

    def in_text(self, off):
        return self.text_off <= off < self.text_off + self.text_size

    # ---- PLT stubs: `adrp x16, got ; ldr x17, [x16, #lo] ; add x16, x16, #lo ; br x17`
    @property
    def plt(self):
        if self._plt is None:
            self._plt = {}
            end = self.text_off + self.text_size
            o = self.text_off
            while o + 16 <= end:
                w0, w1, w2, w3 = struct.unpack_from('<4I', self.d, o)
                if (w0 & 0x9f00001f) == 0x90000010 and (w1 & 0xffc003ff) == 0xf9400211 and w3 == 0xd61f0220:
                    immlo = (w0 >> 29) & 3
                    immhi = (w0 >> 5) & 0x7ffff
                    page = ((immhi << 2) | immlo)
                    if page & (1 << 20):
                        page -= 1 << 21
                    slot = ((o & ~0xfff) + (page << 12)) + ((w1 >> 10) & 0xfff) * 8
                    if slot in self.got:
                        self._plt[o] = self.got[slot]
                        o += 16
                        continue
                o += 4
        return self._plt

    # ---- function starts and sizes from .eh_frame_hdr (every function with unwind info), plus PLT and exports
    @property
    def fdes(self):
        """sorted list of (start, size) from the binary search table of .eh_frame_hdr"""
        if self._fde is None:
            h = self.eh_hdr
            ver, ptr_enc, cnt_enc, tab_enc = struct.unpack_from('<4B', self.d, h)
            assert tab_enc == 0x3b and cnt_enc == 0x03, (cnt_enc, tab_enc)
            n = self.u32(h + 8)
            out = []
            for i in range(n):
                loc, fde = struct.unpack_from('<ii', self.d, h + 12 + i * 8)
                start, fde = h + loc, h + fde
                # FDE: length, cie pointer, pc_begin (pcrel sdata4), pc_range (udata4)
                rng = self.u32(fde + 12)
                out.append((start, rng))
            out.sort()
            self._fde = out
        return self._fde

    def func_size(self, off):
        i = bisect.bisect_left(self.fdes, (off, 0))
        if i < len(self.fdes) and self.fdes[i][0] == off:
            return self.fdes[i][1]
        return None

    def disasm(self, off, size):
        return list(disassembler().disasm(self.d[off:off + size], off))


def demangle_simple(n):
    """Itanium name -> 'A::B::c' for the simple nested names the modules import (arguments dropped)."""
    if not n.startswith('_ZN'):
        if n.startswith('_Z') and n[2:3].isdigit():
            i = 2
            j = i
            while n[j].isdigit():
                j += 1
            return n[j:j + int(n[i:j])]
        return n
    i = 3
    if n[i] == 'K':
        i += 1
    parts = []
    while i < len(n) and n[i] != 'E':
        if n[i].isdigit():
            j = i
            while n[j].isdigit():
                j += 1
            ln = int(n[i:j])
            parts.append(n[j:j + ln])
            i = j + ln
        elif n[i:i + 2] in ('C1', 'C2'):
            parts.append(parts[-1])
            i += 2
        elif n[i:i + 2] in ('D1', 'D2', 'D0'):
            parts.append('~' + parts[-1])
            i += 2
        elif n[i:i + 2] == 'cv':
            parts.append('operator ' + n[i + 2:i + 3])
            i += 3
        elif n[i:i + 2].islower() and len(n) > i + 1:
            parts.append('operator_' + n[i:i + 2])
            i += 2
        else:
            break
    return '::'.join(parts) + ('' if i >= len(n) or n[i] == 'E' else '?')
