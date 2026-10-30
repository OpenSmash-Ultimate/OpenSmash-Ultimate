#!/usr/bin/env python3
"""Install this tree's plugin work into a WindowSmash tree (the playable one, ~/WindowSmash): the code
changed since the tree was copied from it (git: the "baseline" commit), the standard plugins
(skyline/plugins), and the mods given; then the mods applied and the game built for the plugins.

  install_into.py <tree> [--mod <folder in mods/>]... [--go]

Without --go it only lists what it would do. With --go:
  - every file it replaces is kept next to it as <file>.before-realplugins (and mods/mods.json too);
  - the code files and skyline/plugins/*.nro are copied over;
  - each --mod folder is copied into <tree>/mods (installed, turned on; other mods stay as they are);
  - the hook and patch lists (dump/skyline/hooks.txt, patches.txt: 13.0.5 addresses the plugins
    asked for here) are merged into the tree's, so the first build already has them;
  - mods.py apply, then skyline.py build --jobs 2 twice (the second bakes in what the first boot
    recorded), holding /tmp/windowsmash-test.lock and never while the tree's game is running.
Undo: copy each *.before-realplugins back and delete <tree>/skyline/plugins."""
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SKIP = {".gitignore", ".DS_Store"}
SUFFIX = ".before-realplugins"


def changed_code():
    r = subprocess.run(["git", "-C", HERE, "log", "--reverse", "--format=%H", "--max-parents=0"],
                       capture_output=True, text=True, check=True)
    base = r.stdout.split()[0]
    names = subprocess.run(["git", "-C", HERE, "diff", "--name-only", base, "HEAD"],
                           capture_output=True, text=True, check=True).stdout.split("\n")
    out = []
    for n in names:
        if not n or n in SKIP or n.startswith(("skyline/", "bench/")) or not os.path.isfile(os.path.join(HERE, n)):
            continue
        out.append(n)
    return out


def game_running(tree):
    r = subprocess.run(["pgrep", "-fl", "MacOS/ssbu"], capture_output=True, text=True)
    return [line for line in r.stdout.splitlines() if tree in line or "WindowSmash.app" in line]


def merge_lines(src, dst):
    have = open(dst, encoding="utf-8").read().split("\n") if os.path.exists(dst) else []
    new = [line for line in open(src, encoding="utf-8").read().split("\n") if line and line not in have]
    return have, new


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    tree = os.path.abspath(os.path.expanduser(args[0]))
    go = "--go" in args
    mods = [args[i + 1] for i, a in enumerate(args) if a == "--mod"]
    plan = []
    for n in changed_code():
        plan.append(("code", os.path.join(HERE, n), os.path.join(tree, n)))
    for n in sorted(os.listdir(os.path.join(HERE, "skyline", "plugins"))):
        if n.endswith(".nro"):
            plan.append(("plugin", os.path.join(HERE, "skyline", "plugins", n), os.path.join(tree, "skyline", "plugins", n)))
    for m in mods:
        src = os.path.join(HERE, "mods", m)
        if not os.path.isdir(src):
            raise SystemExit(f"no mod folder {src}")
        plan.append(("mod", src, os.path.join(tree, "mods", m)))
    lists = []
    for name in ("hooks.txt", "patches.txt"):
        have, new = merge_lines(os.path.join(HERE, "dump", "skyline", name), os.path.join(tree, "dump", "skyline", name))
        lists.append((name, have, new))
    for kind, src, dst in plan:
        state = "new" if not os.path.exists(dst) else "replaces"
        print(f"{kind:7} {state:9} {os.path.relpath(dst, tree)}")
    for name, _have, new in lists:
        print(f"list    adds {len(new):4} line(s) to dump/skyline/{name}")
    print("then:   mods.py apply; skyline.py build --jobs 2 (twice)")
    if not go:
        print("(a dry run: --go does it)")
        return
    if game_running(tree):
        raise SystemExit("the game is running: close it first")
    state_path = os.path.join(tree, "mods", "mods.json")
    shutil.copy2(state_path, state_path + SUFFIX)
    with open(state_path, encoding="utf-8") as f:
        state = json.load(f)
    src_state = json.load(open(os.path.join(HERE, "mods", "mods.json"), encoding="utf-8"))
    for kind, src, dst in plan:
        if os.path.exists(dst) and not os.path.exists(dst + SUFFIX):
            os.replace(dst, dst + SUFFIX) if kind == "mod" else shutil.copy2(dst, dst + SUFFIX)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if kind == "mod":
            shutil.copytree(src, dst)
            name = os.path.basename(dst)
            state["mods"][name] = dict(src_state["mods"].get(name, {}), enabled=True)
            if name not in state["order"]:
                state["order"].append(name)
        else:
            shutil.copy2(src, dst)
    with open(state_path, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=1)
    for name, have, new in lists:
        path = os.path.join(tree, "dump", "skyline", name)
        if os.path.exists(path):
            shutil.copy2(path, path + SUFFIX)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join([h for h in have if h] + new) + "\n")
    locked = [sys.executable, os.path.join(HERE, "tools", "dev", "locked.py")]
    py = sys.executable
    subprocess.run([py, os.path.join(tree, "tools", "mods.py"), "--tree", tree, "apply"], check=True)
    for _ in range(2):
        subprocess.run(locked + [py, os.path.join(tree, "tools", "skyline.py"), "build", "--tree", tree, "--jobs", "2"], check=True)
    print("installed")


if __name__ == "__main__":
    main()
