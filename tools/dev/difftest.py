#!/usr/bin/env python3
"""Differential test of the lifter against a reference AArch64 emulator (unicorn).

Usage: difftest.py <nso> [--per-template K] [--states R] [--jobs N] [--only <mnemonic,...>] [--show N]
       difftest.py --synthetic [--per-template K] [--states R] [--seed S] [--draws D] [--jobs N] [--only ...] [--show N]
       common: [--opt O2[,O0]] [--exact-nan] [--toolchain windows|linux|mac|rosetta] [--work DIR] [--report FILE]

Every instruction in the module is lifted; the resulting C statements are grouped by
"template" (the statement with numbers abstracted away), and K distinct real encodings
per template are executed from R random CPU states both natively (the lifted C compiled
for this machine) and under unicorn. All registers, flags, the next PC and memory writes must match.

--synthetic needs no game: the instruction words are made from the architecture's bit fields
(difftest_words.py: D random words inside each encoding class, K per template kept, plus edge
cases and exclusive-monitor sequences), placed in an image of their own, and the floating-point
operands of the states are often special values (F32_BITS ...), in some states all of them the
ones of the NaN rules (F32_NAN ...). tests/test_difftest_synthetic.py runs it in CI on Windows,
Linux and macOS.

The native side is built with the game build's compiler and flags for this OS (--toolchain):
clang-cl and tools/cc.py's flags on Windows, tools/linux/toolchain.py's on Linux (x86-64),
tools/mac/toolchain.py's on macOS (arm64); "rosetta", on an Apple silicon Mac, is the Linux
build's x86-64 flags with Apple's clang, run under Rosetta 2. --opt lists the optimisation
levels to build it at (the game's is O2; the synthetic mode also runs O0 by default).

Floating-point results that differ only in NaN sign or payload (NaN on both sides, in the lanes
of the result register at its lane width, both read from the disassembly) are "soft": reported,
not counted as mismatches, unless --exact-nan. That also builds the native side with
RT_EXACT_NAN=1, the x86 builds' NaN checks (runtime/recomp.h), which arm64 builds leave out.
--report writes the results as JSON.
Exit code: 0 no mismatches, 1 mismatches, 3 no native toolchain on this machine.
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))   # tools/, which this builds on
import importlib.util
import json
import os
import platform
import random
import re
import shutil
import struct
import subprocess
import sys
import time
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from multiprocessing import Pool

import cc
import recomp
from lift import BASE, Lifter, Unsupported, const, reg

SCRATCH, SCRATCH_SIZE = 0x7200000000, 0x20000
RESET = 0x7300000000   # the reference's own page: a clrex, run before each test with a store-exclusive
NUM = re.compile(r"(?<![A-Za-z_])(?:0x[0-9A-Fa-f]+(?:ull|u)?|\d+)")
VEC_NUM = re.compile(r"(?<=c->v\[)\d+|(?<=X\()\d+|0x[0-9A-Fa-f]+(?:ull|u)?")
IN_FMT, OUT_FMT = "<II31QQQ64Q", "<31QQQ64QQQQ64sQ"


def template(stmt):
    """Statement shape used to bucket encodings. Vector statements keep lane counts, lane
    indices and shift amounts, since those select different behaviour, not just operands."""
    return (VEC_NUM if "c->v[" in stmt or "rt_" in stmt else NUM).sub("#", stmt)


OUT_SIZE = struct.calcsize(OUT_FMT)
EDGE = [0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
        0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF00000000,
        0xFFFFFFFF80000000, 0x5555555555555555, 0xAAAAAAAAAAAAAAAA]
F32_SPECIAL = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1e-40, 3.4e38, float("inf"), float("-inf"), float("nan"), 1e10, -1e10,
               2147483648.0, -2147483904.0, 4294967296.0, 0.49999997, 1.5, 2.5, -0.5, -1.5, -2.5, 16777216.0]
# Floating-point operands random states seldom hit (synthetic mode, rand_v_fp): signed zeros,
# infinities, quiet and signalling NaNs with payloads, denormals, the ends of the normal range,
# integer-conversion limits and rounding ties (to integer, and to half precision). Generic IEEE
# 754 values, as bit patterns.
F32_BITS = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0xBF000000,   # +-0, +-1, +-0.5
            0x3FC00000, 0xC0200000, 0x7F800000, 0xFF800000,                           # 1.5, -2.5, +-inf
            0x7FC00000, 0xFFC00000, 0x7FC12345, 0xFFD55555,                           # quiet NaNs (default, payloads)
            0x7F800001, 0xFFA00000, 0x7FBFFFFF,                                       # signalling NaNs
            0x00000001, 0x807FFFFF, 0x00400000, 0x00800000, 0x7F7FFFFF, 0xFF7FFFFF,   # denormals, smallest normal, largest
            0x4F000000, 0xCF000000, 0x4EFFFFFF, 0x4F800000,                           # 2^31, -2^31, below 2^31, 2^32
            0x5F000000, 0xDF000000, 0x5F800000, 0xBF7FFFFF,                           # 2^63, -2^63, 2^64, just above -1
            0x477FE000, 0x477FF000, 0x477FEFFF,                                       # half: 65504, 65520 (to inf), below
            0x33000000, 0x33000001, 0x387FFFFF, 0x38800000, 0x3F801000, 0x3F803000]   # half: denormal / normal edges, ties
F64_BITS = [0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000,   # +-0, +-1
            0x3FE0000000000000, 0xBFE0000000000000, 0x3FF8000000000000, 0xC004000000000000,   # +-0.5, 1.5, -2.5
            0x7FF0000000000000, 0xFFF0000000000000,                                           # +-inf
            0x7FF8000000000000, 0xFFF8000000000000, 0x7FF8000000012345, 0xFFFAAAAAAAAAAAAA,   # quiet NaNs
            0x7FF0000000000001, 0xFFF4000000000000, 0x7FF7FFFFFFFFFFFF,                       # signalling NaNs
            0x0000000000000001, 0x800FFFFFFFFFFFFF, 0x0008000000000000,                       # denormals
            0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF,                       # smallest normal, largest
            0x41E0000000000000, 0xC1E0000000000000, 0x41DFFFFFFFC00000, 0xC1E0000000200000,   # 2^31, -2^31, 2^31-1, -2^31-1
            0x41F0000000000000, 0x43E0000000000000, 0xC3E0000000000000, 0x43F0000000000000,   # 2^32, 2^63, -2^63, 2^64
            0x47EFFFFFE0000000, 0x47EFFFFFF0000000, 0x36A0000000000000, 0x380FFFFFFFFFFFFF,   # f32: largest, to inf, ties, denormal
            0x40EFFC0000000000, 0x40EFFE0000000000, 0x3E60000000000000, 0x3E60000000000001]   # half: 65504, 65520, ties
# The NaN rules' operands (synthetic mode, rand_v_nan): zeros, infinities and ones, which make the
# invalid operations (0 / 0, inf - inf, 0 * inf, the square root of -1), and quiet and signalling
# NaNs of both signs, with and without payloads: two NaN operands, a NaN with inf * 0, a NaN and a
# negated one.
F32_NAN = [0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x3F800000, 0xBF800000,
           0x7FC00000, 0xFFC00000, 0x7FC12345, 0xFFD55555, 0x7F800001, 0xFFA00000, 0x7FBFFFFF]
F64_NAN = [0x0000000000000000, 0x8000000000000000, 0x7FF0000000000000, 0xFFF0000000000000,
           0x3FF0000000000000, 0xBFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000,
           0x7FF8000000012345, 0xFFFAAAAAAAAAAAAA, 0x7FF0000000000001, 0xFFF4000000000000, 0x7FF7FFFFFFFFFFFF]
F16_BITS = [0x0000, 0x8000, 0x3C00, 0xBC00, 0x7C00, 0xFC00, 0x7E00, 0xFE00, 0x7E01, 0x7C01, 0xFD55,   # zeros, ones, infs, NaNs
            0x0001, 0x03FF, 0x8200, 0x0400, 0x7BFF, 0x3555, 0xC123]                                   # denormals, normals


class TestLifter(Lifter):
    """Control transfers record the next guest PC instead of transferring."""

    def goto(self, target):
        return f"{{ t_pc = {const(self.base + target)}; goto T_done; }}"

    def call(self, target, ret):
        return f"{{ X(30) = {const(self.base + ret)}; t_pc = {const(self.base + target)}; goto T_done; }}"

    def call_indirect(self, rn, ret):
        return f"{{ t_pc = {reg(rn)}; X(30) = {const(self.base + ret)}; goto T_done; }}"

    def ret(self, rn):
        return f"{{ t_pc = {reg(rn)}; goto T_done; }}"

    def indirect_jump(self, rn):
        return f"{{ t_pc = {reg(rn)}; goto T_done; }}"

    def snippet(self, start, n):
        stmts = []
        for i in range(n):
            pc = start + i * 4
            stmt, _falls = self.insn(struct.unpack_from("<I", self.img, pc)[0], pc)
            if any(s in stmt for s in ("rt_trap", "c->tpidr", "c->fp", "rt_svc", "rt_ticks", "memset(")):
                raise Unsupported("not testable in isolation")
            if "T_done" in stmt and i != n - 1:
                raise Unsupported("control flow inside snippet")
            stmts.append(stmt)
        return stmts


def mem_regs(w):
    """(base, index) register numbers an instruction uses for addressing, if any."""
    if w & 0xFFFFFFE0 == 0xD50B7420:   # dc zva
        return w & 31, None
    if (w >> 25) & 5 != 4:
        return None, None
    group = (w >> 27) & 7
    if group == 3 and not (w >> 24) & 3:
        return None, None
    index = None
    if group == 7 and not (w >> 24) & 1 and (w >> 21) & 1 and (w >> 10) & 3 == 2:
        index = (w >> 16) & 31
    return (w >> 5) & 31, index


def store_exclusive(w):
    return w & 0x3FC00000 == 0x08000000   # stxr, stlxr (and the pairs)


# ---------------------------------------------------------------------------- sampling
_state = {}


def _init(image_path, text_size):
    with open(image_path, "rb") as f:
        _state["img"] = f.read(text_size)
    _state["image_path"] = image_path


def _sample(args):
    funcs, per_template = args
    img = _state["img"]
    lifter = TestLifter(img, set())
    rng = random.Random(funcs[0][0])
    seen = {}
    for start, size in funcs:
        for i, (w,) in enumerate(struct.iter_unpack("<I", img[start:start + size])):
            pc = start + i * 4
            try:
                stmt = lifter.snippet(pc, 1)[0]
            except Unsupported:
                continue
            if stmt == ";" or "cas" in stmt:   # nothing to observe / lone store-exclusive
                continue
            slot = seen.setdefault(template(stmt), [0, {}])
            slot[0] += 1
            if w in slot[1]:
                continue
            if len(slot[1]) < per_template:
                slot[1][w] = pc
            elif rng.randrange(slot[0]) < per_template:
                del slot[1][rng.choice(list(slot[1]))]
                slot[1][w] = pc
    return {k: (n, list(d.items())) for k, (n, d) in seen.items()}


def exclusive_snippets(img, funcs, limit):
    """Straight-line load-exclusive ... store-exclusive sequences, to test the success path."""
    out = []
    for start, size in funcs:
        words = struct.unpack_from(f"<{size // 4}I", img, start)
        for i, w in enumerate(words):
            if w & 0x3FE08000 == 0x08408000 or w & 0x3FE08000 == 0x08400000:   # ldaxr / ldxr
                for n in range(2, 5):
                    if i + n > len(words):
                        break
                    last = words[i + n - 1]
                    if last & 0x3FE00000 == 0x08000000 and (last >> 5) & 31 == (w >> 5) & 31:
                        if all((x >> 26) & 0x1F not in (5, 13, 21) and x >> 24 != 0x54 for x in words[i + 1:i + n - 1]):
                            out.append((start + i * 4, n))
                        break
                if len(out) >= limit:
                    return out
    return out


# ---------------------------------------------------------------------------- execution
def rand_x(rng):
    r = rng.random()
    if r < 0.45:
        return rng.getrandbits(64)
    if r < 0.65:
        return rng.choice(EDGE)
    if r < 0.85:
        return rng.getrandbits(8)
    return rng.getrandbits(32) | (0xFFFFFFFF00000000 if rng.getrandbits(1) else 0)


def rand_v(rng):
    r = rng.random()
    if r < 0.35:
        return struct.unpack("<QQ", struct.pack("<4f", *(round(rng.uniform(-64, 64), rng.randrange(4)) for _ in range(4))))
    if r < 0.55:
        return struct.unpack("<QQ", struct.pack("<2d", *(round(rng.uniform(-64, 64), rng.randrange(4)) for _ in range(2))))
    if r < 0.70:
        return struct.unpack("<QQ", struct.pack("<4f", *(rng.choice(F32_SPECIAL) for _ in range(4))))
    if r < 0.80:
        return struct.unpack("<QQ", struct.pack("<2d", *(rng.choice(F32_SPECIAL) for _ in range(2))))
    if r < 0.86:
        return rng.choice(EDGE), rng.choice(EDGE)
    if r < 0.91:   # small signed 32-bit lanes
        return struct.unpack("<QQ", struct.pack("<4i", *(rng.randrange(-40, 41) for _ in range(4))))
    if r < 0.95:   # small bytes: shift counts, table indices
        return struct.unpack("<QQ", bytes(rng.randrange(-9, 40) & 0xFF for _ in range(16)))
    return rng.getrandbits(64), rng.getrandbits(64)


def rand_v_fp(rng):
    """rand_v, or more often than not lanes of one width (16, 32 or 64 bits) drawn mostly from
    the special values."""
    if rng.random() < 0.4:
        return rand_v(rng)
    width = rng.choice((16, 32, 32, 64, 64))
    table = {16: F16_BITS, 32: F32_BITS, 64: F64_BITS}[width]
    plain = {16: lambda: rng.getrandbits(16),
             32: lambda: struct.unpack("<I", struct.pack("<f", round(rng.uniform(-64, 64), 2)))[0],
             64: lambda: struct.unpack("<Q", struct.pack("<d", round(rng.uniform(-64, 64), 2)))[0]}[width]
    v = 0
    for i in range(128 // width):
        v |= (rng.choice(table) if rng.random() < 0.75 else plain()) << (i * width)
    return v & 0xFFFFFFFFFFFFFFFF, v >> 64


def rand_v_nan(rng, width):
    """Lanes of one width (32 or 64 bits), mostly the NaN rules' operands."""
    table = F32_NAN if width == 32 else F64_NAN
    plain = (lambda: struct.unpack("<I", struct.pack("<f", round(rng.uniform(-64, 64), 2)))[0]) if width == 32 else \
        (lambda: struct.unpack("<Q", struct.pack("<d", round(rng.uniform(-64, 64), 2)))[0])
    v = 0
    for i in range(128 // width):
        v |= (rng.choice(table) if rng.random() < 0.85 else plain()) << (i * width)
    return v & 0xFFFFFFFFFFFFFFFF, v >> 64


NAN = {16: (0x7C00, 0x3FF), 32: (0x7F800000, 0x7FFFFF), 64: (0x7FF0000000000000, 0xFFFFFFFFFFFFF)}   # exponent, fraction


def nan_only(a, b, width=None):
    """True if two 64-bit values differ only in lanes of `width` bits that are NaN on both sides.
    Without a width (functest: a function's results have no one width), as 32-bit halves or whole."""
    if width is None:
        return nan_only(a, b, 32) or nan_only(a, b, 64)
    exp, frac = NAN[width]
    mask = (1 << width) - 1
    for i in range(0, 64, width):
        x, y = a >> i & mask, b >> i & mask
        if x != y and not (x & exp == exp and x & frac and y & exp == exp and y & frac):
            return False
    return True


# A floating-point result in a vector register: an f... mnemonic other than the conversions to
# integers, the compares, fcsel and fmov, and its first operand (d0, s0, h0, v0.2d, v0.4s, v0.8h, v0.s[1]).
FP_RESULT = re.compile(r"f(?!cvt[amnpz][su]|jcvt|cm|ac|ccmp|csel|mov)\w* (?:([hsd])(\d+)|v(\d+)\.\d*([hsd]))\b")
LANE_BITS = {"h": 16, "s": 32, "d": 64}


def fp_result(texts):
    """(register, lane width) of the floating-point result a test leaves in a vector register, from
    the disassembly of its instructions; None if it has none. Only that register's lanes, at that
    width, can differ softly: an integer that looks like a NaN (-1 does, as an f32) is a number."""
    found = set()
    for m in map(FP_RESULT.match, texts):
        if m:
            found.add((int(m[2] or m[3]), LANE_BITS[m[1] or m[4]]))
    return found.pop() if len(found) == 1 else None


def compare(expected, raw, exact_nan):
    """(native faults, hard mismatches, soft NaN-only differences by test) of one native run."""
    hard, soft, faults = [], Counter(), 0
    for k, (idx, ex, esp, enzcv, ev, epc, emem, inputs, fp) in enumerate(expected):
        o = struct.unpack_from(OUT_FMT, raw, k * OUT_SIZE)
        gx, gsp, gnzcv, gv, gpc, goff, glen, gbytes, fault = o[:31], o[31], o[32], o[33:97], o[97], o[98], o[99], o[100], o[101]
        if fault:
            faults += 1
            hard.append((idx, "native fault", "", inputs))
            continue
        why = []
        for i in range(31):
            if gx[i] != ex[i]:
                why.append(f"x{i}: want {ex[i]:#x} got {gx[i]:#x}")
        if gsp != esp:
            why.append(f"sp: want {esp:#x} got {gsp:#x}")
        if gnzcv != enzcv:
            why.append(f"nzcv: want {enzcv:04b} got {gnzcv:04b}")
        if gpc != epc:
            why.append(f"pc: want {epc:#x} got {gpc:#x}")
        nan = False
        for i in range(64):
            if gv[i] != ev[i]:
                if not exact_nan and fp and i >> 1 == fp[0] and nan_only(ev[i], gv[i], fp[1]):
                    nan = True
                else:
                    why.append(f"v{i // 2}.d[{i & 1}]: want {ev[i]:#018x} got {gv[i]:#018x}")
        gmem = (None, 0, b"") if goff == 0xFFFFFFFFFFFFFFFF else (goff, glen, gbytes[:min(glen, 64)])
        if gmem != emem:
            why.append(f"mem: want {emem[0]}+{emem[1]} {emem[2].hex()} got {gmem[0]}+{gmem[1]} {gmem[2].hex()}")
        if why:
            hard.append((idx, why[0], "; ".join(why[1:4]), inputs))
        elif nan:
            soft[idx] += 1
    return faults, hard, soft


def reference(scratch_path):
    """This process's reference emulator, its pristine scratch window and the extent of the
    window its last run wrote. One serves every slice: each instance holds a large translation
    buffer until the collector frees it, and makes each fork (the native runs) slower."""
    if "uc" not in _state:
        from unicorn import Uc, UC_ARCH_ARM64, UC_HOOK_MEM_WRITE, UC_MODE_ARM, UC_PROT_EXEC, UC_PROT_READ
        img = _state["img"]
        with open(scratch_path, "rb") as f:
            pristine = f.read()
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(BASE, (len(img) + 0xFFF) & ~0xFFF, UC_PROT_READ | UC_PROT_EXEC)
        uc.mem_write(BASE, img)
        uc.mem_map(SCRATCH, SCRATCH_SIZE)
        uc.mem_write(SCRATCH, pristine)
        uc.mem_map(RESET, 0x1000, UC_PROT_READ | UC_PROT_EXEC)
        uc.mem_write(RESET, struct.pack("<I", 0xD503305F))   # clrex
        dirty = [None, 0]

        def on_write(_uc, _access, addr, size, _value, _user):
            if not SCRATCH <= addr < SCRATCH + SCRATCH_SIZE:
                return
            if dirty[0] is None or addr < dirty[0]:
                dirty[0] = addr
            if addr + size > dirty[1]:
                dirty[1] = addr + size

        uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
        _state["uc"] = uc, pristine, dirty
    return _state["uc"]


def _run(args):
    from unicorn import UcError
    from unicorn import arm64_const as A
    tests, states, seed, exes, scratch_path, workdir, opts = args
    img = _state["img"]
    uc, pristine, dirty = reference(scratch_path)
    reg_ids = ([A.UC_ARM64_REG_X0 + i for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30,
               A.UC_ARM64_REG_SP, A.UC_ARM64_REG_NZCV] + [A.UC_ARM64_REG_V0 + i for i in range(32)])
    read_ids = reg_ids + [A.UC_ARM64_REG_PC]
    vgen = rand_v_fp if opts.get("fp_special") else rand_v

    rng = random.Random(seed)
    expected, records, skipped = [], [], []
    for idx, addr, n, fp in tests:
        words = struct.unpack_from(f"<{n}I", img, addr)
        bases, indexes = set(), set()
        for w in words:
            b, i = mem_regs(w)
            if b is not None:
                bases.add(b)
            if i is not None:
                indexes.add(i)
        if bases & indexes:
            continue
        # br / blr / ret: give the target register a real code address
        code = {(w >> 5) & 31 for w in words if w & 0xFF9FFC1F == 0xD61F0000}
        # a store-exclusive must not find a reservation an earlier run left in the reference
        reset = any(store_exclusive(w) for w in words)
        for _ in range(states):
            xs = [rand_x(rng) for _ in range(31)]
            if rng.random() < 0.5:   # correlated operands: equal and off-by-one values
                pivot = rand_x(rng)
                for _k in range(8):
                    xs[rng.randrange(31)] = (pivot + rng.choice((0, 0, 1, -1))) & 0xFFFFFFFFFFFFFFFF
            for r in code:
                if r != 31:
                    xs[r] = BASE + 4 * rng.randrange(len(img) // 4)
            sp = SCRATCH + 0x8000 + 16 * rng.randrange(256)
            for b in bases:
                if b != 31:
                    xs[b] = SCRATCH + 0x8000 + 16 * rng.randrange(256)
            for i in indexes:
                if i != 31:
                    xs[i] = rng.randrange(64)
            nzcv = rng.getrandbits(4)
            if opts.get("fp_special") and rng.random() < 0.25:   # every register of the NaN rules, one width
                width = rng.choice((32, 64))
                vs = [rand_v_nan(rng, width) for _ in range(32)]
            else:
                vs = [vgen(rng) for _ in range(32)]
            if rng.random() < 0.4:   # equal vectors, for compares / min / max
                pivot = vgen(rng)
                for _k in range(6):
                    vs[rng.randrange(32)] = pivot
            flat = [h for lo_hi in vs for h in lo_hi]
            if reset:
                uc.emu_start(RESET, 0, count=1)
            uc.reg_write_batch(list(zip(reg_ids, xs + [sp, nzcv << 28] + [lo | hi << 64 for lo, hi in vs])))
            dirty[0], dirty[1] = None, 0
            try:
                uc.emu_start(BASE + addr, 0, count=n)
            except UcError:
                skipped.append(idx)
                if dirty[0] is not None:
                    uc.mem_write(dirty[0], pristine[dirty[0] - SCRATCH:dirty[1] - SCRATCH])
                continue
            got = uc.reg_read_batch(read_ids)
            mem = (None, 0, b"")
            if dirty[0] is not None:
                lo, hi = dirty[0] - SCRATCH, dirty[1] - SCRATCH
                now, was = bytes(uc.mem_read(dirty[0], hi - lo)), pristine[lo:hi]
                uc.mem_write(dirty[0], was)
                diff = [k for k in range(hi - lo) if now[k] != was[k]]
                if diff:
                    mem = (lo + diff[0], diff[-1] - diff[0] + 1, now[diff[0]:diff[-1] + 1][:64])
            ev = []
            for v in got[33:65]:
                ev += [v & 0xFFFFFFFFFFFFFFFF, v >> 64]
            expected.append((idx, list(got[:31]), got[31], got[32] >> 28, ev, got[65], mem,
                             (xs, sp, nzcv, flat), fp))
            records.append(struct.pack(IN_FMT, idx, 0, *xs, sp, nzcv, *flat))

    tag = f"{os.getpid()}-{seed}".replace(":", "-")
    in_path = os.path.join(workdir, f"in-{tag}.bin")
    with open(in_path, "wb") as f:
        f.write(b"".join(records))
    results = []
    for exe in exes:
        out_path = os.path.join(workdir, f"out-{tag}.bin")
        r = subprocess.run([*opts.get("run", ()), exe, _state["image_path"], scratch_path, in_path, out_path],
                           capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f"native harness failed ({r.returncode}): {r.stderr[:500]}")
        with open(out_path, "rb") as f:
            raw = f.read()
        os.remove(out_path)
        results.append(compare(expected, raw, opts.get("exact_nan", False)))
    os.remove(in_path)
    return len(expected), skipped, results


# ---------------------------------------------------------------------------- building
def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Toolchain:
    def __init__(self, name, compile_cmd, link_cmd, opts, obj=".o", exe="", run=()):
        self.name, self.compile_cmd, self.link_cmd, self.opts = name, compile_cmd, link_cmd, opts
        self.obj, self.exe, self.run = obj, exe, list(run)


def toolchain(name=None):
    """The native side's compiler: the game build's, with its flags, for this OS (or `name`).
    Raises SystemExit, saying why, where that compiler is missing or targets another machine."""
    name = name or {"win32": "windows", "darwin": "mac"}.get(sys.platform, "linux")
    machine = platform.machine().lower()
    tools = os.path.join(cc.ROOT, "tools")
    if name == "windows":
        if sys.platform != "win32" or machine not in ("amd64", "x86_64"):
            raise SystemExit(f"the Windows build's clang-cl targets x64 Windows (this is {sys.platform} {machine})")
        if not (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG)):
            raise SystemExit("clang-cl was not found (deps\\llvm, or Visual Studio's C++ Clang tools)")
        return Toolchain("clang-cl with tools/cc.py's flags",
                         lambda src, obj, opt: [cc.CLANG, "/c", opt, *cc.CFLAGS, src, "/Fo" + obj],
                         lambda objs, exe: [cc.CLANG, "/nologo", *cc.LINK_FLAGS, *objs, "/Fe" + exe, "/link",
                                            *cc.LINK_OPTIONS, "/HIGHENTROPYVA:NO"],
                         {"O0": "/Od", "O1": "/O1", "O2": "/O2"}, obj=".obj", exe=".exe")
    if name == "linux":
        if not sys.platform.startswith("linux") or machine not in ("x86_64", "amd64"):
            raise SystemExit(f"the Linux build targets x86-64 Linux (this is {sys.platform} {machine})")
        tc = load_module("difftest_linux_toolchain", os.path.join(tools, "linux", "toolchain.py"))   # (SystemExit without clang + ld.lld)
        return Toolchain("clang with tools/linux/toolchain.py's flags",
                         lambda src, obj, opt: [tc.CLANG, "-c", opt, *tc.CFLAGS, src, "-o", obj],
                         lambda objs, exe: [tc.CLANG, *tc.TARGET, "-fuse-ld=lld", "-no-pie", *objs, "-o", exe, "-lm"],
                         {"O0": "-O0", "O1": "-O1", "O2": "-O2"})
    if name == "mac":
        if sys.platform != "darwin" or machine != "arm64":
            raise SystemExit(f"the macOS build targets arm64 Macs (this is {sys.platform} {machine})")
        tc = load_module("difftest_mac_toolchain", os.path.join(tools, "mac", "toolchain.py"))   # (SystemExit without the Command Line Tools)
        return Toolchain("Apple clang with tools/mac/toolchain.py's flags",
                         lambda src, obj, opt: [tc.CLANG, "-c", opt, *tc.CFLAGS, "-I", cc.RUNTIME, src, "-o", obj],
                         lambda objs, exe: [tc.CLANG, *tc.TARGET, *objs, "-o", exe],
                         {"O0": "-O0", "O1": "-O1", "O2": "-O2"})
    if name == "rosetta":
        # (a developer's check of the x86-64 code on an Apple silicon Mac: the Linux build's
        # instruction set and FP flags, but Mach-O, so without its ELF-only -fno-pic / -fno-plt)
        clang = shutil.which("clang")
        if sys.platform != "darwin" or machine != "arm64" or not clang:
            raise SystemExit("rosetta: needs clang on an Apple silicon Mac")
        os.environ.setdefault("SSBU_LLVM_BIN", os.path.dirname(clang))   # (its LLVM search is for Linux and Windows)
        tc = load_module("difftest_linux_toolchain", os.path.join(tools, "linux", "toolchain.py"))
        flags = [f for f in tc.CFLAGS if f not in tc.TARGET and f not in ("-fno-pic", "-fno-plt")]
        return Toolchain("Apple clang -arch x86_64 with tools/linux/toolchain.py's flags, under Rosetta 2",
                         lambda src, obj, opt: [clang, "-arch", "x86_64", "-c", opt, *flags, src, "-o", obj],
                         lambda objs, exe: [clang, "-arch", "x86_64", *objs, "-o", exe],
                         {"O0": "-O0", "O1": "-O1", "O2": "-O2"}, run=["arch", "-x86_64"])
    raise SystemExit(f"unknown toolchain {name!r} (windows, linux, mac, rosetta)")


def write_sources(tests, lifter, work):
    """The lifted tests as C files, their table, and the harness: the sources of the native side."""
    per_file = 2000
    sources = []
    for f0 in range(0, len(tests), per_file):
        path = os.path.join(work, f"tests_{f0 // per_file:03d}.c")
        with open(path, "w") as f:
            f.write('#include "recomp.h"\ntypedef struct TestIO { u32 n, z, c, v; u64 pc; } TestIO;\n')
            for i in range(f0, min(f0 + per_file, len(tests))):
                addr, n = tests[i]
                f.write(f"void t_{i}(Cpu *c, TestIO *t) {{\n"
                        f"    u32 nf = t->n, zf = t->z, cf = t->c, vf = t->v; u64 ex_a = 0, ex_v = 0, ex_w = 0;"
                        f" u64 t_pc = {const(BASE + addr + 4 * n)};\n")
                for stmt in lifter.snippet(addr, n):
                    f.write(f"    {stmt}\n")
                f.write("T_done:\n    t->n = nf; t->z = zf; t->c = cf; t->v = vf; t->pc = t_pc;\n}\n")
        sources.append(path)
    table = os.path.join(work, "table.c")
    with open(table, "w") as f:
        f.write('#include "recomp.h"\ntypedef struct TestIO TestIO;\ntypedef void TestFn(Cpu *, TestIO *);\n')
        f.write("".join(f"TestFn t_{i};\n" for i in range(len(tests))))
        f.write("TestFn *const t_funcs[] = {\n" + "".join(f"    t_{i},\n" for i in range(len(tests))) + "};\n")
        f.write(f"const u32 t_count = {len(tests)};\n")
    return sources + [table, os.path.join(cc.RUNTIME, "difftest.c")]


def build(tc, sources, work, level, jobs, defines=()):
    """Compile and link the native side at an optimisation level; returns the executable."""
    out = os.path.join(work, level)
    os.makedirs(out, exist_ok=True)
    objs = [os.path.join(out, os.path.splitext(os.path.basename(s))[0] + tc.obj) for s in sources]

    def one(job):
        cmd = tc.compile_cmd(job[0], job[1], tc.opts[level])
        cmd[1:1] = defines
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f"compile failed: {job[0]}\n{(r.stdout + r.stderr)[:4000]}")

    with ThreadPoolExecutor(jobs) as pool:
        list(pool.map(one, zip(sources, objs)))
    exe = os.path.join(work, f"difftest-{level}{tc.exe}")
    r = subprocess.run(tc.link_cmd(objs, exe), capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"link failed\n{(r.stdout + r.stderr)[:4000]}")
    return exe


