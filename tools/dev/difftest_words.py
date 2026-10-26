"""Instruction words for difftest.py's synthetic mode, which needs no game.

Every word is assembled here from the bit fields of the A64 encoding classes (Arm Architecture
Reference Manual, "A64 Instruction Set Encoding"): random words inside each class's fixed bits
(CLASSES), and lists of edge cases (EDGES, SEQUENCES). None is taken from the game or any
other binary. All of it is deterministic for a given random.Random.

Words the architecture leaves UNPREDICTABLE (a load that writes back into its own data
register, a store-exclusive whose status register is its data or base) are dropped: the
reference and the console may both differ from the lifter there, and the game has none.
"""

OFFSET = 256   # branch and literal offsets are drawn from +-OFFSET words, so they land inside the image


def small_offset(w, lo, bits, rng):
    """w with its signed offset field (bits lo .. lo+bits-1) replaced by a small one."""
    m = (1 << bits) - 1
    return (w & ~(m << lo)) | ((rng.randrange(-OFFSET, OFFSET) & m) << lo)


def regs(rng, n, avoid=()):
    """n distinct general registers out of x0-x30, none in avoid."""
    out = []
    while len(out) < n:
        r = rng.randrange(31)
        if r not in out and r not in avoid:
            out.append(r)
    return out


# ---------------------------------------------------------------------------- fix-ups
def fix_branch26(w, rng):
    return small_offset(w, 0, 26, rng)


def fix_branch19(w, rng):
    return small_offset(w, 5, 19, rng)


def fix_branch14(w, rng):
    return small_offset(w, 5, 14, rng)


def fix_ls_reg(w, rng):
    """Load/store register (unscaled, pre/post-indexed, register offset)."""
    vec, rn, rt = (w >> 26) & 1, (w >> 5) & 31, w & 31
    if not (w >> 21) & 1:
        if (w >> 10) & 1 and not vec and rn == rt and rn != 31:   # writeback into the data register
            return None
    elif (w >> 10) & 3 == 2 and rn == (w >> 16) & 31:             # the base is also the index
        return None
    return w


def fix_ls_pair(w, rng):
    vec, load, mode = (w >> 26) & 1, (w >> 22) & 1, (w >> 23) & 3
    rn, rt, rt2 = (w >> 5) & 31, w & 31, (w >> 10) & 31
    if load and rt == rt2:
        return None
    if mode in (1, 3) and not vec and rn != 31 and rn in (rt, rt2):
        return None
    return w


