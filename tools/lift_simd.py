"""AdvSIMD (NEON) lifting.

Vector registers are V128 unions; each instruction becomes per-lane C over local copies of
its operands (a_ = Vn, b_ = Vm, d_ = old Vd), so register aliasing needs no special care.
Lane counts are compile-time constants, which lets the host compiler vectorise the loops.
"""
from a64 import Unsupported, const, reg, wr

UF = {8: "b", 16: "h", 32: "s", 64: "d"}       # unsigned lane fields of V128
SF = {8: "ib", 16: "ih", 32: "is", 64: "id"}   # signed
FF = {32: "f", 64: "g"}                        # floating point


def vop(rd, rn, rm, lanes, out, expr):
    """Same expression for every lane; lanes not written are zeroed."""
    return (f"{{ V128 a_ = c->v[{rn}], b_ = c->v[{rm}], d_ = c->v[{rd}], r_ = {{0}}; "
            f"for (int i_ = 0; i_ < {lanes}; i_++) r_.{out}[i_] = {expr}; c->v[{rd}] = r_; }}")


def vfp(rd, rn, rm, lanes, esize, expr, form, index=-1):
    """vop for floating-point arithmetic: when a lane came out NaN, recomp.h's rt_vnanS / D makes it
    Arm's NaN from the operands (`form` and `index` say which), out of line; never on arm64."""
    return (f"{{ V128 a_ = c->v[{rn}], b_ = c->v[{rm}], d_ = c->v[{rd}], r_ = {{0}}; "
            f"for (int i_ = 0; i_ < {lanes}; i_++) r_.{FF[esize]}[i_] = {expr}; "
            f"if (rt_nan_any{esize}(&r_)) r_ = rt_vnan{'D' if esize == 64 else 'S'}(r_, a_, b_, d_, {form}, {index}); "
            f"c->v[{rd}] = r_; }}")


def vset(rd, rn, rm, assigns, keep=False, then=""):
    """Explicit per-lane assignments; `keep` starts from the old destination value, `then` follows them."""
    body = " ".join(f"r_.{dst} = {src};" for dst, src in assigns)
    return (f"{{ V128 a_ = c->v[{rn}], b_ = c->v[{rm}], d_ = c->v[{rd}], r_ = {'d_' if keep else '{0}'}; "
            f"{body} {then}c->v[{rd}] = r_; }}")


def vpair(rd, rn, rm, lanes, field, fmt, nan=None):
    """Pairwise: the result's lanes from a's pairs, then b's. `nan` (recomp.h's fnan2S / D, for an
    addition): a lane that came out NaN made Arm's NaN from its pair, looked at only when some did."""
    half = lanes // 2
    pairs = []
    for i in range(half):
        pairs.append((f"{field}[{i}]", f"a_.{field}[{2 * i}]", f"a_.{field}[{2 * i + 1}]"))
        pairs.append((f"{field}[{half + i}]", f"b_.{field}[{2 * i}]", f"b_.{field}[{2 * i + 1}]"))
    then = ""
    if nan:
        fix = " ".join(f"r_.{dst} = {nan}(r_.{dst}, {x}, {y});" for dst, x, y in pairs)
        then = f"if (rt_nan_any{64 if field == 'g' else 32}(&r_)) {{ {fix} }} "
    return vset(rd, rn, rm, [(dst, fmt.format(x=x, y=y)) for dst, x, y in pairs], then=then)


def hexf(value, single):
    return float.hex(value) + ("f" if single else "")


