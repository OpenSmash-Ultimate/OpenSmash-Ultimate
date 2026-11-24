#!/usr/bin/env python3
"""Two games of this folder paired for a rollback match through a LOCAL official-style lobby (Windows, test only).

Usage:
  python tools/dev/relay_pair.py <tag> [--stage battle_field] [--seconds 150] [--tz1 <TZ>] [--tz2 <TZ>]
                                 [--no-ticket] [--exe <file in build>] [--items on] [--hazards on]
                                 [VAR=value ...]                                         (VAR=value: both games)
                                 [1:VAR=value] [2:VAR=value]                             (one game only)

What it starts, all on 127.0.0.1 and all stopped at the end (nothing is left running):
  - the lobby server's tools/ssbu_relay.py (OPENSMASH_SERVER_DIR, or ../opensmash-server) --bind 127.0.0.1 --port <free port> --relay-only --ticket-secret-file <a test secret made here>
    --min-client-level 1: the official lobby's rules (relay only, lobby tickets, the online level);
  - two games (build\\ssbu.exe), each with its own copy of the save (SSBU_DATA_ROOT), SSBU_OFFICIAL_LOBBY and
    SSBU_NET_RELAY both that lobby (so each game treats it as the official one: ticket + level in its JOIN, direct paths
    off), a test ticket signed here with the test secret (two accounts), SSBU_NET_PROTOCOL=3, one lobby code,
    SSBU_RB=1 with SSBU_RB_NET=relay,0 and relay,1, a bot on the pads (SSBU_RB_NET_BOT=1), the same SSBU_DEBUG_MATCH
    and SSBU_RB_SEED, the menus pressed through by SSBU_SEQ.
--tz1 / --tz2: each game's TZ (FIXCLOCK's check: other time zones on the two games, e.g. AAA-11 and BBB+11).
Output: build\\relay_pair\\<tag>\\ (lobby.log, game1.log, game2.log, verdict.txt). The verdict reads the games' own
checks: "net check" (frames hashed again after a rollback), DESYNC lines (a final frame's hash differs from the other
game's), the frames checked, and what the lobby and the games said about tickets and direct paths.
Exit code 0: paired, frames compared and no DESYNC."""
import hashlib
import hmac
import os
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "build", "relay_pair")


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def ticket(secret, uid, name):
    body = "%s|%s|%d" % (uid, name, int(time.time()) + 3600)
    return body + "." + hmac.new(secret, body.encode(), hashlib.sha256).hexdigest()


def running_games():
    out = subprocess.run(["tasklist", "/NH"], capture_output=True, text=True).stdout
    # (the game's own executables only: ssbu.exe, ssbu_rb.exe, ...; not another program whose name starts so, such as
    # SSBU Arena's launcher SSBU-Arena.exe, which made this refuse to start)
    return sum(1 for l in out.splitlines() if re.match(r"ssbu([._]|$)", (l.split() or [""])[0].lower()))


