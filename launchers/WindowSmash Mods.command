#!/bin/bash
# Opens WindowSmash Mods, the mod manager (tools/modmanager/server.py in a window of its own).
# Built by tools/mac/build_mods_app.sh; this builds it first if it isn't there. Without Apple's
# Command Line Tools (no app can be built), it opens in the browser instead.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
app="$HOME/Applications/WindowSmash Mods.app"
if [ -d "$app" ] || bash "$root/tools/mac/build_mods_app.sh" >/dev/null 2>&1; then
    open "$app"
    exit 0
fi
py=""
for p in "$root/deps/venv/bin/python" "$root/deps/python/bin/python3" python3; do
    if command -v "$p" >/dev/null 2>&1 && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then py="$p"; break; fi
done
[ -z "$py" ] && { echo "WindowSmash's Python wasn't found: install WindowSmash first."; read -r -p "Press Return to close." _; exit 1; }
echo "WindowSmash Mods is opening in your browser (keep this window open while you use it)."
"$py" "$root/tools/modmanager/server.py" --tree "$root"
