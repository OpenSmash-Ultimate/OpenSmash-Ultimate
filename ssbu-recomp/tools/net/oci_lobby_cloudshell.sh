#!/usr/bin/env bash
# Creates the online lobby server on Oracle Cloud "Always Free" from Oracle Cloud Shell (the terminal inside the Oracle
# console, already signed in to your account: no API key leaves it), then installs the lobby on it.
#   In the Oracle console: the ">_" Cloud Shell button (top right), then paste:
#     curl -fsSL https://raw.githubusercontent.com/Denkishi/ssbu-recomp/main/tools/net/oci_lobby_cloudshell.sh | bash
# What it does, each step skipped when already done (run it again after a stop or a Cloud Shell time-out):
#   1. the network: the VCN named $VCN_NAME (default ssbu-vcn) with its public subnet; if there is none, it says how to
#      make one (console: Networking > Virtual cloud networks > Start VCN Wizard > "Create VCN with Internet Connectivity");
#   2. the firewall of that VCN: UDP $PORT (the lobby) and TCP 80 / 443 (the status page) open to the internet;
#   3. an SSH key pair in Cloud Shell (~/.ssh/ssbu_lobby): it stays in your Cloud Shell, nothing is printed;
#   4. the VM "ssbu-lobby": shape $SHAPE (default VM.Standard.A1.Flex, 1 OCPU, 6 GB; SHAPE=VM.Standard.E2.1.Micro for the
#      small AMD one), Ubuntu 24.04, public IP; "Out of capacity" is retried every $RETRY s (default 120), "too many
#      requests" after 300 s, until it works;
#   5. the lobby on the VM (tools/net/ssbu_lobby.py + deploy_lobby.sh from this repository, systemd service).
# At the end it prints the server's public address: players use online_server = <address>:<port>.
# Options (environment): VCN_NAME, SHAPE, OCPUS, MEMORY_GB, PORT (39500), RETRY (120), REPO_RAW (raw URL of this repo).
set -euo pipefail
VCN_NAME="${VCN_NAME:-ssbu-vcn}"; SHAPE="${SHAPE:-VM.Standard.A1.Flex}"; OCPUS="${OCPUS:-1}"; MEMORY_GB="${MEMORY_GB:-6}"
PORT="${PORT:-39500}"; RETRY="${RETRY:-120}"; NAME=ssbu-lobby
REPO_RAW="${REPO_RAW:-https://raw.githubusercontent.com/Denkishi/ssbu-recomp/main}"
C="${OCI_TENANCY:?run this in Oracle Cloud Shell (OCI_TENANCY is not set)}"
say() { printf '%s  %s\n' "$(date +%H:%M:%S)" "$*"; }

say "1. network"
VCN=$(oci network vcn list -c "$C" --display-name "$VCN_NAME" --query 'data[0].id' --raw-output 2>/dev/null || true)
if [ -z "$VCN" ] || [ "$VCN" = "null" ]; then
  say "no VCN named $VCN_NAME: make it in the console (Networking > Virtual cloud networks > Start VCN Wizard >"
  say "\"Create VCN with Internet Connectivity\", name $VCN_NAME), then run this again"; exit 1
