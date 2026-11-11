#!/bin/sh
# Puts the online lobby with the game's own Online mode (tools/ssbu_online_lobby.py + tools/nex)
# on a Linux server with systemd, over SSH, as the service ssbu-online - next to the plain lobby
# (deploy_lobby.sh's ssbu-lobby), which it leaves alone. Run from the port's folder:
#   sh tools/deploy/deploy_online_lobby.sh <ssh host> install     copy it, install the service,
#                                                                 allow its UDP and TCP port in ufw, start it
#   sh tools/deploy/deploy_online_lobby.sh <ssh host> update      copy new files (the service file too) and restart
#   sh tools/deploy/deploy_online_lobby.sh <ssh host> status      the service's state and its last log lines
#   sh tools/deploy/deploy_online_lobby.sh <ssh host> uninstall   stop and remove it, close the port again
# The SSH login must be root (SSBU_SUDO=sudo for a user who can sudo). SSBU_ONLINE_PORT: 39421.
# Files on the server: /opt/ssbu-online/ (the lobby, ssbu_relay.py, nex/), the shared banlist
# /etc/ssbu-lobby/banlist.txt, /etc/systemd/system/ssbu-online.service, and the game server's
# players and ratings in /var/lib/ssbu-online (kept by uninstall).
set -e
host=$1
what=${2:-status}
port=${SSBU_ONLINE_PORT:-39421}
sudo=${SSBU_SUDO:-}
here=$(dirname "$0")
[ -n "$host" ] || { sed -n '2,14p' "$0"; exit 2; }

remote() {   # a script for the server's sh, from stdin
    ssh "$host" "$sudo sh -s"
}

case $what in
install|update)
    tmp=/tmp/ssbu-online-deploy
    ssh "$host" "rm -rf $tmp && mkdir -m 700 $tmp $tmp/nex"
    scp -q tools/ssbu_online_lobby.py tools/ssbu_relay.py "$here/ssbu-online.service" "$host:$tmp/"
    # (every module of tools/nex but its tests: arenas.py was left out when Battle Arenas came, #290)
    nexfiles=$(cd tools/nex && ls *.py | grep -v '^test_' | grep -v '^selftest')
    for f in $nexfiles; do scp -q "tools/nex/$f" "$host:$tmp/nex/"; done
    remote <<EOF
set -e
command -v python3 > /dev/null || { echo "python3 is missing"; exit 1; }
install -d -m 755 /opt/ssbu-online /opt/ssbu-online/nex /etc/ssbu-lobby
for f in ssbu_online_lobby.py ssbu_relay.py \$(cd $tmp && ls nex/*.py); do
    tr -d '\r' < $tmp/\$f > /opt/ssbu-online/\$f
    chmod 644 /opt/ssbu-online/\$f
done
[ -e /etc/ssbu-lobby/banlist.txt ] || printf '# IP addresses or networks (a.b.c.d/nn) the lobby turns away, one a line\n' > /etc/ssbu-lobby/banlist.txt
sed 's/--port 39421/--port $port/' $tmp/ssbu-online.service > /etc/systemd/system/ssbu-online.service
systemctl daemon-reload
if [ "$what" = install ]; then
    if command -v ufw > /dev/null && ufw status | grep -q '^Status: active'; then
        ufw allow $port/udp comment ssbu-online
        ufw allow $((port + 10))/udp comment ssbu-online-nat
        ufw allow $port/tcp comment ssbu-online
    fi
    systemctl enable --now ssbu-online
else
    systemctl restart ssbu-online
fi
rm -rf $tmp
sleep 2
systemctl --no-pager --lines=0 status ssbu-online | head -5
journalctl -u ssbu-online --no-pager -n 5
EOF
    ;;
status)
    remote <<EOF
systemctl --no-pager --lines=0 status ssbu-online | head -6
journalctl -u ssbu-online --no-pager -n 15
EOF
    ;;
uninstall)
    remote <<EOF
systemctl disable --now ssbu-online 2> /dev/null || true
rm -f /etc/systemd/system/ssbu-online.service
systemctl daemon-reload
if command -v ufw > /dev/null; then
    ufw delete allow $port/udp 2> /dev/null || true
    ufw delete allow $((port + 10))/udp 2> /dev/null || true
    ufw delete allow $port/tcp 2> /dev/null || true
fi
rm -rf /opt/ssbu-online
echo "removed (the players and ratings in /var/lib/ssbu-online and the banlist are kept)"
EOF
    ;;
*)
    sed -n '2,14p' "$0"
    exit 2
    ;;
esac
