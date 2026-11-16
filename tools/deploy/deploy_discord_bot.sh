#!/bin/sh
# Puts the Discord status bot (tools/discord_status_bot.py: "21 players in the OpenSmash lobbies" as its status)
# on the online lobby's server, over SSH, as the service ssbu-discord-bot. Run from the port's folder:
#   sh tools/deploy/deploy_discord_bot.sh <ssh host> token       paste the bot token (from the Discord Developer
#                                                                Portal, Bot tab), then Ctrl-D: kept in
#                                                                /etc/ssbu-discord-bot/token, root only
#   sh tools/deploy/deploy_discord_bot.sh <ssh host> install     copy it, make its Python venv (discord.py), start it
#   sh tools/deploy/deploy_discord_bot.sh <ssh host> update      copy the new script and restart
#   sh tools/deploy/deploy_discord_bot.sh <ssh host> status      the service's state and its last log lines
#   sh tools/deploy/deploy_discord_bot.sh <ssh host> uninstall   stop and remove it (the token is kept)
# SSBU_SUDO=sudo for a login that is not root. The bot needs no open port and no privileged intent.
set -e
host=$1
what=${2:-status}
sudo=${SSBU_SUDO:-}
here=$(dirname "$0")
[ -n "$host" ] || { sed -n '2,12p' "$0"; exit 2; }

remote() { ssh "$host" "$sudo sh -s"; }

case $what in
token)
    echo "paste the bot token, then Enter and Ctrl-D:"
    ssh "$host" "$sudo install -d -m 700 /etc/ssbu-discord-bot && $sudo sh -c 'umask 077; tr -d \"\\r\\n \" > /etc/ssbu-discord-bot/token' && echo saved"
    ;;
install|update)
    tmp=/tmp/ssbu-discord-bot-deploy
    ssh "$host" "rm -rf $tmp && mkdir -m 700 $tmp"
    scp -q tools/discord_status_bot.py "$here/ssbu-discord-bot.service" "$host:$tmp/"
    remote <<EOF
set -e
[ -s /etc/ssbu-discord-bot/token ] || { echo "no token yet: run this script with 'token' first"; exit 1; }
install -d -m 755 /opt/ssbu-discord-bot
tr -d '\r' < $tmp/discord_status_bot.py > /opt/ssbu-discord-bot/discord_status_bot.py
tr -d '\r' < $tmp/ssbu-discord-bot.service > /etc/systemd/system/ssbu-discord-bot.service
if [ ! -x /opt/ssbu-discord-bot/venv/bin/python ]; then
    # (Ubuntu ships python3 without ensurepip: the venv package first, quietly, no questions)
    python3 -c 'import ensurepip' 2> /dev/null || DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=a apt-get install -y -q python3-venv > /dev/null 2>&1
    rm -rf /opt/ssbu-discord-bot/venv
    python3 -m venv /opt/ssbu-discord-bot/venv
fi
/opt/ssbu-discord-bot/venv/bin/pip install -q --upgrade 'discord.py>=2.4,<3'
chmod -R a+rX /opt/ssbu-discord-bot
rm -rf $tmp
systemctl daemon-reload
systemctl enable ssbu-discord-bot > /dev/null 2>&1
systemctl restart ssbu-discord-bot
sleep 8
systemctl --no-pager --lines=5 status ssbu-discord-bot | tail -7
EOF
    ;;
status)
    echo "systemctl --no-pager --lines=10 status ssbu-discord-bot" | remote
    ;;
uninstall)
    remote <<'EOF'
systemctl disable --now ssbu-discord-bot 2> /dev/null || true
rm -f /etc/systemd/system/ssbu-discord-bot.service
rm -rf /opt/ssbu-discord-bot
systemctl daemon-reload
echo "the bot is removed (its token stays in /etc/ssbu-discord-bot/token)"
EOF
    ;;
*)
    sed -n '2,12p' "$0"; exit 2 ;;
esac
