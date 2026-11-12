"""The pipeline sweep: completes the shipped lists of GPU programs and pipelines by playing everything.

Headless, muted, fast (SSBU_SPEED=0) test runs of the game play every fighter (player 1 through a
move-spamming SSBU_SEQ, three level 9 CPU players, items on, each fighter given its Final Smash with
SSBU_DEBUG_FINAL) on every stage (hazards on; with --forms also the Battlefield and Omega forms),
plus the menus, starting from an empty per-device Vulkan list in a shader cache of their own. What
the renderer listed (the per-device "vulkan_<vendor>_<device>.warm": every pipeline drawn with and
every compute program, runtime/gpu_vk.c) is then merged into the shipped lists:

  media/vulkan_pipelines.warm   the Vulkan warm start's records (same header: same translator)
  media/shader_prewarm.txt      the programs those records name, as Direct3D 11's prewarm keys
                                (<stage>_<hash>_<vs outputs>_<tables>: the same key as the cache files)

Only keys go into the lists: no game code or data. Re-run it for each release (docs/BUILDING.md,
"The pipeline sweep").

  python tools/dev/pipeline_sweep.py run   --exe <test exe> [--tree <checkout with dump/modules>]
         [--work <folder>] [--jobs 2] [--frames 3600] [--forms] [--only <n>] [--seconds 420]
  python tools/dev/pipeline_sweep.py merge --work <folder> [--into <checkout>]   (run does it at the end)
  python tools/dev/pipeline_sweep.py count <list.warm>                           records, programs

The exe is a test exe of this checkout's runtime (it needs SSBU_DEBUG_FINAL). The tree is the folder
the game runs in (dump/modules, its save): its save is swapped for the 100% save
(<tree>/../userdata_completo/save/save_data, or --save) for the sweep and put back after. The shader
cache's SPIR-V files are copied from <tree>/dump/shadercache into each job's cache once, so programs
are not translated during play; the driver's own cache starts empty there.
"""
import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# The fighters, by their ui_chara_ names (the select screen's), and the versus stages, by their ui_stage_ names.
FIGHTERS = """mario donkey link samus samusd yoshi kirby fox pikachu luigi ness captain purin peach daisy koopa
ice_climber sheik zelda mariod pichu falco marth lucina younglink ganon mewtwo roy chrom gamewatch metaknight pit
pitb szerosuit wario snake ike ptrainer diddy lucas sonic dedede pikmin lucario robot toonlink wolf murabito rockman
wiifit rosetta littlemac gekkouga palutena pacman reflet shulk koopajr duckhunt ryu ken cloud kamui bayonetta inkling
ridley simon richter krool shizue gaogaen packun jack brave buddy dolly master tantan pickel edge element demon trail""".split()
# (not the Mii Fighters: a debug match with one crashes as it loads - their Mii is chosen on the select screen, which
# SSBU_DEBUG_MATCH does not walk; their moves' effects are the other fighters' and the items')
STAGES = """battle_field battle_field_l battle_field_s end mario_castle64 dk_jungle zelda_hyrule yoshi_story
kirby_pupupu64 fox_corneria poke_yamabuki mario_past64 mother_onett zelda_greatbay zelda_temple metroid_zebesdx
yoshi_yoster kirby_fountain fox_venom poke_stadium mother_fourside mario_castledx mario_rainbow dk_waterfall
zelda_oldin poke_stadium2 mariobros zelda_pirates fzero_bigblue kirby_greens mother_magicant yoshi_island
mario_pastusa kirby_halberd metroid_norfair fox_lylatcruise poke_tengam ice_top mg_shadowmoses luigimansion
zelda_gerudo zelda_tower wario_madein mario_paper pikmin_garden yoshi_cartboard mario_galaxy mario_newbros2
zelda_train animal_city wufuisland fzero_porttown pictochat2 kart_circuitx animal_village tomodachi mario_3dland
pilotwings poke_unova balloonfight kirby_cave pac_land nintendogs streetpass xeno_gaur zelda_skyward poke_tower
mario_uworld wreckingcrew animal_island fe_arena wiifit kart_circuitfor duckhunt metroid_orpheon dk_lodge
plankton icarus_uprising icarus_skyworld icarus_angeland fzero_mutecity3ds flatzonex mario_dolpic sonic_greenhill
sonic_windyhill rock_wily 75m pikmin_planet mother_newpork wario_gamer kirby_gameboy punchoutsb punchoutw
fe_colloseum fe_siege fe_shrine sf_suzaku ff_midgar ff_cave poke_kalos bayo_clock mario_maker spla_parking
dracula_castle mario_odyssey jack_mementoes brave_altar buddy_spiral dolly_stadium tantan_spring pickel_world
ff_cave xeno_alst demon_dojo trail_castle metroid_kraid mario_pastx""".split()
STAGES = list(dict.fromkeys(STAGES))

