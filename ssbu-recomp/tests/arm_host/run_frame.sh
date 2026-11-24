#!/bin/sh
# Lifter / runtime checks for an ARM host (memory ordering, exact FP, native NEON).
#   tests/arm_host/run_frame.sh [fp random pairs = 20000000] [litmus iterations = 1000000]
#       on the Frame (aarch64 Linux): everything runs here, in this checkout (the same as --local)
#   tests/arm_host/run_frame.sh --local [pairs] [iterations]
#       the same on any host. Needs python3, and for the checks themselves a GNU/LLVM toolchain with `as` and objdump.
#   tests/arm_host/run_frame.sh <ssh host> [pairs] [iterations]
#       from a PC: <ssh host> is the aarch64 device (an ssh destination with key login)
# 1. decoder test (python3), lifted litmus programs, software-FP hash (cc / clang, -DSOFT_ONLY)
# 2. the target runs frame_tests.sh: codegen of the ordering macros, litmus tests, FP vs. the instructions, neon.h native vs.
#    portable. From a PC, runtime/{runtime,fpexact,neon}.h and this directory are copied to ~/ssbu-port/lifter/ on the host
#    first. Locally everything is staged in $FRAME_WORK/lifter (default ${TMPDIR:-/tmp}/ssbu-lifter): nothing is written
#    next to the sources.
# The software-FP hash must be the same on every host: from a PC it is compared with the x86 one automatically; locally,
# FRAME_REF_HASH=<file with the "soft-hash ..." line of another host> compares it, otherwise the hash is only printed.
# FRAME_CFLAGS / FRAME_CCS / FRAME_ARCHS (see frame_tests.sh) are passed on when set; FRAME_QUICK=1 (local) keeps the first
# compiler and the default architecture only. No game files are used.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MODE=remote
if [ "${1:-}" = --local ]; then MODE=local; shift
else
    case "${1:-}" in
    "" | [0-9]*) if [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ]; then MODE=local; fi ;;
    esac
fi
export PYTHONDONTWRITEBYTECODE=1

if [ "$MODE" = local ]; then
    PAIRS=${1:-20000000}
    ITERS=${2:-1000000}
    W=${FRAME_WORK:-${TMPDIR:-/tmp}/ssbu-lifter}/lifter
    if ! command -v python3 >/dev/null 2>&1; then echo "SKIP: python3 not found"; exit 77; fi
    rm -rf "$W"
    mkdir -p "$W/runtime" "$W/arm_host"
    python3 "$HERE/test_lifter_atomics.py"
    python3 "$HERE/test_lifter_atomics.py" --gen "$W/arm_host/litmus_gen.h"
    python3 "$HERE/test_lifter_atomics.py" --asm > "$W/arm_host/litmus.s"
    python3 "$HERE/test_lifter_atomics.py" --words > "$W/arm_host/litmus.words"
    cp "$ROOT/runtime/runtime.h" "$ROOT/runtime/fpexact.h" "$ROOT/runtime/neon.h" "$W/runtime/"
    cp "$HERE/frame_tests.sh" "$HERE/litmus.c" "$HERE/codegen_check.c" "$HERE/fp_native_test.c" "$HERE/neon_native_test.c" "$W/arm_host/"
    CCS=""
    for cc in gcc clang; do command -v $cc >/dev/null 2>&1 && CCS="$CCS $cc"; done
    CCS=${CCS# }
    if [ -z "$CCS" ]; then echo "no gcc or clang on PATH"; exit 2; fi
    if [ -n "${FRAME_QUICK:-}" ]; then CCS=${CCS%% *}; FRAME_ARCHS=${FRAME_ARCHS:-default}; fi
    CC1=${CCS%% *}
    "$CC1" -O1 -fno-strict-aliasing -ffp-contract=off -DSOFT_ONLY -w -I"$ROOT/runtime" "$HERE/fp_native_test.c" -o "$W/fp_soft" -lm -lpthread
    "$W/fp_soft" "$PAIRS" | grep soft-hash > "$W/arm_host/this.hash"
    echo "software FP on this host ($CC1): $(cat "$W/arm_host/this.hash")"
    REF=""; HASHFAIL=0
    if [ -n "${FRAME_REF_HASH:-}" ] && [ -f "$FRAME_REF_HASH" ]; then
        cp "$FRAME_REF_HASH" "$W/arm_host/ref.hash"; REF=ref.hash
        if cmp -s "$W/arm_host/this.hash" "$W/arm_host/ref.hash"; then echo "ok   software FP hash = $FRAME_REF_HASH"
        else echo "FAIL software FP hash differs from $FRAME_REF_HASH"; HASHFAIL=1; fi
    else
        echo "NOTE: software-FP hash not compared with another host (set FRAME_REF_HASH=<file>; the hash above is what to compare)"
    fi
    FRAME_CCS=${FRAME_CCS:-$CCS}
    export FRAME_CCS FRAME_ARCHS FRAME_CFLAGS
    st=0
    sh "$W/arm_host/frame_tests.sh" "$PAIRS" "$ITERS" $REF || st=1
    [ "$HASHFAIL" = 0 ] || st=1
    exit $st
fi

HOST=${1:-}
case "$HOST" in
"" | [0-9]* | -*) echo "usage: $0 <ssh host> [pairs] [iterations]   (or, on an aarch64 Linux host: $0 [--local] [pairs] [iterations])"; exit 2 ;;
esac
PAIRS=${2:-20000000}
ITERS=${3:-1000000}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

python3 "$HERE/test_lifter_atomics.py"
python3 "$HERE/test_lifter_atomics.py" --gen "$W/litmus_gen.h"
python3 "$HERE/test_lifter_atomics.py" --asm > "$W/litmus.s"
python3 "$HERE/test_lifter_atomics.py" --words > "$W/litmus.words"

CC=${CC:-$(command -v clang-19 || command -v clang || command -v cc)}
"$CC" -O1 -fno-strict-aliasing -ffp-contract=off -DSOFT_ONLY -w -I"$ROOT/runtime" "$HERE/fp_native_test.c" -o "$W/fp_soft" -lm -lpthread
"$W/fp_soft" "$PAIRS" | grep soft-hash > "$W/x86.hash"
echo "x86 ($CC): $(cat "$W/x86.hash")"

ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/lifter/runtime ~/ssbu-port/lifter/arm_host'
scp -q "$ROOT/runtime/runtime.h" "$ROOT/runtime/fpexact.h" "$ROOT/runtime/neon.h" "$HOST:ssbu-port/lifter/runtime/"
scp -q "$HERE/frame_tests.sh" "$HERE/litmus.c" "$HERE/codegen_check.c" "$HERE/fp_native_test.c" "$HERE/neon_native_test.c" \
    "$W/litmus_gen.h" "$W/litmus.s" "$W/litmus.words" "$W/x86.hash" "$HOST:ssbu-port/lifter/arm_host/"
# FRAME_CFLAGS / FRAME_CCS / FRAME_ARCHS (see frame_tests.sh) are passed on when set
ssh -o BatchMode=yes "$HOST" "FRAME_CFLAGS='${FRAME_CFLAGS:-}' FRAME_CCS='${FRAME_CCS:-}' FRAME_ARCHS='${FRAME_ARCHS:-}' sh ~/ssbu-port/lifter/arm_host/frame_tests.sh $PAIRS $ITERS x86.hash"
