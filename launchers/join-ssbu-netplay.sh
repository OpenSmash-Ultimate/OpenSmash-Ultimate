#!/usr/bin/env bash
# Starts the game built here, joining a host's private room (as Join SSBU Netplay.bat;
# docs/MULTIPLAYER.md).
# (The launchers of the Linux package in tools/linux/bundle, run for this folder.)
root="$(dirname "$(dirname "$(readlink -f "$0")")")"   # (the folder above this one)
export SSBU_GAME_DIR="$root" SSBU_EXE=build-linux/ssbu SSBU_DEPS_HINT="bash tools/linux/install.sh"
exec bash "$root/tools/linux/bundle/join-netplay.sh" "$@"
