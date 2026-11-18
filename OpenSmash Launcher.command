#!/bin/bash
# The OpenSmash Launcher on macOS: first-run setup, updates, Play, settings and the mod manager in a window
# (tools/weblauncher/launcher.py, on this Mac only; Chrome, Edge or Brave show it as an app window, else your
# default browser). Double-click it in Finder. Keep this Terminal window open while you use the launcher.
# (Written for macOS's own bash, 3.2.)
root="$(cd "$(dirname "$0")" && pwd -P)"
cd "$root" || exit 1
# (v0.5.0) The OpenSmash launcher itself - OpenSmash.Avalonia.app in this folder, the same launcher as Windows', with
# the account sign-in the official lobbies need - opens when it is here (tools/launcher_fetch.py puts it here: built
# with the .NET SDK, or the newest release's; double-clicking OpenSmash.Avalonia.app does the same). The web launcher
# below is the fallback: with --web, or when the app is not here or does not open.
if [ "${1:-}" = --web ]; then
    shift
elif [ -d "$root/OpenSmash.Avalonia.app" ] && open "$root/OpenSmash.Avalonia.app" --args --project "$root" "$@"; then
    exit 0
fi
py=""
for p in "$root/deps/venv/bin/python" "$root/deps/python/bin/python3" python3; do
    if command -v "$p" >/dev/null 2>&1 && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then
        py="$p"
        break
    fi
done
if [ -z "$py" ]; then
    echo "The OpenSmash Launcher needs Python 3.10 or newer: run Install WindowSmash.command once (it gets one),"
    echo "or install Python from python.org."
    read -r -p "Press Return to close this window. " _
    exit 1
fi
echo "The OpenSmash Launcher is opening in your browser (keep this window open while you use it)."
exec "$py" -u "$root/tools/weblauncher/launcher.py" --tree "$root" "$@"