def main(argv):
    if len(argv) < 2: raise SystemExit(__doc__)
    tag, args = argv[1], argv[2:]
    stage, seconds, tz, exe_name, no_ticket, save = "battle_field", 150, [None, None], "ssbu.exe", False, os.path.join(ROOT, "save")
    extra = [{}, {}]
    rules = {"items": "off", "hazards": "off"}
    plain = False
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--items", "--hazards"): rules[a[2:]] = args[i + 1]; i += 2; continue
        if a in ("--stage", "--seconds", "--tz1", "--tz2", "--exe", "--save"):
            v = args[i + 1]; i += 2
            if a == "--stage": stage = v
            elif a == "--seconds": seconds = int(v)
            elif a == "--tz1": tz[0] = v
            elif a == "--tz2": tz[1] = v
            elif a == "--save": save = v
            else: exe_name = v
            continue
        if a == "--no-ticket": no_ticket = True; i += 1; continue
        if a == "--plain-lobby": plain = True; i += 1; continue   # (a lobby without the official one's rules, for a game from before them: the reference)
        m = re.match(r"(?:([12]):)?([A-Za-z_0-9]+)=(.*)$", a)
        if not m: raise SystemExit("unknown argument: " + a)
        for g in (0, 1):
            if m.group(1) in (None, str(g + 1)): extra[g][m.group(2)] = m.group(3)
        i += 1
    exe = os.path.join(ROOT, "build", exe_name)
    if not os.path.isfile(exe): raise SystemExit("no build\\%s here" % exe_name)
    if not os.path.isdir(save): raise SystemExit("no save folder: " + save)
    if running_games(): raise SystemExit("another ssbu.exe is running")
    # (the lobby server left this repository with #484: a checkout of it, OPENSMASH_SERVER_DIR or ../opensmash-server
    # next to this folder, as tests/netproto.py and netlobby.sh find it; a tree from before #484 still has its own)
    server = os.environ.get("OPENSMASH_SERVER_DIR") or os.path.join(os.path.dirname(ROOT), "opensmash-server")
    relay = next((r for r in (os.path.join(server, "tools", "ssbu_relay.py"), os.path.join(ROOT, "tools", "ssbu_relay.py"))
                  if os.path.isfile(r)), None)
    if not relay: raise SystemExit("no lobby server: set OPENSMASH_SERVER_DIR to a checkout of it (its tools/ssbu_relay.py)")
    out = os.path.join(OUT, tag)
    if os.path.isdir(out): shutil.rmtree(out)
    os.makedirs(out)
    secret = os.urandom(24).hex().encode()   # (a test secret, of this run only)
    with open(os.path.join(out, "secret.txt"), "wb") as f: f.write(secret)
    port = free_port()
    lobby_log = open(os.path.join(out, "lobby.log"), "w")
    lobby = subprocess.Popen([sys.executable, "-u", relay, "--bind", "127.0.0.1", "--port", str(port),
                              *([] if plain else ["--relay-only", "--ticket-secret-file", os.path.join(out, "secret.txt"), "--min-client-level", "1"])],
                             cwd=out, stdout=lobby_log, stderr=subprocess.STDOUT)
    time.sleep(1.0)
    match = "p1=mario,p2=fox,p2=human,stage=%s,items=%s,hazards=%s" % (stage, rules["items"], rules["hazards"])
    games = []
    try:
        if lobby.poll() is not None: raise SystemExit("the lobby did not start: see " + os.path.join(out, "lobby.log"))
        for g in (0, 1):
            data = os.path.join(out, "data%d" % (g + 1))
            shutil.copytree(save, os.path.join(data, "save"))
            env = {k: v for k, v in os.environ.items() if not k.startswith("SSBU_")}
            addr = "127.0.0.1:%d" % port
            env.update({
                "SSBU_OFFICIAL_LOBBY": addr, "SSBU_NET_RELAY": addr, "SSBU_NET_PROTOCOL": "3", "SSBU_NET_ROOM": "rbtest",
                "SSBU_NET_UPNP": "0", "SSBU_NET_LOG": "1",
                "SSBU_RB": "1", "SSBU_RB_NET": "relay,%d" % g, "SSBU_RB_NET_BOT": "1", "SSBU_RB_SEED": "1", "SSBU_RB_LOG": "1",
                "SSBU_DEBUG_MATCH": match,
                # (the menus as tools/dev/rb_bench.py's: A alone from the title to the fighter select, a fighter, +, then A and +
                # until the fight; the second controller connected and hands off, as a rollback fight wants two humans)
                "SSBU_SEQ": "@movie 60 @until:2500-9000/30:A/120 60:SUP/30 20:SLEFT/18 30:A 90:PLUS 100:A @untilmark:fight:A+PLUS/40/6",
                "SSBU_SEQ2": "900000 900000 900000",
                "SSBU_SKIP_MOVIES": "1", "SSBU_VOLUME": "0", "SSBU_DATA_ROOT": data,
            })
            if plain: del env["SSBU_OFFICIAL_LOBBY"]
            if not no_ticket and not plain:
                env["SSBU_ONLINE_TICKET"] = ticket(secret, "00000000-0000-4000-8000-00000000000%d" % (g + 1), "Tester%d" % (g + 1))
            if tz[g]: env["TZ"] = tz[g]
            env.update(extra[g])
            lf = open(os.path.join(out, "game%d.log" % (g + 1)), "w", encoding="utf-8", errors="replace")
            games.append((subprocess.Popen([exe, "dump\\modules", "--status", "30"], cwd=ROOT, env=env, stdout=lf, stderr=subprocess.STDOUT), lf))
            time.sleep(3.0)
        t0 = time.time()
        while time.time() - t0 < seconds and all(p.poll() is None for p, _ in games): time.sleep(1.0)
    finally:
        for p, lf in games:
            if p.poll() is None: p.kill()
            p.wait(); lf.close()
        lobby.kill(); lobby.wait(); lobby_log.close()
        for g in (0, 1): shutil.rmtree(os.path.join(out, "data%d" % (g + 1)), ignore_errors=True)
        try: os.remove(os.path.join(out, "secret.txt"))
        except OSError: pass
    # the verdict
    lines = []
    ok = True
    for g in (0, 1):
        text = open(os.path.join(out, "game%d.log" % (g + 1)), encoding="utf-8", errors="replace").read()
        des = len(re.findall(r"net mode: DESYNC", text))
        again = re.findall(r"net check: (\d+) frames hashed again after a rollback with their pads all real, (\d+) of them DIFFERENT", text)
        checked = re.findall(r"(\d+) final frames compared with the other games so far \(to logic frame \d+\), (\d+) of them", text)
        rb = re.findall(r"(\d+) rollbacks", text)
        direct = len(re.findall(r"\[net\].*(direct path|punched|introduc)", text, re.I))
        # (the runtime writes "*** host exception ..." and "guest trap at ...": runtime/host.c - in lower case, which the
        # older pattern did not match, so the one crash of this branch's pairs was not counted)
        crash = len(re.findall(r"\*\*\* host exception|guest trap at|HOST EXCEPTION|GUEST TRAP", text))
        fights = len(re.findall(r"fight starts at logic frame", text))
        lines.append("game %d: fights %d, DESYNC lines %d, net check %s, compared %s, rollbacks %s, direct-path lines %d, crash lines %d"
                     % (g + 1, fights, des, again[-1] if again else "-", checked[-1] if checked else "-", rb[-1] if rb else "-", direct, crash))
        if des or crash or not fights or (again and int(again[-1][1])) or not checked or int(checked[-1][0]) < 600 or int(checked[-1][1]): ok = False
    lt = open(os.path.join(out, "lobby.log"), errors="replace").read()
    lines.append("lobby: %d lines; joins %d, refused %d, INTRODUCE %d" % (lt.count("\n"), len(re.findall(r"join", lt, re.I)), len(re.findall(r"refus|reject", lt, re.I)), len(re.findall(r"introduc", lt, re.I))))
    lines.append("VERDICT: " + ("OK (paired, no DESYNC)" if ok else "NOT OK"))
    with open(os.path.join(out, "verdict.txt"), "w") as f: f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
