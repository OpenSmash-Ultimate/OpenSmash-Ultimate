#!/bin/bash
# Sets WindowSmash up on this Mac in one go (as OpenSmash.exe does on Windows): brings this
# folder up to date from GitHub, checks the Mac, installs what is missing (Apple's Command Line Tools,
# Python, SDL3), asks where your game files, keys and firmware are (found by itself when Ryujinx or one
# of yuzu's forks has them), builds the game, and puts Play SSBU in ~/Applications and on the Desktop.
# Internal setup entry point. Open OpenSmash.app, or run this in Terminal:
#
#   bash "tools/mac/install.sh"
#
# A step that is already done is skipped, so running it again carries on where it stopped, and run
# later it updates WindowSmash and builds again. tools/mac/install_mac.py does the work; options such
# as --dry-run are passed on to it (--help lists them). Everything it says also goes to install.log.
# (Written for macOS's own bash, 3.2.)
{
    self="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
    cd "$(dirname "$self")/../.." || exit 1

    # A Python of its own when the Mac has none: python-build-standalone's (the build Astral's uv
    # uses), unpacked into deps/python; it touches nothing else. Pinned, with its SHA-256 (GitHub's).
    PY_VERSION=3.14.8
    PY_RELEASE=20261013
    PY_FILE="cpython-$PY_VERSION+$PY_RELEASE-aarch64-apple-darwin-install_only.tar.gz"
    PY_URL="https://github.com/astral-sh/python-build-standalone/releases/download/$PY_RELEASE/cpython-$PY_VERSION%2B$PY_RELEASE-aarch64-apple-darwin-install_only.tar.gz"
    PY_SHA256=709f419d175eb015f810b68aa54bdae1d776c9824cb362aa701c93b0a9f2a9e7

    pause() {
        if [ -t 0 ]; then
            echo
            read -r -p "Press Return to close this window. " _
        fi
    }

    # Apple silicon only; a Terminal running under Rosetta (Open using Rosetta, or an Intel shell)
    # would make every program after it an Intel one: start again as arm64
    if [ "$(uname -m)" != arm64 ]; then
        if [ "$(sysctl -in sysctl.proc_translated 2>/dev/null)" = 1 ] && [ -z "$WINDOWSMASH_ARM64" ]; then
            export WINDOWSMASH_ARM64=1
            exec arch -arm64 /bin/bash "$self" "$@"
        fi
        echo "WindowSmash needs a Mac with Apple silicon (M1 or newer); this one has $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m)."
        pause
        exit 1
    fi
    if [ "$(id -u)" = 0 ]; then
        echo "Run this as yourself, not as root (not with sudo): it asks for an administrator's password itself when"
        echo "it needs one, and everything else belongs to you."
        pause
        exit 1
    fi

    # A Python 3.10 or newer that runs as arm64; never /usr/bin/python3, Apple's 3.9 (which, without
    # the Command Line Tools, only opens a window offering to install them)
    usable_python() {
        case "$1" in
            '' | /usr/bin/python3 | /usr/bin/python) return 1 ;;
        esac
        [ -x "$1" ] && "$1" -c 'import platform, sys; sys.exit(not (sys.version_info >= (3, 10) and platform.machine() == "arm64"))' >/dev/null 2>&1
    }
    # where to look, in order, one per line: WindowSmash's own, then each name on the PATH and where
    # Homebrew and python.org's installer put it (a Terminal's PATH may lack them)
    python_candidates() {
        local name
        echo "$PWD/deps/python/bin/python3"
        for name in python3 python3.15 python3.14 python3.13 python3.12 python3.11 python3.10; do
            type -aP "$name" 2>/dev/null
            echo "/opt/homebrew/bin/$name"
            echo "/usr/local/bin/$name"
            echo "/Library/Frameworks/Python.framework/Versions/Current/bin/$name"
        done
    }
    find_python() {
        local p
        while IFS= read -r p; do
            if usable_python "$p"; then
                echo "$p"
                return 0
            fi
        done < <(python_candidates)
        return 1
    }
    get_python() {
        local tgz="deps/downloads/$PY_FILE" got
        echo "Setting up WindowSmash needs Python 3.10 or newer, and this Mac has none that fits: putting Python"
        echo "$PY_VERSION into deps/python (this touches nothing else on the Mac)."
        mkdir -p deps/downloads || return 1
        if [ ! -f "$tgz" ] || [ "$(shasum -a 256 "$tgz" | cut -d ' ' -f 1)" != "$PY_SHA256" ]; then
            echo "  downloading $PY_URL"
            if ! curl -fsSL --retry 3 -o "$tgz.partial" "$PY_URL"; then
                rm -f "$tgz.partial"
                echo "Could not download Python. Check the internet connection, then run this again."
                return 1
            fi
            got=$(shasum -a 256 "$tgz.partial" | cut -d ' ' -f 1)
            if [ "$got" != "$PY_SHA256" ]; then
                rm -f "$tgz.partial"
                echo "The downloaded Python does not match its SHA-256 ($got, expected $PY_SHA256). Run this again to"
                echo "download it afresh."
                return 1
            fi
            mv -f "$tgz.partial" "$tgz"
        fi
        rm -rf deps/python.partial && mkdir -p deps/python.partial || return 1
        if ! tar -xzf "$tgz" -C deps/python.partial || [ ! -x deps/python.partial/python/bin/python3 ]; then
            rm -rf deps/python.partial
            echo "The downloaded Python could not be unpacked. Run this again to download it afresh."
            rm -f "$tgz"
            return 1
        fi
        rm -rf deps/python && mv deps/python.partial/python deps/python && rm -rf deps/python.partial && rm -f "$tgz"
        usable_python "$PWD/deps/python/bin/python3"
    }

    setup() {
        local py f
        echo "WindowSmash setup: $(date '+%Y-%m-%d %H:%M')"
        # Files downloaded from the internet are marked, and macOS asks about each one it runs: take
        # the mark off this folder's (what the build and the game make has none)
        for f in * .[!.]*; do
            case "$f" in
                dump | build | build-mac | deps | save | save_backups | screenshots | reports | .git | v[0-9]* | '*' | '.[!.]*') continue ;;
            esac
            xattr -dr com.apple.quarantine "$f" 2>/dev/null
        done
        py=$(find_python)
        if [ -z "$py" ]; then
            get_python || return 1
            py="$PWD/deps/python/bin/python3"
        fi
        "$py" -u tools/mac/install_mac.py "$@"
    }

    setup "$@" 2>&1 | tee -a install.log
    status=${PIPESTATUS[0]}
    pause
    exit "$status"
}
