#!/usr/bin/env python3
"""Skyline plugins: the code moveset and new-character mods ship (plugin.nro), made part of the
game (docs/SKYLINE.md).

Usage:
  skyline.py sync  [--tree <dir>]               the turned-on mods' plugins: recompiled, listed to load
  skyline.py build [--tree <dir>] [--jobs N]    translate each mod plugin once; link the game only when it must
  skyline.py status [--tree <dir>]              what is listed, and whether the game has it built in
  skyline.py probe [--tree <dir>]               start the game once for the plugins' select-screen entries; apply the mods again if they changed
  skyline.py uptodate [--tree <dir>]            exit 0 when the game needs no build (the installer skips it), else 1, saying why

sync (tools/mods.py apply runs it): each turned-on mod's plugin goes into dump/nro/ (named and
placed by tools/recomp.py, as a game NRO the runtime harvested would be), its C is generated
(build/nro/<name>/) when it is new, and dump/skyline/plugins.txt lists the plugins to load, in
the mods' order. The plugins the runtime provides itself (Smashline 2, lib_paramconfig,
ARCropolis, nro_hook, the CSK Collection) are left out wherever a mod ships them. A plugin that
is listed but not compiled into the game yet is left out at start-up (the log says so): build
compiles it - the runtime and the plugins only, a minute or two, not the whole game.

Hooks: plugins hook game functions with Skyline's A64HookFunction; the runtime does that
through the dispatch tables, which reach every indirect call. A function the game calls
directly as well needs it built in: the runtime lists every hook target it is asked for in
dump/skyline/hooks.txt, and the targets the generated C calls directly are made hookable
(tools/recomp.py HOOKABLE) by the next build, which regenerates the C of the modules they are in
(that takes long for main: tens of minutes)."""
import filecmp
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMPILER = ("recomp.py", "lift.py", "lift_simd.py", "regcache.py", "plugin_eh.py", "eh.py")   # (what makes a plugin's C)
sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)

# Game functions runtime/hle_skyline.c hooks itself that the game also calls directly (the CSK
# Collection's select-screen redirect: the fighter set-up and the portrait paths)
NATIVE_HOOKS = ("main 66dcf0", "main 3238a70", "main 64b730", "main 6079d0") + tuple(   # (+ Smashline's cloned-weapon swaps, inline)
    f"main {o:x} inline" for o in (0x33b6bd8, 0x33ad528, 0x33af3e8, 0x33ae488, 0x33ac5f0, 0x17e0a0c, 0x33ad42c, 0x33af2ec,
                                   0x33ae38c, 0x33ac504, 0x17e094c, 0x33b6de0, 0x33b6fa0, 0x33b72c4, 0x33ad43c, 0x33af2fc,
                                   0x33ae39c, 0x33ac514))
# (provided natively by runtime/hle_skyline.c, or linked into the plugins that use them)
NATIVE = {"libsmashline_plugin.nro", "libsmashline_hook.nro", "libparam_config.nro", "libarcropolis.nro",
          "libnro_hook.nro", "libthe_csk_collection.nro", "libskyline.nro"}


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


class Paths:
    def __init__(self, tree):
        self.tree = os.path.abspath(tree)
        self.nro = os.path.join(self.tree, "dump", "nro")
        self.modules = os.path.join(self.tree, "dump", "modules")
        self.skyline = os.path.join(self.tree, "dump", "skyline")
        self.list = os.path.join(self.skyline, "plugins.txt")
        self.record = os.path.join(self.skyline, "plugins.json")   # build id -> module name, mod, source
        self.hooks = os.path.join(self.skyline, "hooks.txt")
        self.build = os.path.join(self.tree, "build", "nro")