def execute(pool, tests, dis, states, exes, scratch_path, work, jobs, opts, seed=None):
    """Run every test from `states` random states under the reference and each executable:
    (states compared, tests the reference faulted on, [{faults, hard, soft}] per executable).
    With a seed, the states depend on it alone (slices of a fixed size), not on `jobs`."""
    indexed = [(i, a, n, fp_result([dis(a + 4 * k) for k in range(n)])) for i, (a, n) in enumerate(tests)]
    step = 128 if seed is not None else max(1, len(indexed) // (jobs * 4))
    slices = [(indexed[i:i + step], states, 1000 + i if seed is None else f"{seed}:states:{i}", exes, scratch_path, work, opts)
              for i in range(0, len(indexed), step)]
    total, skipped = 0, []
    per = [{"faults": 0, "hard": [], "soft": Counter()} for _ in exes]
    for r in pool.imap_unordered(_run, slices):
        total += r[0]
        skipped += r[1]
        for res, (faults, hard, soft) in zip(per, r[2]):
            res["faults"] += faults
            res["hard"] += hard
            res["soft"].update(soft)
    return total, skipped, per


def mnemonic(text):
    return (text.split() or ["(not an instruction)"])[0]   # (capstone decodes nothing there)


def summarize(levels, tests, total, skipped, per, dis, lifter, show):
    """Print the results; returns them for --report."""
    report = []
    for level, res in zip(levels, per):
        head = f"[{level}] " if len(levels) > 1 else ""
        hard, soft = res["hard"], res["soft"]
        bad_tests = {h[0] for h in hard}
        print(f"{head}ran {total:,} state comparisons; reference faulted on {len(skipped):,} (skipped); "
              f"native faults: {res['faults']:,}")
        if skipped:
            kinds = Counter(mnemonic(dis(tests[i][0])) for i in skipped)
            print(f"{head}  reference faults by mnemonic: " + ", ".join(f"{m} x{n}" for m, n in kinds.most_common(8)))
        print(f"{head}encodings with mismatches: {len(bad_tests):,} / {len(tests):,}   "
              f"(NaN-payload-only differences on {len(soft):,} more, in {sum(soft.values()):,} states)")
        by_mnem = defaultdict(list)
        for h in hard:
            by_mnem[mnemonic(dis(tests[h[0]][0]))].append(h)
        for m, items in sorted(by_mnem.items(), key=lambda kv: -len({h[0] for h in kv[1]})):
            print(f"\n{head}{m}: {len({h[0] for h in items})} encodings, {len(items)} failing states")
            shown = set()
            for idx, why, more, inputs in items:
                if idx in shown or len(shown) >= show:
                    continue
                shown.add(idx)
                addr, n = tests[idx]
                print(f"  {addr:#x}  {dis(addr, n)}")
                for stmt in lifter.snippet(addr, n):
                    print(f"      C: {stmt}")
                print(f"      {why}" + (f"; {more}" if more else ""))
        failing, by_test = [], defaultdict(list)
        for h in hard:
            by_test[h[0]].append(h)
        for idx in sorted(bad_tests):
            addr, n = tests[idx]
            stmts = lifter.snippet(addr, n)
            states_ = by_test[idx]
            failing.append({
                "words": [f"{w:08x}" for w in struct.unpack_from(f"<{n}I", lifter.img, addr)],
                "disasm": dis(addr, n), "mnemonic": mnemonic(dis(addr, n)), "c": stmts,
                "template": " ".join(template(s) for s in stmts), "states": len(states_),
                "differences": [why + (f"; {more}" if more else "") for _i, why, more, _in in states_[:100]],
                # (every failing state, at most --states: the test checks each against its known bugs)
                "examples": [{"why": why, "more": more, "x": [f"{x:#x}" for x in inputs[0]], "sp": f"{inputs[1]:#x}",
                              "nzcv": f"{inputs[2]:04b}",
                              "v": [f"{inputs[3][2 * r + 1]:016x}{inputs[3][2 * r]:016x}" for r in range(32)]}
                             for _i, why, more, inputs in states_]})
        report.append({"level": level, "native_faults": res["faults"], "soft_tests": len(soft),
                       "soft_states": sum(soft.values()), "failing": failing})
    return report


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def capstone_arm64():
    import capstone
    arch = capstone.CS_ARCH_AARCH64 if hasattr(capstone, "CS_ARCH_AARCH64") else capstone.CS_ARCH_ARM64   # (capstone 6's name)
    return capstone.Cs(arch, capstone.CS_MODE_ARM)


def native_side(tests, lifter, work, jobs, default_levels):
    """(toolchain, [(level, exe)]) built for --opt, or SystemExit if this machine has no toolchain."""
    tc = toolchain(arg("--toolchain"))
    levels = arg("--opt", default_levels).split(",")
    t0 = time.monotonic()
    sources = write_sources(tests, lifter, work)
    defines = ["-DRT_EXACT_NAN=1"] if "--exact-nan" in sys.argv else []
    exes = [(level, build(tc, sources, work, level, jobs, defines)) for level in levels]
    print(f"native side: {tc.name}, at {', '.join(levels)}: built in {time.monotonic() - t0:.0f} s")
    return tc, exes


def write_report(results):
    path = arg("--report")
    if path:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1)