def fix_exclusive(w, rng):
    o2, load, o1 = (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1
    rs, rt2, rn, rt = (w >> 16) & 31, (w >> 10) & 31, (w >> 5) & 31, w & 31
    if not load and not o2 and (rs == rt or (rs == rn and rn != 31) or (o1 and rs == rt2)):
        return None
    if load and o1 and rt == rt2:
        return None
    return w


def fix_literal(w, rng):
    return small_offset(w, 5, 19, rng)


def fix_shift_imm(w, rng):
    """immh = 0 is the modified-immediate class instead."""
    if not (w >> 19) & 15:
        w |= rng.randrange(1, 16) << 19
    return w


def branch_register(rng):
    """br / blr / ret Xn."""
    return rng.choice((0xD61F0000, 0xD63F0000, 0xD65F0000)) | rng.randrange(32) << 5


def dc_zva(rng):
    return 0xD50B7420 | rng.randrange(31)


# (name, mask, value, fix-up): a class's fixed bits; the other bits are random, then the fix-up
# (if any) adjusts the word or drops it (None). A name with a function instead draws the word.
CLASSES = [
    # data processing, immediate
    ("adr/adrp", 0x1F000000, 0x10000000, None),
    ("add/sub immediate", 0x1F800000, 0x11000000, None),
    ("logical immediate", 0x1F800000, 0x12000000, None),
    ("move wide", 0x1F800000, 0x12800000, None),
    ("bitfield", 0x1F800000, 0x13000000, None),
    ("extract", 0x1F800000, 0x13800000, None),
    # data processing, register
    ("logical shifted register", 0x1F000000, 0x0A000000, None),
    ("add/sub shifted register", 0x1F200000, 0x0B000000, None),
    ("add/sub extended register", 0x1F200000, 0x0B200000, None),
    ("add/sub with carry", 0x1FE0FC00, 0x1A000000, None),
    ("conditional compare", 0x3FE00410, 0x3A400000, None),
    ("conditional select", 0x3FE00800, 0x1A800000, None),
    ("data processing, 1 source", 0x5FFF0000, 0x5AC00000, None),
    ("data processing, 2 sources", 0x5FE00000, 0x1AC00000, None),
    ("data processing, 3 sources", 0x7F000000, 0x1B000000, None),
    # branches (offsets kept small: the snippet's next PC must be fetchable by the reference)
    ("b/bl", 0x7C000000, 0x14000000, fix_branch26),
    ("cbz/cbnz", 0x7E000000, 0x34000000, fix_branch19),
    ("tbz/tbnz", 0x7E000000, 0x36000000, fix_branch14),
    ("b.cond", 0xFF000010, 0x54000000, fix_branch19),
    ("br/blr/ret", branch_register),
    ("dc zva", dc_zva),
    # loads and stores (base registers point into the scratch window: difftest.mem_regs)
    ("load/store exclusive, acquire/release", 0x3F000000, 0x08000000, fix_exclusive),
    ("load literal", 0x3B000000, 0x18000000, fix_literal),
    ("load/store pair", 0x3A000000, 0x28000000, fix_ls_pair),
    ("load/store register (unscaled, indexed, register offset)", 0x3B000000, 0x38000000, fix_ls_reg),
    ("load/store register (unsigned offset)", 0x3B000000, 0x39000000, None),
    ("advsimd load/store multiple structures", 0xBFBF0000, 0x0C000000, None),
    ("advsimd load/store multiple structures, post-indexed", 0xBFA00000, 0x0C800000, None),
    ("advsimd load/store single structure", 0xBF9F0000, 0x0D000000, None),
    ("advsimd load/store single structure, post-indexed", 0xBF800000, 0x0D800000, None),
    # scalar floating point
    ("fp data processing, 1 source", 0xFF207C00, 0x1E204000, None),
    ("fp compare", 0xFF203C00, 0x1E202000, None),
    ("fp immediate", 0xFF201FE0, 0x1E201000, None),
    ("fp conditional compare", 0xFF200C00, 0x1E200400, None),
    ("fp data processing, 2 sources", 0xFF200C00, 0x1E200800, None),
    ("fp conditional select", 0xFF200C00, 0x1E200C00, None),
    ("fp data processing, 3 sources", 0xFF000000, 0x1F000000, None),
    ("fp <-> integer", 0x7F20FC00, 0x1E200000, None),
    ("fp <-> fixed point", 0x7F200000, 0x1E000000, None),
    # advanced SIMD
    ("advsimd three same", 0x9F200400, 0x0E200400, None),
    ("advsimd scalar three same", 0xDF200400, 0x5E200400, None),
    ("advsimd three different", 0x9F200C00, 0x0E200000, None),
    ("advsimd two-register misc", 0x9F3E0C00, 0x0E200800, None),
    ("advsimd scalar two-register misc", 0xDF3E0C00, 0x5E200800, None),
    ("advsimd across lanes", 0x9F3E0C00, 0x0E300800, None),
    ("advsimd scalar pairwise", 0xDF3E0C00, 0x5E300800, None),
    ("advsimd copy", 0x9FE08400, 0x0E000400, None),
    ("advsimd scalar copy", 0xDFE08400, 0x5E000400, None),
    ("advsimd permute", 0xBF208C00, 0x0E000800, None),
    ("advsimd extract", 0xBFE08400, 0x2E000000, None),
    ("advsimd table lookup", 0xBFE08C00, 0x0E000000, None),
    ("advsimd modified immediate", 0x9FF80400, 0x0F000400, None),
    ("advsimd shift by immediate", 0x9F800400, 0x0F000400, fix_shift_imm),
    ("advsimd scalar shift by immediate", 0xDF800400, 0x5F000400, fix_shift_imm),
    ("advsimd vector x indexed element", 0x9F000400, 0x0F000000, None),
    ("advsimd scalar x indexed element", 0xDF000400, 0x5F000000, None),
]


def draw(entry, rng):
    """One word of a class (or None: dropped by its fix-up)."""
    if callable(entry[1]):
        return entry[1](rng)
    _name, mask, value, fix = entry
    w = value | (rng.getrandbits(32) & ~mask & 0xFFFFFFFF)
    return fix(w, rng) if fix else w


# ---------------------------------------------------------------------------- edge cases
def logical_immediates(rng):
    """decode_bit_masks at its edges: every element size, the smallest and largest run of
    ones, rotations 0, 1 and esize-1, and immr bits above the element size (ignored)."""
    out = []
    for sf in (0, 1):
        for length in range(1, 7 if sf else 6):
            esize, levels = 1 << length, (1 << length) - 1
            for s in sorted({0, 1, esize // 2 - 1, esize - 2}):
                if s >= levels:
                    continue
                for r in sorted({0, 1, esize - 1}):
                    n = 1 if length == 6 else 0
                    imms = s if length == 6 else (0x3F & ~((1 << (length + 1)) - 1)) | s
                    immr = r | (rng.getrandbits(6) & ~levels & 0x3F)
                    rd, rn = regs(rng, 2)
                    opc = len(out) & 3
                    rd = 31 if len(out) % 7 == 0 else rd   # sp (and, orr, eor) or xzr (ands: tst)
                    out.append(sf << 31 | opc << 29 | 0x12000000 | n << 22 | immr << 16 | imms << 10 | rn << 5 | rd)
    return out


def shifted_registers(rng):
    out = []
    for sf in (0, 1):
        size = 32 << sf
        for kind in range(4):
            for amount in (0, 1, size - 1):
                rd, rn, rm = regs(rng, 3)
                opc, n = rng.randrange(4), rng.randrange(2)
                out.append(sf << 31 | opc << 29 | 0x0A000000 | kind << 22 | n << 21 | rm << 16 | amount << 10 | rn << 5 | rd)
                if kind < 3:
                    op, s = rng.randrange(2), rng.randrange(2)
                    out.append(sf << 31 | op << 30 | s << 29 | 0x0B000000 | kind << 22 | rm << 16 | amount << 10 | rn << 5 | rd)
    return out


def extended_registers(rng):
    """add/sub (extended register): every extend and shift, with sp as base and destination."""
    out = []
    for sf in (0, 1):
        for option in range(8):
            for shift in (0, rng.randrange(1, 5), 4):
                rd, rn, rm = regs(rng, 3)
                op, s = rng.randrange(2), rng.randrange(2)
                if rng.random() < 0.3:
                    rn = 31
                if not s and rng.random() < 0.3:
                    rd = 31
                out.append(sf << 31 | op << 30 | s << 29 | 0x0B200000 | rm << 16 | option << 13 | shift << 10 | rn << 5 | rd)
    return out


def bitfields(rng):
    out = []
    for sf in (0, 1):
        size = 32 << sf
        for opc in (0, 1, 2):   # sbfm, bfm, ubfm
            for immr, imms in ((0, 0), (0, size - 1), (size - 1, size - 1), (1, 0), (size - 1, 0), (size - 1, size - 2),
                               (size // 2, size // 2 - 1), (0, 7), (0, 15), (0, 31)):
                rd, rn = regs(rng, 2)
                out.append(sf << 31 | opc << 29 | 0x13000000 | sf << 22 | immr << 16 | imms << 10 | rn << 5 | rd)
        for imms in (0, 1, size - 1):   # extr (and ror, rn = rm)
            for same in (0, 1):
                rd, rn, rm = regs(rng, 3)
                rm = rn if same else rm
                out.append(sf << 31 | 0x13800000 | sf << 22 | rm << 16 | imms << 10 | rn << 5 | rd)
    return out


def move_wide(rng):
    out = []
    for sf in (0, 1):
        for opc in (0, 2, 3):   # movn, movz, movk
            for hw in range(4 if sf else 2):
                for imm16 in (0, 0xFFFF, rng.getrandbits(16)):
                    out.append(sf << 31 | opc << 29 | 0x12800000 | hw << 21 | imm16 << 5 | rng.randrange(31))
    return out


def conditionals(rng):
    """The csel family and ccmp / ccmn, including the always conditions (AL, NV)."""
    out = []
    for sf in (0, 1):
        for cond in (rng.randrange(14), 14, 15):
            for op in range(2):
                for op2 in range(2):
                    rd, rn, rm = regs(rng, 3)
                    out.append(sf << 31 | op << 30 | 0x1A800000 | rm << 16 | cond << 12 | op2 << 10 | rn << 5 | rd)
                for imm in range(2):
                    rn, rm = regs(rng, 2)
                    out.append(sf << 31 | op << 30 | 0x3A400000 | rm << 16 | cond << 12 | imm << 11 | rn << 5 | rng.getrandbits(4))
    return out


def data_processing(rng):
    """Every one- and two-source operation the lifter has, and the three-source family with
    and without an accumulator (ra = xzr: mul, mneg, smull ...)."""
    out = []
    for sf in (0, 1):
        for opcode in (2, 3, 8, 9, 10, 11):   # udiv sdiv lslv lsrv asrv rorv
            rd, rn, rm = regs(rng, 3)
            out.append(sf << 31 | 0x1AC00000 | rm << 16 | opcode << 10 | rn << 5 | rd)
        for opcode in range(6):               # rbit rev16 rev32/rev rev clz cls
            if opcode == 3 and not sf:
                continue
            rd, rn = regs(rng, 2)
            out.append(sf << 31 | 0x5AC00000 | opcode << 10 | rn << 5 | rd)
    for sf, op31, o0 in ((0, 0, 0), (0, 0, 1), (1, 0, 0), (1, 0, 1), (1, 1, 0), (1, 1, 1), (1, 5, 0), (1, 5, 1), (1, 2, 0), (1, 6, 0)):
        for ra in ((31,) if op31 in (2, 6) else (31, None)):   # (smulh / umulh: ra is all ones)
            rd, rn, rm, r = regs(rng, 4)
            out.append(sf << 31 | 0x1B000000 | op31 << 21 | rm << 16 | o0 << 15 | (r if ra is None else ra) << 10 | rn << 5 | rd)
    return out


def crc32(rng):
    """crc32b/h/w/x and crc32cb/ch/cw/cx with distinct registers, rd = rn, rd = rm, rn = rm, and wzr / xzr
    as operands; and the unallocated pairings of sf and size (x with a byte, half or word; w with x)."""
    out = []
    for c in (0, 1):
        for sz in range(4):
            for sf in (0, 1):
                rd, rn, rm = regs(rng, 3)
                cases = [(rd, rn, rm)]
                if sf == (sz == 3):
                    cases += [(rd, rd, rm), (rd, rn, rd), (rd, rn, rn), (rd, 31, rm), (rd, rn, 31)]
                for d, n, m in cases:
                    out.append(sf << 31 | 0x1AC04000 | m << 16 | c << 12 | sz << 10 | n << 5 | d)
    return out


def loads_stores(rng):
    """Register-offset forms with every extend and scale, for every size, sign and register file."""
    out = []
    for size in range(4):
        for vec in (0, 1):
            for opc in range(4):
                if vec and opc >= 2 and size:
                    continue
                if not vec and ((opc == 3 and size >= 2) or (opc == 2 and size == 3)):
                    continue   # (reserved; prfm)
                for option in (2, 3, 6, 7):
                    for s in (0, 1):
                        rn, rm, rt = regs(rng, 3)
                        out.append(size << 30 | 0x38200800 | vec << 26 | opc << 22 | rm << 16 | option << 13 | s << 12 | rn << 5 | rt)
    return out


def fp_scalar(rng):
    out = []
    for ftype in (0, 1):
        for imm8 in (0x00, 0x70, 0x7F, 0x80, 0xF0, rng.getrandbits(8)):   # fmov (immediate)
            out.append(0x1E201000 | ftype << 22 | imm8 << 13 | rng.randrange(32))
        for sf in (0, 1):   # fixed-point conversions at their scale limits
            for opcode, rmode in ((2, 0), (3, 0), (0, 3), (1, 3)):
                for fbits in sorted({1, 2, 16, 31, 32} | ({33, 63, 64} if sf else set())):
                    out.append(sf << 31 | 0x1E000000 | ftype << 22 | rmode << 19 | opcode << 16 | (64 - fbits) << 10
                               | rng.randrange(32) << 5 | rng.randrange(32))
        for opcode in (8, 9, 10, 11, 12, 14, 15):   # frintn frintp frintm frintz frinta frintx frinti
            out.append(0x1E204000 | ftype << 22 | opcode << 15 | rng.randrange(32) << 5 | rng.randrange(32))
        for e in (0, 1):    # fcmp / fcmpe, with a register and with zero
            for zero in (0, 1):
                out.append(0x1E202000 | ftype << 22 | rng.randrange(32) << 16 | rng.randrange(32) << 5 | e << 4 | zero << 3)
    for ftype, opcode in ((0, 5), (1, 4), (3, 4), (3, 5), (0, 7), (1, 7)):   # fcvt: s->d d->s h->s h->d s->h d->h
        for _ in range(2):
            out.append(0x1E204000 | ftype << 22 | opcode << 15 | rng.randrange(32) << 5 | rng.randrange(32))
    return out


def simd_shifts(rng):
    """Shift by immediate at the shift limits: 1, 2, half, esize-1 and esize to the right; 0, 1
    and esize-1 to the left; vector and scalar."""
    out = []
    kinds = [(0x00, (0, 1), "right"), (0x02, (0, 1), "right"), (0x0A, (0,), "left"), (0x14, (0, 1), "long"),
             (0x10, (0,), "narrow"), (0x1C, (0, 1), "fixed"), (0x1F, (0, 1), "fixed")]
    for opcode, us, kind in kinds:
        for u in us:
            for esize in (8, 16, 32, 64):
                if kind in ("long", "narrow") and esize == 64:
                    continue
                if kind == "fixed" and esize < 32:
                    continue
                left = kind in ("left", "long")
                amounts = sorted({0, 1, esize - 1}) if left else sorted({1, 2, esize // 2, esize - 1, esize})
                for amount in amounts:
                    immhb = esize + amount if left else 2 * esize - amount
                    for q in (0, 1):
                        if esize == 64 and not q:
                            continue
                        out.append(q << 30 | u << 29 | 0x0F000400 | immhb << 16 | opcode << 11 | rng.randrange(32) << 5 | rng.randrange(32))
                    if kind in ("right", "left") and esize == 64 or kind == "fixed":
                        out.append(u << 29 | 0x5F000400 | immhb << 16 | opcode << 11 | rng.randrange(32) << 5 | rng.randrange(32))
    return out


def simd_round_insert(rng):
    """srshr / urshr / srsra / ursra and rshrn / rshrn2 by 1, 2, half, esize-1 and esize; sri by every
    amount (1 to esize), sli by every amount (0 to esize-1); vector at both widths and scalar (d), every
    other amount with rd = rn."""
    out = []
    for opcode, u in ((0x04, 0), (0x04, 1), (0x06, 0), (0x06, 1), (0x08, 1), (0x0A, 1), (0x11, 0)):
        for esize in (8, 16, 32, 64):
            if opcode == 0x11 and esize == 64:
                continue
            amounts = (range(esize) if opcode == 0x0A else range(1, esize + 1) if opcode == 0x08 else
                       sorted({1, 2, esize // 2, esize - 1, esize}))
            for k, amount in enumerate(amounts):
                immhb = esize + amount if opcode == 0x0A else 2 * esize - amount
                rd = rng.randrange(32)
                rn = rd if k % 2 else rng.randrange(32)
                for q in (0, 1):
                    if q or esize < 64:
                        out.append(q << 30 | u << 29 | 0x0F000400 | immhb << 16 | opcode << 11 | rn << 5 | rd)
                if esize == 64 and opcode != 0x11:
                    out.append(u << 29 | 0x5F000400 | immhb << 16 | opcode << 11 | rn << 5 | rd)
    return out


def simd_pairwise_long(rng):
    """saddlp / uaddlp / sadalp / uadalp: every element size (3 is reserved) at both widths, with rd = rn too."""
    out = []
    for opcode in (2, 6):
        for u in (0, 1):
            for size in range(4):
                for q in (0, 1):
                    rd, rn = rng.randrange(32), rng.randrange(32)
                    for n in (rn, rd):
                        out.append(q << 30 | u << 29 | 0x0E200800 | size << 22 | opcode << 12 | n << 5 | rd)
    return out


def simd_permutes(rng):
    """ext at every byte position, tbl / tbx of one to four registers (wrapping past v31),
    and every element copy at its first and last index."""
    out = []
    for q in (0, 1):
        for imm4 in range(8 << q):
            out.append(q << 30 | 0x2E000000 | rng.randrange(32) << 16 | imm4 << 11 | rng.randrange(32) << 5 | rng.randrange(32))
        for length in range(4):
            for op in (0, 1):
                for rn in (rng.randrange(32), 30, 31):
                    out.append(q << 30 | 0x0E000000 | rng.randrange(32) << 16 | length << 13 | op << 12 | rn << 5 | rng.randrange(32))
    for size in range(4):
        for index in (0, (16 >> size) - 1):
            imm5 = index << (size + 1) | 1 << size
            rn, rd = rng.randrange(32), rng.randrange(32)
            for q in (0, 1):
                if size == 3 and not q:
                    continue
                out.append(q << 30 | 0x0E000400 | imm5 << 16 | rn << 5 | rd)                      # dup (element)
                out.append(q << 30 | 0x0E000400 | imm5 << 16 | 1 << 11 | rng.randrange(31) << 5 | rd)   # dup (general)
            out.append(1 << 30 | 0x0E000400 | imm5 << 16 | 3 << 11 | rng.randrange(31) << 5 | rd)  # ins (general)
            out.append((size == 3) << 30 | 0x0E000400 | imm5 << 16 | 7 << 11 | rn << 5 | rng.randrange(31))   # umov
            if size < 3:                                                                          # smov
                for q in ((0, 1) if size < 2 else (1,)):
                    out.append(q << 30 | 0x0E000400 | imm5 << 16 | 5 << 11 | rn << 5 | rng.randrange(31))
            for index2 in (0, (16 >> size) - 1):                                                  # ins (element)
                out.append(1 << 30 | 1 << 29 | 0x0E000400 | imm5 << 16 | (index2 << size) << 11 | rn << 5 | rd)
            out.append(0x5E000400 | imm5 << 16 | rn << 5 | rd)                                    # dup (scalar)
    return out


def simd_structures(rng):
    """ld1-ld4 / st1-st4, whole registers and single lanes, with the register list wrapping past
    v31, offset and post-indexed by immediate and by register."""
    out = []
    for rt in (29, 30, 31):
        for opcode in (0, 2, 4, 6, 7, 8, 10):   # ld4 ld1x4 ld3 ld1x3 ld1 ld2 ld1x2
            for load in (0, 1):
                post, rm = rng.choice(((0, 0), (1, 31), (1, rng.randrange(31))))
                rn = rng.randrange(31) if post and rm != 31 else rng.randrange(32)
                rn = rn if rn != rm else (rm + 1) % 31
                out.append(1 << 30 | 0x0C000000 | post << 23 | load << 22 | rm << 16 | opcode << 12
                           | rng.randrange(4) << 10 | rn << 5 | rt)
        # single lanes: ld4 / st4 of the last byte, ld3 / st3 of a half, ld2 of a double, ld4r / ld3r
        for opcode, r, s, size, load in ((1, 1, 1, 3, 1), (1, 1, 1, 3, 0), (3, 0, 1, 2, 1), (3, 0, 0, 0, 0),
                                         (4, 1, 0, 1, 1), (7, 1, 0, 2, 1), (7, 0, 0, 3, 1), (6, 1, 0, 0, 1)):
            post, rm = rng.choice(((0, 0), (1, 31), (1, rng.randrange(31))))
            rn = rng.randrange(31)
            rn = rn if rn != rm else (rm + 1) % 31
            out.append(1 << 30 | 0x0D000000 | post << 23 | load << 22 | r << 21 | rm << 16 | opcode << 13 | s << 12
                       | size << 10 | rn << 5 | rt)
    return out


def simd_immediates(rng):
    """movi / mvni / orr / bic / fmov (vector, immediate): every cmode, both ops, both widths."""
    out = []
    for op in (0, 1):
        for cmode in range(16):
            for q in (0, 1):
                if op and cmode == 15 and not q:
                    continue
                for imm8 in (0x00, 0xFF, rng.getrandbits(8)):
                    out.append(q << 30 | op << 29 | 0x0F000400 | (imm8 >> 5) << 16 | cmode << 12 | (imm8 & 31) << 5 | rng.randrange(32))
    return out


EDGES = [
    ("logical immediate", logical_immediates),
    ("shifted register", shifted_registers),
    ("extended register", extended_registers),
    ("bitfield / extract", bitfields),
    ("move wide", move_wide),
    ("conditional select / compare", conditionals),
    ("data processing", data_processing),
    ("crc32", crc32),
    ("register offset", loads_stores),
    ("fp scalar", fp_scalar),
    ("advsimd shift by immediate", simd_shifts),
    ("advsimd rounding shift / insert / rshrn", simd_round_insert),
    ("advsimd pairwise add long", simd_pairwise_long),
    ("advsimd permute / copy", simd_permutes),
    ("advsimd load / store structures", simd_structures),
    ("advsimd modified immediate", simd_immediates),
]


# ---------------------------------------------------------------------------- sequences
def exclusive_sequences(rng):
    """The exclusive monitor: a store-exclusive after a load-exclusive of the same address
    succeeds; after a different address, a changed value or a clrex it fails, and alone it
    fails (the reference's monitor is cleared before each run: difftest._run). Every size,
    with and without acquire / release, and with sp as the base."""
    ldxr = lambda size, o0, rt, rn: size << 30 | 0x08400000 | 31 << 16 | o0 << 15 | 31 << 10 | rn << 5 | rt
    stxr = lambda size, o0, rs, rt, rn: size << 30 | 0x08000000 | rs << 16 | o0 << 15 | 31 << 10 | rn << 5 | rt
    out = []
    for size in range(4):
        for o0l, o0s in ((0, 0), (1, 1), (0, 1), (1, 0)):
            rn, rt, rs, a = regs(rng, 4)
            if size == 3 and o0l == 1 and o0s == 0:
                rn = 31                                                       # sp
            ld, st = ldxr(size, o0l, rt, rn), stxr(size, o0s, rs, a, rn)
            out.append(("ldxr; stxr", (ld, st)))                              # succeeds
            out.append(("ldxr; add; stxr", (ld, 0x91004000 | rn << 5 | rn, st)))   # add base, base, #16: fails
            out.append(("ldxr; str; stxr", (ld, size << 30 | 0x39000000 | rn << 5 | a, stxr(size, o0s, rs, rt, rn))))   # value changed
            out.append(("ldxr; clrex; stxr", (ld, 0xD503305F, st)))          # clrex: fails
            out.append(("stxr", (st,)))                                       # no reservation: fails
    return out


def exclusive_pair_sequences(rng):
    """The same for ldxp / stxp, of two W registers (one 8-byte location) and two X registers
    (16 bytes): alone, a load and a store, a store to another address, either half changed in
    between (by a str), a clrex in between, a second store after one that succeeded and wrote
    back what was read, a store alone. With and without acquire / release, and with sp as the
    base."""
    ldxp = lambda sz, o0, rt, rt2, rn: 1 << 31 | sz << 30 | 0x08600000 | 31 << 16 | o0 << 15 | rt2 << 10 | rn << 5 | rt
    stxp = lambda sz, o0, rs, rt, rt2, rn: 1 << 31 | sz << 30 | 0x08200000 | rs << 16 | o0 << 15 | rt2 << 10 | rn << 5 | rt
    out = []
    for sz in (0, 1):
        for o0l, o0s in ((0, 0), (1, 1), (0, 1), (1, 0)):
            rn, rt, rt2, rs, rs2, a, b = regs(rng, 7)
            if o0l == 1 and o0s == 0:
                rn = 31                                                       # sp
            ld, st = ldxp(sz, o0l, rt, rt2, rn), stxp(sz, o0s, rs, a, b, rn)
            low = 0xF9000000 | rn << 5 | a                                    # str xa, [base]
            high = (0xF9000400 if sz else 0xB9000400) | rn << 5 | a           # str xa, [base, #8] / str wa, [base, #4]
            again = stxp(sz, o0s, rs2, rt, rt2, rn)                           # (stores what was loaded)
            out.append(("ldxp", (ld,)))
            out.append(("ldxp; stxp", (ld, st)))                              # succeeds
            out.append(("ldxp; add; stxp", (ld, 0x91004000 | rn << 5 | rn, st)))   # add base, base, #16: fails
            out.append(("ldxp; str; stxp", (ld, low, again)))                 # the first doubleword changed: fails
            out.append(("ldxp; str; stxp (high)", (ld, high, again)))         # the high half changed: fails
            out.append(("ldxp; clrex; stxp", (ld, 0xD503305F, st)))          # clrex: fails
            out.append(("ldxp; stxp; stxp", (ld, again, st)))                 # succeeds, memory as it was; then fails: the monitor went
            out.append(("stxp", (st,)))                                       # no reservation: fails
    return out


SEQUENCES = [("exclusive monitor", exclusive_sequences), ("exclusive pairs", exclusive_pair_sequences)]
