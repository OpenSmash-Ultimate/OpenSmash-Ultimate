# Sourced (from ssbu-recomp/) by the "native" mode of the tests/port suites (tests/port/winshim/run.sh): what a test build
# on a Linux machine, or on a device such as the Steam Frame, needs and may not have. Sets
#   SDL3_ROOT      (read, optional) an SDL3 install prefix to build against, such as one built from source
#   NAT_INC        compiler flags for the headers: the real SDL3 headers when SDL3_ROOT has them; the stand-ins of
#                  tests/port/common/stub (SDL3/SDL.h, openssl/evp.h) as a last resort (-idirafter: real system headers win)
#   NAT_SDL_LIBS   link flags of the real libSDL3, empty when there is none
#   NAT_SSL_LIBS   path of the system libcrypto.so.3 (no -dev package or symlink needed), empty when there is none
# nat_stubs OUT.c obj...  writes abort() stubs for the SDL_* / EVP_* symbols the objects need and no real library provides.
# No test that uses the stubs reaches those functions; one that does aborts with "stub called: <name>".
NAT_INC="-idirafter tests/port/common/stub"
NAT_SDL_LIBS=""
NAT_SSL_LIBS=""
if [ -n "${SDL3_ROOT:-}" ] && [ -f "$SDL3_ROOT/include/SDL3/SDL.h" ]; then
    NAT_INC="-I$SDL3_ROOT/include $NAT_INC"
    if ls "$SDL3_ROOT"/lib/libSDL3.so* >/dev/null 2>&1; then NAT_SDL_LIBS="-L$SDL3_ROOT/lib -Wl,-rpath,$SDL3_ROOT/lib -lSDL3"; fi
fi
for _d in /usr/lib /usr/lib64 /lib /lib64 /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu; do
    for _n in libcrypto.so.3 libcrypto.so; do
        if [ -z "$NAT_SSL_LIBS" ] && [ -e "$_d/$_n" ]; then NAT_SSL_LIBS="$_d/$_n"; fi
    done
done
nat_stubs() {
    _out=$1; shift
    _pat=""
    [ -n "$NAT_SDL_LIBS" ] || _pat="SDL"
    [ -n "$NAT_SSL_LIBS" ] || _pat="${_pat:+$_pat|}EVP"
    {
        echo 'int dprintf(int, const char *, ...); void abort(void);'
        if [ -n "$_pat" ]; then
            nm -u "$@" | sed -E -n "s/^ *(U +)?${NAT_SYM_PREFIX:-}(($_pat)_[A-Za-z0-9_]+)\$/void \\2(void) { dprintf(2, \"stub called: \\2\\\\n\"); abort(); }/p" | sort -u
        fi
    } > "$_out"
}
