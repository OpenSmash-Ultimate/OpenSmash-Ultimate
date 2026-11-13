#!/usr/bin/env bash
# Unlocks every fighter in this folder's save (save/), keeping a copy of it first in
# save_backups/ (as Unlock All Fighters.bat). Run it with the game closed. See docs/DLC-AND-UNLOCKS.md.
# (The launchers of the Linux package in tools/linux/bundle, run for this folder.)
root="$(dirname "$(dirname "$(readlink -f "$0")")")"   # (the folder above this one)
export SSBU_GAME_DIR="$root" SSBU_EXE=build-linux/ssbu SSBU_DEPS_HINT="bash tools/linux/install.sh"
exec bash "$root/tools/linux/bundle/unlock-all-fighters.sh" "$@"