# Boot to the first battle frame: the movie, the title, the main menu, Smash, the select screen (a fighter
# picked: SSBU_DEBUG_MATCH replaces it), Ready to fight, the stage screen (replaced too), the fight
# (as plan/phases/R7_tools/gate2.seq; draw counts of this renderer).
MENUS = """@movie
@draws:430-485/30
30:A
@until:400-3000/40:A/150
300:A
300:A
300:A
@until:3000-3400/30:A/300
60:SUP/30 20:SLEFT/18 30:A
90:PLUS
100:A
@fight
240
"""
# One round of player 1's moves (about 900 frames): jabs, tilts (stick held, then A), smash attacks (stick and
# A together), aerials, the four specials on the ground and in the air (charged too), grabs and throws,
# shield, dodges, taunts. Its neutral special after SSBU_DEBUG_FINAL is its Final Smash.
MOVES = """20:A/4 8:A/4 8:A/4
30:SRIGHT/5 0:SRIGHT+A/6 30:SUP/5 0:SUP+A/6 30:SDOWN/5 0:SDOWN+A/6
40:SRIGHT+A/6 60:SLEFT+A/6 60:SUP+A/6 60:SDOWN+A/6
60:X/4 4:A/6 40:X/4 4:SRIGHT+A/6 40:X/4 4:SLEFT+A/6 40:X/4 4:SUP+A/6 40:X/4 4:SDOWN+A/6
60:B/8 60:B/70 60:SRIGHT+B/8 60:SLEFT+B/8 60:SDOWN+B/8 60:SDOWN+B/50 60:SUP+B/8
90:X/4 4:B/8 50:X/4 4:SRIGHT+B/8 50:X/4 4:SDOWN+B/8
90:R/6 30:SRIGHT/6 60:R/6 30:SUP/6 60:R/6 30:SDOWN/6 60:R/6 30:SLEFT/6
60:ZR/40 10:ZR+SDOWN/6 40:ZR+SRIGHT/6 40:ZR+SLEFT/6
40:UP/6 60:DOWN/6 60:LEFT/6 60:RIGHT/6
30:SLEFT/40 10:SRIGHT/40
"""
FINAL_AT = "600,900"   # SSBU_DEBUG_FINAL: each fighter's Final Smash at its fight frame 600, and every 900 after

WARM_HEAD = 32
WARM_RECORD = 440
WARM_MAX = 16384       # (gpu_vk.c WARM_RECORDS)
STAGE_NAMES = ("vs", "ps", "cs")


# ---- the lists ---------------------------------------------------------------------------------------
def read_warm(path):
    """(header bytes, [record bytes]) of a warm list; header None if the file is missing or too short."""
    try:
        data = open(path, "rb").read()
    except OSError:
        return None, []
    if len(data) < WARM_HEAD or data[:8] != b"OSVKWRM\0":
        return None, []
    n = (len(data) - WARM_HEAD) // WARM_RECORD
    return data[:WARM_HEAD], [data[WARM_HEAD + i * WARM_RECORD:WARM_HEAD + (i + 1) * WARM_RECORD] for i in range(n)]


