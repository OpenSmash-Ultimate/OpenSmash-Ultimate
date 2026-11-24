#!/bin/sh
# Runs on the AArch64 target (copied there by run_frame.sh, next to ../runtime/*.h). No root, writes only here.
#   sh frame_tests.sh [fp random pairs] [litmus iterations per thread] [x86 soft-hash file]
# Exit status 0 only if everything passed.
cd "$(dirname "$0")" || exit 2
# Frame safety caps: bound the user's process count (litmus is multithreaded) and every test binary's run time
N=0; if [ "$(uname -s)" = Linux ]; then N=$(ps -eLf --no-headers -u "$(id -un)" 2>/dev/null | wc -l); fi
if [ "${N:-0}" -gt 0 ] 2>/dev/null; then ulimit -u $((N + 300)) 2>/dev/null; fi
TMO="timeout ${TEST_TIMEOUT:-600}"
PAIRS=${1:-20000000}
ITERS=${2:-1000000}
X86HASH=${3:-}
# FRAME_CFLAGS / FRAME_CCS / FRAME_ARCHS: run the same checks with the flags of the real build (scripts/setup.py --target
# linux-arm64: FRAME_CCS=clang FRAME_ARCHS=-mcpu=cortex-x4 FRAME_CFLAGS="-O2 -fno-math-errno -fno-omit-frame-pointer")
CFLAGS="-O1 -fno-strict-aliasing -ffp-contract=off -I../runtime -I. $FRAME_CFLAGS"
CCS=${FRAME_CCS:-gcc clang}
ARCHS=${FRAME_ARCHS:-default -mno-outline-atomics -march=armv8.2-a+rcpc}      # "default": no -m flag
FAILED=0
pass() { echo "ok   $*"; }
fail() { echo "FAIL $*"; FAILED=1; }

# ---------------------------------------------------------------- 0. guest words of the litmus programs
if as litmus.s -o litmus_s.o 2>as.err; then
    objdump -d litmus_s.o | awk '/^ +[0-9a-f]+:\t[0-9a-f]{8} /{print $2}' > litmus_as.words
    if cmp -s litmus_as.words litmus.words; then pass "litmus guest words = GNU as ($(wc -l < litmus.words) words)"
    else fail "litmus guest words differ from GNU as"; diff litmus_as.words litmus.words | head; fi
else fail "as litmus.s"; cat as.err; fi