def load_record(p):
    try:
        with open(p.record, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


STANDARD = ("skyline", "plugins")   # (<tree>/skyline/plugins: the community plugins WindowSmash ships, run as they are)


def standard(tree):
    """[(label, plugin file)] of the standard plugins (<tree>/skyline/plugins/*.nro), by name."""
    folder = os.path.join(tree, *STANDARD)
    names = sorted(n for n in os.listdir(folder) if n.lower().endswith(".nro")) if os.path.isdir(folder) else []
    return [("(standard) " + n, os.path.join(folder, n)) for n in names]


def turned_on(tree):
    """[(mod name, plugin file)]: the standard plugins, then the turned-on mods' in their order. A
    mod's own copy of a standard plugin (or of one the runtime provides) is left out."""
    import mods
    t = mods.Tree(tree)
    state = t.load()
    out = standard(tree)
    have = {os.path.basename(f).lower() for _n, f in out}
    for name in state["order"]:
        if not state["mods"].get(name, {}).get("enabled"):
            continue
        for rel in mods.plugins(os.path.join(t.mods, name)):
            if os.path.basename(rel).lower() in NATIVE | have:
                continue
            out.append((name, os.path.join(t.mods, name, rel)))
    return out


def recomp(p, *args):
    cmd = [sys.executable, os.path.join(HERE, "recomp.py"), *args, "--tree", p.tree]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"recomp.py {' '.join(args)} failed:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    return r.stdout


def recomp_stream(p, log, *args):
    """recomp.py with its progress bar forwarded as it runs (a captured run only keeps the last line,
    so generating a module's C looked stuck)."""
    cmd = [sys.executable, os.path.join(HERE, "recomp.py"), *args, "--tree", p.tree]
    if run_logged(cmd, log):
        raise SystemExit(f"recomp.py {' '.join(args)} failed")


def mod_slug(mod):
    """The character a mod folder names: "(Moveset) Kratos" -> kratos."""
    words = re.findall(r"[A-Za-z0-9]+", mod)
    name = (words[-1] if words else "plugin").lower()
    return name if name[0].isalpha() else "n_" + name


def template_module(name):
    return name == "smashline_test" or name.startswith("smashline_test_")


def write_module_name(path, name):
    """Smashline identifies a plugin by the length-prefixed name in front of MOD0, not by the
    file name. That slot ends at MOD0, so a longer name stops there."""
    from nso import Module
    m = Module(path)
    start = m.rodata.mem + 8
    mod0 = bytes(m.image).find(b"MOD0", start)
    if mod0 < 0:
        return m.module_name()
    raw = name.encode("ascii", "ignore")[:mod0 - start]
    if not raw:
        return m.module_name()
    blob = struct.pack("<I", len(raw)) + raw + b"\0" * (mod0 - start - len(raw))
    with open(path, "r+b") as f:
        f.seek(m.rodata.mem + 4)
        f.write(blob)
    return raw.decode("ascii")


def stamp_identity(path, prefer, p):
    """A Skyline template leaves every moveset named smashline_test. Smashline then treats the
    second one as the first, and its character never reaches the select screen. The name becomes
    the character the mod folder names."""
    from nso import Module
    m = Module(path)
    if not prefer or not template_module(m.module_name()):
        return
    taken = set()
    bases = os.path.join(p.nro, "bases.txt")
    if os.path.isfile(bases):
        for line in open(bases, encoding="utf-8"):
            row = line.split()
            if len(row) == 4 and row[3] != m.build_id:
                taken.add(row[0])
    name = prefer
    while name in taken:
        name += "_"
    write_module_name(path, name)


def rename_template(p, build_id, current, prefer):
    """Smashline's sample project is named smashline_test, so a second moveset built from it
    would be smashline_test_ and the two would share one name. Name each after its character,
    in the file's own identity and in the record."""
    if not prefer or not template_module(current):
        return current
    bases = os.path.join(p.nro, "bases.txt")
    rows = [l.split() for l in open(bases, encoding="utf-8") if l.strip()]
    taken = {r[0] for r in rows if len(r) == 4 and r[3] != build_id}
    name = prefer
    while name in taken:
        name += "_"
    src, dst = os.path.join(p.nro, current + ".nro"), os.path.join(p.nro, name + ".nro")
    if name != current and os.path.isfile(src):
        if os.path.isfile(dst):
            os.remove(dst)
        os.replace(src, dst)
    if os.path.isfile(dst):
        write_module_name(dst, name)
    if name != current:
        for r in rows:
            if len(r) == 4 and r[3] == build_id:
                r[0] = name
        with open(bases, "w", encoding="utf-8", newline="\n") as f:
            f.writelines(" ".join(r) + "\n" for r in rows)
    return name


def module_of(p, source, record, prefer=None):
    """The module name tools/recomp.py gives the plugin: placed in dump/nro/ if it is new."""
    import recomp as rc
    from nso import Module
    stamp_identity(source, prefer, p)
    m = Module(source)
    placed = os.path.join(p.nro, record[m.build_id]["module"] + ".nro") if m.build_id in record else None
    if placed and os.path.exists(placed):
        if not filecmp.cmp(source, placed, shallow=False):   # (same build, other bytes: a ported plugin, tools/port_plugin.py)
            shutil.copyfile(source, placed)
        return m.build_id, rename_template(p, m.build_id, record[m.build_id]["module"], prefer)
    rc.NRO_DIR = p.nro
    shutil.copyfile(source, os.path.join(p.nro, m.build_id[:16] + ".nro"))
    rc.nro_modules()   # (names it after the module, gives it an address: bases.txt)
    for row in open(os.path.join(p.nro, "bases.txt"), encoding="utf-8").read().splitlines():
        parts = row.split()
        if len(parts) == 4 and parts[3] == m.build_id:
            return m.build_id, rename_template(p, m.build_id, parts[0], prefer)
    raise SystemExit(f"{source}: recomp.py did not place it")


def c_stale(table):
    """Whether a plugin's C (its g_module.c) is missing or older than the recompiler: a fix made in it
    since reaches the plugin only when its C is made again."""
    made_by = max(os.path.getmtime(os.path.join(HERE, n)) for n in RECOMPILER if os.path.exists(os.path.join(HERE, n)))
    return not os.path.exists(table) or os.path.getmtime(table) < made_by


def build_jobs():
    """How many files to write or compile at once. The launcher used to pin this at 3, so a 22-thread
    machine spent the long part of a plugin build (rewriting a module's C, then compiling it) mostly
    idle. Leave two cores for the launcher, and stop at 12 so the compile of the biggest files still
    fits in memory."""
    cpus = os.cpu_count() or 4
    return max(4, min(cpus - 2, 12))


def generate(p, name, log, own_exports=False, jobs=1):
    """The plugin's image and C, if missing or older than the plugin. own_exports: a standard plugin,
    whose exports stay its own (not the runtime's native versions of the same names)."""
    nro = os.path.join(p.nro, name + ".nro")
    img = os.path.join(p.modules, name + ".reloc.bin")
    out = os.path.join(p.build, name)
    table = os.path.join(out, "g_module.c")
    fresh_c = c_stale(table) or os.path.getmtime(table) < os.path.getmtime(nro)
    # (the image goes with the C: its size, and the unwind tables recomp.py adds to a plugin's)
    if fresh_c or not os.path.exists(img) or os.path.getmtime(img) < os.path.getmtime(nro):
        recomp(p, "image", nro, "--out", img)
    if fresh_c:
        log(f"writing {name}'s C — 0%, estimating the time left")
        if own_exports:
            os.environ["SSBU_OWN_EXPORTS"] = "1"
        try:
            recomp_stream(p, log, "build", nro, "--out", out, "--jobs", str(max(1, jobs)))
        finally:
            os.environ.pop("SSBU_OWN_EXPORTS", None)
        os.utime(table)   # (made now: the same C as before is left as it was, dated then - c_stale goes by this)


SEED = os.path.join(HERE, "skyline_seed")   # (tools/skyline_seed: hooks.txt, patches.txt the standard plugins need, 13.0.5)


def seed_lists(p):
    """The standard plugins' hook points and code patches (what their probe boots and a first match
    would record: tools/skyline_seed), added to dump/skyline's lists, so the first build has them."""
    if not standard(p.tree):
        return
    for name in ("hooks.txt", "patches.txt"):
        seed = os.path.join(SEED, name)
        if not os.path.exists(seed):
            continue
        dest = os.path.join(p.skyline, name)
        have = open(dest, encoding="utf-8").read().splitlines() if os.path.exists(dest) else []
        new = [l for l in open(seed, encoding="utf-8").read().splitlines() if l.strip() and l not in have]
        if new:
            with open(dest, "a", encoding="utf-8", newline="\n") as f:
                f.writelines(l + "\n" for l in new)


def sync(tree, log=print, jobs=1):
    """Recompile the turned-on mods' plugins and list them to load; returns the list."""
    p = Paths(tree)
    os.makedirs(p.skyline, exist_ok=True)
    seed_lists(p)
    have = open(p.hooks, encoding="utf-8").read().split("\n") if os.path.exists(p.hooks) else []
    missing = [h for h in NATIVE_HOOKS if h not in have]
    if missing:
        with open(p.hooks, "a", encoding="utf-8", newline="\n") as f:
            f.writelines(h + "\n" for h in missing)
    record = load_record(p)
    names = []
    skip = set()   # (dump/skyline/skip.txt: plugins left out, by module name or mod-relative file - one that does not run yet)
    if os.path.exists(os.path.join(p.skyline, "skip.txt")):
        skip = {l.split("#")[0].strip() for l in open(os.path.join(p.skyline, "skip.txt"), encoding="utf-8")} - {""}
    for mod, source in turned_on(tree):
        prefer = None if mod.startswith("(standard) ") else mod_slug(mod)
        build_id, name = module_of(p, source, record, prefer)
        if name in skip or os.path.relpath(source, os.path.join(tree, "mods")).replace(os.sep, "/") in skip:
            log(f"{mod}: plugin {os.path.relpath(source, os.path.join(tree, 'mods', mod))} left out (dump/skyline/skip.txt)")
            continue
        record[build_id] = {"module": name, "mod": mod, "file": os.path.relpath(source, tree)}
        generate(p, name, log, mod.startswith("(standard) "), jobs)
        if name not in names:
            names.append(name)
            log(f"{mod}: plugin {os.path.basename(source)} -> {name}")
    with open(p.record, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=1)
    import mods
    t = mods.Tree(tree)
    state = t.load()
    with open(os.path.join(p.skyline, "mods.txt"), "w", encoding="utf-8", newline="\n") as f:   # (the runtime's mods: mount)
        written = set()
        for n in state["order"]:   # (each data root of a mod: a pack keeps its mods in subfolders)
            if state["mods"].get(n, {}).get("enabled"):
                folder = os.path.join(t.mods, n)
                roots = []
                for dp, dirs, _f in os.walk(folder):
                    if any(d.lower() in mods.ROOTS for d in dirs):
                        roots.append(dp)
                    dirs[:] = [d for d in dirs if d.lower() not in mods.ROOTS]   # (not into a root's own data)
                roots = roots or [folder]
                for r in roots:   # (once each: a pack and its parts can name the same folder)
                    rel = os.path.relpath(r, t.mods).replace(os.sep, "/")
                    if rel not in written:
                        written.add(rel)
                        f.write(rel + "\n")
        if os.path.isdir(os.path.join(t.mods, mods.CHARACTERS)):   # (the plugins' new characters: last, mods.characters_mod)
            f.write(mods.CHARACTERS + "\n")
    # With the real ARCropolis among the standard plugins, it loads the mods' plugins itself (its
    # chainloader, from each mod's folder, as on the console): only the standard ones are listed
    if any(m == "(standard) libarcropolis.nro" for m, _s in turned_on(tree)):
        std = {module_of(p, s, record)[1] for m, s in turned_on(tree) if m.startswith("(standard) ")}
        names = [n for n in names if n in std]
    text = "".join(f"{n}\n" for n in names)
    old = open(p.list, encoding="utf-8").read() if os.path.exists(p.list) else ""
    if text != old:
        with open(p.list, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    missing = [n for n in names if not built_in(tree, n)]
    if missing:
        log("not in the game yet (build them: Mods app > Build plugins into the game, or tools/skyline.py build): " + ", ".join(missing))
    return names


def built_in(tree, name):
    """Whether the game's program has the plugin compiled in (its nro_table names it), from C the
    current recompiler made (an older one's can carry bugs fixed since), and compiled since (Apply
    makes a plugin's C again; only Build plugins compiles it into the program)."""
    c = os.path.join(tree, "build", "nro", name, "g_module.c")
    if c_stale(c):
        return False
    programs = [os.path.join(tree, "build-mac", "WindowSmash.app", "Contents", "MacOS", "ssbu"), os.path.join(tree, "build", "ssbu.exe")]
    built = max((os.path.getmtime(x) for x in programs if os.path.exists(x)), default=0)
    if os.path.getmtime(c) > built:
        return False
    for table in (os.path.join(tree, "build-mac", "gen", "nro_table.c"), os.path.join(tree, "build", "rt", "nro_table.c")):
        if os.path.exists(table) and f"rt_module_{name}," in open(table, encoding="utf-8").read():
            return True
    return False


def nro_build_id(path):
    """The build id in an NRO or NSO header. The mod list only needs this, not the whole module."""
    with open(path, "rb") as f:
        head = f.read(0x60)
    if len(head) < 0x60 or (head[0x10:0x14] != b"NRO0" and head[:4] != b"NSO0"):
        raise ValueError(path)
    return head[0x40:0x60].hex().rstrip("0").upper() or "0"


def plugin_state(tree, mod, rels):
    """[{"file", "module", "built"}] of a mod's code plugins (`rels`: paths in its folder) the game
    runs: built is whether the game's program has it compiled in (None: never recompiled - the mod
    was not turned on and applied yet). The plugins WindowSmash provides itself are left out."""
    record = load_record(Paths(tree))
    out = []
    for rel in rels:
        if os.path.basename(rel).lower() in NATIVE:
            continue
        try:
            entry = record.get(nro_build_id(os.path.join(tree, "mods", mod, rel)))
        except Exception:   # (not an NRO the recompiler reads)
            entry = None
        name = entry and entry["module"]
        out.append({"file": rel, "module": name, "built": dll_current(tree, name) if name else None})
    return out


def hook_targets(p):
    """{module: {offset}} of the hook targets the runtime listed (dump/skyline/hooks.txt)."""
    out = {}
    if os.path.exists(p.hooks):
        for line in open(p.hooks, encoding="utf-8"):
            m = re.match(r"(\w+)\s+([0-9a-f]+)$", line.strip())
            if m:
                out.setdefault(m.group(1), set()).add(int(m.group(2), 16))
    return out


def inline_targets(p):
    """{module: {offset}} of the inline hook points the runtime listed ("<module> <offset> inline")."""
    out = {}
    if os.path.exists(p.hooks):
        for line in open(p.hooks, encoding="utf-8"):
            m = re.match(r"(\w+)\s+([0-9a-f]+)\s+inline$", line.strip())
            if m:
                out.setdefault(m.group(1), set()).add(int(m.group(2), 16))
    return out


def patch_lines(p):
    """{module: text} of the plugins' code patches (dump/skyline/patches.txt), as recomp.py records
    the ones a module's C was generated with (<build dir>/plugin_patches.txt)."""
    out = {}
    path = os.path.join(p.skyline, "patches.txt")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            parts = line.split()
            if len(parts) == 4:
                out[parts[0]] = out.get(parts[0], "") + " ".join(parts[1:]) + "\n"
    return out


def _hook_sources(tree):
    """(module, folder of its generated C) for every module with an inline hook or a code patch."""
    p = Paths(tree)
    inline, patches = inline_targets(p), patch_lines(p)
    found = []
    for mod in sorted(set(inline) | set(patches)):
        table = os.path.join(tree, "build", mod, "g_module.c") if mod in ("main", "subsdk0", "sdk") else \
            os.path.join(tree, "build", "nro", mod, "g_module.c")
        found.append((mod, os.path.dirname(table), inline, patches))
    return found


def _hooks_sig(tree):
    """Changes when the hook lists or the generated C that carries them change."""
    p = Paths(tree)
    parts = []
    for path in (p.hooks, os.path.join(p.skyline, "patches.txt")):
        if os.path.exists(path):
            st = os.stat(path)
            parts.append(f"{os.path.basename(path)}:{st.st_mtime_ns}:{st.st_size}")
        else:
            parts.append(os.path.basename(path) + ":missing")
    for mod, folder, _inline, _patches in _hook_sources(tree):
        newest = 0
        if os.path.isdir(folder):
            for n in os.listdir(folder):
                if n.startswith("g") and n.endswith(".c"):
                    newest = max(newest, os.stat(os.path.join(folder, n)).st_mtime_ns)
        rec = os.path.join(folder, "plugin_patches.txt")
        if os.path.exists(rec):
            newest = max(newest, os.stat(rec).st_mtime_ns)
        parts.append(f"{mod}:{newest}")
    return "|".join(parts)


def stale_hooks(tree):
    """The game modules whose generated C lacks an inline hook or a code patch the runtime listed.
    A function hook (A64HookFunction) is patched into that function when the game starts, so it
    is not a reason to rebuild the program. The answer is cached: reading every generated file is
    what made the mods page wait."""
    cache = os.path.join(tree, "build", "work", "stale_hooks.json")
    sig = _hooks_sig(tree)
    try:
        with open(cache, encoding="utf-8") as f:
            saved = json.load(f)
        if saved.get("sig") == sig and isinstance(saved.get("mods"), list):
            return saved["mods"]
    except (OSError, ValueError):
        pass
    out = _stale_hooks(tree)
    try:
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        with open(cache, "w", encoding="utf-8") as f:
            json.dump({"sig": sig, "mods": out}, f)
    except OSError:
        pass
    return out


def _stale_hooks(tree):
    import recomp as rc
    rc.TREE = tree
    out = []
    for mod, folder, inline, patches in _hook_sources(tree):
        table = os.path.join(folder, "g_module.c")
        base = rc.MODULES.get(mod) or rc.nro_modules().get(mod) if os.path.exists(table) else None
        if not base:
            continue
        rec = os.path.join(folder, "plugin_patches.txt")
        if patches.get(mod, "") != (open(rec, encoding="utf-8").read() if os.path.exists(rec) else ""):
            out.append(mod)
            continue
        if inline.get(mod):   # (the C files: is each point's rt_inline_hook in one?)
            have = set()
            for n in os.listdir(folder):
                if n.startswith("g") and n.endswith(".c") and n != "g_module.c":
                    have |= {int(h, 16) for h in re.findall(r"rt_inline_hook\(c, 0x([0-9A-Fa-f]+)ull\)", open(os.path.join(folder, n), encoding="utf-8").read())}
            if any(base + o not in have for o in inline[mod]):
                out.append(mod)
    return out


def program(tree):
    for exe in (os.path.join(tree, "build-mac", "WindowSmash.app", "Contents", "MacOS", "ssbu"),
                os.path.join(tree, "build", "ssbu.exe"), os.path.join(tree, "build-linux", "ssbu")):
        if os.path.exists(exe):
            return exe
    return None


def probe(tree, log=print):
    """Start the game until its plugins have loaded (SSBU_SKYLINE_PROBE): what they add to the select
    screen goes to dump/skyline/csk.json. The window stays hidden. True if that changed."""
    exe, p = program(tree), Paths(tree)
    if not exe or not os.path.exists(p.list) or not open(p.list, encoding="utf-8").read().strip():
        return False
    path = os.path.join(p.skyline, "csk.json")

    def entries():   # (as data: a plugin's maps come out in a different order each start)
        try:
            with open(path, encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None
    def outputs():   # (the files plugins' file callbacks are for, and what they made: dump/skyline/arc/*.out)
        out = {}
        for n in ["callbacks.txt"] + [os.path.join("arc", x) for x in sorted(os.listdir(os.path.join(p.skyline, "arc")))
                                       if x.endswith(".out")] if os.path.isdir(os.path.join(p.skyline, "arc")) else ["callbacks.txt"]:
            try:
                with open(os.path.join(p.skyline, n), "rb") as f:
                    out[n] = hashlib.sha256(f.read()).hexdigest()
            except OSError:
                pass
        return out
    before = entries(), outputs()
    env = dict(os.environ, SSBU_SKYLINE_PROBE="1", SDL_AUDIO_DRIVER="dummy", SSBU_CWD=tree, SSBU_TESTRUN="skyline-probe",
               SSBU_LOG=os.path.join(p.skyline, "probe.log"))
    log("loading the plugins once, with no window, for their select-screen entries")
    try:
        subprocess.run([exe, os.path.join(tree, "dump", "modules")], env=env, timeout=180, cwd=tree)
    except subprocess.TimeoutExpired:
        log("the probe did not finish (see dump/skyline/probe.log)")
        return False
    return (entries(), outputs()) != before


def probe_and_apply(tree, log=print):
    """Probe, and apply the mods again while that changed what the plugins add (at most 3 times: the
    file callbacks need an apply for their input, a start for their output, an apply to put it in)."""
    import mods
    for _ in range(3):
        if not probe(tree, log):
            return
        log("the plugins' select-screen entries or file changes changed: applying the mods again")
        mods.apply(mods.Tree(tree), log)


_BAR = re.compile(r"^\[[#-]+\]\s+(\d+)%\s+(.*?)\s*(?:\(\s*(.*?)\s*\))?\s*$")


def run_logged(cmd, log, keep=60):
    """Run the game's build with its output in `log` (the Mods app's window and manager.log - its
    pythonw.exe has no console, and a failed build said nothing of why): what reads as a problem as
    it comes, and when it fails, its last `keep` lines. Each progress line becomes
    `what it is doing — percent%, time left` so a bar can follow the step, not only a spinner."""
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace", bufsize=1)
    tail = []
    for line in proc.stdout:
        line = line.rstrip()
        if not line:
            continue
        tail = (tail + [line])[-keep:]
        bar = _BAR.match(line.strip())
        if bar:
            detail = (bar.group(3) or "building the game").strip()
            eta = (bar.group(2) or "").strip()
            log(f"{detail} — {int(bar.group(1))}%, {eta}")
        elif re.search(r"error|failed|warning: .*(undeclared|implicit)|did not compile|LNK\d", line, re.I):
            log(line[:400])
        elif re.search(r"generating its C|C up to date|modules reused|linking the|applying the mods", line, re.I):
            log(line[:300])
    rc = proc.wait()
    if rc:
        log(f"the build stopped (code {rc}); its last lines:")
        for line in tail:
            log("  " + line[:400])
    return rc


def dll_path(tree, name):
    """Where a mod plugin's translated code is kept."""
    return os.path.join(tree, "build", "plugins", name + ".dll")


def dll_current(tree, name):
    """Whether that file is newer than the plugin's C and its objects."""
    dll = dll_path(tree, name)
    folder = os.path.join(tree, "build", "nro", name)
    table = os.path.join(folder, "g_module.c")
    nro = os.path.join(tree, "dump", "nro", name + ".nro")
    if not os.path.isfile(dll) or not os.path.isfile(table) or c_stale(table):
        return False
    newest = max(os.path.getmtime(table), os.path.getmtime(nro) if os.path.isfile(nro) else 0)
    try:
        for n in os.listdir(folder):
            if n.endswith(".obj"):
                newest = max(newest, os.path.getmtime(os.path.join(folder, n)))
    except OSError:
        return False
    return os.path.getmtime(dll) >= newest


_BIND = """\
#include <windows.h>
#include "module.h"
/* A moveset that catches a C++ exception calls this. It lives in the game too; the plugin's
 * own file needs a copy or the plugin does not link. */
int rt_eh_filter(struct _EXCEPTION_POINTERS *e, u64 sp, u64 lp) {{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != 0xE0E4C5A0u || r->NumberParameters < 2) return 0;
    return r->ExceptionInformation[0] == lp && (r->ExceptionInformation[1] == sp || sp == ~0ull) ? 1 : 0;
}}
extern const RtModule rt_module_{name};
static GuestFn *(*plugin_lookup)(u64);
static GuestFn *(*plugin_lookup_cached)(u64, RtCache *);
static void (*plugin_trap)(Cpu *, u64, const char *);
GuestFn *rt_lookup(u64 addr) {{ return plugin_lookup(addr); }}
GuestFn *rt_lookup_cached(u64 addr, RtCache *cache) {{ return plugin_lookup_cached(addr, cache); }}
void rt_trap(Cpu *c, u64 pc, const char *why) {{ plugin_trap(c, pc, why); }}
__declspec(dllexport) const RtModule *ssbu_plugin_module(void) {{ return &rt_module_{name}; }}
__declspec(dllexport) void ssbu_plugin_bind(GuestFn *(*lookup)(u64), GuestFn *(*lookup_cached)(u64, RtCache *),
                                            void (*trap)(Cpu *, u64, const char *)) {{
    plugin_lookup = lookup; plugin_lookup_cached = lookup_cached; plugin_trap = trap;
}}
"""


def cache_mod_plugins(tree, jobs, log=print, quiet=False):
    """Compile each turned-on mod's plugin into build/plugins/<name>.dll and list those files.
    The standard plugins stay in the program. Returns the module names that translated."""
    import cc
    import recomp
    p = Paths(tree)
    out_dir = os.path.join(tree, "build", "plugins")
    os.makedirs(out_dir, exist_ok=True)
    ready = []
    for mod, source in turned_on(tree):
        if mod.startswith("(standard) "):
            continue
        record = load_record(p)
        _build_id, name = module_of(p, source, record, mod_slug(mod))
        folder = os.path.join(tree, "build", "nro", name)
        if not os.path.isfile(os.path.join(folder, "manifest.tsv")):
            log(f"{mod}: {name} has no C yet")
            continue
        if not dll_current(tree, name):
            log(f"translating {name}")
            if not recomp.compile_dir(folder, max(1, jobs)):
                log(f"{name}: this plugin did not compile, so its mod will not run")
                continue
            bind = os.path.join(out_dir, name + "_bind.c")
            text = _BIND.format(name=name)
            if not os.path.isfile(bind) or open(bind, encoding="utf-8").read() != text:
                with open(bind, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
            obj = bind[:-2] + ".obj"
            r = subprocess.run(cc.compile_command(bind, obj, "/O1", cc.GAME_CFLAGS), capture_output=True, text=True)
            if r.returncode:
                log(f"{name}: the plugin loader did not compile\n{(r.stdout + r.stderr)[:1500]}")
                continue
            objs = sorted(os.path.join(folder, n) for n in os.listdir(folder) if n.endswith(".obj"))
            objs.append(obj)
            try:
                cc.link_library(objs, dll_path(tree, name))
            except RuntimeError as e:
                log(f"{name}: the plugin did not link\n{e}")
                continue
            log(f"{name}: saved ({os.path.getsize(dll_path(tree, name)):,} bytes)")
        else:
            if not quiet:
                log(f"{name}: already translated")
        ready.append(name)
    listed = "".join(f"build/plugins/{n}.dll\n" for n in ready)
    path = os.path.join(p.skyline, "plugin_dlls.txt")
    old = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
    if listed != old:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(listed)
    return ready


def game_link_reason(tree):
    """Why the program itself must be linked again, or None. A mod plugin's own file is not a reason:
    the game loads that at start."""
    exe = program(tree)
    if not exe:
        return "the game isn't built"
    built = os.path.getmtime(exe)
    for top in ("runtime", "tools"):
        for dp, dirs, files in os.walk(os.path.join(tree, top)):
            dirs[:] = [d for d in dirs if not d.startswith((".", "__pycache__"))]
            for n in files:
                rel = os.path.relpath(os.path.join(dp, n), tree).replace(os.sep, "/")
                if n.startswith(".") or ".before-" in n or n.endswith((".orig", ".pyc")) or rel.startswith(NOT_BUILT):
                    continue
                if os.path.getmtime(os.path.join(dp, n)) > built:
                    return f"{rel} is newer than the game"
    p = Paths(tree)
    patches = os.path.join(p.skyline, "patches.txt")
    if os.path.exists(patches) and os.path.getmtime(patches) > built:
        return "dump/skyline/patches.txt is newer than the game"
    import std_plugins
    for name, plugin in std_plugins.PLUGINS.items():
        if plugin[5] == "runs" and not os.path.exists(os.path.join(std_plugins.folder(tree), name)):
            return f"{name} isn't fetched"
    listed = os.path.join(p.skyline, "plugins.txt")
    names = [l.split("#")[0].strip() for l in open(listed, encoding="utf-8")] if os.path.exists(listed) else []
    for name in names:
        if name and not built_in(tree, name):
            return f"the standard plugin {name} isn't in the game"
    if stale_hooks(tree):
        return "a plugin hooks a function the game has no slot for"
    return None


def build(tree, jobs, log=print, again=True):
    """Translate each turned-on mod plugin into its own file. Link the game only when a standard
    plugin is missing, the runtime changed, or a plugin's inline hook or code patch is not in it.
    A function hook is patched into that one function when the game starts."""
    sync(tree, log, jobs)
    cache_mod_plugins(tree, jobs, log)
    reason = game_link_reason(tree)
    if reason:
        exefs = os.path.join(tree, "dump", "exefs")
        for mod in stale_hooks(tree):
            log(f"writing {mod}'s C again — 0%, estimating the time left")
            src = os.path.join(exefs, mod) if mod in ("main", "subsdk0", "sdk") else os.path.join(tree, "dump", "nro", mod + ".nro")
            out = os.path.join(tree, "build", mod) if mod in ("main", "subsdk0", "sdk") else os.path.join(tree, "build", "nro", mod)
            recomp_stream(Paths(tree), log, "build", src, "--out", out, "--jobs", str(jobs))
        log(f"linking the game once ({reason})")
        if sys.platform == "darwin":
            cmd = [sys.executable, os.path.join(HERE, "mac", "build_mac.py"), "--port", tree, "--jobs", str(jobs)]
        else:
            cmd = [sys.executable, os.path.join(HERE, "recomp.py"), "all", exefs, "--out", os.path.join(tree, "build"),
                   "--jobs", str(jobs), "--tree", tree]
        log(" ".join(cmd))
        rc = run_logged(cmd, log)
        if rc:
            return rc
    else:
        log("the mod plugins are translated; the game itself was left as it is")
    probe_and_apply(tree, log)
    if again and (stale_hooks(tree) or game_link_reason(tree)):
        log("the plugins asked for more hook points: building once more")
        return build(tree, jobs, log, again=False)
    return 0


# What isn't part of the game's program: an update that changes only these needs no build (the Mods
# app's files are read as it runs; the tools here run as they are)
NOT_BUILT = ("tools/modmanager/", "tools/dev/", "tools/mods.py", "tools/os_mods.py", "tools/progress.py", "tools/std_plugins.py", "tools/csk.py", "tools/miis.py",
             "tools/unlock_all.py", "tools/install_windowsmash.ps1", "tools/update.py", "tools/arc.py", "tools/prc.py",
             "tools/msbt.py", "tools/ssbu_online_lobby.py", "tools/ssbu_relay.py", "tools/skyline.py",
             "tools/shader_aot.py")


def needs_build(tree):
    """Why the game's program needs building again, or None: it's missing, a file that goes into it
    (the runtime, the recompiler, the build's tools, the plugins' hook lists) is newer than it, a
    standard plugin isn't fetched, or a plugin that runs isn't compiled in (current)."""
    programs = [os.path.join(tree, "build-mac", "WindowSmash.app", "Contents", "MacOS", "ssbu"), os.path.join(tree, "build", "ssbu.exe")]
    built = max((os.path.getmtime(x) for x in programs if os.path.exists(x)), default=0)
    if not built:
        return "it isn't built"
    for top in ("runtime", "tools"):
        for dp, dirs, files in os.walk(os.path.join(tree, top)):
            dirs[:] = [d for d in dirs if not d.startswith((".", "__pycache__"))]
            for n in files:
                rel = os.path.relpath(os.path.join(dp, n), tree).replace(os.sep, "/")
                if n.startswith(".") or ".before-" in n or n.endswith((".orig", ".pyc")) or rel.startswith(NOT_BUILT):
                    continue
                if os.path.getmtime(os.path.join(dp, n)) > built:
                    return f"{rel} is newer than it"
    p = Paths(tree)
    patches = os.path.join(p.skyline, "patches.txt")
    if os.path.exists(patches) and os.path.getmtime(patches) > built:
        return "dump/skyline/patches.txt is newer than it"
    import std_plugins
    for name, plugin in std_plugins.PLUGINS.items():
        if plugin[5] == "runs" and not os.path.exists(os.path.join(std_plugins.folder(tree), name)):
            return f"{name} isn't fetched"
    import mods
    t = mods.Tree(tree)
    state = t.load()
    for n in state["order"]:   # (a turned-on mod's plugin: translated on its own, loaded when the game starts)
        if state["mods"][n].get("enabled"):
            for s in plugin_state(tree, n, mods.plugins(os.path.join(t.mods, n))):
                if s["module"]:
                    if not s["built"]:
                        return f"{n}'s plugin isn't translated yet"
                elif s["built"] is None:
                    return f"{n}'s plugin isn't recompiled yet"
    return game_link_reason(tree)


def status(tree):
    p = Paths(tree)
    names = open(p.list, encoding="utf-8").read().split() if os.path.exists(p.list) else []
    if not names:
        print("no plugins listed")
    for n in names:
        print(f"{n}: {'built in' if built_in(tree, n) else 'NOT built in yet'}")
    for mod, offs in sorted(hook_targets(p).items()):
        print(f"hooks in {mod}: " + " ".join(f"{o:#x}" for o in sorted(offs)))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    tree = os.path.abspath(arg("--tree", ROOT))
    if sys.argv[1] == "sync":
        sync(tree)
    elif sys.argv[1] == "build":
        sys.exit(build(tree, int(arg("--jobs", "2"))))
    elif sys.argv[1] == "status":
        status(tree)
    elif sys.argv[1] == "probe":   # (the plugins' select-screen entries again, and the mods applied if they changed)
        probe_and_apply(tree)
    elif sys.argv[1] == "uptodate":
        why = needs_build(tree)
        print(f"the game needs a build: {why}" if why else "the game is up to date: no build needed")
        sys.exit(1 if why else 0)
    else:
        sys.exit(__doc__)
