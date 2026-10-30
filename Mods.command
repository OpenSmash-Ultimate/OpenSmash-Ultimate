#!/bin/bash
# Opens WindowSmash Mods, the mod manager (GameBanana's mods, installed and turned on and off).
# Built by tools/mac/build_mods_app.sh; this builds it first if it isn't there.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
app="$HOME/Applications/WindowSmash Mods.app"
[ -d "$app" ] || bash "$root/tools/mac/build_mods_app.sh" || exit 1
open "$app"