# ---------------------------------------------------------------------------- the game's module
def main_nso():
    nso = sys.argv[1]
    per_template, states = int(arg("--per-template", "4")), int(arg("--states", "16"))
    jobs = int(arg("--jobs", "16"))
    only = set(filter(None, arg("--only", "").split(",")))
    work = arg("--work", os.path.join(cc.ROOT, "build", "difftest"))
    os.makedirs(work, exist_ok=True)

    # Any module can be tested: instructions are lifted for the default base and only the
    # text is consulted, so the unrelocated image serves both sides.
    mod, found, funcs = recomp.load(nso)
    image_path = recomp.image_file(mod)
    img = bytes(mod.image[:mod.text.end])
    cs = capstone_arm64()

    def dis(addr, n=1):
        return "; ".join(f"{m} {o}" for _a, _s, m, o in cs.disasm_lite(img[addr:addr + 4 * n], addr))

    chunks = [(funcs[i:i + 500], per_template) for i in range(0, len(funcs), 500)]
    merged = {}
    with Pool(jobs, _init, (image_path, mod.text.end)) as pool:
        for part in pool.imap_unordered(_sample, chunks):
            for key, (n, items) in part.items():
                slot = merged.setdefault(key, [0, {}])
                slot[0] += n
                slot[1].update(items)
        rng = random.Random(1)
        tests = []
        for key in sorted(merged):
            items = sorted(merged[key][1].items())
            for w, addr in rng.sample(items, min(per_template, len(items))):
                tests.append((addr, 1))
        lifter = TestLifter(img, set())
        for addr, n in exclusive_snippets(img, funcs, 200):
            try:
                lifter.snippet(addr, n)
                tests.append((addr, n))
            except Unsupported:
                pass
        if only:
            tests = [t for t in tests if mnemonic(dis(t[0])) in only]
        print(f"{len(merged):,} statement templates -> {len(tests):,} test encodings x {states} states")

        try:
            tc, exes = native_side(tests, lifter, work, jobs, "O2")
        except SystemExit as e:
            print(f"no native toolchain: {e}")
            return 3
        scratch_path = os.path.join(work, "scratch.bin")
        with open(scratch_path, "wb") as f:
            f.write(random.Random(7).randbytes(SCRATCH_SIZE))
        opts = {"exact_nan": "--exact-nan" in sys.argv, "run": tc.run}
        total, skipped, per = execute(pool, tests, dis, states, [e for _l, e in exes], scratch_path, work, jobs, opts)

    report = summarize([l for l, _e in exes], tests, total, skipped, per, dis, lifter, int(arg("--show", "2")))
    write_report({"mode": "nso", "tests": len(tests), "states": states, "compared": total,
                  "reference_faults": len(skipped), "levels": report})
    return 1 if any(res["hard"] for res in per) else 0


