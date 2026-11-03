"""The character test bench: runs installed characters through boot -> match -> 20 s of play in
muted test copies of the game (SSBU_BENCH, runtime/hle_bench.c), one at a time, and writes a
pass/fail table.

  bench.py run <fighter>[:<costume>] [<opponent>[:<costume>]] [--tag T]   one run
  bench.py play <fighter>[:<costume>] [<opponent>[:<costume>]] [--minutes M]   you play it (your pads, sound on, 10 min)
  bench.py list                                                         what `all` would run
  bench.py all [--only <substring>] [--repeat-failed] [--runs N]        every installed character, N runs each (2)

Each run: <tree>/bench/<tag>/ holds log.txt, a few frames (shot_*.jpg) and result.json; the table is
<tree>/bench/results.md (and results.json). A run takes the shared test lock
(/tmp/windowsmash-test.lock), never starts while a copy of the game that isn't a test copy runs
(the user's), and closes only its own copy (by its SSBU_TESTRUN tag).

macOS only for now (open -n of build-mac/WindowSmash.app)."""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

TREE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
APP = os.path.join(TREE, "build-mac", "WindowSmash.app")
OUT = os.path.join(TREE, "bench")
LOCK = "/tmp/windowsmash-test.lock"
SECONDS = 20
TIMEOUT = 330          # a run's whole time (boot ~35 s, menus to ~60 s, load, play, slack)


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def game_pids():
    r = subprocess.run(["pgrep", "-f", "MacOS/ssbu"], capture_output=True, text=True)
    return [int(p) for p in r.stdout.split()]


def env_of(pid):
    r = subprocess.run(["ps", "eww", "-p", str(pid)], capture_output=True, text=True)
    return r.stdout


def users_game_running():
    return any("SSBU_TESTRUN=" not in env_of(p) for p in game_pids())


def take_lock(owner):
    while True:
        while users_game_running():
            print("the user's game is running: waiting", flush=True)
            time.sleep(60)
        try:
            os.mkdir(LOCK)
            with open(os.path.join(LOCK, "owner"), "w") as f:
                f.write(owner + "\n")
            return
        except FileExistsError:
            time.sleep(15)


def drop_lock():
    try:
        os.remove(os.path.join(LOCK, "owner"))
    except OSError:
        pass
    try:
        os.rmdir(LOCK)
    except OSError:
        pass


def memory_free():
    r = subprocess.run(["memory_pressure"], capture_output=True, text=True)
    m = re.search(r"free percentage: (\d+)%", r.stdout)
    return int(m.group(1)) if m else 100


def shrink_frames(out):
    """window_N.bmp -> shot_N.jpg (640 wide); the rest of the captures go (6 MB each)."""
    for p in sorted(glob.glob(os.path.join(out, "window_*.bmp"))):
        n = re.search(r"window_(\d+)", p).group(1)
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "60", "--resampleWidth", "640", p,
                        "--out", os.path.join(out, f"shot_{int(n):03d}.jpg")], capture_output=True)
    for p in glob.glob(os.path.join(out, "*.bmp")):
        os.remove(p)


def reports_since(t0):
    found = []
    for p in glob.glob(os.path.join(TREE, "reports", "*.txt")):
        if os.path.getmtime(p) >= t0 - 1:
            found.append(p)
    return sorted(found)


