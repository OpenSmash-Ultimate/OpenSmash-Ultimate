#!/usr/bin/env bash
# Super Smash Bros. Ultimate (native port) on Linux, online: the game joins the lobby named in
# online_server.txt, where Local Wireless lists everyone's rooms (see docs/MULTIPLAYER.md). If the lobby
# does not answer, the game plays offline; run-offline.sh never goes online.
# Usage: bash run.sh [--trace]. The log goes to build/play.log (and this terminal). See docs/LINUX.md.
# (--online: the game reads those files itself too, so the settings menu's changes count when it
# starts itself again.)
here="$(dirname "$(readlink -f "$0")")"
cd "${SSBU_GAME_DIR:-$here}" || exit 1   # (a WindowSmash folder built on Linux: see scripts/common.sh)
. "$here/scripts/common.sh"
find_game_data || exit 1
check_libraries || exit 1
go_online
mkdir -p build
echo "Starting the game (log: build/play.log)..."
set -o pipefail  # Preserve a game failure through tee for callers such as the desktop launcher.
"$SSBU_EXE" dump/modules --status 30 --online "$@" 2>&1 | tee build/play.log
