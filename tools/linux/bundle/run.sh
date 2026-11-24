#!/usr/bin/env bash
# Super Smash Bros. Ultimate (native port) on Linux, online: the game joins the lobby named in
# online_server.txt, where Local Wireless lists everyone's rooms (see docs/MULTIPLAYER.md). If the lobby
# does not answer, the game plays offline; run-offline.sh never goes online.
# Usage: bash run.sh [--trace]. The log goes to build/play.log (and this terminal). See docs/LINUX.md.
# (--online: the game reads those files itself too, so the settings menu's changes count when it
# starts itself again.)
here="$(dirname "$(readlink -f "$0")")"
cd "${SSBU_GAME_DIR:-$here}" || exit 1   # (an OpenSmash folder built on Linux: see scripts/common.sh)
. "$here/scripts/common.sh"
find_game_data || exit 1
check_libraries || exit 1
go_online
mkdir -p build
# (v0.5.0) the official lobbies let in only a signed-in account's game: started without the launcher, the lobby ticket
# the OpenSmash launcher wrote last (build/cache/online_pass.txt; good while it has not expired)
if [ -z "${SSBU_ONLINE_TICKET_FILE:-}" ] && [ -f build/cache/online_pass.txt ]; then
    export SSBU_ONLINE_TICKET_FILE="$PWD/build/cache/online_pass.txt"
fi
echo "Starting the game (log: build/play.log)..."
"$SSBU_EXE" dump/modules --status 30 --online "$@" 2>&1 | tee build/play.log
