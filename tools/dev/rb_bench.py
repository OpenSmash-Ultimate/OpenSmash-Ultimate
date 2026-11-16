#!/usr/bin/env python3
"""The standard rollback bench (Windows; ported from SSBU Arena's rb8ms/bench.sh + benchsum.py, round 12).

Usage:
  python tools/dev/rb_bench.py run <tag> [--warm 1885] [--meas 1800] [--stages "battle_field ..."] [--lag 5] [--delay 2]
                                         [--save <folder>] [--timeout <s>] [--exe <file in build>] [VAR=value ...]
  python tools/dev/rb_bench.py sum <tag> [<tag> ...]
  python tools/dev/rb_bench.py save <folder>      (makes the bench's save: see below)

`run` starts ONE game (build\\ssbu.exe, this folder's) with no hands on the menus and waits for it to end by itself:
  - the six competitive stages chained (SSBU_DEBUG_MATCH_LIST: Battlefield, Small Battlefield, Final Destination,
    Pokemon Stadium 2, Town and City, Smashville), Mario against a CPU Fox (level 9), 9 stocks;
  - rollback on in its loop mode (SSBU_RB=1, SSBU_RB_NET=loop,<delay>,<lag>,<lag>,0,<frames>): player 1 is the loop bot
    (net_pad), player 2's pads arrive <lag> frames after they were read, so a changed pad is guessed wrong and rolled
    back; pads and arrivals are drawn from the fight's frame, so every run has the same rollback schedule. The fight hash
    runs every frame (as an online fight's);
  - in every fight a warm-up of --warm real frames, then a segment of --meas real frames measured: one "BENCH" line per
    stage, then "BENCH SUMMARY" (runtime/hle_rb_core.c, SSBU_RB_STDBENCH);
  - the menus are pressed through by an input sequence (SSBU_SEQ: A and + until the bench's mark rbb_fight<n>);
  - its save is a copy of --save (default build\\rb_bench\\save_snap), taken fresh every run: every run starts alike.
Output: build\\rb_bench\\<tag>\\ (game.log, bench.txt, ticks_<n>_<stage>.txt) and a line in build\\rb_bench\\progress.log.
Nothing runs at the same time: it refuses while another ssbu.exe runs, and kills its game if it outlives --timeout.

`save <folder>`: the save the bench starts from, made from this folder's save\\ (the game started once to its main menu):
copied to <folder>\\save and every fighter unlocked there (tools/unlock_all.py), so no challenger interrupts the matches.

`sum`: per stage the rollback tick median of each run, their mean and sd, the other columns' means; the six-stage
average per run and its mean / sd (Arena's benchsum.py)."""
import os
import re
import shutil
import statistics as st
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "build", "rb_bench")
STAGES = "battle_field battle_field_s end poke_stadium2 animal_city animal_village"
# The menus, from the boot: the opening movie skipped, then A (and +, which starts the match at character select once
# both players are in) every 40 polls until the bench's mark says the fight began; after each match the same through
# the results, character select and stage select.
def menu_seq(stages):
    steps = ["@movie"] + ["@untilmark:rbb_fight%d:A+PLUS/40/6" % (n + 1) for n in range(stages)]
    return " ".join(steps)


def running_games():
    """ssbu*.exe processes running (this folder's or another's: the bench runs alone)"""
    try:
        out = subprocess.run(["tasklist", "/NH"], capture_output=True, text=True).stdout
    except OSError:
        return 0
    return sum(1 for l in out.splitlines() if l.lower().startswith("ssbu"))


def run(tag, args):
    warm, meas, stages, lag, delay, save, timeout, extra = 1885, 1800, STAGES, 5, 2, os.path.join(OUT, "save_snap"), 0, []
    exe_name = "ssbu.exe"
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--warm", "--meas", "--stages", "--lag", "--delay", "--save", "--timeout", "--exe"):
            v = args[i + 1]; i += 2
            if a == "--warm": warm = int(v)
            elif a == "--meas": meas = int(v)
            elif a == "--stages": stages = v
            elif a == "--lag": lag = int(v)
            elif a == "--delay": delay = int(v)
            elif a == "--save": save = v
            elif a == "--exe": exe_name = v
            else: timeout = int(v)
            continue
        if "=" in a: extra.append(a); i += 1; continue
        raise SystemExit("unknown argument: " + a)
    exe = os.path.join(ROOT, "build", exe_name)
    if not os.path.isfile(exe): raise SystemExit("no build\\%s here" % exe_name)
    if not os.path.isdir(os.path.join(save, "save")): raise SystemExit("no bench save in %s (rb_bench.py save %s)" % (save, save))
    if running_games(): raise SystemExit("another ssbu.exe is running: the bench runs alone")
    out = os.path.join(OUT, tag)
    if os.path.isdir(out): shutil.rmtree(out)
    os.makedirs(out)
    st_list = stages.split()
    with open(os.path.join(out, "list.txt"), "w") as f:
        for s in st_list: f.write("p1=mario,p2=fox,p2=cpu,cpu=9,stage=%s,stock=9,items=off,hazards=off\n" % s)
    data = os.path.join(out, "data")
    shutil.copytree(os.path.join(save, "save"), os.path.join(data, "save"))
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("SSBU_"): del env[k]
    env.update({
        "SSBU_RB": "1", "SSBU_RB_NET": "loop,%d,%d,%d,0,1000000" % (delay, lag, lag), "SSBU_RB_SEED": "1",
        "SSBU_RB_STDBENCH": "%d,%d,%d" % (warm, meas, len(st_list)), "SSBU_RB_STDBENCH_TICKS": os.path.join(out, "ticks"),
        "SSBU_DEBUG_MATCH_LIST": os.path.join(out, "list.txt"), "SSBU_SEQ": menu_seq(len(st_list)),
        "SSBU_SKIP_MOVIES": "1", "SSBU_VOLUME": "0", "SSBU_DATA_ROOT": data,
    })
    for kv in extra:
        k, v = kv.split("=", 1); env[k] = v
    if not timeout: timeout = int(len(st_list) * ((warm + meas) / 60 + 150) + 240)
    log = os.path.join(out, "game.log")
    t0 = time.time()
    with open(log, "w", encoding="utf-8", errors="replace") as lf:
        p = subprocess.Popen([exe, "dump\\modules", "--status", "30"], cwd=ROOT, env=env, stdout=lf, stderr=subprocess.STDOUT)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
            print("TIMEOUT: the game was stopped after %d s" % timeout)
    el = time.time() - t0
    lines = [l.rstrip("\n") for l in open(log, encoding="utf-8", errors="replace") if l.startswith("[bench] BENCH")]
    with open(os.path.join(out, "bench.txt"), "w") as f:
        for l in lines: f.write(l[8:] + "\n")
    shutil.rmtree(data, ignore_errors=True)
    crash = sum(1 for l in open(log, encoding="utf-8", errors="replace") if "HOST EXCEPTION" in l or "GUEST TRAP" in l or "crash report" in l.lower())
    summ = next((l[8:] for l in lines if "BENCH SUMMARY" in l), "NO SUMMARY (%d stages)" % sum(1 for l in lines if re.match(r"\[bench\] BENCH \d", l)))
    msg = "%s | BENCH %s | %s | %s | %.0f s | crash lines %d" % (time.strftime("%d/%m %H:%M"), tag, " ".join(extra) or "default", summ, el, crash)
    with open(os.path.join(OUT, "progress.log"), "a") as f: f.write(msg + "\n")
    print(msg)
    for l in lines: print(l[8:][:400])


