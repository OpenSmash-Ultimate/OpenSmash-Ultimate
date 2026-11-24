#!/bin/sh
# The code generated for x86-64 must not change: lift the litmus guest programs of test_lifter_atomics.py with the
# lifter of a base revision and with the working tree, compile each with its own runtime headers for this host
# (-fno-strict-aliasing -ffp-contract=off, at each level of OPTS) and compare the disassembly function by function.
# The FP and NEON helpers that have native versions behind RECOMP_A64_NATIVE are compared too (FRECPS / FRSQRTS
# included). Meant for x86-64; on any host without RECOMP_A64_NATIVE (macOS on Apple Silicon too) it must also pass.
#   tests/arm_host/x86_codegen_same.sh [base revision = merge-base of HEAD and origin/main] [compilers = "clang-19 gcc"]
#   OPTS="-O0 -O1 -O2" by default (the game library is built at -O1, a few large files at -O0; build_opt.py -O1 / -O2).
# Reads git objects only (git show); no game files.
set -e
HERE0=$(cd "$(dirname "$0")" && pwd)
BASE=${1:-$(git -C "$HERE0" merge-base HEAD origin/main)}
CCS=${2:-"clang-19 gcc"}
OPTS=${OPTS:-"-O0 -O1 -O2"}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
W=$(mktemp -d)
[ -n "$KEEP" ] && echo "work dir: $W" || trap 'rm -rf "$W"' EXIT
mkdir -p "$W/old/tools/lifter" "$W/old/tools/nro" "$W/old/runtime" "$W/old/tests/arm_host"
# the old lifter reads main.elf (and its layout) at import: drop those lines in the temporary copy
git -C "$ROOT" show "$BASE:./tools/lifter/recomp.py" | sed "s/^IMG = open(ELF, 'rb').read()/IMG = None/; s/^\(TEXT_SIZE, .*\) = _layout(IMG)\$/\1 = (0,) * 7/" > "$W/old/tools/lifter/recomp.py"
git -C "$ROOT" show "$BASE:./tools/nro/recomp_nro.py" > "$W/old/tools/nro/recomp_nro.py"
for f in runtime.h fpexact.h neon.h; do git -C "$ROOT" show "$BASE:./runtime/$f" > "$W/old/runtime/$f"; done
cp "$HERE/test_lifter_atomics.py" "$W/old/tests/arm_host/"
python3 "$W/old/tests/arm_host/test_lifter_atomics.py" --gen "$W/old_gen.h" > /dev/null
python3 "$HERE/test_lifter_atomics.py" --gen "$W/new_gen.h" > /dev/null
sed -i 's/^static void/void/' "$W/old_gen.h" "$W/new_gen.h"
cat > "$W/cmp.c" <<'EOF'
#include "runtime.h"
#include GEN
float u_recpe(float x) { return recpe_f(x); }
double u_rsqrte(double x) { return rsqrte_d(x); }
uint16_t u_f2h(float x) { return f32_to_half(x); }
double u_h2d(uint16_t h) { return half_to_f64(h); }
float u_maxnm(float a, float b) { return fp_maxmin32(a, b, 0, 1); }
double u_min(double a, double b) { return fp_maxmin64(a, b, 1, 0); }
uint32_t u_f2s(double v) { return f2s32(v); }
uint64_t u_f2u(double v) { return f2u64(v); }
void u_fadd(V128 *d, const V128 *n, const V128 *m) { v_f3v(d, n, m, F_ADD, 0, 4); }
void u_fmla(V128 *d, const V128 *n, const V128 *m) { v_f3v(d, n, m, F_MLA, 1, 2); }
void u_fmax(V128 *d, const V128 *n, const V128 *m) { v_f3v(d, n, m, F_MAX, 0, 4); }
void u_sqrt(V128 *d, const V128 *n) { v_f1v(d, n, F1_SQRT, 0, 4); }
void u_recpe_v(V128 *d, const V128 *n) { v_f1v(d, n, F1_RECPE, 0, 4); }
float u_fnan(float a, float b) { return fnan32(a * b); }
void u_i3v(V128 *d, const V128 *n, const V128 *m) { v_i3v(d, n, m, I_CMGT, 1, 8); }
void u_tbl(Ctx *c) { v_tbl(c, 1, 30, 3, 4, 1, 0); }
void u_ldn(Ctx *c, uint64_t a) { v_ldn(c, a, 2, 3, 2, 1); }
void u_stn(Ctx *c, uint64_t a) { v_stn(c, a, 2, 4, 1, 0); }
void u_f1v(V128 *d, const V128 *n) { v_f1v(d, n, F1_CVTZS, 1, 2); }
void u_recps(V128 *d, const V128 *n, const V128 *m) { v_f3v(d, n, m, F_RECPS, 0, 4); }
void u_rsqrts(V128 *d, const V128 *n, const V128 *m) { v_f3v(d, n, m, F_RSQRTS, 1, 2); }
#ifdef HAVE_PAIRS   /* upstream f4cc28c helpers, when the base revision has them */
uint32_t u_crc(uint32_t a, uint64_t v) { return crc32_arm(a, v, 8, 0x82f63b78u); }
uint32_t u_stxp(Ctx *c, uint64_t a, uint64_t lo, uint64_t hi) { return stxp64(c, a, lo, hi); }
uint32_t u_stx64(Ctx *c, uint64_t a, uint64_t v) { return stx64(c, a, v); }
void u_i3x(V128 *d, const V128 *n, const V128 *m) { v_i3x(d, n, m, IX_USHL, 2, 4); }
void u_across(V128 *d, const V128 *n) { v_across(d, n, AX_ADDV, 1, 8); }
void u_shiftx(V128 *d, const V128 *n) { v_shiftx(d, n, 3, 2, SX_SRSRA, 4); }
#endif
EOF
status=0
for CC in $CCS; do
  for OPT in $OPTS; do
    for v in old new; do
        inc="$W/old/runtime"; [ $v = new ] && inc="$ROOT/runtime"
        pairs=""; grep -q 'stxp64' "$W/old/runtime/runtime.h" && pairs=-DHAVE_PAIRS
        "$CC" $OPT -fno-strict-aliasing -ffp-contract=off -w -c $pairs -I"$inc" -DGEN="\"$W/${v}_gen.h\"" "$W/cmp.c" -o "$W/$v.o"
        # one line per instruction, prefixed by its function; no addresses
        objdump -d --no-show-raw-insn "$W/$v.o" | grep -v 'file format' |
            sed -E 's/^ *[0-9a-f]+:\t//; s/[0-9a-f]+ <([a-z_0-9]+)\+0x[0-9a-f]+>/<\1+X>/; s/(call|jmp) +[0-9a-f]+ <[^>]*>/\1/' |
            awk '/^[0-9a-f]+ <.*>:$/ {f = $2; next} {print f " " $0}' > "$W/$v.dis"
    done
    # Every function must be identical, instruction for instruction (addresses and call targets left out).
    python3 - "$W/old.dis" "$W/new.dis" "$CC $OPT" <<'PY' || status=1
import collections, sys
def load(p):
    d = collections.OrderedDict()
    for line in open(p):
        f, _, ins = line.rstrip('\n').partition(' ')
        d.setdefault(f, []).append(ins.replace(f[1:-2], 'SELF'))
    return d
old, new, cc = load(sys.argv[1]), load(sys.argv[2]), sys.argv[3]
bad = [f[1:-2] + ('' if f in old else '(new)') for f in new if old.get(f) != new[f]] + [f[1:-2] + '(gone)' for f in old if f not in new]
if bad:
    print('FAIL %s: functions with different x86 code: %s' % (cc, ' '.join(bad)))
    sys.exit(1)
print('ok   %s: %d functions, all identical' % (cc, len(new)))
PY
  done
done
exit $status
