#!/bin/sh
# Puts the online netplay lobby (tools/ssbu_relay.py) on a Linux server with systemd, over SSH.
# Run from the port's folder:
#   sh tools/deploy/deploy_lobby.sh <ssh host> install     copy the lobby, install the ssbu-lobby
#                                                          service, allow its UDP port in ufw, start it
#   sh tools/deploy/deploy_lobby.sh <ssh host> update      copy a new ssbu_relay.py and restart
#   sh tools/deploy/deploy_lobby.sh <ssh host> status      the service's state and its last log lines
#   sh tools/deploy/deploy_lobby.sh <ssh host> uninstall   stop and remove it, close the port again
# The SSH login must be root (SSBU_SUDO=sudo for a user who can sudo). SSBU_LOBBY_PORT: 39420.
# The lobby's NAT ports (port + 10, and SU's 39500 + 10: games compare their routers' ports there, for
# direct paths through routers that change ports) are allowed in ufw on install and update.
# Files on the server: /opt/ssbu-lobby/ssbu_relay.py, /etc/ssbu-lobby/banlist.txt (IP addresses
# to turn away - edit it there, the lobby re-reads it), /etc/systemd/system/ssbu-lobby.service.
set -e
host=$1
what=${2:-status}
port=${SSBU_LOBBY_PORT:-39420}
sudo=${SSBU_SUDO:-}
here=$(dirname "$0")
[ -n "$host" ] || { sed -n '2,12p' "$0"; exit 2; }

remote() {   # a script for the server's sh, from stdin
    ssh "$host" "$sudo sh -s"
}

case $what in
install|update)
    tmp=/tmp/ssbu-lobby-deploy
    ssh "$host" "rm -rf $tmp && mkdir -m 700 $tmp"
    scp -q tools/ssbu_relay.py "$here/ssbu-lobby.service" "$host:$tmp/"
    remote <<EOF
set -e
command -v python3 > /dev/null || { echo "python3 is missing"; exit 1; }
install -d -m 755 /opt/ssbu-lobby /etc/ssbu-lobby
tr -d '\r' < $tmp/ssbu_relay.py > /opt/ssbu-lobby/ssbu_relay.py
chmod 644 /opt/ssbu-lobby/ssbu_relay.py
[ -e /etc/ssbu-lobby/banlist.txt ] || printf '# IP addresses or networks (a.b.c.d/nn) the lobby turns away, one a line\n' > /etc/ssbu-lobby/banlist.txt
if command -v ufw > /dev/null && ufw status | grep -q '^Status: active'; then
    ufw allow $((port + 10))/udp comment ssbu-lobby-nat
    ufw allow 39510/udp comment ssbu-lobby-nat
fi
if [ "$what" = install ]; then
    sed 's/--port 39420/--port $port/' $tmp/ssbu-lobby.service > /etc/systemd/system/ssbu-lobby.service
    systemctl daemon-reload
    if command -v ufw > /dev/null && ufw status | grep -q '^Status: active'; then
        ufw allow $port/udp comment ssbu-lobby
    fi
    systemctl enable --now ssbu-lobby
else
    systemctl restart ssbu-lobby
fi
rm -rf $tmp
sleep 2
systemctl --no-pager --lines=0 status ssbu-lobby | head -5
journalctl -u ssbu-lobby --no-pager -n 5
EOF
    ;;
status)
    remote <<EOF
systemctl --no-pager --lines=0 status ssbu-lobby | head -6
journalctl -u ssbu-lobby --no-pager -n 15
EOF
    ;;
uninstall)
    remote <<EOF
systemctl disable --now ssbu-lobby 2> /dev/null || true
rm -f /etc/systemd/system/ssbu-lobby.service
systemctl daemon-reload
if command -v ufw > /dev/null; then
    for p in $port $((port + 10)) 39510; do ufw delete allow \$p/udp 2> /dev/null || true; done
fi
rm -rf /opt/ssbu-lobby
echo "removed (the banlist in /etc/ssbu-lobby is kept)"
EOF
    ;;
*)
    sed -n '2,12p' "$0"
    exit 2
    ;;
esac
