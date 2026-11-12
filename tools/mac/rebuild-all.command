#!/bin/bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd -P)"
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
    echo "Python 3.10 or newer was not found. Run Install WindowSmash.command first." >&2
    read -r -p "Press Return to close this window. " _
    exit 1
fi

"$py" -u tools/build_game.py "$@"
echo "Checking the Python launcher syntax..."
"$py" -c 'import pathlib, sys; p = pathlib.Path(sys.argv[1]); compile(p.read_text(encoding="utf-8"), str(p), "exec")' \
    tools/weblauncher/launcher.py
echo "Game build and launcher syntax check complete."
read -r -p "Press Return to close this window. " _
