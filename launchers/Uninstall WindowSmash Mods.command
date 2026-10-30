#!/bin/bash
# Removes WindowSmash Mods and puts the game back as it was: the game reads its own files again,
# the modded data is deleted, the mod manager is removed (files it replaced put back); it asks
# whether to delete your downloaded mods too. (tools/modmanager/install_mods.py --uninstall)
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
py=""
for p in "$root/deps/venv/bin/python" "$root/deps/python/bin/python3" python3; do
    if command -v "$p" >/dev/null 2>&1 && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then py="$p"; break; fi
done
[ -z "$py" ] && { echo "WindowSmash's Python wasn't found."; read -r -p "Press Return to close." _; exit 1; }
"$py" "$root/tools/modmanager/install_mods.py" --target "$root" --uninstall "$@"
read -r -p "Press Return to close." _