def record_programs(rec):
    """The cache keys of a record's programs: <vs|ps|cs>_<hash>_<vs outputs>_<tables>."""
    out = []
    for off in (8, 32):   # (kind, sum; then WarmKey vs, ps: hash, table hash, vs outputs, stage)
        h, table, vs_outputs, stage = struct.unpack_from("<QQII", rec, off)
        if stage < 3:
            out.append(f"{STAGE_NAMES[stage]}_{h:016x}_{vs_outputs:08x}_{table:016x}")
    return out


def read_prewarm(path):
    head, keys = [], []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        (head if line.startswith("#") or not line.strip() else keys).append(line)
    return head, keys


def merge(sources, into):
    """The records of every per-device list in sources joined to <into>/media's two lists (once each, the shipped
    order first, then the sweep's in first-use order). Returns the counts."""
    warm_path = os.path.join(into, "media", "vulkan_pipelines.warm")
    pre_path = os.path.join(into, "media", "shader_prewarm.txt")
    ship_head, ship = read_warm(warm_path)
    found_head, found = None, []
    for s in sources:
        h, recs = read_warm(s)
        if h is None:
            continue
        if found_head is not None and h != found_head:
            sys.exit(f"{s}: a list of another translator than the first one read")
        found_head = h
        found += recs
    if found_head is None:
        sys.exit("no per-device list found: nothing to merge")
    if ship_head is not None and ship_head != found_head:
        # (the same record layout and list version: its records name programs by their keys, which a new translator
        # or glslang keeps - they are kept under the new header; another layout: the sweep's alone)
        same_layout = ship_head[8:16] == found_head[8:16] and ship_head[24:] == found_head[24:]
        print(f"the shipped list is of another translator (SPIR-V version {struct.unpack_from('<I', ship_head, 16)[0]}, "
              f"library {struct.unpack_from('<I', ship_head, 20)[0]:08x}): "
              + ("its records kept under the new header" if same_layout else "replaced by the sweep's (another record layout)"))
        if not same_layout:
            ship = []
    seen, merged = set(), []
    for r in ship + found:
        if r[8:] in seen:   # (the checksum is over the rest)
            continue
        seen.add(r[8:])
        merged.append(r)
    found_unique = {r[8:] for r in found}
    new = len(merged) - len(ship)
    if len(merged) > WARM_MAX:
        print(f"warning: {len(merged)} records, the renderer reads {WARM_MAX}: the last ones left out")
        merged = merged[:WARM_MAX]
    with open(warm_path, "wb") as f:
        f.write(found_head + b"".join(merged))
    head, keys = read_prewarm(pre_path)
    have = set(keys)
    added = []
    for r in found:
        for k in record_programs(r):
            if k not in have:
                have.add(k)
                added.append(k)
    with open(pre_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(head + keys + added) + "\n")
    res = {"shipped_before": len(ship), "sweep_records": len(found_unique), "new_records": new, "shipped_after": len(merged),
           "programs_before": len(keys), "programs_added": len(added), "programs_after": len(keys) + len(added)}
    print(f"vulkan_pipelines.warm: {res['shipped_before']} records before, the sweep listed {res['sweep_records']}, "
          f"{res['new_records']} new: {res['shipped_after']} now")
    print(f"shader_prewarm.txt: {res['programs_before']} programs before, {res['programs_added']} added: {res['programs_after']} now")
    return res


