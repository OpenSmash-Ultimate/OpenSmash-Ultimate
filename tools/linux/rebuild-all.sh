#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "$root"

py=""
for candidate in "$root/deps/venv/bin/python" "$root/deps/python/bin/python3" python3; do
    if command -v "$candidate" >/dev/null 2>&1 &&
       "$candidate" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then
        py="$candidate"
        break
    fi
done
if [ -z "$py" ]; then
    echo "Python 3.10 or newer was not found. Run the setup first." >&2
    exit 1
fi

"$py" -u tools/build_game.py "$@"
# The desktop launcher (Avalonia) needs the .NET 8 SDK, which players need not have: they keep the release
# launcher, so without dotnet only the game is rebuilt. Close OpenSmash before its binary is replaced.
if command -v dotnet >/dev/null 2>&1; then
    echo "Building the OpenSmash launcher..."
    "$py" -u tools/build_launcher.py --rid linux-x64
    echo "Game and launcher build complete."
else
    echo "dotnet (.NET 8 SDK) was not found: the launcher was not rebuilt. Game build complete."
fi
