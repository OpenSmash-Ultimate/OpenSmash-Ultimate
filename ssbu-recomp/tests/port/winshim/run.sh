#!/bin/sh
# The Win32 stand-in layer (runtime/plat/windows.h + plat_posix.c) as the runtime uses it, beyond tests/plat_selftest.c:
#   test_files     FindFirstFileA's order and wildcards, MoveFileExA without replacing (8 threads onto one name),
#                  CopyFileA from a folder failing before the target is touched, file views from 8 threads
# No game data. SDL3 and OpenSSL are stand-ins when their headers are missing (no test reaches them).
#   tests/port/winshim/run.sh [mode]      (from anywhere: it changes to ssbu-recomp/ itself)
#     native  (default) build and run here with $CC (cc): any Linux
#     mac     macOS: build and run natively with clang (macOS's own code paths; not yet run on a Mac)
#   OUT (default ${TMPDIR:-/tmp}/ssbu-winshim-<mode>) for the binaries, CC, CFLAGS_EXTRA.
set -eu
cd "$(dirname "$0")/../../.."
MODE=${1:-native}
OUT=${OUT:-${TMPDIR:-/tmp}/ssbu-winshim-$MODE}; mkdir -p "$OUT"
T=tests/port/winshim
. tests/port/native_env.sh
BASE="-O1 -g -w -fvisibility=hidden -fno-omit-frame-pointer -include runtime/plat/plat_pre.h -I runtime/plat -I runtime -I tests/port/common ${CFLAGS_EXTRA:-}"
case "$MODE" in
native)
    CC=${CC:-cc}
    F="-fno-pie $BASE $NAT_INC"
    L="-no-pie -rdynamic $NAT_SDL_LIBS $NAT_SSL_LIBS -lm -lpthread -ldl" ;;
mac)
    CC=${CC:-clang}
    SDK=$(xcrun --show-sdk-path)
    NAT_SSL_LIBS=""; NAT_SDL_LIBS=""; NAT_SYM_PREFIX=_
    F="-isysroot $SDK -mmacosx-version-min=14.4 $BASE -idirafter tests/port/common/stub"
    L="" ;;
*) echo "mode: native | mac"; exit 2 ;;
esac
$CC $F -c runtime/plat_posix.c -o "$OUT/plat_posix.o"
nat_stubs "$OUT/stubs.c" "$OUT/plat_posix.o"                       # abort() stubs for the SDL_* / EVP_* symbols no real library gives
$CC $F -c "$OUT/stubs.c" -o "$OUT/stubs.o"
OBJS="$OUT/plat_posix.o $OUT/stubs.o"
for t in test_files; do
    $CC $F $T/$t.c $OBJS $L -o "$OUT/$t"
done
TO=""                                                          # a time limit where the system has one (stock macOS has neither)
if command -v timeout >/dev/null 2>&1; then TO="timeout 300"; elif command -v gtimeout >/dev/null 2>&1; then TO="gtimeout 300"; fi
fail=0
for t in test_files; do
    echo "== $OUT/$t"
    $TO "$OUT/$t" || { echo "$t: exit $?"; fail=1; }
done
exit $fail