# ---- the runs ----------------------------------------------------------------------------------------
def plan(forms, only):
    """The matches: every stage once (each form with --forms), four fighters each, every fighter player 1
    at least once and all of them in turn."""
    stages = [(s, "normal") for s in STAGES]
    if forms:
        stages += [(s, f) for f in ("battlefield", "omega") for s in STAGES if not s.startswith("battle_field") and s != "end"]
    # Match i: player 1 is fighter i (each fighter in turn: the CPU players never press what player 1's
    # sequence presses), the CPU players fighters i + q, i + 2q, i + 3q (a quarter of the list apart).
    n, nf = max(len(stages), len(FIGHTERS)), len(FIGHTERS)
    q = nf // 4
    matches = []
    for i in range(n):
        stage, form = stages[i % len(stages)]
        matches.append({"stage": stage, "form": form, "fighters": [FIGHTERS[(i + k * q) % nf] for k in range(4)]})
    if only:
        matches = matches[:only]
    return matches


def finals_played(fh, log_text):
    """The fighter entries the hash log (SSBU_FIGHT_HASH) shows in the Final Smash's status (the log names its number)."""
    import re
    m = re.search(r"FIGHTER_STATUS_KIND_FINAL (0x[0-9a-f]+)", log_text)
    if not m:
        return set()
    final = int(m.group(1), 16)
    entries = set()
    try:
        for line in open(fh, encoding="utf-8", errors="replace"):
            p = line.split()
            if len(p) > 5 and p[0] == "F" and p[4] == "st" and int(p[5], 16) == final:
                entries.add(p[3])
    except (OSError, ValueError):
        pass
    return entries


def match_env(m, frames, cache):
    f = m["fighters"]
    dm = (f"p1={f[0]},p2={f[1]},p2=cpu,cpu=9,p3={f[2]},p4={f[3]},stage={m['stage']},form={m['form']},"
          f"stock=99,items=on,hazards=on")
    rounds = frames // 900 + 1
    return {
        "SSBU_HEADLESS": "1", "SSBU_VOLUME": "0", "SSBU_SPEED": "0", "SSBU_SKIP_MOVIES": "1",
        "SSBU_RENDERER": "vulkan", "SSBU_SHADER_CACHE": cache,
        "SSBU_SEQ": MENUS + MOVES * rounds, "SSBU_DEBUG_MATCH": dm, "SSBU_DEBUG_END": str(frames),
        "SSBU_DEBUG_FINAL": FINAL_AT, "SSBU_VK_MAKE_LOG": "0",
    }


def menus_env(cache):
    """The main menus and modes, walked through (no fight): Smash's rules, Spirits, Games & More, Vault, Online's
    first screen, Options, and back; a sequence that loses its way only lists less."""
    seq = """@movie
@draws:430-485/30
30:A
@until:400-3000/40:A/150
300
""" + "".join(f"{60 + 20 * k}:SRIGHT/8 240:A 400:B/8 200:B/8 200:B/8\n" for k in range(6)) + \
          "".join(f"60:SDOWN/8 240:A 400:B/8 200:B/8 200:B/8\n" for _ in range(2)) + "60:Y 300:B 200:X 300:B 600\n"
    return {"SSBU_HEADLESS": "1", "SSBU_VOLUME": "0", "SSBU_SPEED": "0", "SSBU_SKIP_MOVIES": "1",
            "SSBU_RENDERER": "vulkan", "SSBU_SHADER_CACHE": cache, "SSBU_SEQ": seq, "SSBU_VK_MAKE_LOG": "0"}


