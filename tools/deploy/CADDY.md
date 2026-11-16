# HTTPS for the online lobby (Caddy)

The online lobby (`tools/ssbu_online_lobby.py`) answers plain HTTP on TCP 39421. A web page served over HTTPS (the
site, https://denkishi.github.io/opensmash-site/) cannot read it: the browser blocks the mixed content. Today the site
works around that with a GitHub Actions job that copies `/status.json` and `/leaderboard.json` to a branch. This is the
direct way: a Caddy reverse proxy on the same server that gives
`https://opensmash-lobby.duckdns.org/` a real certificate (Let's Encrypt, renewed by Caddy) and forwards only

- `/status.json`
- `/leaderboard.json`
- `/gsp/*` (a player's own GSP and place, the signed profile POST)

to `127.0.0.1:39421`. Everything else answers 404. The game, the launchers and the UDP/NEX traffic keep using the
existing ports; nothing about the lobby services changes.

This was written and checked for syntax only: it has **not** been run on the server.

## What the owner has to do first (Oracle Cloud)

Caddy needs Let's Encrypt to reach the server, so the instance's firewall must let these in:

1. Oracle Cloud console > Networking > Virtual cloud networks > the lobby's VCN > Security Lists (or the instance's
   Network Security Group) > Add ingress rules, source `0.0.0.0/0`:
   - TCP, destination port **80** (certificate challenge and the redirect to HTTPS)
   - TCP, destination port **443**
   - UDP, destination port **443** (optional, HTTP/3)
2. The DuckDNS name `opensmash-lobby.duckdns.org` must already point at the server's public IP (it does: the lobby is
   reached by that name).
3. The instance's own firewall is handled by the script (ufw, and the iptables `REJECT` rule that Oracle's Ubuntu images
   ship with). If it is not Debian/Ubuntu, install Caddy by hand (https://caddyserver.com/docs/install) and copy
   `Caddyfile` to `/etc/caddy/Caddyfile`.

## Install

From the repository root (the SSH login must be root, or set `SSBU_SUDO=sudo`):

    sh tools/deploy/deploy_caddy.sh <ssh host> install
    sh tools/deploy/deploy_caddy.sh <ssh host> status

`install` adds the official Caddy apt repository, installs the package, validates and installs the Caddyfile, opens
80/443, and starts the service. The first request for the certificate takes a few seconds. Check from anywhere:

    curl -i https://opensmash-lobby.duckdns.org/status.json

`update` copies a changed Caddyfile (validated before it replaces the old one) and reloads Caddy without dropping
connections. `uninstall` stops Caddy and closes 80/443 again.

## Switching the site over

Once the URL answers over HTTPS, the site (repository Denkishi/opensmash-site) can read the lobby directly: in
`js/snapshot.js` tries `https://opensmash-lobby.duckdns.org/status.json` first and falls back to the `data` branch
copy (the Actions updater keeps running as that fallback and for `leaderboard.json`, which the site builds from several
lobbies).

## Notes

- CORS: Caddy answers preflight (OPTIONS) itself and sets `Access-Control-Allow-Origin: *` on the three paths, replacing the lobby's own header so it is never doubled.
- The lobby sees every proxied request as coming from 127.0.0.1 (Caddy adds `X-Forwarded-For` with the real address, but
  the lobby does not read it). The ban list therefore does not apply to requests that come through HTTPS; the game's own
  TCP/UDP connections are unaffected.
- Logs: `journalctl -u caddy` and `/var/log/caddy/opensmash-lobby.log` (rotated, 5 MiB x 3).
