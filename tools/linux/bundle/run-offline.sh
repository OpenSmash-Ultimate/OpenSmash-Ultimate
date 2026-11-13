#!/usr/bin/env bash
# Super Smash Bros. Ultimate (native port) on Linux, offline (run.sh plays online).
# Usage: bash run-offline.sh [--trace]. The log goes to build/play.log (and this terminal).
here="$(dirname "$(readlink -f "$0")")"
cd "${SSBU_GAME_DIR:-$here}" || exit 1   # (a WindowSmash folder built on Linux: see scripts/common.sh)
. "$here/scripts/common.sh"
find_game_data || exit 1
check_libraries || exit 1
unset SSBU_NET_RELAY SSBU_NET_HOST
mkdir -p build
echo "Starting the game offline (log: build/play.log)..."
set -o pipefail  # Preserve a game failure through tee for callers such as the desktop launcher.
"$SSBU_EXE" dump/modules --status 30 "$@" 2>&1 | tee build/play.log
