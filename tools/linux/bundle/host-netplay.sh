#!/usr/bin/env bash
# Starts the game as the netplay host: it runs the relay the other players join (UDP port
# 39420, opened on the router with UPnP when the router allows it). See docs/MULTIPLAYER.md.
# The address to give the other players appears in the game's overlay (F2) and in
# netplay_address.txt; the log goes to build/play.log.
here="$(dirname "$(readlink -f "$0")")"
cd "${SSBU_GAME_DIR:-$here}" || exit 1   # (an OpenSmash folder built on Linux: see scripts/common.sh)
. "$here/scripts/common.sh"
find_game_data || exit 1
check_libraries || exit 1
read -r -p "Your player name [$USER]: " SSBU_NET_NAME
read -r -p "Room name - the other players use the same one [smash]: " SSBU_NET_ROOM
read -r -p "UDP port [39420]: " SSBU_NET_PORT
export SSBU_NET_NAME="${SSBU_NET_NAME:-$USER}" SSBU_NET_ROOM="${SSBU_NET_ROOM:-smash}" SSBU_NET_PORT="${SSBU_NET_PORT:-39420}" SSBU_NET_HOST=1
rm -f netplay_address.txt
mkdir -p build
"$SSBU_EXE" dump/modules --status 30 > build/play.log 2>&1 &
echo
echo "Starting the game as host of room \"$SSBU_NET_ROOM\"..."
echo "In the game: Online > Local Wireless > Create a Room. The others pick your room from the list."
echo
for _ in $(seq 20); do [ -f netplay_address.txt ] && break; sleep 1; done
if [ -f netplay_address.txt ]; then cat netplay_address.txt; else echo "(The address to share will show in the game's overlay - press F2.)"; fi
echo
echo "(In a VMware virtual machine with NAT networking, players outside this PC reach the relay"
echo " only if VMware forwards the UDP port to this VM - see docs/LINUX.md.)"
wait