# ---------------------------------------------------------------- 1. codegen of the ordering macros
body() {   # body <object> <function>: its disassembly, one instruction per line
    objdump -d --no-show-raw-insn "$1" | awk -v f="<$2>:" '$2 == f {p = 1; next} /^$/ {p = 0} p' | cut -f2-
}
want() {   # want <object> <function> <regex that must match> [regex that must not match]
    b=$(body "$1" "$2")
    if [ -z "$b" ]; then fail "$TAG $2: function not found"; return; fi
    if ! printf '%s\n' "$b" | grep -Eq "$3"; then fail "$TAG $2: no /$3/"; printf '%s\n' "$b" | sed 's/^/       /'; return; fi
    if [ -n "$4" ] && printf '%s\n' "$b" | grep -Eq "$4"; then fail "$TAG $2: has /$4/"; printf '%s\n' "$b" | sed 's/^/       /'; return; fi
    pass "$TAG $2: $(printf '%s\n' "$b" | grep -Eo "$3" | head -1)"
}
for CC in $CCS; do
    for ARCH in $ARCHS; do
        [ "$ARCH" = default ] && ARCH=""
        TAG="[$CC ${ARCH:-default}]"
        O=cg_$CC$(echo "$ARCH" | tr -c 'a-z0-9' '_').o
        if ! $CC $CFLAGS $ARCH -c codegen_check.c -o "$O"; then fail "$TAG compile codegen_check.c"; continue; fi
        want "$O" t_rdacq64 'ldar[[:space:]]+x' 'ldapr|dmb'
        want "$O" t_rdacq32 'ldar[[:space:]]+w' 'ldapr|dmb'
        want "$O" t_rdacq16 'ldarh[[:space:]]' 'ldapr|dmb'
        want "$O" t_rdacq8 'ldarb[[:space:]]' 'ldapr|dmb'
        want "$O" t_wrrel64 'stlr[[:space:]]+x' 'dmb'
        want "$O" t_wrrel32 'stlr[[:space:]]+w' 'dmb'
        want "$O" t_wrrel16 'stlrh[[:space:]]' 'dmb'
        want "$O" t_wrrel8 'stlrb[[:space:]]' 'dmb'
        want "$O" t_fence_full 'dmb[[:space:]]+ish$' 'ishld|ishst'
        want "$O" t_fence_acq 'dmb[[:space:]]+ishld'
        want "$O" t_fence_st 'dmb[[:space:]]+ishst'
        want "$O" t_compiler_barrier '^ret' 'dmb|isb'
        # STXR: acquire CAS (LSE casa, outline helper _acq, or an ldaxr/stxr loop), no release; STLXR: acquire+release CAS
        want "$O" t_stx64 '(^casa[[:space:]])|__aarch64_cas8_acq$|__aarch64_cas8_acq>|(^ldaxr[[:space:]])' 'casal|casl[[:space:]]|stlxr|_acq_rel|_rel|dmb'
        want "$O" t_stx32 '(^casa[[:space:]])|__aarch64_cas4_acq$|__aarch64_cas4_acq>|(^ldaxr[[:space:]])' 'casal|casl[[:space:]]|stlxr|_acq_rel|_rel|dmb'
        want "$O" t_stlx64 '(^casal[[:space:]])|__aarch64_cas8_acq_rel|(^stlxr[[:space:]])'
        want "$O" t_stlx32 '(^casal[[:space:]])|__aarch64_cas4_acq_rel|(^stlxr[[:space:]])'
        # STXP / STLXP: one inline 16-byte CAS or the outline helper, never libatomic; STLXP must release (GCC: __sync = full)
        want "$O" t_stxp64 '(^casp)|__aarch64_cas16_|(^ldaxp[[:space:]])|(^ldxp[[:space:]])' '__atomic_compare_exchange'
        want "$O" t_stlxp64 '(^caspal[[:space:]])|__aarch64_cas16_(acq_rel|sync)|(^stlxp[[:space:]])' '__atomic_compare_exchange'
        want "$O" t_rd64 '^ldr[[:space:]]+x0, \[x0\]' 'ldar|dmb'
        want "$O" t_wr64 '^str[[:space:]]+x1, \[x0\]' 'stlr|dmb'
    done
done

# ---------------------------------------------------------------- 2. litmus tests on lifted code
for CC in $CCS; do
    if $CC $CFLAGS litmus.c -o litmus_$CC -lpthread; then
        echo "---- litmus ($CC, $ITERS iterations, 8 threads)"
        if $TMO ./litmus_$CC "$ITERS" 8; then pass "litmus $CC"; else fail "litmus $CC"; fi
    else fail "compile litmus.c ($CC)"; fi
done

# ---------------------------------------------------------------- 3. floating point vs. the instructions; neon.h native vs. portable
for CC in $CCS; do
    if $CC $CFLAGS fp_native_test.c -o fp_native_$CC -lm -lpthread; then
        echo "---- fp ($CC, $PAIRS random pairs)"
        EX=""; [ "$CC" = gcc ] && EX=--exhaustive
        $TMO ./fp_native_$CC "$PAIRS" $EX > fp_$CC.out; st=$?
        cat fp_$CC.out
        if [ $st -eq 0 ]; then pass "fp $CC"; else fail "fp $CC"; fi
    else fail "compile fp_native_test.c ($CC)"; fi
done
for CC in $CCS; do
    if $CC $CFLAGS neon_native_test.c -o neon_native_$CC -lm; then
        echo "---- neon native vs portable ($CC)"
        if $TMO ./neon_native_$CC 200000; then pass "neon $CC"; else fail "neon $CC"; fi
    else fail "compile neon_native_test.c ($CC)"; fi
done
if [ -n "$X86HASH" ] && [ -f "$X86HASH" ]; then
    set -- $CCS
    A=$(grep soft-hash "fp_$1.out"); B=$(cat "$X86HASH")
    if [ "$A" = "$B" ]; then pass "software FP results on x86 = on AArch64 ($A)"; else fail "x86 soft hash '$B' != aarch64 '$A'"; fi
fi

if [ $FAILED -eq 0 ]; then echo "ALL FRAME TESTS PASSED"; else echo "SOME FRAME TESTS FAILED"; fi
exit $FAILED
