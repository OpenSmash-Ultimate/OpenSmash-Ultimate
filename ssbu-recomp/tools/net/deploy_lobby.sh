#!/usr/bin/env bash
# Installs the online lobby (ssbu_lobby.py) as a systemd service on a Linux server, e.g. an Oracle Cloud "Always Free"
# Ubuntu VM. Run it ON the server, from the folder that holds ssbu_lobby.py:
#     sudo bash deploy_lobby.sh [port]            (default UDP port 39500)
# Then players set online_server = <server address>:<port> (or SSBU_ONLINE=<server address>:<port>).
# Oracle Cloud also needs the port opened in the VCN: Networking > Virtual cloud networks > your VCN > Security List >
# Add Ingress Rule: source 0.0.0.0/0, protocol UDP, destination port <port>. This script opens the VM's own firewall.
# The lobby itself is standard library Python only.
#
# Status endpoint for the project site (players online, rooms, up/down; counts only, never addresses or names):
#   the lobby always serves GET /status on 127.0.0.1:$STATUS_PORT (default 8080, TCP, reachable from this machine only).
#   With a domain name that points at this server (a DNS A/AAAA record you made), the script also installs Caddy from
#   its official apt repository and publishes it as https://<domain>/status with an automatic TLS certificate:
#       sudo DOMAIN=lobby.example.org bash deploy_lobby.sh [port]
#       (optional: ACME_EMAIL=<an address of yours> for the certificate authority's expiry notices, STATUS_PORT=8080;
#        Caddy works without ACME_EMAIL)
#   That needs TCP 80 and 443 open in the cloud firewall too (Oracle: two more ingress rules, protocol TCP).
#   Without DOMAIN, Caddy is skipped; run the script again later with DOMAIN=... to add it.
#   Then set STATUS_URL = "https://<domain>/status" in the site's config.js.
# Remove: systemctl disable --now ssbu-lobby; rm -r /opt/ssbu-lobby (and, if it was added: apt-get remove caddy)
set -euo pipefail
PORT="${1:-39500}"
STATUS_PORT="${STATUS_PORT:-8080}"
NAT_PORT=$((PORT + 10))   # (the lobby's NAT port: games' PROBEs show their routers' second port, for direct paths)
DOMAIN="${DOMAIN:-}"
ACME_EMAIL="${ACME_EMAIL:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$HERE/ssbu_lobby.py" ] || { echo "ssbu_lobby.py not found next to this script"; exit 1; }
[[ "$PORT" =~ ^[0-9]+$ && "$STATUS_PORT" =~ ^[0-9]+$ ]] || { echo "ports must be numbers"; exit 1; }
DOMAIN_RE='^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?\.[A-Za-z]{2,}$'
if [ -n "$DOMAIN" ] && ! [[ "$DOMAIN" =~ $DOMAIN_RE ]]; then
    echo "DOMAIN must be a plain host name such as lobby.example.org (no https://, no path)"; exit 1
fi
command -v python3 >/dev/null || { apt-get update -q && apt-get install -y -q python3; }
python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' || { echo "python 3.8 or newer needed"; exit 1; }

install -d -m 755 /opt/ssbu-lobby
install -m 644 "$HERE/ssbu_lobby.py" /opt/ssbu-lobby/ssbu_lobby.py

cat > /etc/systemd/system/ssbu-lobby.service <<EOF
[Unit]
Description=SSBU native port online lobby (UDP $PORT and $NAT_PORT, status on 127.0.0.1:$STATUS_PORT)
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/bin/python3 /opt/ssbu-lobby/ssbu_lobby.py --bind 0.0.0.0 --port $PORT --nat-port $NAT_PORT --status-port $STATUS_PORT --status-bind 127.0.0.1
Restart=always
RestartSec=3
DynamicUser=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
NoNewPrivileges=yes
MemoryMax=256M
CPUQuota=50%
LogRateLimitIntervalSec=60
LogRateLimitBurst=200

