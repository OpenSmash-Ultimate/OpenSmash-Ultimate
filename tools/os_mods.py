#!/usr/bin/env python3
"""What the OpenSmash launcher asks the WindowSmash mod manager to do.

    os_mods.py status
    os_mods.py enable|disable|remove <folder>
    os_mods.py remove-id <gamebanana id>
    os_mods.py install <gamebanana id> [file id]
    os_mods.py apply
    os_mods.py vanilla
    os_mods.py fetch [--again]
    os_mods.py build

status prints one JSON object. The others print "LOG ..." lines and a final "RESULT" line.
"""
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mods  # noqa: E402
import skyline  # noqa: E402
import std_plugins  # noqa: E402

ROOT = os.path.dirname(HERE)


def log(text):
    print("LOG " + str(text), flush=True)


def result(ok, **extra):
    print("RESULT " + json.dumps({"ok": ok, **extra}), flush=True)
    return 0 if ok else 1


def thumb_for(folder, name, cache):
    src = ""
    for n in ("preview.webp", "preview.png", "preview.jpg", "preview.jpeg"):
        p = os.path.join(folder, n)
        if os.path.isfile(p):
            src = p
            break
    if not src:
        return ""
    dest = os.path.join(cache, mods.safe_name(name) + ".png")
    try:
        if not os.path.exists(dest) or os.path.getmtime(dest) < os.path.getmtime(src):
            from PIL import Image
            im = Image.open(src).convert("RGBA")
            im.thumbnail((128, 128))
            os.makedirs(cache, exist_ok=True)
            im.save(dest)
    except Exception:
        return ""
    return dest


def url_thumb(url, cache, name):
    if not url:
        return ""
    dest = os.path.join(cache, mods.safe_name(name) + ".png")
    try:
        if not os.path.exists(dest) or os.path.getsize(dest) == 0:
            import urllib.request
            from PIL import Image
            import io
            req = urllib.request.Request(url, headers={"User-Agent": "OpenSmash"})
            with urllib.request.urlopen(req, timeout=20) as r:
                raw = r.read()
            im = Image.open(io.BytesIO(raw)).convert("RGBA")
            im.thumbnail((128, 128))
            os.makedirs(cache, exist_ok=True)
            im.save(dest)
    except Exception:
        return ""
    return dest


def annotate(tree, m):
    """The row details WindowSmash Mods shows: what the mod is, online safety, new costume slots,
    and whether its code plugin is compiled into the game."""
    folder = os.path.join(tree.mods, m["name"])
    sys.path.insert(0, os.path.join(HERE, "modmanager"))
    import describe
    import packs
    try:
        m["about"] = describe.describe(folder) or ""
    except Exception:
        m["about"] = ""
    try:
        m["online"] = (packs.online_safety(folder) or {}).get("level") or ""
    except Exception:
        m["online"] = ""
    m["new_slots"] = False
    cfg = os.path.join(folder, "config.json")
    if os.path.exists(cfg):
        try:
            with open(cfg, encoding="utf-8-sig") as f:
                c = json.load(f)
            m["new_slots"] = bool(c.get("new-dir-infos") or c.get("new-dir-infos-base"))
        except (OSError, ValueError):
            pass
    m["code"] = ""
    rels = mods.plugins(folder)
    if rels:
        try:
            plugs = skyline.plugin_state(tree.root, m["name"], rels)
            m["code"] = "waiting" if any(not p.get("built") for p in plugs) else "built"
        except Exception:
            m["code"] = "waiting"


def status(tree):
    sys.path.insert(0, os.path.join(HERE, "modmanager"))
    import describe
    data = mods.listing(tree)
    cache = os.path.join(tree.root, "build", "work", "modthumbs")
    for m in data["mods"]:
        m["thumb"] = thumb_for(os.path.join(tree.mods, m["name"]), m["name"], cache)
        annotate(tree, m)
    keys = describe.groups(data["mods"])
    titles = {}
    for m in data["mods"]:
        key = keys.get(m["name"])
        if not key:
            continue
        m["group"] = key
        m["part"] = describe.part_label(m["name"], m.get("title"))
        title = (m.get("title") if key.startswith("gb:") else None) or m["name"].split(" - ", 1)[0]
        titles.setdefault(key, {"title": title, "image": ""})
        titles[key]["image"] = titles[key]["image"] or m.get("image") or ""
    for key, g in titles.items():
        g["thumb"] = url_thumb(g["image"], cache, key) or next(
            (m["thumb"] for m in data["mods"] if m.get("group") == key and m.get("thumb")), "")
    for m in data["mods"]:
        g = titles.get(m.get("group", ""))
        if g:
            m["group_title"] = g["title"]
            m["group_thumb"] = g["thumb"]
    data["needs_build"] = skyline.needs_build(tree.root) or ""
    plugins = []
    folder = std_plugins.folder(tree.root)
    for name, (version, _url, _member, _sha, source, run) in std_plugins.PLUGINS.items():
        plugins.append({
            "name": name,
            "version": version,
            "fetched": os.path.exists(os.path.join(folder, name)),
            "status": run,
            "source": source,
        })
    data["plugins"] = plugins
    print(json.dumps(data), flush=True)
    return 0