fi
SUBNET=$(oci network subnet list -c "$C" --vcn-id "$VCN" --all --output json | python3 -c '
import json, sys
d = json.load(sys.stdin)["data"]
pub = [s for s in d if not s.get("prohibit-public-ip-on-vnic")]
print(pub[0]["id"] if pub else "")')
[ -n "$SUBNET" ] || { say "the VCN has no public subnet: make the VCN with the wizard named above"; exit 1; }

say "2. firewall (security list of $VCN_NAME)"
SL=$(oci network vcn get --vcn-id "$VCN" --query 'data."default-security-list-id"' --raw-output)
oci network security-list get --security-list-id "$SL" --output json > /tmp/ssbu_sl.json
python3 - "$PORT" > /tmp/ssbu_ingress.json <<'PY'
import json, sys
port = int(sys.argv[1])
rules = json.load(open("/tmp/ssbu_sl.json"))["data"]["ingress-security-rules"]
def has(proto, p):
    for r in rules:
        o = r.get("udp-options" if proto == "17" else "tcp-options") or {}
        rng = o.get("destination-port-range") or {}
        if r.get("protocol") == proto and r.get("source") == "0.0.0.0/0" and rng.get("min", 0) <= p <= rng.get("max", -1):
            return True
    return False
def rule(proto, p, what):
    key = "udp-options" if proto == "17" else "tcp-options"
    return {"protocol": proto, "source": "0.0.0.0/0", "source-type": "CIDR_BLOCK", "is-stateless": False,
            "description": what, key: {"destination-port-range": {"min": p, "max": p}}}
add = []
if not has("17", port): add.append(rule("17", port, "SSBU online lobby"))
if not has("17", port + 10): add.append(rule("17", port + 10, "SSBU online lobby NAT port"))
for p in (80, 443):
    if not has("6", p): add.append(rule("6", p, "SSBU lobby status page"))
print(json.dumps(rules + add if add else None))
PY
if [ "$(cat /tmp/ssbu_ingress.json)" != "null" ]; then
  oci network security-list update --security-list-id "$SL" --ingress-security-rules file:///tmp/ssbu_ingress.json --force >/dev/null
  say "   ports opened: UDP $PORT and $((PORT + 10)), TCP 80, 443"
else say "   already open"; fi

say "3. SSH key (stays in this Cloud Shell)"
[ -f ~/.ssh/ssbu_lobby ] || ssh-keygen -q -t rsa -b 4096 -N "" -f ~/.ssh/ssbu_lobby      # RSA: Cloud Shell runs in FIPS mode (no ED25519)

say "4. the VM $NAME ($SHAPE)"
ID=$(oci compute instance list -c "$C" --display-name "$NAME" --lifecycle-state RUNNING --query 'data[0].id' --raw-output 2>/dev/null || true)
if [ -z "$ID" ] || [ "$ID" = "null" ]; then
  AD=$(oci iam availability-domain list -c "$C" --query 'data[0].name' --raw-output)
  IMG=$(oci compute image list -c "$C" --operating-system "Canonical Ubuntu" --operating-system-version "24.04" --shape "$SHAPE" \
        --sort-by TIMECREATED --sort-order DESC --query 'data[0].id' --raw-output)
  SHAPE_CFG=(); [[ "$SHAPE" == *Flex ]] && SHAPE_CFG=(--shape-config "{\"ocpus\": $OCPUS, \"memoryInGBs\": $MEMORY_GB}")
  until oci compute instance launch -c "$C" --availability-domain "$AD" --shape "$SHAPE" "${SHAPE_CFG[@]}" \
          --subnet-id "$SUBNET" --image-id "$IMG" --display-name "$NAME" --assign-public-ip true \
          --ssh-authorized-keys-file ~/.ssh/ssbu_lobby.pub --wait-for-state RUNNING --output json > /tmp/ssbu_vm.json 2> /tmp/ssbu_vm.err; do
    if grep -qi "capacity" /tmp/ssbu_vm.err; then say "   Milan has no free $SHAPE right now: trying again in $RETRY s (leave this tab open)"; sleep "$RETRY"
    elif grep -qi "TooManyRequests\|\"status\": 429" /tmp/ssbu_vm.err; then say "   Oracle says too many requests: waiting 300 s, then trying again"; sleep 300
    else cat /tmp/ssbu_vm.err; exit 1; fi
  done
  ID=$(python3 -c 'import json; print(json.load(open("/tmp/ssbu_vm.json"))["data"]["id"])')
  say "   created"
else say "   already running"; fi
IP=$(oci compute instance list-vnics --instance-id "$ID" --query 'data[0]."public-ip"' --raw-output)
[ -n "$IP" ] && [ "$IP" != "null" ] || { say "the VM has no public IP: its subnet must be the public one"; exit 1; }

say "5. the lobby on $IP"
SSH=(ssh -n -i ~/.ssh/ssbu_lobby -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 "ubuntu@$IP")      # -n: run through "curl | bash", ssh must not read the rest of the script from stdin
for i in $(seq 1 30); do "${SSH[@]}" true 2>/dev/null && break; sleep 10; done
"${SSH[@]}" "set -e; mkdir -p ssbu && cd ssbu && curl -fsSLO $REPO_RAW/tools/net/ssbu_lobby.py && curl -fsSLO $REPO_RAW/tools/net/deploy_lobby.sh && sudo bash deploy_lobby.sh $PORT"
say "DONE. Lobby address for the players: $IP:$PORT  (online_server = $IP:$PORT)"
say "Status page (from the server itself): ssh -i ~/.ssh/ssbu_lobby ubuntu@$IP curl -s 127.0.0.1:8080/status"