[Install]
WantedBy=multi-user.target
EOF

# the VM's own firewall (Oracle's Ubuntu images use iptables rules; ufw if it is active)
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    ufw allow "$PORT/udp"
    ufw allow "$NAT_PORT/udp"
else
    for p in "$PORT" "$NAT_PORT"; do
        iptables -C INPUT -p udp --dport "$p" -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p udp --dport "$p" -j ACCEPT
    done
    if command -v netfilter-persistent >/dev/null; then netfilter-persistent save; else
        echo "note: install iptables-persistent to keep the firewall rule after a reboot"; fi
fi

systemctl daemon-reload
systemctl enable ssbu-lobby
systemctl restart ssbu-lobby
sleep 1
systemctl --no-pager status ssbu-lobby | head -5
echo "lobby running on UDP $PORT (NAT port $NAT_PORT). Logs: journalctl -u ssbu-lobby -f"
echo "remember the cloud firewall (Oracle: VCN Security List ingress rules, UDP $PORT and UDP $NAT_PORT)"
echo "status endpoint (local only): curl -s http://127.0.0.1:$STATUS_PORT/status"

# ---- optional: https://DOMAIN/status through Caddy (automatic TLS)
if [ -z "$DOMAIN" ]; then
    echo
    echo "No DOMAIN given: the status endpoint stays local and Caddy was not installed."
    echo "To publish it for the website later: point a DNS A/AAAA record of your domain at this server, open TCP 80 and 443"
    echo "in the cloud firewall, then run:  sudo DOMAIN=<your.domain> bash deploy_lobby.sh $PORT"
    echo "and set STATUS_URL = \"https://<your.domain>/status\" in the site's config.js."
    exit 0
fi

if ! command -v caddy >/dev/null; then
    # official Caddy apt repository (https://caddyserver.com/docs/install#debian-ubuntu-raspbian)
    apt-get update -q
    apt-get install -y -q debian-keyring debian-archive-keyring apt-transport-https curl gnupg
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | gpg --dearmor --yes -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' > /etc/apt/sources.list.d/caddy-stable.list
    chmod o+r /usr/share/keyrings/caddy-stable-archive-keyring.gpg /etc/apt/sources.list.d/caddy-stable.list
    apt-get update -q
    apt-get install -y -q caddy
fi

if [ -f /etc/caddy/Caddyfile ] && [ ! -f /etc/caddy/Caddyfile.before-ssbu ]; then
    cp /etc/caddy/Caddyfile /etc/caddy/Caddyfile.before-ssbu
fi
GLOBAL=""
[ -n "$ACME_EMAIL" ] && GLOBAL="{
	email $ACME_EMAIL
}
"
cat > /etc/caddy/Caddyfile <<EOF
${GLOBAL}# written by deploy_lobby.sh: the lobby's status endpoint (counts only), nothing else
$DOMAIN {
	encode gzip
	handle /status {
		reverse_proxy 127.0.0.1:$STATUS_PORT
	}
	handle {
		respond "not found" 404
	}
	log {
		output discard
	}
}
EOF
caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile

if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    ufw allow 80/tcp
    ufw allow 443/tcp
else
    for p in 80 443; do
        iptables -C INPUT -p tcp --dport "$p" -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p tcp --dport "$p" -j ACCEPT
    done
    if command -v netfilter-persistent >/dev/null; then netfilter-persistent save; fi
fi

systemctl enable caddy >/dev/null 2>&1 || true
systemctl reload caddy 2>/dev/null || systemctl restart caddy
echo
echo "Caddy serves https://$DOMAIN/status (the certificate is fetched on first use; DNS must already point here)."
echo "Open TCP 80 and 443 in the cloud firewall too. Check: curl -s https://$DOMAIN/status"
echo "Site: set STATUS_URL = \"https://$DOMAIN/status\" in config.js."
