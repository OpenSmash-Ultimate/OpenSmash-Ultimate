#!/usr/bin/env python3
"""What the OpenSmash launcher asks the WindowSmash mod manager to do.

    os_mods.py status
    os_mods.py enable|disable|remove <folder>   (":code_patches": ARCropolis' code patches, enable/disable only)
    os_mods.py switch <folder>|--all on|off   the game's settings menu: mods.json only, applied at the next start
    os_mods.py remove-id <gamebanana id>
    os_mods.py install <gamebanana id> [file id]
    os_mods.py apply
    os_mods.py vanilla
    os_mods.py fetch [--again]
    os_mods.py refresh                 a replaced plugin.nro: rebuild its DLL, leave the game
    os_mods.py build
    os_mods.py conflicts               files more than one turned-on mod changes (RESULT: conflicts)
    os_mods.py updates                 installed GameBanana mods with a newer file (RESULT: updates)
    os_mods.py export-list <file>      the installed mods, to share (mods.py export-list)
    os_mods.py import-list <file>      install a shared list's mods from GameBanana (RESULT: installed,
                                       already, missing)

status prints one JSON object. The others print "LOG ..." lines and a final "RESULT" line.

Every mod's switch is mods/mods.json's "enabled" (tools/mods.py's), which the game's settings menu
(runtime/hle_mod_switch.c, through "switch") changes too. ARCropolis' code patches (the costume-slot
fixes tools/recomp.py builds into the game both ways) have theirs there as well, "code_patches";
absent, they are on while a mod runs (dump/skyline/mods.txt lists one), so the game with no mod runs
its own code. status lists them as a mod named ":code_patches", for the launcher's switch.
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
CODE = ":code_patches"                      # (runtime/mod_switch.h MOD_SWITCH_CODE)
CODE_TITLE = "ARCropolis code patches"
CODE_ABOUT = ("The game-code fixes ARCropolis makes for costume slots a mod adds (c08 and up). Off: the game's own "
              "code. Counts when the game starts; players online need the same.")


def mods_running(tree):
    """How many mods the game runs: dump/skyline/mods.txt's (the last apply's) lines."""
    try:
        with open(os.path.join(tree.root, "dump", "skyline", "mods.txt"), encoding="utf-8") as f:
            return sum(1 for line in f if line.strip())
    except OSError:
        return 0


def code_patches_on(tree, state=None):
    """The code patches' switch: mods.json's code_patches, else on while a mod runs (mod_switch.h's rule)."""
    state = tree.load() if state is None else state
    setting = state.get("code_patches")
    return setting if isinstance(setting, bool) else mods_running(tree) > 0


def apply_pending(tree):
    """Ask the game for an apply when it next starts (runtime/hle_mod_switch.c mods_start)."""
    os.makedirs(tree.work, exist_ok=True)
    with open(os.path.join(tree.work, "apply_pending"), "w", encoding="utf-8") as f:
        f.write("the settings menu changed the mods\n")


def set_switches(tree, names, on):
    """mods.json's switches for these names (CODE: the code patches'); whether a mod's changed."""
    state = tree.load()
    changed = False
    if list(names) == ["--all"]:   # (the settings menu's "Turn every mod off")
        names = list(state["mods"])
    for name in names:
        if name == CODE:
            state["code_patches"] = bool(on)
            continue
        if name not in state["mods"]:
            raise mods.Error(f"no mod called {name}")
        changed |= bool(state["mods"][name].get("enabled")) != bool(on)
        state["mods"][name]["enabled"] = bool(on)
    tree.save(state)
    return changed


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


def _describe():
    sys.path.insert(0, os.path.join(HERE, "modmanager"))
    import describe
    return describe


def annotate_groups(data):
    """Fill group / part / pick / variant on listing mods (and return pick group keys)."""
    describe = _describe()
    keys = describe.groups(data["mods"])
    picks = describe.pick_groups(data["mods"], keys)
    titles = {}
    for m in data["mods"]:
        key = keys.get(m["name"])
        if not key:
            continue
        m["group"] = key
        m["part"] = describe.part_label(m["name"], m.get("title"))
        if key in picks:
            m["pick"] = True
            m["variant"] = describe.variant_label(m["name"], m.get("title"), m["part"])
        title = (m.get("title") if key.startswith("gb:") else None) or m["name"].split(" - ", 1)[0]
        titles.setdefault(key, {"title": title, "image": ""})
        titles[key]["image"] = titles[key]["image"] or m.get("image") or ""
    return keys, picks, titles


def normalize_picks(tree, prefer=None):
    """Only one part of a pick group (UI color pack) stays on. prefer: names just turned on."""
    describe = _describe()
    state = tree.load()
    listing = mods.listing(tree)
    keys = describe.groups(listing["mods"])
    picks = describe.pick_groups(listing["mods"], keys)
    if not picks:
        return False
    prefer = set(prefer or ())
    changed = False
    by = {}
    for m in listing["mods"]:
        k = keys.get(m["name"])
        if k in picks:
            by.setdefault(k, []).append(m["name"])
    for names in by.values():
        on = [n for n in names if state["mods"].get(n, {}).get("enabled")]
        if len(on) <= 1:
            continue
        keep = next((n for n in on if n in prefer), None) or on[-1]
        for n in on:
            if n != keep:
                state["mods"][n]["enabled"] = False
                changed = True
    if changed:
        tree.save(state)
    return changed


def status(tree):
    data = mods.listing(tree)
    cache = os.path.join(tree.root, "build", "work", "modthumbs")
    for m in data["mods"]:
        m["thumb"] = thumb_for(os.path.join(tree.mods, m["name"]), m["name"], cache)
        annotate(tree, m)
    keys, picks, titles = annotate_groups(data)
    for key, g in titles.items():
        g["thumb"] = url_thumb(g["image"], cache, key) or next(
            (m["thumb"] for m in data["mods"] if m.get("group") == key and m.get("thumb")), "")
    for m in data["mods"]:
        g = titles.get(m.get("group", ""))
        if g:
            m["group_title"] = g["title"]
            m["group_thumb"] = g["thumb"]
    data["needs_build"] = skyline.game_link_reason(tree.root) or ""
    # (Play without mods asks only for this one: 30 mods turned on but not built don't hold up the plain game)
    data["needs_build_vanilla"] = data["needs_build"] and (skyline.game_link_reason(tree.root, mods=False) or "")
    data["needs_refresh"] = skyline.plugin_refresh_reason(tree.root) or ""
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
    # Custom plugins the user dropped into skyline/plugins: not part of the standard list.
    known = {p["name"] for p in plugins}
    if os.path.isdir(folder):
        for name in sorted(os.listdir(folder)):
            if not name.lower().endswith(".nro") or name in known:
                continue
            plugins.append({
                "name": name,
                "version": "",
                "fetched": True,
                "status": "runs",
                "source": "custom",
            })
    data["plugins"] = plugins
    data["mods"].append({"name": CODE, "title": CODE_TITLE, "enabled": code_patches_on(tree), "files": 0, "about": CODE_ABOUT,
                         "online": "same", "code": "", "new_slots": False})
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
            changed = set_switches(tree, argv[2:], cmd == "enable")
            if cmd == "enable":
                changed = normalize_picks(tree, prefer=[n for n in argv[2:] if n != CODE]) or changed
            if changed or CODE not in argv[2:]:
                mods.apply(tree, log)
            else:
                log(f"{CODE_TITLE}: {'on' if cmd == 'enable' else 'off'} (counts when the game starts)")
            return result(True)
        if cmd == "switch":
            if len(argv) < 4 or argv[-1] not in ("on", "off"):
                raise mods.Error("switch <folder>... on|off")
            changed = set_switches(tree, argv[2:-1], argv[-1] == "on")
            if argv[-1] == "on":
                changed = normalize_picks(tree, prefer=[n for n in argv[2:-1] if n != CODE]) or changed
            if changed:
                apply_pending(tree)
            log(f"{', '.join(CODE_TITLE if n == CODE else n for n in argv[2:-1])}: {argv[-1]} (counts when the game starts)")
            return result(True)
        if cmd == "apply":
            normalize_picks(tree)
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
            # Color packs unpack to many folders: keep one on so they don't all fight over ui/.
            normalize_picks(tree, prefer=names[:1] if names else None)
            log("percent 92")
            mods.apply(tree, log)
            log("percent 100")
            log("installed: " + ", ".join(names))
            return result(True, names=names)
        if cmd == "remove":
            if len(argv) < 3:
                raise mods.Error("name the mod's folder")
            if CODE in argv[2:]:
                raise mods.Error(f"{CODE_TITLE} are part of the game: turn them off instead")
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
        if cmd == "refresh":
            skyline.sync(tree.root, log, skyline.build_jobs())
            skyline.cache_mod_plugins(tree.root, skyline.build_jobs(), log)
            return result(True)
        if cmd == "build":
            if recomp_busy():
                raise mods.Error("plugins are already being built into the game")
            fetch_plugins(tree, False, log)
            code = skyline.build(tree.root, skyline.build_jobs(), log)
            if code:
                raise mods.Error("the plugin build stopped")
            return result(True)
        if cmd == "conflicts":   # (TRLLZR's mod manager ideas, NOTICE.md; mods.py does the work)
            found = mods.conflicts(tree)
            log(f"{len(found)} file(s) changed by more than one turned-on mod")
            return result(True, conflicts=found)
        if cmd == "updates":
            found = mods.updates(tree)
            state = tree.load()
            for u in found:   # (the page to install the newer file from: install <gamebanana> <file_id>)
                u["gamebanana"] = str(state["mods"].get(u["name"], {}).get("gamebanana", ""))
            log(f"{len(found)} mod(s) with a newer file on GameBanana")
            return result(True, updates=found)
        if cmd in ("export-list", "import-list"):
            if len(argv) < 3:
                raise mods.Error("name the list file")
            if cmd == "export-list":
                written = mods.export_list(tree, argv[2])
                log(f"{len(written)} mods written to {argv[2]}")
                return result(True, count=len(written))
            got = mods.import_list(tree, argv[2], log)
            normalize_picks(tree)
            mods.apply(tree, log)
            return result(True, **got)
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