def run(spec, opponent="mario:0", tag=None, seconds=SECONDS, play=False):
    tag = tag or re.sub(r"[^a-z0-9_]+", "_", f"{spec}_vs_{opponent}".lower())
    out = os.path.join(OUT, tag)
    if not play:   # (play mode runs beside the bench's own test copies: no lock)
        take_lock(f"bench {tag} {os.getpid()}")
    try:
        while memory_free() < 40:
            print("memory below 40% free: waiting", flush=True)
            time.sleep(60)
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out)
        log = os.path.join(out, "log.txt")
        env = {"SSBU_DISCORD": "off", "SSBU_CWD": TREE, "SSBU_TESTRUN": tag, "SSBU_LOG": log,
               "SSBU_BENCH": f"{spec},{opponent}", "SSBU_BENCH_SECONDS": str(seconds), "SSBU_CAPTURE": out,
               "SSBU_CAPTURE_WINDOW": "1", "SSBU_CAPTURE_EVERY": "6", "SSBU_CAPTURE_MAX": "45"}
        for i, a in enumerate(sys.argv):   # (--env K=V: more for the runtime, e.g. a debug switch)
            if a == "--env" and i + 1 < len(sys.argv) and "=" in sys.argv[i + 1]:
                env.update([sys.argv[i + 1].split("=", 1)])
        if play:   # (the player's: sound on, their pads in the match, no frames captured)
            env.update(SSBU_BENCH_PLAY="1")
            for k in ("SSBU_CAPTURE", "SSBU_CAPTURE_WINDOW", "SSBU_CAPTURE_EVERY", "SSBU_CAPTURE_MAX"):
                env.pop(k)
        else:
            env["SDL_AUDIO_DRIVER"] = "dummy"
        cmd = ["open", "-n"] if play else ["open", "-n", "-g"]   # (test copies open behind: a player's window keeps the focus)
        for k, v in env.items():
            cmd += ["--env", f"{k}={v}"]
        cmd += ["-a", APP, "--args", os.path.join(TREE, "dump", "modules"), "--status", "30"]
        t0 = time.time()
        subprocess.run(cmd, check=True)
        pid = None
        for _ in range(40):
            time.sleep(0.5)
            pid = next((p for p in game_pids() if f"SSBU_TESTRUN={tag}" in env_of(p)), None)
            if pid:
                break
        killed = False
        awake = subprocess.Popen(["caffeinate", "-d", "-w", str(pid)]) if pid else None
        while pid and pid in game_pids():
            if time.time() - t0 > TIMEOUT + (seconds if play else 0):
                if f"SSBU_TESTRUN={tag}" in env_of(pid):
                    subprocess.run(["kill", str(pid)])
                    time.sleep(3)
                    if pid in game_pids():
                        subprocess.run(["kill", "-9", str(pid)])
                killed = True
                break
            time.sleep(1)
        if awake:
            awake.terminate()
        took = time.time() - t0
    finally:
        if not play:
            drop_lock()
    shrink_frames(out)
    text = open(log, errors="replace").read() if os.path.exists(log) else ""
    m = re.search(r"\[bench\] t=[\d.]+ RESULT (\S+) (.*)", text)
    reports = reports_since(t0)
    crash = next((r for r in reports if os.path.basename(r).startswith(("crash_", "stopped_"))), None)
    if m:
        verdict, detail = m.group(1), m.group(2)
    elif crash:
        verdict, detail = "CRASH", first_lines(crash)
    elif killed:
        verdict, detail = "TIMEOUT", "no result in %d s" % TIMEOUT
    elif not pid:
        verdict, detail = "NO-START", last_lines(text)
    else:
        verdict, detail = "EXITED", last_lines(text)
    res = {"tag": tag, "spec": spec, "opponent": opponent, "verdict": verdict, "detail": detail, "seconds": round(took),
           "reports": [os.path.relpath(r, TREE) for r in reports], "plugins": plugins_in(text),
           "shots": len(glob.glob(os.path.join(out, "shot_*.jpg"))), "when": time.strftime("%Y-%m-%d %H:%M")}
    with open(os.path.join(out, "result.json"), "w") as f:
        json.dump(res, f, indent=1)
    print(f"{tag}: {verdict} - {detail} ({took:.0f} s)", flush=True)
    time.sleep(20)   # (a gap for other sessions' runs)
    return res


def first_lines(path, n=6):
    with open(path, errors="replace") as f:
        lines = [l.strip() for l in f.readlines() if l.strip()]
    keep = [l for l in lines if re.search(r"exception|signal|bus error|access|guest thread|main\+0x|\+0x", l)]
    return " | ".join((keep or lines)[:n])[:600]


def last_lines(text, n=4):
    return " | ".join([l for l in text.splitlines() if l.strip()][-n:])[:600]


def plugins_in(text):
    return sorted(set(re.findall(r"\[skyline\] plugin (\S+) at 0x", text)))


