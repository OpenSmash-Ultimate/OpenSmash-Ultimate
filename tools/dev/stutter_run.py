#!/usr/bin/env python3
"""The hitches a player sees in a first match (Windows, test only): one game started with no hands on the menus, a
normal match (no rollback), every frame's time written by the game (SSBU_PERF_FRAMES), counted here.

Usage:
  python tools/dev/stutter_run.py <tag> [--stage battle_field] [--seconds 60] [--save <folder>] [--exe <file in build>]
                                        [--fighters mario,fox] [VAR=value ...]

What it starts: build\\ssbu.exe with a copy of --save (default build\\rb_bench\\save_snap, tools/dev/rb_bench.py's),
SSBU_DEBUG_MATCH_LIST with one match (player 1 and a CPU, 9 stocks, no items), the menus pressed through by SSBU_SEQ
until the match loads (the mark of player 2's script module), then --seconds of its loading and fight; then the game is
closed.
Output: build\\stutter\\<tag>\\ (game.log, frames.txt) and one line in build\\stutter\\progress.log:
  - boot: seconds from the start to the match's loading, and what the log says the shader loading waited before the first
    frame and before the match ("shader loading", "match shaders");
  - menus / fight: frames, and how many took over 25 ms, over 50 ms, over 100 ms, the longest, the 99th percentile;
  - "made in play": the pipelines and programs the renderer had to make during play (each a wait), and their total.
Nothing else may run: it refuses while another ssbu.exe runs."""
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "build", "stutter")
MENUS = ["@movie", "60", "@until:2500-9000/30:A/120", "60:SUP/30", "20:SLEFT/18", "30:A", "90:PLUS", "100:A",
         "@untilmark:module:lua2cpp_%s:A+PLUS/40/6"]   # (tools/dev/rb_bench.py's way to the first fight: until the match
                                                      # loads player 2's scripts, then hands off - a + in the fight pauses it)


def running_games():
    try:
        out = subprocess.run(["tasklist", "/NH"], capture_output=True, text=True).stdout
    except OSError:
        return 0
    return sum(1 for l in out.splitlines() if l.lower().startswith("ssbu"))


def part(times):
    if not times:
        return "0 frames"
    s = sorted(times)
    return "%d frames, over 25 ms %d, over 50 ms %d, over 100 ms %d, longest %.0f ms, p99 %.1f ms" % (
        len(times), sum(1 for t in times if t > 25), sum(1 for t in times if t > 50), sum(1 for t in times if t > 100), s[-1],
        s[min(len(s) - 1, int(len(s) * 0.99))])


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    tag, stage, seconds, save, exe_name, fighters, extra = args[0], "battle_field", 60, os.path.join(ROOT, "build", "rb_bench", "save_snap"), "ssbu.exe", "mario,fox", []
    i = 1
    while i < len(args):
        a = args[i]
        if a in ("--stage", "--seconds", "--save", "--exe", "--fighters"):
            v = args[i + 1]
            i += 2
            if a == "--stage": stage = v
            elif a == "--seconds": seconds = int(v)
            elif a == "--save": save = v
            elif a == "--exe": exe_name = v
            else: fighters = v
            continue
        if "=" in a:
            extra.append(a)
            i += 1
            continue
        raise SystemExit("unknown argument: " + a)
    exe = os.path.join(ROOT, "build", exe_name)
    if not os.path.isfile(exe):
        raise SystemExit("no build\\%s here" % exe_name)
    if not os.path.isdir(os.path.join(save, "save")):
        raise SystemExit("no save in %s (tools/dev/rb_bench.py save %s)" % (save, save))
    if running_games():
        raise SystemExit("another ssbu.exe is running: this runs alone")
    out = os.path.join(OUT, tag)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    p1, p2 = fighters.split(",")
    listf = os.path.join(out, "list.txt")
    with open(listf, "w") as f:
        f.write("p1=%s,p2=%s,p2=cpu,cpu=9,stage=%s,stock=9,items=off,hazards=off\n" % (p1, p2, stage))
    data = os.path.join(out, "data")
    shutil.copytree(os.path.join(save, "save"), os.path.join(data, "save"))
    frames = os.path.join(out, "frames.txt")
    env = {k: v for k, v in os.environ.items() if not k.startswith("SSBU_")}
    env.update({"SSBU_DEBUG_MATCH_LIST": listf, "SSBU_SEQ": " ".join(MENUS) % p2, "SSBU_SKIP_MOVIES": "1", "SSBU_VOLUME": "0",
                "SSBU_DATA_ROOT": data, "SSBU_PERF_FRAMES": frames})
    for kv in extra:
        k, v = kv.split("=", 1)
        env[k] = v
    log = os.path.join(out, "game.log")
    t0 = time.time()
    fight_at = fight_frame = None
    with open(log, "w", encoding="utf-8", errors="replace") as lf:
        p = subprocess.Popen([exe, os.path.join("dump", "modules"), "--status", "30"], cwd=ROOT, env=env, stdout=lf, stderr=subprocess.STDOUT)
        try:
            while p.poll() is None and time.time() - t0 < 900:
                time.sleep(0.5)
                if fight_at is None:
                    with open(log, encoding="utf-8", errors="replace") as r:
                        if "[seq] mark module:lua2cpp_%s" % p2 in r.read():
                            fight_at = time.time()
                            try:
                                with open(frames) as fr:
                                    fight_frame = sum(1 for _ in fr)
                            except OSError:
                                fight_frame = 0
                elif time.time() - fight_at >= seconds:
                    break
        finally:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
    shutil.rmtree(data, ignore_errors=True)
    try:
        with open(frames) as fr:
            times = [float(l) for l in fr.read().split()]
    except OSError:
        times = []
    text = open(log, encoding="utf-8", errors="replace").read()
    first = re.search(r"shader loading: the first frame waited ([\d.]+) s", text)
    match = re.search(r"match shaders: the match waited ([\d.]+) s for (\d+)", text)
    made = re.findall(r"made in play: (pipeline|program) .*?(?:waited|took) ([\d.]+) ms", text)
    crash = sum(1 for l in text.splitlines() if "HOST EXCEPTION" in l or "GUEST TRAP" in l)
    cut = fight_frame if fight_frame is not None else len(times)
    msg = ("%s | STUTTER %s | %s | to the fight %s s (first frame waited %s s, the match %s s for %s pipelines) | menus: %s | fight: %s | "
           "made in play: %d pipelines %.0f ms, %d programs %.0f ms | crash lines %d" % (
               time.strftime("%d/%m %H:%M"), tag, " ".join(extra) or "default",
               "%.0f" % (fight_at - t0) if fight_at else "never", first.group(1) if first else "-", match.group(1) if match else "-",
               match.group(2) if match else "-", part(times[:cut]), part(times[cut:]),
               sum(1 for k, _ in made if k == "pipeline"), sum(float(ms) for k, ms in made if k == "pipeline"),
               sum(1 for k, _ in made if k == "program"), sum(float(ms) for k, ms in made if k == "program"), crash))
    with open(os.path.join(OUT, "progress.log"), "a") as f:
        f.write(msg + "\n")
    print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