COLS = ["rbmed", "p99", "over", "norm", "real", "save", "load", "first", "others", "hash", "lastwait"]
def sum_(tags):
    runs, order = {}, []
    for t in tags:
        for l in open(os.path.join(OUT, t, "bench.txt"), errors="replace"):
            m = re.match(r"BENCH (\d+) (\S+) \|", l)
            if not m: continue
            d = {}
            for c in COLS:
                mm = re.search(r"\b%s ([\d.]+)" % c, l)
                if mm: d[c] = float(mm.group(1))
            mm = re.search(r"rollbacks (\d+)", l); d["rb"] = int(mm.group(1)) if mm else 0
            runs.setdefault(m.group(2), []).append((t, d))
            if m.group(2) not in order: order.append(m.group(2))
    sd = lambda v: st.stdev(v) if len(v) > 1 else 0.0
    print("%-16s %-30s %6s %5s | %6s %5s %5s | %5s %5s %5s %5s %5s %5s %5s | rb" % ("stage", "rbmed per run", "mean", "sd", "p99", "over", "norm", "real", "save", "load", "first", "others", "hash", "lwait"))
    for s in order:
        v = runs[s]; r = [d["rbmed"] for _, d in v]
        mean = lambda c: st.mean(d.get(c, 0) for _, d in v)
        print("%-16s %-30s %6.2f %5.2f | %6.2f %5.2f %5.2f | %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f %5.2f | %d" % (
            s, " ".join("%.2f" % x for x in r), st.mean(r), sd(r), mean("p99"), mean("over"), mean("norm"), mean("real"), mean("save"),
            mean("load"), mean("first"), mean("others"), mean("hash"), mean("lastwait"), v[0][1]["rb"]))
    avg = {}
    for s in order:
        for t, d in runs[s]: avg.setdefault(t, []).append(d["rbmed"])
    a = [st.mean(v) for t, v in avg.items() if len(v) == len(order)]
    print("six-stage average per run: %s -> mean %.3f sd %.3f" % (" ".join("%.3f" % x for x in a), st.mean(a) if a else 0, sd(a)))
    nm = {}
    for s in order:
        for t, d in runs[s]: nm.setdefault(t, []).append(d.get("norm", 0))
    n = [st.mean(v) for t, v in nm.items() if len(v) == len(order)]
    print("normal tick (no rollback) six-stage average per run: %s -> mean %.3f" % (" ".join("%.3f" % x for x in n), st.mean(n) if n else 0))


def make_save(folder):
    src = os.path.join(ROOT, "save")
    if not os.path.isdir(src): raise SystemExit("no save\\ here: start the game once to its main menu, close it, then run this again")
    if running_games(): raise SystemExit("close the game first")
    if os.path.isdir(folder): shutil.rmtree(folder)
    shutil.copytree(src, os.path.join(folder, "save"))
    users = [u for u in os.listdir(os.path.join(folder, "save")) if os.path.isdir(os.path.join(folder, "save", u, "save_data"))]
    for u in users:
        path = os.path.join(folder, "save", u, "save_data", "system_data.bin")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "unlock_all.py"), "--save", path], cwd=ROOT)
        print("unlock_all.py on %s: exit %d" % (path, r.returncode))


if __name__ == "__main__":
    if len(sys.argv) < 3: raise SystemExit(__doc__)
    cmd = sys.argv[1]
    if cmd == "run": run(sys.argv[2], sys.argv[3:])
    elif cmd == "sum": sum_(sys.argv[2:])
    elif cmd == "save": make_save(sys.argv[2])
    else: raise SystemExit(__doc__)
