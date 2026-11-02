"""Unwind tables for a module that has none (a Skyline plugin: Rust built with panic=abort).

The game's C++ exceptions unwind through every guest frame between the throw and the catch with
the game's own libunwind, which finds a module's .eh_frame_hdr through its MOD0 header. A
plugin's functions are called by the game and call it back (a Smashline script runs inside the
game's ACMD coroutine, whose clean-up throws through it), so each needs a frame description:
made here from its prologue, which is all that matters at its call sites - where the stack
pointer and the frame pointer are, and where it saved its callee-saved registers.

The tables go after the module's image (its .bss end, 16-aligned) and MOD0's eh_frame_hdr
fields point at them: tables(mod, funcs) gives (offset, bytes), and recomp.py writes them into
the image and counts them in the module's size."""
import struct

SP, FP, LR = 31, 29, 30


def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def sleb(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        done = (v == 0 and not b & 0x40) or (v == -1 and b & 0x40)
        out.append(b | (0 if done else 0x80))
        if done:
            return bytes(out)


def sx(v, bits):
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


def _stores(w):
    """([(DWARF register, offset from sp after any write-back)], write-back) of a store to [sp],
    or None."""
    if (w >> 27) & 7 == 5 and not (w >> 25) & 1 and not (w >> 22) & 1 and (w >> 5) & 31 == SP:   # stp
        opc, v, mode = w >> 30, (w >> 26) & 1, (w >> 23) & 3
        if (opc, v) not in ((2, 0), (1, 1)) or mode not in (2, 3):   # x or d pair; offset or pre-index
            return None
        imm = sx((w >> 15) & 0x7F, 7) * 8
        base = 64 if v else 0
        at = 0 if mode == 3 else imm
        return [(base + (w & 31), at), (base + ((w >> 10) & 31), at + 8)], (imm if mode == 3 else 0)
    if w & 0xFFE00C00 == 0xF8000C00 and (w >> 5) & 31 == SP:   # str x, [sp, #imm]! (pre-index)
        return [(w & 31, 0)], sx((w >> 12) & 0x1FF, 9)
    if w & 0xFFE00C00 == 0xFC000C00 and (w >> 5) & 31 == SP:   # str d, [sp, #imm]! (pre-index: a function keeping d8 first)
        return [(64 + (w & 31), 0)], sx((w >> 12) & 0x1FF, 9)
    if w & 0xFFC00000 == 0xF9000000 and (w >> 5) & 31 == SP:   # str x, [sp, #imm]
        return [(w & 31, ((w >> 10) & 0xFFF) * 8)], 0
    if w & 0xFFC00000 == 0xFD000000 and (w >> 5) & 31 == SP:   # str d, [sp, #imm]
        return [(64 + (w & 31), ((w >> 10) & 0xFFF) * 8)], 0
    return None


def describe(mod, start, size, depth=0):
    """(prologue end offset, CFA register, CFA offset, {DWARF reg: CFA-relative offset}) of a
    function, from its instructions up to the first unconditional branch, call or return
    (calls to the outliner's helpers followed, once)."""
    sp, fp_at, saved, end = 0, None, {}, start
    pc = start
    while pc < start + size and pc < start + 0x200:
        w = mod.u32(pc)
        pc += 4
        st = _stores(w)
        if st:
            regs, wb = st
            # a pre-index store writes back first; the slots are relative to the new sp
            if wb:
                sp += wb
            for r, off in regs:
                if (19 <= r <= 30 or 72 <= r <= 79) and r not in saved:
                    saved[r] = sp + off
            end = pc
            continue
        if w & 0xFF8003FF == 0xD10003FF:                     # sub sp, sp, #imm{, lsl 12}
            sp -= ((w >> 10) & 0xFFF) << (12 if (w >> 22) & 1 else 0)
            end = pc
            continue
        if w & 0xFF8003FF == 0x910003FD:                     # add x29, sp, #imm (mov x29, sp)
            fp_at = sp + (((w >> 10) & 0xFFF) << (12 if (w >> 22) & 1 else 0))
            end = pc
            continue
        if w & 0xFC000000 == 0x94000000:                     # bl: an outlined prologue part?
            t = pc - 4 + sx(w & 0x3FFFFFF, 26) * 4
            if depth == 0 and mod.text.mem <= t < mod.text.end:
                _e, _r, _o, more = describe(mod, t, 0x40, 1)
                for r, off in more.items():
                    saved.setdefault(r, sp + off)
            break
        if w & 0x7C000000 == 0x14000000 or w & 0xFFFFFC1F in (0xD65F0000, 0xD61F0000, 0xD63F0000):
            break                                            # b, ret, br, blr
    if fp_at is not None:
        return end - start, FP, -fp_at, saved
    return end - start, SP, -sp, saved


def cie():
    body = struct.pack("<I", 0) + bytes([1]) + b"zR\0" + uleb(4) + sleb(-8) + uleb(LR) + uleb(1) + bytes([0x1B])
    body += bytes([0x0C]) + uleb(SP) + uleb(0)                # def_cfa sp, 0
    while (len(body) + 4) % 4:
        body += b"\0"
    return struct.pack("<I", len(body)) + body


def fde(at, cie_at, start, size, prologue, reg, cfa, saved):
    """An FDE at section offset `at` (addresses are module offsets)."""
    ins = bytearray()
    delta = prologue // 4
    if delta:
        if delta < 0x40:
            ins.append(0x40 | delta)
        elif delta < 0x100:
            ins += bytes([0x02, delta])
        else:
            ins += bytes([0x03]) + struct.pack("<H", delta)
        ins += bytes([0x0C]) + uleb(reg) + uleb(cfa)        # def_cfa reg, offset
        for r, off in sorted(saved.items()):
            if off < 0 and -off % 8 == 0:
                ins += (bytes([0x80 | r]) if r < 64 else bytes([0x05]) + uleb(r)) + uleb(-off // 8)
    head_len = 4 + 4 + 4 + 1   # cie pointer, pc_begin, pc_range, aug length
    while (head_len + len(ins)) % 4:
        ins.append(0)
    length = head_len + len(ins)
    out = struct.pack("<I", length) + struct.pack("<I", at + 4 - cie_at)
    out += struct.pack("<i", start - (at + 8)) + struct.pack("<I", size) + uleb(0) + bytes(ins)
    return out


def tables(mod, funcs):
    """(offset in the module, bytes): .eh_frame_hdr, then .eh_frame, for funcs [(start, size)]."""
    at = (mod.mem_end + 15) & ~15
    n = len(funcs)
    hdr_size = 12 + 8 * n
    frame_at = at + ((hdr_size + 15) & ~15)
    frame = bytearray(cie())
    rows = []
    for start, size in sorted(funcs):
        prologue, reg, cfa, saved = describe(mod, start, size)
        rows.append((start, frame_at + len(frame)))
        frame += fde(frame_at + len(frame), frame_at, start, size, prologue, reg, cfa, saved)
    frame += struct.pack("<I", 0)                            # terminator
    hdr = bytearray([1, 0x1B, 0x03, 0x3B])
    hdr += struct.pack("<i", frame_at - (at + 4)) + struct.pack("<I", n)
    for start, f in rows:
        hdr += struct.pack("<ii", start - at, f - at)
    hdr = hdr.ljust(frame_at - at, b"\0")
    return at, bytes(hdr + frame), hdr_size


def mod0_patch(mod, at, hdr_size):
    """(offset, bytes) to write over MOD0's eh_frame_hdr start and end."""
    mod0 = mod.u32(4)
    return mod0 + 0x10, struct.pack("<ii", at - mod0, at + hdr_size - mod0)