# ---------------------------------------------------------------------------- without the game
PAD = 0x1000   # random bytes before and after the tests: what their branches and literal loads reach
RT_REGS = [(re.compile(r"rt_tbl\(c, \d+, \d+, (\d), \d+,"), r"rt_tbl(c, #, #, \1, #,"),
           (re.compile(r"rt_ldst_multi\(c, (\d), \d+,"), r"rt_ldst_multi(c, \1, #,"),
           (re.compile(r"rt_ldst_single\(c, (\d), (\d), \d+,"), r"rt_ldst_single(c, \1, \2, #,")]


def shape(stmt):
    """template(), with the register numbers rt_tbl and rt_ldst_* take abstracted as well: drawn
    at random, every encoding of those would be a template of its own. (Their registers wrapping
    past v31 are edge cases: difftest_words.EDGES.)"""
    key = template(stmt)
    for pattern, repl in RT_REGS:
        key = pattern.sub(repl, key)
    return key


def synthetic_tests(seed, draws, per_template, cs):
    """[(words, origin)] to test, and what each instruction class gave: random words of each
    class (difftest_words.CLASSES) that capstone decodes and the lifter lifts, up to
    per_template for each statement template; then every edge case and sequence."""
    import difftest_words as words
    img = bytearray(random.Random(f"{seed}:pad").randbytes(PAD) + bytes(32) + random.Random(f"{seed}:pad2").randbytes(PAD))
    lifter = TestLifter(img, set())

    def decode(ws):
        got = list(cs.disasm_lite(struct.pack(f"<{len(ws)}I", *ws), BASE + PAD))
        return got if len(got) == len(ws) else None

    def lift(ws):
        img[PAD:PAD + 4 * len(ws)] = struct.pack(f"<{len(ws)}I", *ws)
        return lifter.snippet(PAD, len(ws))

    tests, chosen, seen, stats = [], {}, set(), {}

    def consider(ws, origin, st, bucketed):
        if ws in seen:
            st["duplicate"] += 1
            return
        seen.add(ws)
        d = decode(ws)
        try:
            stmts = lift(ws)
        except Unsupported as e:
            if "isolation" in str(e) or "control flow" in str(e):
                st["untestable"] += 1
            elif d:
                st["unlifted"][d[0][2]] += 1
            else:
                st["reserved"] += 1
            return
        except Exception as e:   # (a lifter crash on an encoding the game does not have)
            st["errors"].append(f"{ws[0]:08x} {d[0][2] + ' ' + d[0][3] if d else '(no instruction)'}: {type(e).__name__}: {e}")
            return
        if d is None:
            st["undecoded"].append(f"{ws[0]:08x}")   # lifted, though no instruction is encoded there
            return
        if all(s == ";" for s in stmts):
            st["silent"] += 1
            return
        if bucketed:
            bucket = chosen.setdefault(shape(stmts[0]), [])
            if len(bucket) >= per_template:
                st["beyond"] += 1
                return
            bucket.append(ws)
        st["tested"] += 1
        tests.append((ws, origin))

    def new_stats():
        return {"drawn": 0, "dropped": 0, "duplicate": 0, "tested": 0, "beyond": 0, "untestable": 0, "silent": 0,
                "reserved": 0, "unlifted": Counter(), "undecoded": [], "errors": []}

    for entry in words.CLASSES:
        name = entry[0]
        st = stats[name] = new_stats()
        rng = random.Random(f"{seed}:{name}")
        for _ in range(draws):
            st["drawn"] += 1
            w = words.draw(entry, rng)
            if w is None:
                st["dropped"] += 1
                continue
            consider((w,), name, st, True)
    for name, make in words.EDGES:
        st = stats["edge cases: " + name] = new_stats()
        for w in make(random.Random(f"{seed}:edge:{name}")):
            st["drawn"] += 1
            consider((w,), "edge cases: " + name, st, False)
    for name, make in words.SEQUENCES:
        st = stats["sequences: " + name] = new_stats()
        for _label, ws in make(random.Random(f"{seed}:sequence:{name}")):
            st["drawn"] += 1
            consider(tuple(ws), "sequences: " + name, st, False)
    return tests, stats


