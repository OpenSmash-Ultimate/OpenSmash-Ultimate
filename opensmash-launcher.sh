#!/usr/bin/env bash
# The OpenSmash Launcher on Linux: first-run setup, updates, Play, settings and the mod manager in a window, no
# terminal needed (what OpenSmash.exe is on Windows). tools/weblauncher/launcher.py serves it on this computer only
# and opens it in a browser's app window (Chrome, Chromium, Brave or Edge - installed or a Flatpak - else your
# default browser). Double-click "OpenSmash Launcher" in this folder, or run:  bash opensmash-launcher.sh
#
# Steam Deck / SteamOS / Bazzite Game Mode: add this script (or "OpenSmash Launcher" in the applications menu,
# which the launcher puts there) to Steam with "Add a Non-Steam Game" in Desktop Mode; in Game Mode the page
# fills the screen and a game pad drives it (docs/LINUX.md). It stays open while the game it started runs.
# Its log: build/launcher.log. Options for launcher.py (--fullscreen, --no-window, --port N) are passed on.
#
# (v0.5.0) The OpenSmash launcher itself - OpenSmash.Avalonia in this folder, the same launcher as Windows', with the
# account sign-in the official lobbies need - is started when it is here (tools/launcher_fetch.py puts it here: built
# from this folder with the .NET SDK, or the newest release's). The web launcher below is the fallback: with --web,
# when OpenSmash.Avalonia is not here, or when it stops at once (a missing system library: see build/launcher.log).
root="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
cd "$root" || exit 1
mkdir -p build
launcher=""
if [ -f "$root/OpenSmash.Avalonia" ] && [ -x "$root/OpenSmash.Avalonia" ]; then
    launcher="$root/OpenSmash.Avalonia"
elif [ -f "$root/build/OpenSmash.Avalonia" ] && [ -x "$root/build/OpenSmash.Avalonia" ]; then
    launcher="$root/build/OpenSmash.Avalonia"
fi
if [ "${1:-}" = --web ]; then
    shift
elif [ -n "$launcher" ]; then
    "$launcher" --project "$root" "$@" >>build/launcher.log 2>&1 &
    pid=$!
    for _ in 1 2 3 4 5 6; do
        sleep 1
        kill -0 "$pid" 2>/dev/null || break
    done
    if kill -0 "$pid" 2>/dev/null; then
        wait "$pid"
        exit $?
    fi
    wait "$pid"
    rc=$?
    [ "$rc" = 0 ] && exit 0
    echo "$launcher stopped at once (exit $rc): the web launcher instead" >>build/launcher.log
    export OPENSMASH_LAUNCHER_FAILED="$rc"   # (the web page says so where it asks to sign in)
fi
py=""
for p in "$root/deps/venv/bin/python" python3 python3.14 python3.13 python3.12 python3.11 python3.10 python; do
    if command -v "$p" >/dev/null 2>&1 && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then
        py="$p"
        break
    fi
done
if [ -z "$py" ]; then
    msg="The OpenSmash Launcher needs Python 3.10 or newer. Install it from your distribution (python3), or run  bash install-windowsmash.sh  in a terminal."
    if command -v zenity >/dev/null 2>&1; then zenity --error --title=OpenSmash --text="$msg" 2>/dev/null
    elif command -v kdialog >/dev/null 2>&1; then kdialog --title OpenSmash --error "$msg" 2>/dev/null
    else echo "$msg"; fi
    exit 1
fi
exec "$py" -u "$root/tools/weblauncher/launcher.py" --tree "$root" "$@" >>build/launcher.log 2>&1
