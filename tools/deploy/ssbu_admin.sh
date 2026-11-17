#!/bin/sh
# Maintenance of the OpenSmash lobby server from a commit of OpenSmash-Ultimate's main branch, for a maintenance user
# allowed only this script through sudoers. Installed root-owned (not writable by that user) at
# /opt/ssbu-tools/ssbu_admin.sh; the owner updates it by hand. Every file it deploys comes from a commit that GitHub
# says is on main, downloaded from GitHub here: nothing the calling user writes is ever run as root.
#   ssbu_admin.sh status                    services, last log lines, the lobby's /status.json
#   ssbu_admin.sh logs <service> [lines]    journalctl of ssbu-online | ssbu-lobby | ssbu-discord-bot | caddy
#   ssbu_admin.sh restart <service>         restart one of those
#   ssbu_admin.sh update-online <commit>    tools/ssbu_online_lobby.py, ssbu_relay.py, nex/, the service file; backup,
#                                           restart, health check, the backup put back if the service does not come up
#   ssbu_admin.sh update-bot <commit>       tools/discord_status_bot.py + its service file, same way
#   ssbu_admin.sh update-threads <commit>   tools/deploy/discord_threads.py into /opt/ssbu-discord-tools
#   ssbu_admin.sh restore-online <backup>   put a backup of /opt/ssbu-online back (names: ls /var/backups/ssbu)
#   ssbu_admin.sh reset-board --yes         back up and empty the leaderboard (nex/reset_board.py), Discord links kept
#   ssbu_admin.sh digest [now]              the bot's feedback-thread digest (JSON); "now": have the bot remake it first
# <commit> is a full 40-character commit id, or "main" for main's newest commit.
set -eu
REPO=OpenSmash-Ultimate/OpenSmash-Ultimate
BK=/var/backups/ssbu
SERVICES="ssbu-online ssbu-lobby ssbu-discord-bot caddy"
umask 022
export PATH=/usr/sbin:/usr/bin:/sbin:/bin

die() { echo "error: $*" >&2; exit 1; }
usage() { sed -n '2,16p' "$0"; exit 2; }
svc_ok() { for s in $SERVICES; do [ "$1" = "$s" ] && return 0; done; return 1; }

# a commit id on main -> $SRC holds that commit's tools/ (removed on exit)
fetch() {
    c=$1
    if [ "$c" = main ]; then
        c=$(curl -fsS -H 'Accept: application/vnd.github.sha' "https://api.github.com/repos/$REPO/commits/main") || die "cannot ask GitHub for main"
    fi
    echo "$c" | grep -Eq '^[0-9a-f]{40}$' || die "not a full commit id: $c"
    st=$(curl -fsS "https://api.github.com/repos/$REPO/compare/$c...main" | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["status"])') \
        || die "GitHub does not know commit $c"
    case $st in ahead|identical) ;; *) die "commit $c is not on main ($st)";; esac
    SRC=$(mktemp -d /tmp/ssbu-admin.XXXXXX)
    trap 'rm -rf "$SRC"' EXIT
    curl -fsSL "https://codeload.github.com/$REPO/tar.gz/$c" | tar -xz -C "$SRC" --strip-components=1 --wildcards '*/tools/*' \
        || die "download of $c failed"
    echo "commit $c (on main)"
}

backup() {   # dir name -> prints the backup file
    install -d -m 700 "$BK"
    f="$BK/$2-$(date -u +%Y%m%d-%H%M%S).tgz"
    tar -czf "$f" -C / "${1#/}" "etc/systemd/system/$2.service" 2>/dev/null || tar -czf "$f" -C / "${1#/}"
    echo "$f"
}

health() {   # service -> 0 if it is active after a few seconds
    sleep "${2:-4}"
    systemctl is-active --quiet "$1"
}

put() {   # source file -> destination, LF line ends, 644
    tr -d '\r' < "$1" > "$2.new" && chmod 644 "$2.new" && mv -f "$2.new" "$2"
}

cmd=${1:-}
[ -n "$cmd" ] || usage
case $cmd in
status)
    for s in $SERVICES; do printf '%-18s %s\n' "$s" "$(systemctl is-active "$s" 2>/dev/null || true)"; done
    curl -fsS -m 5 http://127.0.0.1:39421/status.json 2>/dev/null | head -c 600 && echo || echo "(no /status.json on 39421)"
    df -h / | tail -1
    ;;
logs)
    svc_ok "${2:-}" || die "service: one of $SERVICES"
    n=${3:-50}; echo "$n" | grep -Eq '^[0-9]{1,4}$' || die "lines: a number"
    journalctl -u "$2" --no-pager -n "$n"
    ;;
restart)
    svc_ok "${2:-}" || die "service: one of $SERVICES"
    systemctl restart "$2"; health "$2" 3 && echo "$2 active" || { systemctl --no-pager status "$2" | tail -15; exit 1; }
    ;;
