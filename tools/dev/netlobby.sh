#!/bin/sh
# Three games on this PC in the online lobby (the lobby server's ssbu_relay.py, run here on loopback): Alpha
# and Bravo each create a Local Wireless room, Charlie opens the Rooms List - which must show
# both - and joins the first. Menus only (no battle), no router or firewall change.
# Usage: tools/dev/netlobby.sh <out prefix> [seconds]
#   SSBU_EXE (default build/ssbu.exe), SSBU_LOBBY_PORT (39510), SSBU_CAPTURE_MAX (80, Charlie's frames)
#   SSBU_LOBBY=host:port: use that lobby (e.g. the online one) instead of starting one here
#   OPENSMASH_SERVER_DIR: a checkout of the lobby server (default ../opensmash-server), to start one here
#   SSBU_INPUT_HOST, SSBU_INPUT_JOIN: the scripted input of the hosts and of Charlie (defaults:
#     11.0.0's menus - Local Wireless, create a room; Rooms List, first room)
#   SSBU_KEEP=1: keep Charlie's frames (<out>-charlie/) instead of deleting them at the end
#   SSBU_DIRECT_TEST: which direct connection run (every game on 127.0.0.1 with its own port,
#     each filtering like a router: SSBU_NET_TEST_NAT=1)
#     (default)  Alpha and Charlie connect directly; at about 95 s Charlie's direct path is cut
#                (SSBU_NET_TEST_BLOCK) and both must go on through the lobby - 125 s
#     blocked    Charlie's direct packets are dropped from the start (as a symmetric NAT): both
#                must give up punching and play through the lobby
#     off        Charlie turns direct connections off (SSBU_NET_DIRECT=0): the lobby must never
#                introduce it, nor give Alpha its address
#     three      Alpha hosts, Bravo and Charlie both join it (two stations: their own pair too) - 125 s
#     none       no direct connection checks
# Logs: <out>-lobby.log, <out>-alpha.log, -bravo.log, -charlie.log. Prints PASS or FAIL.
out=$1
mode=${SSBU_DIRECT_TEST:-cut}
case $mode in cut|three) secs=${2:-125} ;; *) secs=${2:-110} ;; esac
exe=${SSBU_EXE:-build/ssbu.exe}
port=${SSBU_LOBBY_PORT:-39510}
nav="9:A,13:DOWN,13.5:DOWN,14:DOWN,14.5:RIGHT,15:RIGHT,15.5:RIGHT,16:RIGHT,17:UP,17.6:UP,18.6:A,21.5:PLUS"
host_input=${SSBU_INPUT_HOST:-"$nav,25:A,28.5:PLUS,36:A"}
join_input=${SSBU_INPUT_JOIN:-"$nav,30:DOWN,31:A"}
for who in alpha bravo charlie; do
    rm -rf "$out-$who" "build/testdata-$who"
    mkdir -p "build/testdata-$who"
    cp -r save "build/testdata-$who/" 2>/dev/null
done
mkdir -p "$out-charlie"
lobby=
if [ -n "$SSBU_LOBBY" ]; then
    where=$SSBU_LOBBY
    : > "$out-lobby.log"
else
    where=127.0.0.1:$port
    relay=${OPENSMASH_SERVER_DIR:-../opensmash-server}/tools/ssbu_relay.py
    [ -f "$relay" ] || { echo "no lobby server at $relay: set OPENSMASH_SERVER_DIR, or SSBU_LOBBY=host:port"; exit 2; }
    python -I "$relay" --bind 127.0.0.1 --port "$port" --verbose --stats 10 > "$out-lobby.log" 2>&1 &
    lobby=$!
    sleep 1
fi
run() {   # who input seconds [capture dir] [more VAR=value ...]
    who=$1 input=$2 limit=$3 capture=$4
    shift 4
    env SSBU_NET_RELAY="$where" SSBU_NET_LOG="${SSBU_NET_LOG:-1}" SSBU_NET_NAME="$who" SSBU_NET_TEST_NAT=1 SSBU_LOG_STATS=1 \
        SSBU_DATA_ROOT="build/testdata-$(echo $who | tr A-Z a-z)" SSBU_INPUT="$input" ${capture:+SSBU_CAPTURE="$capture"} \
        ${capture:+SSBU_CAPTURE_EVERY=1} ${capture:+SSBU_CAPTURE_MAX="${SSBU_CAPTURE_MAX:-80}"} "$@" \
        timeout "$limit" "./$exe" dump/modules --status 30 > "$out-$(echo $who | tr A-Z a-z).log" 2>&1
}
# Alpha accepts its challenger at 85 s (Charlie asks at about 71 s); Bravo never presses A in its room.
# (three: Bravo asks to join Alpha at about 43 s, accepted at 60 s)
alpha_input="$host_input,85:A"
bravo_input=$host_input
charlie_env=
case $mode in
    cut) charlie_env=SSBU_NET_TEST_BLOCK=55 ;;   # (Charlie starts 40 s in: 95 s)
    blocked) charlie_env=SSBU_NET_TEST_BLOCK=0 ;;
    off) charlie_env=SSBU_NET_DIRECT=0 ;;
    three) alpha_input="$host_input,60:A,85:A"; bravo_input=$join_input ;;
