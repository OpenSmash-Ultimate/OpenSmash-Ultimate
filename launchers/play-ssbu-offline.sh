#!/usr/bin/env bash
# Starts the game built here by tools/linux/install.sh, offline (as Play SSBU Offline.bat).
# The log goes to build/play.log and this terminal.
# (The launchers of the Linux package in tools/linux/bundle, run for this folder.)
root="$(dirname "$(dirname "$(readlink -f "$0")")")"   # (the folder above this one)
export SSBU_GAME_DIR="$root" SSBU_EXE=build-linux/ssbu SSBU_DEPS_HINT="bash tools/linux/install.sh"
exec bash "$root/tools/linux/bundle/run-offline.sh" "$@"