update-online)
    fetch "${2:-}"
    [ -f "$SRC/tools/ssbu_online_lobby.py" ] && [ -d "$SRC/tools/nex" ] || die "tools/ssbu_online_lobby.py or tools/nex missing in that commit"
    b=$(backup /opt/ssbu-online ssbu-online); echo "backup $b"
    install -d -m 755 /opt/ssbu-online /opt/ssbu-online/nex
    put "$SRC/tools/ssbu_online_lobby.py" /opt/ssbu-online/ssbu_online_lobby.py
    put "$SRC/tools/ssbu_relay.py" /opt/ssbu-online/ssbu_relay.py
    for f in "$SRC"/tools/nex/*.py; do
        n=$(basename "$f"); case $n in test_*|selftest*) continue;; esac
        put "$f" "/opt/ssbu-online/nex/$n"
    done
    port=$(sed -n 's/.*--port \([0-9]*\).*/\1/p' /etc/systemd/system/ssbu-online.service | head -1)
    sed "s/--port 39421/--port ${port:-39421}/" "$SRC/tools/deploy/ssbu-online.service" | tr -d '\r' > /etc/systemd/system/ssbu-online.service
    systemctl daemon-reload
    systemctl restart ssbu-online
    if health ssbu-online 5; then
        echo "ssbu-online active"; journalctl -u ssbu-online --no-pager -n 8
    else
        echo "ssbu-online did NOT come up: putting $b back"; journalctl -u ssbu-online --no-pager -n 20
        rm -rf /opt/ssbu-online && tar -xzf "$b" -C / && systemctl daemon-reload && systemctl restart ssbu-online
        health ssbu-online 4 && echo "old version active again"; exit 1
    fi
    ;;
update-bot)
    fetch "${2:-}"
    [ -f "$SRC/tools/discord_status_bot.py" ] || die "tools/discord_status_bot.py missing in that commit"
    b=$(backup /opt/ssbu-discord-bot/discord_status_bot.py ssbu-discord-bot); echo "backup $b"
    put "$SRC/tools/discord_status_bot.py" /opt/ssbu-discord-bot/discord_status_bot.py
    tr -d '\r' < "$SRC/tools/deploy/ssbu-discord-bot.service" > /etc/systemd/system/ssbu-discord-bot.service
    systemctl daemon-reload
    systemctl restart ssbu-discord-bot
    if health ssbu-discord-bot 10; then
        echo "ssbu-discord-bot active"; journalctl -u ssbu-discord-bot --no-pager -n 6
    else
        echo "bot did NOT come up: putting $b back"; journalctl -u ssbu-discord-bot --no-pager -n 20
        tar -xzf "$b" -C / && systemctl daemon-reload && systemctl restart ssbu-discord-bot; exit 1
    fi
    ;;
update-threads)
    fetch "${2:-}"
    [ -f "$SRC/tools/deploy/discord_threads.py" ] || die "tools/deploy/discord_threads.py missing in that commit"
    install -d -m 755 /opt/ssbu-discord-tools
    tr -d '\r' < "$SRC/tools/deploy/discord_threads.py" > /opt/ssbu-discord-tools/discord_threads.py.new
    chmod 755 /opt/ssbu-discord-tools/discord_threads.py.new
    python3 -I -m py_compile /opt/ssbu-discord-tools/discord_threads.py.new || die "does not compile"
    rm -rf /opt/ssbu-discord-tools/__pycache__
    mv -f /opt/ssbu-discord-tools/discord_threads.py.new /opt/ssbu-discord-tools/discord_threads.py
    echo "discord_threads.py updated"
    ;;
restore-online)
    f=${2:-}; case $f in ssbu-online-*.tgz) ;; *) ls "$BK"; die "name of an ssbu-online-*.tgz backup";; esac
    [ -f "$BK/$f" ] || die "no $BK/$f"
    rm -rf /opt/ssbu-online && tar -xzf "$BK/$f" -C / && systemctl daemon-reload && systemctl restart ssbu-online
    health ssbu-online 4 && echo "ssbu-online active ($f)"
    ;;
digest)
    f=/var/lib/ssbu-discord-bot/threads_digest.json
    if [ "${2:-}" = now ]; then
        old=$(stat -c %Y "$f" 2>/dev/null || echo 0)
        systemctl kill -s USR1 ssbu-discord-bot
        i=0; while [ $i -lt 120 ] && [ "$(stat -c %Y "$f" 2>/dev/null || echo 0)" = "$old" ]; do sleep 1; i=$((i+1)); done
    fi
    [ -f "$f" ] || die "no digest yet (the bot writes it ~20 s after it starts)"
    cat "$f"
    ;;
reset-board)
    [ "${2:-}" = --yes ] || die "reset-board empties the leaderboard: add --yes"
    [ -f /opt/ssbu-online/nex/reset_board.py ] || die "reset_board.py not on the server: update-online first"
    systemctl stop ssbu-online
    python3 -I /opt/ssbu-online/nex/reset_board.py --state-dir /var/lib/ssbu-online --yes || { systemctl start ssbu-online; exit 1; }
    systemctl start ssbu-online
    health ssbu-online 3 && echo "ssbu-online active"
    ;;
*) usage ;;
esac
