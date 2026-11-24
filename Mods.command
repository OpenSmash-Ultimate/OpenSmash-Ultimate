#!/bin/bash
# Opens OpenSmash Mods, the mod manager (GameBanana's mods, installed and turned on and off).
# Built by tools/mac/build_mods_app.sh; this builds it first if it isn't there.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
app="$HOME/Applications/OpenSmash Mods.app"
old="$HOME/Applications/WindowSmash Mods.app"   # (its name before the rename: build_mods_app.sh replaces it)
if [ ! -d "$app" ] && ! bash "$root/tools/mac/build_mods_app.sh"; then
    [ -d "$old" ] || exit 1
    app="$old"
fi
open "$app"