def run_one(exe, tree, env, log, seconds, stall=60, end_line="fight frames at frame"):
    """One game; killed when its log has not grown for `stall` s or after `seconds`. Returns (ok, wall seconds)."""
    full = dict(os.environ)
    full.update(env)
    t0 = time.time()
    with open(log, "wb") as lf:
        p = subprocess.Popen([exe, "dump\\modules"], cwd=tree, env=full, stdout=lf, stderr=subprocess.STDOUT,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    last_size, last_t = -1, time.time()
    while p.poll() is None:
        time.sleep(2)
        size = os.path.getsize(log)
        if size != last_size:
            last_size, last_t = size, time.time()
        if time.time() - last_t > stall or time.time() - t0 > seconds:
            p.kill()
            p.wait()
            break
    text = open(log, "rb").read().decode("utf-8", "replace")
    return end_line in text if end_line else p.returncode is not None, time.time() - t0


def copy_files(src, dst, pattern):
    """Thousands of small files: robocopy's threads on Windows (minutes less than one at a time)."""
    os.makedirs(dst, exist_ok=True)
    if sys.platform == "win32" and shutil.which("robocopy"):
        if subprocess.call(["robocopy", src, dst, pattern, "/NFL", "/NDL", "/NJH", "/NJS", "/NP", "/MT:16"]) < 8:
            return
    for f in glob.glob(os.path.join(src, pattern)):
        shutil.copy2(f, dst)


def swap_save(tree, save):
    user = glob.glob(os.path.join(tree, "save", "*"))
    user = [u for u in user if os.path.isdir(u) and ".work." not in u]
    if not user or not os.path.isdir(save):
        sys.exit(f"no save folder in {tree}\\save, or no {save}")
    dst = os.path.join(user[0], "save_data")
    bak = dst + ".sweep_bak"
    if os.path.exists(bak):
        sys.exit(f"{bak} exists: a sweep did not put the save back (move it back first)")
    os.rename(dst, bak)
    shutil.copytree(save, dst)
    return dst, bak


def restore_save(dst, bak):
    shutil.rmtree(dst, ignore_errors=True)
    os.rename(bak, dst)


def cmd_run(a):
    exe, tree = os.path.abspath(a.exe), os.path.abspath(a.tree)
    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    base = os.path.join(work, "spv")
    if not os.path.isdir(base):   # (the SPIR-V the game would translate during play, once: the newest translator's)
        names = [os.path.basename(f) for f in glob.glob(os.path.join(tree, "dump", "shadercache", "g*.*_*.spv"))]
        newest = max((n.split("_")[0] for n in names), default=None, key=lambda v: tuple(int(x, 16) for x in v[1:].split(".")))
        copy_files(os.path.join(tree, "dump", "shadercache"), base, f"{newest}_*.spv" if newest else "none")
    caches = []
    for j in range(a.jobs):
        c = os.path.join(work, f"cache{j}")
        if not os.path.isdir(c):
            copy_files(base, c, "*.spv")
        for w in glob.glob(os.path.join(c, "vulkan_*.warm")):
            if not a.keep:
                os.remove(w)   # (an empty per-device list: what the sweep plays is what it lists)
        caches.append(c)
    # no shipped list joined to the per-device one (a file of another header beside the exe is not read)
    blocker = os.path.join(os.path.dirname(exe), "vulkan_pipelines.warm")
    made_blocker = not os.path.exists(blocker)
    if made_blocker:
        open(blocker, "wb").write(b"NOT-READ")
    dst, bak = swap_save(tree, os.path.abspath(a.save or os.path.join(tree, "..", "userdata_completo", "save", "save_data")))
    results = []
    try:
        matches = plan(a.forms, a.only)
        if a.redo:   # (matches again, one a line: <stage> <form> <f1>,<f2>,<f3>,<f4> - those that did not finish)
            matches = [{"stage": l.split()[0], "form": l.split()[1], "fighters": l.split()[2].split(",")}
                       for l in open(a.redo, encoding="utf-8") if len(l.split()) == 3]
        jobs = [("menus", None, None)] + [(f"{'r' if a.redo else 'm'}{i:03d}", m, None) for i, m in enumerate(matches)]
        if a.no_menus:
            jobs = jobs[1:]
        import threading
        lock = threading.Lock()
        queue = list(jobs)

        def worker(slot):
            while True:
                with lock:
                    if not queue:
                        return
                    name, m, _ = queue.pop(0)
                log = os.path.join(work, f"{name}.log")
                if name == "menus":
                    env, end = menus_env(caches[slot]), None
                else:
                    env, end = match_env(m, a.frames if m["form"] == "normal" else a.form_frames, caches[slot]), "fight frames at frame"
                    env["SSBU_FIGHT_HASH"] = os.path.join(work, f"{name}.fh.txt")
                ok, secs = run_one(exe, tree, env, log, a.seconds if name != "menus" else 240, end_line=end)
                text = open(log, "rb").read().decode("utf-8", "replace")
                finals = finals_played(env.get("SSBU_FIGHT_HASH", ""), text)
                if "SSBU_FIGHT_HASH" in env and os.path.exists(env["SSBU_FIGHT_HASH"]):
                    os.remove(env["SSBU_FIGHT_HASH"])
                with lock:
                    results.append((name, ok, secs, finals, m))
                    what = "menus" if name == "menus" else f"{m['stage']}/{m['form']} {','.join(m['fighters'])}"
                    print(f"{name} {'ok' if ok else 'NOT FINISHED'} {secs:.0f}s, Final Smash seen for entries {sorted(finals) or '-'}: {what}", flush=True)

        threads = [threading.Thread(target=worker, args=(s,)) for s in range(a.jobs)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    finally:
        restore_save(dst, bak)
        if made_blocker:
            os.remove(blocker)
    bad = [r for r in results if not r[1]]
    fs = {r[4]["fighters"][int(e)] for r in results if r[4] for e in r[3] if e.isdigit() and int(e) < 4}
    missing = [f for f in FIGHTERS if f not in fs]
    print(f"Final Smash seen for {len(fs)} of {len(FIGHTERS)} fighters; not: {', '.join(missing) or 'none'}")
    print(f"{len(results) - len(bad)} of {len(results)} runs finished; not: {', '.join(r[0] for r in bad) or 'none'}")
    lists = [w for c in caches for w in glob.glob(os.path.join(c, "vulkan_*.warm"))]
    for w in lists:
        print(f"{w}: {len(read_warm(w)[1])} records")
    if not a.no_merge:
        merge(lists, a.into or REPO)


def cmd_merge(a):
    lists = glob.glob(os.path.join(os.path.abspath(a.work), "cache*", "vulkan_*.warm"))
    merge(lists, a.into or REPO)


def cmd_count(a):
    head, recs = read_warm(a.list)
    progs = {k for r in recs for k in record_programs(r)}
    kinds = {}
    for r in recs:
        kinds[struct.unpack_from("<I", r)[0]] = kinds.get(struct.unpack_from("<I", r)[0], 0) + 1
    print(f"{a.list}: {len(recs)} records ({kinds.get(1, 0)} pipelines, {kinds.get(2, 0)} compute programs), {len(progs)} programs")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--exe", required=True)
    r.add_argument("--tree", default=os.path.join(REPO, "..", "OpenSmash-Ultimate"))
    r.add_argument("--work", default=os.path.join(REPO, "build", "pipeline_sweep"))
    r.add_argument("--save")
    r.add_argument("--jobs", type=int, default=2)
    r.add_argument("--frames", type=int, default=3600)
    r.add_argument("--seconds", type=int, default=420)
    r.add_argument("--forms", action="store_true", help="every stage's Battlefield and Omega forms too")
    r.add_argument("--form-frames", type=int, default=900, help="fight frames of a form's match (its stage drawn: shorter)")
    r.add_argument("--only", type=int, default=0)
    r.add_argument("--keep", action="store_true", help="keep the per-device lists of a run before (continue a sweep)")
    r.add_argument("--no-menus", action="store_true")
    r.add_argument("--redo", help="a file of matches to play instead of the plan (with --keep: added to the lists so far)")
    r.add_argument("--no-merge", action="store_true")
    r.add_argument("--into")
    m = sub.add_parser("merge")
    m.add_argument("--work", default=os.path.join(REPO, "build", "pipeline_sweep"))
    m.add_argument("--into")
    c = sub.add_parser("count")
    c.add_argument("list")
    a = ap.parse_args()
    {"run": cmd_run, "merge": cmd_merge, "count": cmd_count}[a.cmd](a)


if __name__ == "__main__":
    main()