class SimdMixin:
    # ------------------------------------------------------------------ dispatch
    def simd(self, w):
        scalar = (w >> 28) & 1
        if w >> 31 or (scalar and not (w >> 30) & 1):
            raise Unsupported("simd: reserved")
        if (w >> 24) & 1:
            if (w >> 10) & 1:
                if (w >> 23) & 1:
                    raise Unsupported("simd: reserved")
                if not (w >> 19) & 0xF:
                    if scalar:
                        raise Unsupported("simd: reserved")
                    return self.simd_mod_imm(w)
                return self.simd_shift_imm(w, scalar)
            return self.simd_indexed(w, scalar)
        if (w >> 21) & 1:
            if (w >> 10) & 1:
                return self.simd_three_same(w, scalar)
            if (w >> 10) & 3 == 2:
                group = (w >> 17) & 0xF
                if group == 0:
                    return self.simd_two_reg(w, scalar)
                if group == 8:
                    return self.simd_across(w, scalar)
                raise Unsupported("simd: crypto / half-precision")
            if scalar:
                raise Unsupported("simd: scalar saturating long op")
            return self.simd_three_diff(w)
        if (w >> 15) & 1:
            raise Unsupported("simd: reserved")
        sized = (w >> 22) & 3
        if (w >> 10) & 1:
            if sized:
                raise Unsupported("simd: reserved")
            return self.simd_copy(w, scalar)
        if scalar:
            raise Unsupported("simd: reserved")
        if (w >> 29) & 1:
            if sized:
                raise Unsupported("simd: reserved")
            return self.simd_ext(w)
        if (w >> 11) & 1:
            return self.simd_permute(w)
        if sized:
            raise Unsupported("simd: reserved")
        return self.simd_tbl(w)

    # ------------------------------------------------------------------ three registers, same type
    def simd_three_same(self, w, scalar):
        q, u, size = (w >> 30) & 1, (w >> 29) & 1, (w >> 22) & 3
        rm, opcode, rn, rd = (w >> 16) & 31, (w >> 11) & 31, (w >> 5) & 31, w & 31
        if opcode >= 0x18:
            return self.three_same_fp(q, u, size, rm, opcode, rn, rd, scalar)
        if opcode == 3:
            if scalar:
                raise Unsupported("simd: reserved")
            a, b, d = "a_.d[i_]", "b_.d[i_]", "d_.d[i_]"
            expr = ([f"{a} ^ {b}", f"({d} & {a}) | (~{d} & {b})", f"({d} & ~{b}) | ({a} & {b})",
                     f"({d} & {b}) | ({a} & ~{b})"] if u else
                    [f"{a} & {b}", f"{a} & ~{b}", f"{a} | {b}", f"{a} | ~{b}"])[size]
            return vop(rd, rn, rm, 1 + q, "d", expr)
        esize = 8 << size
        if (scalar and size != 3) or (size == 3 and not (q or scalar)):
            raise Unsupported("simd: reserved element size")
        lanes = 1 if scalar else (64 << q) // esize
        uf, xf = UF[esize], (UF if u else SF)[esize]
        a, b = f"a_.{xf}[i_]", f"b_.{xf}[i_]"
        ua, ub, ud = f"a_.{uf}[i_]", f"b_.{uf}[i_]", f"d_.{uf}[i_]"
        if opcode in (0x14, 0x15, 0x17) and not scalar:
            fmt = {0x14: "{x} > {y} ? {x} : {y}", 0x15: "{x} < {y} ? {x} : {y}", 0x17: "(u64){x} + {y}"}[opcode]
            if opcode == 0x17 and u:
                raise Unsupported("simd: reserved")
            return vpair(rd, rn, rm, lanes, uf if opcode == 0x17 else xf, fmt)
        table = {
            0x06: f"{a} > {b} ? -1 : 0",
            0x07: f"{a} >= {b} ? -1 : 0",
            0x08: f"rt_{'u' if u else 's'}shl({a}, (i8){ub}, {esize})",
            0x0C: f"{a} > {b} ? {a} : {b}",
            0x0D: f"{a} < {b} ? {a} : {b}",
            0x0E: f"{a} > {b} ? (u64){ua} - {ub} : (u64){ub} - {ua}",
            0x10: f"(u64){ua} {'-' if u else '+'} {ub}",
            0x11: f"{ua} == {ub} ? -1 : 0" if u else f"({ua} & {ub}) ? -1 : 0",
        }
        if size != 3:
            table[0x12] = f"(u64){ud} {'-' if u else '+'} (u64){ua} * {ub}"
            if not u:
                table[0x13] = f"(u64){ua} * {ub}"
        if opcode not in table:
            raise Unsupported(f"simd: three-same integer opcode {opcode:#x}")
        return vop(rd, rn, rm, lanes, uf, table[opcode])

    def three_same_fp(self, q, u, size, rm, opcode, rn, rd, scalar):
        hi, sz = size >> 1, size & 1
        esize = 64 if sz else 32
        if sz and not (q or scalar):
            raise Unsupported("simd: reserved element size")
        lanes = 1 if scalar else (64 << q) // esize
        f, o, p = FF[esize], UF[esize], "D" if sz else "S"
        fma, fabs_ = ("fma", "fabs") if sz else ("fmaf", "fabsf")
        a, b, d = f"a_.{f}[i_]", f"b_.{f}[i_]", f"d_.{f}[i_]"
        key = (opcode, u, hi)
        arith = {   # (helpers that make Arm's NaNs themselves)
            (0x18, 0, 0): f"fmaxnm{p}({a}, {b})", (0x18, 0, 1): f"fminnm{p}({a}, {b})",
            (0x1B, 0, 0): f"rt_fmulx{esize}({a}, {b})",
            (0x1E, 0, 0): f"fmax{p}({a}, {b})", (0x1E, 0, 1): f"fmin{p}({a}, {b})",
            (0x1F, 0, 0): f"rt_frecps{esize}({a}, {b})", (0x1F, 0, 1): f"rt_frsqrts{esize}({a}, {b})",
        }
        if key in arith:
            return vop(rd, rn, rm, lanes, f, arith[key])
        nan = {   # (expression, rt_vnan's form: its operands)
            (0x19, 0, 0): (f"{fma}({a}, {b}, {d})", "3"), (0x19, 0, 1): (f"{fma}(-{a}, {b}, {d})", "3 | RT_VNAN_NEG"),
            (0x1A, 0, 0): (f"{a} + {b}", "2"), (0x1A, 0, 1): (f"{a} - {b}", "2"),
            (0x1A, 1, 1): (f"{fabs_}({a} - {b})", "2 | RT_VNAN_ABS"), (0x1B, 1, 0): (f"{a} * {b}", "2"),
            (0x1F, 1, 0): (f"{a} / {b}", "2"),
        }
        if key in nan:
            return vfp(rd, rn, rm, lanes, esize, *nan[key])
        compare = {(0x1C, 0, 0): f"{a} == {b}", (0x1C, 1, 0): f"{a} >= {b}", (0x1C, 1, 1): f"{a} > {b}",
                   (0x1D, 1, 0): f"{fabs_}({a}) >= {fabs_}({b})", (0x1D, 1, 1): f"{fabs_}({a}) > {fabs_}({b})"}
        if key in compare:
            return vop(rd, rn, rm, lanes, o, f"({compare[key]}) ? -1 : 0")
        pairwise = {(0x1A, 1, 0): "{x} + {y}", (0x1E, 1, 0): f"fmax{p}({{x}}, {{y}})", (0x1E, 1, 1): f"fmin{p}({{x}}, {{y}})",
                    (0x18, 1, 0): f"fmaxnm{p}({{x}}, {{y}})", (0x18, 1, 1): f"fminnm{p}({{x}}, {{y}})"}
        if key in pairwise and not scalar:
            return vpair(rd, rn, rm, lanes, f, pairwise[key], f"fnan2{p}" if key == (0x1A, 1, 0) else None)
        raise Unsupported(f"simd: three-same fp opcode {opcode:#x}")

    # ------------------------------------------------------------------ two-register miscellaneous
    def simd_two_reg(self, w, scalar):
        q, u, size = (w >> 30) & 1, (w >> 29) & 1, (w >> 22) & 3
        opcode, rn, rd = (w >> 12) & 31, (w >> 5) & 31, w & 31
        esize = 8 << size
        uf, sf_ = UF[esize], SF[esize]
        lanes = 1 if scalar else (64 << q) // esize
        ua, sa = f"a_.{uf}[i_]", f"a_.{sf_}[i_]"
        if not scalar:
            if opcode in (0, 1):   # rev64 / rev32 / rev16
                container = {(0, 0): 64, (0, 1): 32, (1, 0): 16}.get((opcode, u))
                if container is None or esize >= container:
                    raise Unsupported("simd: reserved")
                k = container // esize
                return vset(rd, rn, rn, [(f"{uf}[{i}]", f"a_.{uf}[{i - i % k + k - 1 - i % k}]") for i in range(lanes)])
            if opcode == 5 and size == 0:
                if u:
                    return vop(rd, rn, rn, 1 + q, "d", "~a_.d[i_]")
                return vop(rd, rn, rn, lanes, "b", "__builtin_popcount(a_.b[i_])")
            if opcode == 5 and u and size == 1:
                return vop(rd, rn, rn, 8 << q, "b", "__builtin_bitreverse8(a_.b[i_])")
            if opcode == 4 and u and size != 3:
                return vop(rd, rn, rn, lanes, uf, f"clz32({ua}) - {32 - esize}")
            if opcode == 0x12 and not u and size != 3:   # xtn / xtn2
                m, wide = 64 // esize, UF[esize * 2]
                return vset(rd, rn, rn, [(f"{uf}[{i + (m if q else 0)}]", f"a_.{wide}[{i}]") for i in range(m)], bool(q))
            if opcode == 0x13 and u and size != 3:       # shll / shll2
                m, wide = 64 // esize, UF[esize * 2]
                return vset(rd, rn, rn, [(f"{wide}[{i}]", f"(u64)a_.{uf}[{i + (m if q else 0)}] << {esize}")
                                         for i in range(m)])
            if opcode in (2, 6) and size != 3:           # saddlp / uaddlp, sadalp / uadalp (6: accumulating)
                xf, wide = (UF if u else SF)[esize], UF[esize * 2]
                cast = "(u64)" if u else "(u64)(i64)"
                acc = f"(u64)d_.{wide}[i_] + " if opcode == 6 else ""
                return vop(rd, rn, rn, lanes // 2, wide, f"{acc}{cast}a_.{xf}[2 * i_] + {cast}a_.{xf}[2 * i_ + 1]")
        if opcode in (8, 9, 10, 11):
            if (scalar and size != 3) or (size == 3 and not (q or scalar)):
                raise Unsupported("simd: reserved element size")
            expr = {(8, 0): f"{sa} > 0 ? -1 : 0", (8, 1): f"{sa} >= 0 ? -1 : 0", (9, 0): f"{sa} == 0 ? -1 : 0",
                    (9, 1): f"{sa} <= 0 ? -1 : 0", (10, 0): f"{sa} < 0 ? -1 : 0",
                    (11, 0): f"{sa} < 0 ? 0 - (u64){ua} : {ua}", (11, 1): f"0 - (u64){ua}"}.get((opcode, u))
            if expr is None:
                raise Unsupported("simd: reserved")
            return vop(rd, rn, rn, lanes, uf, expr)
        if opcode < 0x0C or opcode in (0x12, 0x13, 0x14):
            raise Unsupported(f"simd: two-reg integer opcode {opcode:#x}")

        hi, sz = size >> 1, size & 1
        fe = 64 if sz else 32
        if sz and not (q or scalar):
            raise Unsupported("simd: reserved element size")
        ff, fu, fs = FF[fe], UF[fe], SF[fe]
        n = 1 if scalar else (64 << q) // fe
        a, x = f"a_.{ff}[i_]", "" if sz else "f"
        key = (opcode, u, hi)
        zero_compare = {(0x0C, 0, 1): ">", (0x0C, 1, 1): ">=", (0x0D, 0, 1): "==", (0x0D, 1, 1): "<=", (0x0E, 0, 1): "<"}
        if key in zero_compare:
            return vop(rd, rn, rn, n, fu, f"{a} {zero_compare[key]} 0 ? -1 : 0")
        unary = {
            (0x0F, 0, 1): f"fabs{x}({a})", (0x0F, 1, 1): f"-{a}",
            (0x18, 0, 0): f"__builtin_roundeven{x}({a})", (0x18, 1, 0): f"round{x}({a})", (0x18, 0, 1): f"ceil{x}({a})",
            (0x19, 0, 0): f"floor{x}({a})", (0x19, 1, 0): f"rint{x}({a})", (0x19, 0, 1): f"trunc{x}({a})",
            (0x19, 1, 1): f"nearbyint{x}({a})",
            (0x1D, 0, 1): f"rt_frecpe{fe}({a})", (0x1D, 1, 1): f"rt_frsqrte{fe}({a})",
            (0x1D, 0, 0): f"(f{fe})a_.{fs}[i_]", (0x1D, 1, 0): f"(f{fe})a_.{fu}[i_]",
        }
        if key in unary:
            return vop(rd, rn, rn, n, ff, unary[key])
        if key == (0x1F, 1, 1):
            return vfp(rd, rn, rn, n, fe, f"sqrt{x}({a})", "1")
        to_int = {(0x1B, 1): "{}", (0x1A, 0): "__builtin_roundeven({})", (0x1A, 1): "ceil({})", (0x1B, 0): "floor({})",
                  (0x1C, 0): "round({})"}.get((opcode, hi))
        if to_int:
            return vop(rd, rn, rn, n, fu, f"f2{'u' if u else 'i'}{fe}({to_int.format(f'(f64){a}')})")
        if not scalar and key == (0x16, 0, 0):   # fcvtn / fcvtn2
            if sz:
                return vset(rd, rn, rn, [(f"f[{i + (2 if q else 0)}]", f"(f32)a_.g[{i}]") for i in range(2)], bool(q))
            return vset(rd, rn, rn, [(f"h[{i + (4 if q else 0)}]", f"rt_d2h((f64)a_.f[{i}])") for i in range(4)], bool(q))
        if not scalar and key == (0x17, 0, 0):   # fcvtl / fcvtl2
            if sz:
                return vset(rd, rn, rn, [(f"g[{i}]", f"(f64)a_.f[{i + (2 if q else 0)}]") for i in range(2)])
            return vset(rd, rn, rn, [(f"f[{i}]", f"rt_h2f(a_.h[{i + (4 if q else 0)}])") for i in range(4)])
        raise Unsupported(f"simd: two-reg fp opcode {opcode:#x}")

    # ------------------------------------------------------------------ reductions / scalar pairwise
    def simd_across(self, w, scalar):
        q, u, size = (w >> 30) & 1, (w >> 29) & 1, (w >> 22) & 3
        opcode, rn, rd = (w >> 12) & 31, (w >> 5) & 31, w & 31
        if scalar:
            if opcode == 0x1B and not u and size == 3:
                return vset(rd, rn, rn, [("d[0]", "a_.d[0] + a_.d[1]")])
            sz = size & 1
            f, p = FF[64 if sz else 32], "D" if sz else "S"
            x, y = f"a_.{f}[0]", f"a_.{f}[1]"
            expr = {(0x0D, 0): f"fnan2{p}({x} + {y}, {x}, {y})", (0x0F, 0): f"fmax{p}({x}, {y})",
                    (0x0F, 1): f"fmin{p}({x}, {y})",
                    (0x0C, 0): f"fmaxnm{p}({x}, {y})", (0x0C, 1): f"fminnm{p}({x}, {y})"}.get((opcode, size >> 1))
            if expr is None or not u:
                raise Unsupported(f"simd: scalar pairwise opcode {opcode:#x}")
            return vset(rd, rn, rn, [(f"{f}[0]", expr)])
        esize = 8 << size
        lanes = (64 << q) // esize
        uf, xf = UF[esize], (UF if u else SF)[esize]
        if opcode in (0x0C, 0x0F) and u and q and not size & 1:
            p = {(0x0C, 0): "fmaxnmS", (0x0C, 1): "fminnmS", (0x0F, 0): "fmaxS", (0x0F, 1): "fminS"}[(opcode, size >> 1)]
            return vset(rd, rn, rn, [("f[0]", f"{p}({p}(a_.f[0], a_.f[1]), {p}(a_.f[2], a_.f[3]))")])
        if size == 3 or (size == 2 and not q):
            raise Unsupported("simd: reserved element size")
        if opcode == 0x1B and not u:
            return vset(rd, rn, rn, [(f"{uf}[0]", " + ".join(f"(u64)a_.{uf}[{i}]" for i in range(lanes)))])
        if opcode == 0x03:
            cast = "(u64)" if u else "(u64)(i64)"
            return vset(rd, rn, rn, [(f"{UF[esize * 2]}[0]", " + ".join(f"{cast}a_.{xf}[{i}]" for i in range(lanes)))])
        if opcode in (0x0A, 0x1A):
            t = f"{'u' if u else 'i'}{esize}"
            return (f"{{ V128 a_ = c->v[{rn}], r_ = {{0}}; {t} m_ = a_.{xf}[0]; for (int i_ = 1; i_ < {lanes}; i_++) "
                    f"if (a_.{xf}[i_] {'>' if opcode == 0x0A else '<'} m_) m_ = a_.{xf}[i_]; "
                    f"r_.{xf}[0] = m_; c->v[{rd}] = r_; }}")
        raise Unsupported(f"simd: across-lanes opcode {opcode:#x}")

    # ------------------------------------------------------------------ widening three-register ops
    def simd_three_diff(self, w):
        q, u, size = (w >> 30) & 1, (w >> 29) & 1, (w >> 22) & 3
        rm, opcode, rn, rd = (w >> 16) & 31, (w >> 12) & 15, (w >> 5) & 31, w & 31
        if size == 3:
            raise Unsupported("simd: polynomial / reserved")
        esize = 8 << size
        m, part = 64 // esize, (64 // esize if q else 0)
        xf, wide = (UF if u else SF)[esize], UF[esize * 2]
        cast = "(u64)" if u else "(u64)(i64)"

        def lane(i):
            x, y = f"{cast}a_.{xf}[{i + part}]", f"{cast}b_.{xf}[{i + part}]"
            acc, wa = f"(u64)d_.{wide}[{i}]", f"(u64)a_.{wide}[{i}]"
            if opcode == 0x7:
                sx_, sy = f"a_.{xf}[{i + part}]", f"b_.{xf}[{i + part}]"
                return f"{sx_} > {sy} ? {x} - {y} : {y} - {x}"
            return {0x0: f"{x} + {y}", 0x1: f"{wa} + {y}", 0x2: f"{x} - {y}", 0x3: f"{wa} - {y}",
                    0x8: f"{acc} + {x} * {y}", 0xA: f"{acc} - {x} * {y}", 0xC: f"{x} * {y}"}.get(opcode)

        if lane(0) is None:
            raise Unsupported(f"simd: three-different opcode {opcode:#x}")
        return vset(rd, rn, rm, [(f"{wide}[{i}]", lane(i)) for i in range(m)])

    # ------------------------------------------------------------------ element copies
    def simd_copy(self, w, scalar):
        q, op, imm5, imm4, rn, rd = (w >> 30) & 1, (w >> 29) & 1, (w >> 16) & 31, (w >> 11) & 15, (w >> 5) & 31, w & 31
        if not imm5 & 15:
            raise Unsupported("simd: reserved")
        size = (imm5 & -imm5).bit_length() - 1
        esize, idx = 8 << size, imm5 >> (size + 1)
        f, s = UF[esize], SF[esize]
        if scalar:
            if op or imm4:
                raise Unsupported("simd: reserved")
            return vset(rd, rn, rn, [(f"{f}[0]", f"a_.{f}[{idx}]")])
        lanes = (64 << q) // esize
        if op:
            if not q:
                raise Unsupported("simd: reserved")
            return vset(rd, rn, rn, [(f"{f}[{idx}]", f"a_.{f}[{imm4 >> size}]")], keep=True)
        if imm4 == 0:
            return vset(rd, rn, rn, [(f"{f}[{i}]", f"a_.{f}[{idx}]") for i in range(lanes)])
        if imm4 == 1 and (q or esize != 64):
            return vset(rd, rn, rn, [(f"{f}[{i}]", f"(u{esize}){reg(rn)}") for i in range(lanes)])
        if imm4 == 3 and q:
            return f"c->v[{rd}].{f}[{idx}] = (u{esize}){reg(rn)};"
        if imm4 == 7 and (esize == 64) == bool(q):
            return wr(rd, 1, f"(u64)c->v[{rn}].{f}[{idx}]")
        if imm4 == 5 and esize < (64 if q else 32):
            return wr(rd, 1, f"({'u64)(i64' if q else 'u32)(i32'})c->v[{rn}].{s}[{idx}]")
        raise Unsupported("simd: reserved copy")

    def simd_permute(self, w):
        q, size, rm, opcode, rn, rd = (w >> 30) & 1, (w >> 22) & 3, (w >> 16) & 31, (w >> 12) & 7, (w >> 5) & 31, w & 31
        esize = 8 << size
        lanes = (64 << q) // esize
        if (w >> 29) & 1 or not opcode & 3 or (size == 3 and not q):
            raise Unsupported("simd: reserved")
        f, half, part, kind = UF[esize], lanes // 2, opcode >> 2, opcode & 3
        assigns = []
        for i in range(half):
            if kind == 1:     # uzp
                assigns += [(f"{f}[{i}]", f"a_.{f}[{2 * i + part}]"), (f"{f}[{half + i}]", f"b_.{f}[{2 * i + part}]")]
            elif kind == 2:   # trn
                assigns += [(f"{f}[{2 * i}]", f"a_.{f}[{2 * i + part}]"), (f"{f}[{2 * i + 1}]", f"b_.{f}[{2 * i + part}]")]
            else:             # zip
                assigns += [(f"{f}[{2 * i}]", f"a_.{f}[{part * half + i}]"), (f"{f}[{2 * i + 1}]", f"b_.{f}[{part * half + i}]")]
        return vset(rd, rn, rm, assigns)

    def simd_ext(self, w):
        q, rm, imm4, rn, rd = (w >> 30) & 1, (w >> 16) & 31, (w >> 11) & 15, (w >> 5) & 31, w & 31
        n = 8 << q
        if (w >> 10) & 1 or imm4 >= n:
            raise Unsupported("simd: reserved")
        return vset(rd, rn, rm, [(f"b[{i}]", f"a_.b[{i + imm4}]" if i + imm4 < n else f"b_.b[{i + imm4 - n}]")
                                 for i in range(n)])

    def simd_tbl(self, w):
        q, rm, length, tbx, rn, rd = (w >> 30) & 1, (w >> 16) & 31, (w >> 13) & 3, (w >> 12) & 1, (w >> 5) & 31, w & 31
        return f"rt_tbl(c, {rd}, {rn}, {length + 1}, {rm}, {q}, {tbx});"

    # ------------------------------------------------------------------ immediates
    def simd_mod_imm(self, w):
        q, op, cmode, rd = (w >> 30) & 1, (w >> 29) & 1, (w >> 12) & 15, w & 31
        imm8 = ((w >> 16) & 7) << 5 | (w >> 5) & 31
        if (w >> 11) & 1:
            raise Unsupported("simd: half-precision immediate")
        kind, c3 = "mov", cmode >> 1
        if c3 < 4:
            imm = (imm8 << (8 * c3)) * 0x100000001
        elif c3 < 6:
            imm = (imm8 << (8 * (c3 - 4))) * 0x0001000100010001
        elif c3 == 6:
            imm = ((imm8 << 16 | 0xFFFF) if cmode & 1 else (imm8 << 8 | 0xFF)) * 0x100000001
        elif not cmode & 1:
            imm = sum(0xFF << (8 * i) for i in range(8) if imm8 >> i & 1) if op else imm8 * 0x0101010101010101
        else:
            b6 = (imm8 >> 6) & 1
            if op:
                if not q:
                    raise Unsupported("simd: reserved")
                imm = (imm8 >> 7) << 63 | (b6 ^ 1) << 62 | (0xFF if b6 else 0) << 54 | (imm8 & 0x3F) << 48
            else:
                imm = ((imm8 >> 7) << 31 | (b6 ^ 1) << 30 | (0x1F if b6 else 0) << 25 | (imm8 & 0x3F) << 19) * 0x100000001
        if cmode < 12 and cmode & 1:
            kind = "bic" if op else "orr"
        elif cmode < 14 and op:
            imm = ~imm
        k = const(imm)
        if kind == "mov":
            return f"{{ c->v[{rd}].d[0] = {k}; c->v[{rd}].d[1] = {k if q else '0'}; }}"
        o = "&= ~" if kind == "bic" else "|= "
        return f"{{ c->v[{rd}].d[0] {o}{k}; c->v[{rd}].d[1] {(o + k) if q else '= 0'}; }}"

    def simd_shift_imm(self, w, scalar):
        q, u, immh, immb = (w >> 30) & 1, (w >> 29) & 1, (w >> 19) & 15, (w >> 16) & 7
        opcode, rn, rd = (w >> 11) & 31, (w >> 5) & 31, w & 31
        esize = 8 << (immh.bit_length() - 1)
        imm = immh << 3 | immb
        rsh, lsh = 2 * esize - imm, imm - esize
        uf, sf_ = UF[esize], SF[esize]
        if opcode in (0x1C, 0x1F):   # fixed-point <-> floating point
            if esize < 32 or (esize == 64 and not (q or scalar)):
                raise Unsupported("simd: reserved element size")
            n, ff = 1 if scalar else (64 << q) // esize, FF[esize]
            if opcode == 0x1C:
                return vop(rd, rn, rn, n, ff, f"(f{esize})a_.{uf if u else sf_}[i_] * {hexf(2.0 ** -rsh, esize == 32)}")
            return vop(rd, rn, rn, n, uf, f"f2{'u' if u else 'i'}{esize}((f64)a_.{ff}[i_] * {hexf(2.0 ** rsh, False)})")
        if opcode in (0x10, 0x11, 0x14):
            if scalar or esize == 64:
                raise Unsupported("simd: reserved")
            m, wide = 64 // esize, UF[esize * 2]
            part = m if q else 0
            if opcode == 0x14:   # sshll / ushll (sxtl / uxtl)
                src = f"(u64)a_.{uf}[{{}}]" if u else f"(u64)(i64)a_.{sf_}[{{}}]"
                return vset(rd, rn, rn, [(f"{wide}[{i}]", f"{src.format(i + part)} << {lsh}") for i in range(m)])
            if u:
                raise Unsupported("simd: saturating narrow")
            # shrn / rshrn (and the "2" forms: the upper half, the lower one kept). rshrn rounds in
            # 64 bits: a carry out of them would land above the narrow lane.
            src = f"a_.{wide}[{{}}] >> {rsh}" if opcode == 0x10 else f"((u64)a_.{wide}[{{}}] + {const(1 << (rsh - 1))}) >> {rsh}"
            return vset(rd, rn, rn, [(f"{uf}[{i + part}]", src.format(i)) for i in range(m)], bool(q))
        if (scalar and esize != 64) or (esize == 64 and not (q or scalar)):
            raise Unsupported("simd: reserved element size")
        n = 1 if scalar else (64 << q) // esize
        ua, sa, ud = f"a_.{uf}[i_]", f"a_.{sf_}[i_]", f"d_.{uf}[i_]"
        right = ("0" if rsh == esize else f"{ua} >> {rsh}") if u else f"{sa} >> {min(rsh, esize - 1)}"
        rounded = f"(u64)({right}) + (({ua} >> {rsh - 1}) & 1)"   # (x + 2^(rsh-1)) >> rsh, with no overflow
        ones = (1 << esize) - 1
        sli = f"({ud} & {const((1 << lsh) - 1)}) | ((u64){ua} << {lsh})"
        if opcode == 0x00:
            return vop(rd, rn, rn, n, uf, right)
        if opcode == 0x02:
            return vop(rd, rn, rn, n, uf, f"(u64){ud} + (u64)({right})")
        if opcode == 0x04:   # srshr / urshr
            return vop(rd, rn, rn, n, uf, rounded)
        if opcode == 0x06:   # srsra / ursra
            return vop(rd, rn, rn, n, uf, f"(u64){ud} + {rounded}")
        if opcode == 0x08 and u:   # sri: the destination keeps its top rsh bits
            return vop(rd, rn, rn, n, uf, ud if rsh == esize else f"({ud} & {const(ones ^ ones >> rsh)}) | ({ua} >> {rsh})")
        if opcode == 0x0A:   # shl / sli (which keeps the destination's low lsh bits)
            return vop(rd, rn, rn, n, uf, sli if u else f"(u64){ua} << {lsh}")
        raise Unsupported(f"simd: shift-by-immediate opcode {opcode:#x}")

    # ------------------------------------------------------------------ vector x indexed element
    def simd_indexed(self, w, scalar):
        q, u, size = (w >> 30) & 1, (w >> 29) & 1, (w >> 22) & 3
        l, m, rm4, opcode, h = (w >> 21) & 1, (w >> 20) & 1, (w >> 16) & 15, (w >> 12) & 15, (w >> 11) & 1
        rn, rd = (w >> 5) & 31, w & 31
        if opcode in (1, 5, 9):
            if size < 2 or (size == 3 and l):
                raise Unsupported("simd: half-precision / reserved")
            esize = 64 if size & 1 else 32
            index, rm = (h if size & 1 else h << 1 | l), m << 4 | rm4
            if esize == 64 and not (q or scalar):
                raise Unsupported("simd: reserved element size")
            n, f = 1 if scalar else (64 << q) // esize, FF[esize]
            fma = "fma" if esize == 64 else "fmaf"
            a, d, e = f"a_.{f}[i_]", f"d_.{f}[i_]", f"b_.{f}[{index}]"
            if (opcode, u) == (9, 1):
                return vop(rd, rn, rm, n, f, f"rt_fmulx{esize}({a}, {e})")
            expr = {(1, 0): (f"{fma}({a}, {e}, {d})", "3"), (5, 0): (f"{fma}(-{a}, {e}, {d})", "3 | RT_VNAN_NEG"),
                    (9, 0): (f"{a} * {e}", "2")}.get((opcode, u))
            if expr is None:
                raise Unsupported("simd: reserved")
            return vfp(rd, rn, rm, n, esize, *expr, index)
        if size not in (1, 2):
            raise Unsupported("simd: reserved element size")
        esize = 8 << size
        index, rm = (h << 2 | l << 1 | m, rm4) if size == 1 else (h << 1 | l, m << 4 | rm4)
        uf = UF[esize]
        if opcode in (0x2, 0x6, 0xA) and not scalar:   # mlal / mlsl / mull by element
            k, wide = 64 // esize, UF[esize * 2]
            xf, cast = (UF if u else SF)[esize], "(u64)" if u else "(u64)(i64)"
            part = k if q else 0
            pre = {0xA: "", 0x2: "(u64)d_.{w}[{i}] + ", 0x6: "(u64)d_.{w}[{i}] - "}[opcode]
            return vset(rd, rn, rm, [(f"{wide}[{i}]", pre.format(w=wide, i=i) +
                                      f"{cast}a_.{xf}[{i + part}] * {cast}b_.{xf}[{index}]") for i in range(k)])
        if scalar:
            raise Unsupported("simd: scalar saturating multiply")
        n = (64 << q) // esize
        a, d, e = f"a_.{uf}[i_]", f"d_.{uf}[i_]", f"b_.{uf}[{index}]"
        expr = {(8, 0): f"(u64){a} * {e}", (0, 1): f"(u64){d} + (u64){a} * {e}", (4, 1): f"(u64){d} - (u64){a} * {e}"}.get((opcode, u))
        if expr is None:
            raise Unsupported(f"simd: indexed opcode {opcode:#x}")
        return vop(rd, rn, rm, n, uf, expr)

    # ------------------------------------------------------------------ structure loads / stores
    def simd_ldst(self, w, base):
        q, post, load, rm, rt = (w >> 30) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 16) & 31, w & 31
        if w >> 31 or (not post and rm):
            raise Unsupported("simd: reserved")
        if not (w >> 24) & 1:
            layout = {0: (1, 4), 2: (4, 1), 4: (1, 3), 6: (3, 1), 7: (1, 1), 8: (1, 2), 10: (2, 1)}.get((w >> 12) & 15)
            if layout is None or (w >> 21) & 1:
                raise Unsupported("simd: reserved")
            rpt, selem = layout
            ebytes = 1 << ((w >> 10) & 3)
            total = (8 << q) * rpt * selem
            call = f"rt_ldst_multi(c, {load}, {rt}, {rpt}, {selem}, {ebytes}, {q}, a_);"
        else:
            r, opcode, s, size = (w >> 21) & 1, (w >> 13) & 7, (w >> 12) & 1, (w >> 10) & 3
            scale, selem, replicate, index = opcode >> 1, ((opcode & 1) << 1 | r) + 1, 0, 0
            if scale == 3:
                if not load or s:
                    raise Unsupported("simd: reserved")
                scale, replicate = size, 1
            elif scale == 0:
                index = q << 3 | s << 2 | size
            elif scale == 1:
                if size & 1:
                    raise Unsupported("simd: reserved")
                index = q << 2 | s << 1 | size >> 1
            elif size & 1:
                if size & 2 or s:
                    raise Unsupported("simd: reserved")
                index, scale = q, 3
            elif size & 2:
                raise Unsupported("simd: reserved")
            else:
                index = q << 1 | s
            ebytes = 1 << scale
            total = selem * ebytes
            call = f"rt_ldst_single(c, {load}, {replicate}, {rt}, {selem}, {ebytes}, {index}, {q}, a_);"
        if not post:
            return f"{{ u64 a_ = {base}; {call} }}"
        return f"{{ u64 a_ = {base}; {call} {base} = a_ + {const(total) if rm == 31 else reg(rm)}; }}"

    # ------------------------------------------------------------------ scalar fp stragglers
    def fp_fixed(self, w):
        sf, ftype, rmode, opcode = w >> 31, (w >> 22) & 3, (w >> 19) & 3, (w >> 16) & 7
        scale, rn, rd = (w >> 10) & 63, (w >> 5) & 31, w & 31
        fbits = 64 - scale
        if ftype > 1 or (not sf and scale < 32):
            raise Unsupported("fp: reserved fixed-point conversion")
        p, isz = "SD"[ftype], 64 if sf else 32
        if rmode == 0 and opcode in (2, 3):
            src = f"({'i' if opcode == 2 else 'u'}{isz}){reg(rn, sf)}"
            return f"w{p}(c, {rd}, (f{32 << ftype}){src} * {hexf(2.0 ** -fbits, ftype == 0)});"
        if rmode == 3 and opcode in (0, 1):
            fn = f"f2{'u' if opcode else 'i'}{isz}"
            return wr(rd, sf, f"({'u64' if sf else 'u32'}){fn}((f64)R{p}({rn}) * {hexf(2.0 ** fbits, False)})")
        raise Unsupported("fp: reserved fixed-point conversion")

    def fp_half(self, ftype, opcode, rn, rd):
        if ftype == 3 and opcode == 4:
            return f"wS(c, {rd}, rt_h2f(c->v[{rn}].h[0]));"
        if ftype == 3 and opcode == 5:
            return f"wD(c, {rd}, (f64)rt_h2f(c->v[{rn}].h[0]));"
        if ftype < 2 and opcode == 7:
            return f"{{ u16 h_ = rt_d2h((f64)R{'SD'[ftype]}({rn})); c->v[{rd}].d[0] = h_; c->v[{rd}].d[1] = 0; }}"
        raise Unsupported("fp: half-precision arithmetic")