# ---- what's installed: each turned-on mod's characters (fighter, costumes) ----
SLOT = re.compile(r"fighter/([a-z_0-9]+)/model/[a-z_0-9]+/c(\d\d\d?)/")


def characters():
    """[(mod, fighter, [costumes])]: the costume slots each turned-on mod has models for."""
    with open(os.path.join(TREE, "mods", "mods.json")) as f:
        state = json.load(f)
    found = []
    for name in state.get("order", []):
        info = state["mods"].get(name)
        if not info or not info.get("enabled"):
            continue
        slots = {}
        root = os.path.join(TREE, "mods", name)
        for dirpath, _dirs, files in os.walk(root):
            rel = os.path.relpath(dirpath, root).replace(os.sep, "/") + "/"
            m = SLOT.search(rel if rel.startswith("fighter/") else "/" + rel)
            if m and files:
                slots.setdefault(m.group(1), set()).add(int(m.group(2)))
        for fighter, cs in sorted(slots.items()):
            if fighter in ("nana", "ptrainer_low", "element", "kirby") and len(slots) > 1:   # (parts of another's, copies)
                continue
            found.append((name, fighter, sorted(cs)))
    return found


def plan():
    """The runs: per mod and fighter, its first costume, and its first new one (c08+) if it has one."""
    runs, seen = [], set()
    for mod, fighter, cs in characters():
        picks = [cs[0]] + [c for c in cs if c >= 8][:1]
        for c in dict.fromkeys(picks):
            if (fighter, c) not in seen:
                seen.add((fighter, c))
                runs.append((mod, f"{fighter}:{c}"))
    return runs


def table(results):
    lines = ["| Mod | Character | Result | Details | Time |", "|---|---|---|---|---|"]
    for r in results:
        detail = r["detail"].replace("|", "/")
        lines.append(f"| {r.get('mod', '')} | {r['spec']} | **{r['verdict']}** | {detail} | {r['seconds']} s |")
    return "\n".join(lines) + "\n"


def save(results):
    with open(os.path.join(OUT, "results.json"), "w") as f:
        json.dump(results, f, indent=1)
    with open(os.path.join(OUT, "results.md"), "w") as f:
        f.write(f"# Character bench, {time.strftime('%Y-%m-%d %H:%M')}\n\n" + table(results))


def main():
    os.makedirs(OUT, exist_ok=True)
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "run":
        spec = sys.argv[2]
        opp = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else "mario:0"
        run(spec, opp, arg("--tag"), int(arg("--seconds", SECONDS)))
    elif cmd == "play":
        spec = sys.argv[2]
        opp = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else "mario:0"
        run(spec, opp, "play_" + re.sub(r"[^a-z0-9_]+", "_", spec.lower()), int(float(arg("--minutes", "10")) * 60), play=True)
    elif cmd == "list":
        for mod, spec in plan():
            print(f"{spec:20} {mod}")
    elif cmd == "all":
        only = arg("--only")
        try:
            with open(os.path.join(OUT, "results.json")) as f:
                results = json.load(f)
        except OSError:
            results = []
        for mod, spec in plan():
            if only and only.lower() not in (mod + " " + spec).lower():
                continue
            done = [r for r in results if r["spec"] == spec and r.get("mod") == mod]
            if done and ("--repeat-failed" not in sys.argv or done[-1]["verdict"] == "PASS") and "--again" not in sys.argv:
                continue
            r = run(spec)
            for k in range(1, int(arg("--runs", "2"))):   # (again while it passes: a crash can depend on which move gets cut short)
                if r["verdict"] != "PASS":
                    break
                again = run(spec, tag=r["tag"] + f"_run{k + 1}")
                if again["verdict"] != "PASS":
                    again["detail"] = f"run {k + 1} of {arg('--runs', '2')}: " + again["detail"]
                    r = again
                else:
                    r["seconds"] += again["seconds"]
            r["mod"] = mod
            results = [x for x in results if not (x["spec"] == spec and x.get("mod") == mod)] + [r]
            save(results)
        save(results)
        print(table(results))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