def recomp_busy():
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command",
             "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Select-Object -ExpandProperty CommandLine"],
            text=True, timeout=20, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.SubprocessError):
        return False
    return "recomp.py" in out and " all " in out


def fetch_plugins(tree, every, log):
    for name, plugin in std_plugins.PLUGINS.items():
        if plugin[5] != "runs":
            continue
        have = os.path.exists(os.path.join(std_plugins.folder(tree.root), name))
        if every or not have:
            std_plugins.fetch(tree.root, name, log)


def main(argv):
    tree = mods.Tree(ROOT)
    cmd = argv[1] if len(argv) > 1 else "status"
    try:
        if cmd == "status":
            return status(tree)
        if cmd in ("enable", "disable"):
            if len(argv) < 3:
                raise mods.Error("name the mod's folder")
            state = tree.load()
            for name in argv[2:]:
                if name not in state["mods"]:
                    raise mods.Error(f"no mod called {name}")
                state["mods"][name]["enabled"] = cmd == "enable"
            tree.save(state)
            mods.apply(tree, log)
            return result(True)
        if cmd == "apply":
            mods.apply(tree, log)
            return result(True)
        if cmd == "vanilla":
            mods.play_vanilla(tree, log)
            return result(True)
        if cmd == "install":
            if len(argv) < 3:
                raise mods.Error("name the GameBanana mod")
            file_id = argv[3] if len(argv) > 3 and argv[3] else None
            last = [0.0, -1]

            def progress(done, total):
                if total:
                    pct = min(85, int(done * 85 / total))
                else:
                    # No length from the server: still move, one step per 2 MB.
                    pct = min(84, int(done / (2 << 20)))
                now = time.time()
                if pct == last[1]:
                    return
                if pct < 85 and now - last[0] < 0.3:
                    return
                last[0], last[1] = now, pct
                log(f"percent {pct}")

            names = mods.gamebanana_install(tree, argv[2], file_id, progress)
            log("percent 92")
            mods.apply(tree, log)
            log("percent 100")
            log("installed: " + ", ".join(names))
            return result(True, names=names)
        if cmd == "remove":
            if len(argv) < 3:
                raise mods.Error("name the mod's folder")
            for name in argv[2:]:
                mods.remove_folder(os.path.join(tree.mods, name))
            tree.save(tree.load())
            mods.apply(tree, log)
            return result(True)
        if cmd == "remove-id":
            if len(argv) < 3:
                raise mods.Error("name the GameBanana id")
            state = tree.load()
            want = str(argv[2])
            name = next((n for n, m in state["mods"].items() if str(m.get("gamebanana", "")) == want), None)
            if not name:
                raise mods.Error("that mod is not installed")
            return main([argv[0], "remove", name])
        if cmd == "fetch":
            fetch_plugins(tree, "--again" in argv, log)
            return result(True)
        if cmd == "build":
            if recomp_busy():
                raise mods.Error("plugins are already being built into the game")
            fetch_plugins(tree, False, log)
            code = skyline.build(tree.root, skyline.build_jobs(), log)
            if code:
                raise mods.Error("the plugin build stopped")
            return result(True)
        raise mods.Error(f"unknown command {cmd}")
    except mods.Error as e:
        if cmd == "status":
            print(json.dumps({"error": str(e)}), flush=True)
            return 1
        return result(False, error=str(e))
    except SystemExit as e:
        return result(False, error=str(e))
    except (OSError, Exception) as e:
        if cmd == "status":
            print(json.dumps({"error": str(e)}), flush=True)
            return 1
        return result(False, error=str(e))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
