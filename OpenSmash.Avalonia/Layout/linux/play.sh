#!/usr/bin/env bash
# Starts the game built here by tools/linux/install.sh, online, as OpenSmash.exe does on Windows:
# the game joins the lobby named in online_server.txt, where Local Wireless lists everyone's rooms
# (docs/MULTIPLAYER.md). If the lobby does not answer, the game plays offline. The log goes to
# build/play.log and this terminal. Usage: bash tools/linux/play.sh
# (The launchers of the Linux package in tools/linux/bundle, run for this folder.)
root="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd -P)"
export SSBU_GAME_DIR="$root" SSBU_EXE=build-linux/ssbu SSBU_DEPS_HINT="bash tools/linux/install.sh"
exec bash "$root/tools/linux/bundle/run.sh" "$@"
