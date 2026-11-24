#!/usr/bin/env bash
# Starts the game as a netplay player joining a host's relay (or an online lobby).
# No port forwarding needed here. See docs/MULTIPLAYER.md. The log goes to build/play.log.
here="$(dirname "$(readlink -f "$0")")"
cd "${SSBU_GAME_DIR:-$here}" || exit 1   # (an OpenSmash folder built on Linux: see scripts/common.sh)
. "$here/scripts/common.sh"
find_game_data || exit 1
check_libraries || exit 1
read -r -p "Host's address (IP or IP:port, from the host's netplay_address.txt): " SSBU_NET_RELAY
if [ -z "$SSBU_NET_RELAY" ]; then echo "An address is needed - ask the host for theirs."; exit 1; fi
read -r -p "Your player name [$USER]: " SSBU_NET_NAME
read -r -p "Room name - the same as the host's [smash]: " SSBU_NET_ROOM
export SSBU_NET_RELAY SSBU_NET_NAME="${SSBU_NET_NAME:-$USER}" SSBU_NET_ROOM="${SSBU_NET_ROOM:-smash}"
mkdir -p build
echo
echo "Starting the game, joining room \"$SSBU_NET_ROOM\" at $SSBU_NET_RELAY..."
echo "In the game: Online > Local Wireless, then pick the host's room from the Rooms List."
echo "The overlay (F2) shows whether the relay is reached and the ping to it."
"$SSBU_EXE" dump/modules --status 30 > build/play.log 2>&1