def print_classes(stats):
    print("instruction classes: tested / drawn; not lifted (by mnemonic); reserved or unallocated:")
    for name, st in stats.items():
        line = f"  {name}: {st['tested']} / {st['drawn']}"
        if st["unlifted"]:
            top = st["unlifted"].most_common(6)
            more = sum(st["unlifted"].values()) - sum(n for _m, n in top)
            line += "; not lifted: " + ", ".join(f"{m} x{n}" for m, n in top) + (f", others x{more}" if more else "")
        if st["reserved"]:
            line += f"; reserved x{st['reserved']}"
        if st["untestable"]:
            line += f"; not testable alone x{st['untestable']}"
        if st["undecoded"]:
            line += f"; lifted but no instruction (capstone) x{len(st['undecoded'])}: {', '.join(st['undecoded'][:3])}"
        print(line)
        for e in st["errors"][:3]:
            print(f"    lifter error: {e}")


def main_synthetic():
    per_template, states = int(arg("--per-template", "2")), int(arg("--states", "8"))
    seed, draws = int(arg("--seed", "1")), int(arg("--draws", "2000"))
    jobs = int(arg("--jobs", str(min(16, os.cpu_count() or 4))))
    only = set(filter(None, arg("--only", "").split(",")))
    work = arg("--work", os.path.join(cc.ROOT, "build", "difftest-synthetic"))
    os.makedirs(work, exist_ok=True)
    cs = capstone_arm64()

    chosen, stats = synthetic_tests(seed, draws, per_template, cs)
    print_classes(stats)
    if only:
        chosen = [(ws, o) for ws, o in chosen
                  if next(cs.disasm_lite(struct.pack("<I", ws[0]), 0))[2] in only]
    rng = random.Random(f"{seed}:pad")
    body = b"".join(struct.pack(f"<{len(ws)}I", *ws) for ws, _o in chosen)
    img = rng.randbytes(PAD) + body + random.Random(f"{seed}:pad2").randbytes(PAD)
    tests, at = [], PAD
    for ws, _o in chosen:
        tests.append((at, len(ws)))
        at += 4 * len(ws)
    image_path = os.path.join(work, "image.bin")
    with open(image_path, "wb") as f:
        f.write(img)
    lifter = TestLifter(img, set())

    def dis(addr, n=1):
        return "; ".join(f"{m} {o}" for _a, _s, m, o in cs.disasm_lite(img[addr:addr + 4 * n], addr))

    print(f"{len(tests):,} test encodings (seed {seed}, {draws} words drawn per class, {per_template} per template) "
          f"x {states} states")
    try:
        tc, exes = native_side(tests, lifter, work, jobs, "O2,O0")
    except SystemExit as e:
        print(f"no native toolchain: {e}")
        return 3
    scratch_path = os.path.join(work, "scratch.bin")
    with open(scratch_path, "wb") as f:
        f.write(random.Random(7).randbytes(SCRATCH_SIZE))
    opts = {"exact_nan": "--exact-nan" in sys.argv, "run": tc.run, "fp_special": True}
    t0 = time.monotonic()
    with Pool(jobs, _init, (image_path, len(img))) as pool:
        total, skipped, per = execute(pool, tests, dis, states, [e for _l, e in exes], scratch_path, work, jobs, opts, seed)
    print(f"ran in {time.monotonic() - t0:.0f} s ({jobs} processes)")

    report = summarize([l for l, _e in exes], tests, total, skipped, per, dis, lifter, int(arg("--show", "2")))
    write_report({"mode": "synthetic", "seed": seed, "draws": draws, "per_template": per_template,
                  "toolchain": tc.name, "tests": len(tests), "states": states, "compared": total,
                  "reference_faults": len(skipped), "levels": report,
                  "classes": {name: {**st, "unlifted": dict(st["unlifted"])} for name, st in stats.items()}})
    return 1 if any(res["hard"] for res in per) else 0


def main():
    if "--synthetic" in sys.argv:
        return main_synthetic()
    return main_nso()


if __name__ == "__main__":
    sys.exit(main())