esac
run Alpha "$alpha_input" "$secs" "" &
a=$!
sleep 12
run Bravo "$bravo_input" $((secs - 12)) "" &
b=$!
sleep 28
run Charlie "$join_input" $((secs - 40)) "$out-charlie" $charlie_env &
c=$!
wait $a $b $c
if [ -n "$lobby" ]; then
    kill "$lobby" 2>/dev/null
    wait "$lobby" 2>/dev/null
fi
ok=1
check() {   # what file pattern
    if grep -q "$3" "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; ok=0; fi
}
never() {   # what file pattern
    if grep -q "$3" "$2"; then echo "  WRONG: $1"; grep -m 2 "$3" "$2" | sed 's/^/    /'; ok=0; else echo "  ok: $1"; fi
}
grew() {   # what file peer: the packets from that peer through the lobby grew after its direct path went quiet
    if awk -v who="peer $3:" '/went quiet/ { q = 1 }
        index($0, who) && match($0, /\+ [0-9]+ relayed, out/) { n = substr($0, RSTART + 2, RLENGTH - 15) + 0; if (q) after = n; else before = n }
        END { exit !(after > before) }' "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; ok=0; fi
}
A=$out-alpha.log B=$out-bravo.log C=$out-charlie.log
echo "netlobby $out ($mode):"
check "Alpha joined the lobby" "$A" "joined the lobby"
check "Alpha created a room" "$A" "network created by"
if [ "$mode" = three ]; then
    check "Bravo joined Alpha's room" "$B" "network: joined"
else
    check "Bravo created a room" "$B" "network created by"
    check "Charlie saw both rooms" "$C" "scan found 2 networks"
fi
check "Charlie joined a room" "$C" "network: joined"
check "Alpha took Charlie in" "$A" "joined as node"
case $mode in
cut)
    check "Alpha direct to Charlie" "$A" "Charlie: direct, via public 127.0.0.1:"
    check "Charlie direct to Alpha" "$C" "Alpha: direct, via public 127.0.0.1:"
    check "the overlay shows it" "$A" "Room: Charlie direct"
    check "game packets came directly" "$A" "peer Charlie: direct.*in [1-9][0-9]* direct"
    never "no direct packet dropped" "$A" "peer Charlie: .* [1-9][0-9]* dropped"
    check "Alpha noticed the cut" "$A" "direct path to Charlie went quiet; using the relay"
    check "Charlie noticed the cut" "$C" "direct path to Alpha went quiet; using the relay"
    grew "then Charlie's packets came through the lobby" "$A" Charlie
    never "Charlie stayed in the room" "$C" "ldn: state 5 -> "
    ;;
blocked)
    check "Alpha gave up punching" "$A" "no direct path to Charlie"
    check "Charlie gave up punching" "$C" "no direct path to Alpha"
    never "never direct" "$A" "Charlie: direct"
    check "the overlay shows it" "$A" "Room: Charlie relayed"
    ;;
off)
    check "Charlie turned direct off" "$C" "direct connections off"
    check "the overlay shows it" "$A" "Room: Charlie relayed (direct off)"
    never "Charlie never introduced" "$A" "Charlie: introduced"
    cport=$(grep -o "Charlie joined lobby .* from 127.0.0.1:[0-9]*" "$out-lobby.log" | head -1 | sed 's/.*://')
    if [ -n "$cport" ]; then never "Charlie's address never reached Alpha" "$A" "127.0.0.1:$cport"; fi
    ;;
three)
    check "Alpha direct to Bravo" "$A" "Bravo: direct, via public"
    check "Alpha direct to Charlie" "$A" "Charlie: direct, via public"
    if grep -q "Charlie: direct" "$B"; then echo "  (Bravo and Charlie: direct)"; else echo "  (Bravo and Charlie: not direct - $(grep -c 'Charlie: introduced' "$B") introductions)"; fi
    ;;
esac
grep -h 'exception\|stopped during' "$A" "$B" "$C" | head -4
grep 'player' "$out-lobby.log" | tail -2
[ -n "$SSBU_KEEP" ] || rm -rf "$out-charlie"
rm -rf build/testdata-alpha build/testdata-bravo build/testdata-charlie
if [ $ok = 1 ]; then echo PASS; else echo FAIL; fi
