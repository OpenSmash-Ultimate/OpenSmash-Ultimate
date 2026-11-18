#!/bin/sh
# Two games on this PC playing netplay with each other through the host's relay, on loopback
# (no router change, no firewall prompt) - for menu-only checks of the netplay path.
# Usage: tools/dev/netpair.sh <out prefix> [seconds]
#   SSBU_INPUT_HOST, SSBU_INPUT_JOIN: each game's scripted input (see tools/dev/menurun.sh)
#   SSBU_PAIR_RELAY=addr:port: both games join that relay (e.g. a local lobby: tools/dev/netlobby.sh) instead of
#     the first hosting one
#   SSBU_JOIN_DELAY: seconds before the joining game starts (default 2) - two games booting at
#     once run slower than one, enough to throw off scripted menu timings
# Frames go to <out>-host/ and <out>-join/ (one a second), logs to <out>-host.log / -join.log.
out=$1
secs=${2:-45}
delay=${SSBU_JOIN_DELAY:-2}
exe=${SSBU_EXE:-build/ssbu.exe}
for who in host join; do
    rm -rf "$out-$who" "build/testdata-$who"
    mkdir -p "$out-$who" "build/testdata-$who"
    cp -r save "build/testdata-$who/" 2>/dev/null
done
run() {   # who name relay-variable input seconds
    env SSBU_NET_UPNP=0 SSBU_NET_BIND=127.0.0.1 SSBU_NET_LOG="${SSBU_NET_LOG:-1}" SSBU_NET_NAME="$2" "$3" \
        SSBU_DATA_ROOT="build/testdata-$1" SSBU_INPUT="$4" SSBU_GFX_DEBUG=16 SSBU_GFX_LOG_AT=99 \
        SSBU_CAPTURE="$out-$1" SSBU_CAPTURE_EVERY=1 SSBU_CAPTURE_MAX="${SSBU_CAPTURE_MAX:-50}" \
        timeout "$5" "./$exe" dump/modules --status 30 > "$out-$1.log" 2>&1
}
hostvar=SSBU_NET_HOST=1
[ -n "$SSBU_PAIR_RELAY" ] && hostvar=SSBU_NET_RELAY=$SSBU_PAIR_RELAY
run host Alpha "$hostvar" "$SSBU_INPUT_HOST" $((secs + delay)) &
sleep "$delay"
run join Bravo SSBU_NET_RELAY="${SSBU_PAIR_RELAY:-127.0.0.1}" "$SSBU_INPUT_JOIN" "$secs" &
wait
grep -h 'exception\|stopped during' "$out-host.log" "$out-join.log" | head -4
