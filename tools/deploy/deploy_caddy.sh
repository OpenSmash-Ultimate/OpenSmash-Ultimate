#!/bin/sh
# Puts a Caddy reverse proxy in front of the online lobby's HTTP side, over SSH, so that
# https://opensmash-lobby.duckdns.org/status.json, /leaderboard.json and /gsp/* work (the lobby itself only speaks
# plain HTTP on TCP 39421, which an HTTPS web page may not read). Run from the repository root:
#   sh tools/deploy/deploy_caddy.sh <ssh host> install     install Caddy (Debian/Ubuntu, the official apt repository),
#                                                          add /etc/caddy/conf.d/opensmash-lobby.caddy (imported by
#                                                          the main Caddyfile), allow 80/443 in ufw, start it
#   sh tools/deploy/deploy_caddy.sh <ssh host> update      copy a new Caddyfile, validate it, reload Caddy
#   sh tools/deploy/deploy_caddy.sh <ssh host> status      Caddy's state, its last log lines and a local HTTPS check
#   sh tools/deploy/deploy_caddy.sh <ssh host> uninstall   stop Caddy and close 80/443 again (the package stays)
# The SSH login must be root (SSBU_SUDO=sudo for a user who can sudo).
# BEFORE `install`: open TCP 80 and 443 (and UDP 443 for HTTP/3) in Oracle's security list - see tools/deploy/CADDY.md.
# An existing Caddy is kept: its Caddyfile is backed up (Caddyfile.pre-opensmash) and only gets one import line.
# Nothing in this script touches the lobby services.
set -e
host=$1
what=${2:-status}
sudo=${SSBU_SUDO:-}
here=$(dirname "$0")
[ -n "$host" ] || { sed -n '2,12p' "$0"; exit 2; }

remote() {   # a script for the server's sh, from stdin
    ssh "$host" "$sudo sh -s"
}

case $what in
install|update)
    tmp=/tmp/ssbu-caddy-deploy
    ssh "$host" "rm -rf $tmp && mkdir -m 700 $tmp"
    scp -q "$here/Caddyfile" "$host:$tmp/"
    remote <<EOS
set -e
if ! command -v caddy > /dev/null; then
    [ "$what" = install ] || { echo "caddy is not installed: run install first"; exit 1; }
    command -v apt-get > /dev/null || { echo "this script installs Caddy with apt (Debian/Ubuntu); install Caddy by hand and re-run"; exit 1; }
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq debian-keyring debian-archive-keyring apt-transport-https curl gpg
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | gpg --dearmor --yes -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' | tr -d '\r' > /etc/apt/sources.list.d/caddy-stable.list
    apt-get update -qq
    apt-get install -y -qq caddy
fi
install -d -m 755 /etc/caddy /etc/caddy/conf.d
# Caddy may already be here with other sites: never overwrite its Caddyfile. Our site goes to its own file, which the
# main Caddyfile imports; a backup is kept, and any validation failure puts the old file back.
main=/etc/caddy/Caddyfile
[ -f \$main ] || printf '# managed by deploy_caddy.sh\n' > \$main
cp -p \$main \$main.pre-opensmash
# (deploy_lobby.sh's own Caddyfile served only the old /status for the same address: ours replaces it, kept as a backup)
grep -q '^# written by deploy_lobby.sh' \$main && printf '# managed by deploy_caddy.sh (the old one: Caddyfile.pre-opensmash)\n' > \$main
tr -d '\r' < $tmp/Caddyfile > /etc/caddy/conf.d/opensmash-lobby.caddy
grep -q 'conf.d/\*.caddy' \$main || printf '\nimport /etc/caddy/conf.d/*.caddy\n' >> \$main
if ! caddy validate --config \$main --adapter caddyfile; then
    cp -p \$main.pre-opensmash \$main
    grep -rn 'opensmash-lobby' /etc/caddy/ 2> /dev/null | grep -v 'conf.d/opensmash-lobby.caddy' || true
    rm -f /etc/caddy/conf.d/opensmash-lobby.caddy
    echo "the Caddyfile did not validate (an existing site with the same address?): the old one is back, nothing changed"
    exit 1
fi
if command -v ufw > /dev/null && ufw status | grep -q '^Status: active'; then
    ufw allow 80/tcp comment ssbu-caddy
    ufw allow 443/tcp comment ssbu-caddy
    ufw allow 443/udp comment ssbu-caddy
fi
# Oracle Ubuntu images ship an iptables INPUT chain that rejects everything but SSH: put 80/443 in front of the REJECT
if command -v iptables > /dev/null && iptables -S INPUT 2> /dev/null | grep -q -- '-j REJECT'; then
    for p in 80 443; do
        iptables -C INPUT -p tcp --dport \$p -j ACCEPT 2> /dev/null || iptables -I INPUT 1 -p tcp --dport \$p -j ACCEPT
    done
    iptables -C INPUT -p udp --dport 443 -j ACCEPT 2> /dev/null || iptables -I INPUT 1 -p udp --dport 443 -j ACCEPT
    command -v netfilter-persistent > /dev/null && netfilter-persistent save || echo "note: iptables rules are not saved across reboot (install iptables-persistent)"
fi
install -d -o caddy -g caddy /var/log/caddy 2> /dev/null || true
chown -R caddy:caddy /var/log/caddy 2> /dev/null || true   # (caddy validate ran as root above and may have made the log file root's)
systemctl enable caddy
systemctl reload-or-restart caddy
rm -rf $tmp
sleep 3
systemctl --no-pager --lines=0 status caddy | head -5
journalctl -u caddy --no-pager -n 8
EOS
    ;;
status)
    remote <<'EOS'
systemctl --no-pager --lines=0 status caddy | head -6
journalctl -u caddy --no-pager -n 15
curl -sS -m 10 -o /dev/null -w 'https://opensmash-lobby.duckdns.org/status.json -> HTTP %{http_code}\n' --resolve opensmash-lobby.duckdns.org:443:127.0.0.1 https://opensmash-lobby.duckdns.org/status.json || true
EOS
    ;;
uninstall)
    remote <<'EOS'
systemctl disable --now caddy 2> /dev/null || true
if command -v ufw > /dev/null; then
    ufw delete allow 80/tcp 2> /dev/null || true
    ufw delete allow 443/tcp 2> /dev/null || true
    ufw delete allow 443/udp 2> /dev/null || true
fi
if command -v iptables > /dev/null; then
    for p in 80 443; do iptables -D INPUT -p tcp --dport $p -j ACCEPT 2> /dev/null || true; done
    iptables -D INPUT -p udp --dport 443 -j ACCEPT 2> /dev/null || true
fi
echo "caddy stopped, 80/443 closed (the package and /etc/caddy/Caddyfile are kept)"
EOS
    ;;
*)
    sed -n '2,12p' "$0"
    exit 2
    ;;
esac
