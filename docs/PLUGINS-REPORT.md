# Real Skyline plugins and the character bench: report (2026-10-24, updated 2026-10-25)

## Update, 2026-10-25: Sans, the Super HYPE Bros. pack, the real CSK Collection, more than 4 new characters

**Sans** crashed in the weapons' set-up of his costumes (main+0x497350 reading a model that never loaded): his
`config.json` lists Palutena's wing and portal model files for c120 (`model/godwing/c120`, `model/gate/c120`, 34
files) as new files and doesn't ship them (the GameBanana download itself, same MD5). `mods.py apply` now shares
every such file from the game's own costume 0 (`mods/.characters/config.json`; the mod's own config untouched). It
also covered 2 of Shadow Mewtwo's animations.

**Super HYPE Bros.** (Z Broly Lite, Jiren, Sasuke, Vegito, All Might; the zip in Downloads) installed in the clone,
Z Broly's plugin no longer left out (`dump/skyline/skip.txt`): all five pass. Its stop at boot (a 512 GB allocation:
a formatted path's length) was the Rust digit-loop bug lift.py fixed on 10-24: while a plugin is left out its C isn't
made again, and the real ARCropolis loads it from the mod's folder anyway, so the 10-23 C ran. `skyline.py` now makes a
plugin's C again whenever the recompiler is newer than it.

**The CSK Collection 5.0.6 runs for real**, ported to 13.0.5 (it only exists for 13.0.4). Main's code moved by
different amounts in different places (0, -0x40, -0xe0, -0x1a0, +0x1bc, +0x450, +0x490, +0x5b0) and its data and
read-only data by +0x1000. `tools/port_plugin.py` rewrites the 81 main addresses CSK's code builds, from
`tools/ports/the_csk_collection-5.0.6.txt`: region shifts from address pairs of plugins released for both versions
(Smashline's 13.0.5 commit, One Slot Effects' 13.0.4 and 13.0.5 builds, HewDraw Remix's 13.0.5 offsets), each address
checked against 13.0.5's code (a function hook on a function start; an inline hook where its callback's registers fit,
e.g. the "SLI" file in x20 right after it loads; a patch on the instruction it replaces). Its set-up runs to the end,
every hook on its place; its hooks and 50 code patches are built in. `std_plugins.py fetch` ports it after the hash
check. The runtime's own CSK steps aside by itself (plugins call the real one's API).

**More than 4 new characters**: not CSK's job. The select screen's arrangement
(`ui/param/menu/chara_icon_arrangement.prc`) has 91 places (13 x 7); the game's fighters fill 87. A 5th new character
breaks it (one column of tiles, or `abort` from main+0x19f1550, a lookup in that arrangement). On the console too: the
community fix is a mod, **CSS 91+ Layout Fix** (GameBanana 468582: a bigger arrangement and a smaller-tile layout).
With it, all 12 new characters show and play. `mods.py apply` now says so when more than 4 are on without it.

Also: a 13.0.4 plugin's function hook that misses a function start moves by its region's shift when that lands on one
(Shadow Mewtwo's 3 hooks); a standard plugin keeps its own exports when its C is made again for hooks into it
(ARCropolis' `arcrop_*` API went to the runtime's imitation); ported plugins (same build id, other bytes) are rebuilt;
`SSBU_DEBUG_AT=<module>+<offset>` logs registers at inline points; `tools/dev/install_into.py` installs into the
playable tree (dry run by default).

### Bench, 2026-10-25 07:00-08:24: real ARCropolis, Smashline 2, One Slot Effects, nro_hook and the ported CSK; all
12 new characters on, with the CSS 91+ Layout Fix; 2 runs each: **27 of 27 pass**

| Mod | Character | Result (2 runs) |
|---|---|---|
| Knuckles over Incineroar | `gaogaen:1` | pass |
| Fury Bowser + Giga Bowser (Model Port) | `koopa:0` | pass |
| Dr. Eggman over Ganondorf | `ganon:6` | pass |
| Wolverine - X-Men Mod | `wolf:7` | pass |
| Galacta Knight | `metaknight:6` | pass |
| The Warden | `pickel:6` | pass |
| Uncensored Pyra_Mythra | `eflame:0` | pass |
| Uncensored Pyra_Mythra | `elight:0` | pass |
| Mr. L | `luigi:0` | pass |
| Swimsuit Pyra and Mythra | `eflame:7` | pass |
| Swimsuit Pyra and Mythra | `elight:7` | pass |
| Kagamine Rin FT | `daisy:8` | pass |
| The Roaring Knight over Sepiroth | `edge:0` | pass |
| Jirachi | `ness:0` | pass |
| Mr. Krabs | `wario:1` | pass |
| Shadow The Hedgehog (Moveset) - Moveset_ Shadow | `wolf:120` | pass |
| Paper Mario Moveset | `pikmin:64` | pass |
| Sans Moveset | `palutena:120` | pass |
| Shadow Mewtwo (Moveset) | `mewtwo:24` | pass |
| Blaziken (Moveset) | `captain:120` | pass |
| Dark Meta Knight Moveset | `metaknight:72` | pass |
| Springtrap | `ganon:16` | pass |
| Super HYPE Bros. - (Moveset) Z Broly Lite | `ganon:40` | pass |
| Super HYPE Bros. - (Moveset) Jiren | `ganon:80` | pass |
| Super HYPE Bros. - (Moveset) Sasuke Uchiha | `marth:64` | pass |
| Super HYPE Bros. - (Moveset) Vegito | `captain:80` | pass |
| Super HYPE Bros. - (Moveset) All Might | `dolly:88` | pass |

## Update, 2026-10-24 afternoon: the real ARCropolis and the real Smashline 2 run

You said "straight to Smashline, every plugin": done for Smashline 2, ARCropolis and One Slot Effects. They now run
**as released** (recompiled like any plugin, no imitation), loaded from `<tree>/skyline/plugins/`, and the
runtime's imitations step aside when they're there. With ARCropolis, the game keeps its own data
(`romfs_vanilla.cfg`); ARCropolis serves the turned-on mods from `sd:/ultimate/mods` as the game loads, and
**chainloads the mods' plugins itself** (each mod's `plugin.nro`, as on the console). `mods.py apply` then
only syncs the plugins.

What it took (runtime + tools, all plain C/Python; no `recomp.h` change, so no game recompile):
- **nn::ro as rtld keeps it** (`runtime/hle_rtld.c`, new): a `ModuleObject` per loaded module, filled from its
  dynamic section, on the SDK's own `g_pAutoLoadList` / `g_pManualLoadList`; `LoadModule` goes through
  `LoadModuleInternal`'s dispatch entry so a plugin's hook on it sees every module load; `nn::ro::Module`
  carries the `ModuleObject*`; `LookupModuleSymbol`. QueryMemory now reports code / read-only data / data as
  separate regions (Smashline finds sections that way).
- **Smashline's code caves**: it hooks calls in `lua2cpp_common` by writing a `bl` to a cave and
  inline-hooking the cave; the runtime puts the hook at the call itself and the recompiler leaves the cave
  patches out. Its interpreter's landing pad (taken by address, resumed by its libunwind patch) is a landing
  pad of the recompiled code, taking any stack pointer.
- **SDK functions plugins use and the game doesn't** (their SDK code asks system services that aren't here):
  `nn::oe::GetDisplayVersion` ("13.0.5"), `ExitApplication`, `nn::err::ShowApplicationError` (now prints the
  plugin's message - that's how ARCropolis' and Smashline's errors became readable), `hid::detail`,
  `ssl::Initialize` (fails: no update check), `crypto::GenerateSha256Hash` (the CPU's SHA-256 instructions
  aren't translated), `fs` SetFileSize / FlushFile / RenameFile / RenameDirectory / timestamps /
  `fsa::Register`. Installed on the SDK's dispatch entries when plugins load.
- `sd:/ultimate/mods` lists the turned-on mods only (a pack's nested mods as their own folders);
  ARCropolis' menu resources go in `dump/skyline/sd/ultimate/arcropolis/resources`.
- Hook/inline/patch lists rebuild `main`, `sdk`, `lua2cpp_common` as needed: one build after the first
  boot, one after the first match (Smashline hooks `lua2cpp_common` when it loads).
- The bench runs each character twice; `bench.py play <fighter:costume>` lets you play the match yourself.

### Bench, real ARCropolis 4.1.1 + Smashline 2 1.6.8 + One Slot Effects (13.0.5), 2 runs each, 2026-10-24 13:00-14:30

| Mod | Character | Result (2 runs) | Before (mods.py + imitations, morning) |
|---|---|---|---|
| Knuckles over Incineroar | `gaogaen:1` | pass | pass |
| Fury Bowser + Giga Bowser (Model Port) | `koopa:0` | pass | pass |
| Dr. Eggman over Ganondorf | `ganon:6` | pass | pass |
| Wolverine - X-Men Mod | `wolf:7` | pass | pass |
| Galacta Knight | `metaknight:6` | pass | pass |
| The Warden | `pickel:6` | pass | pass |
| Uncensored Pyra_Mythra | `eflame:0` | pass | pass |
| Uncensored Pyra_Mythra | `elight:0` | pass | pass |
| Mr. L | `luigi:0` | pass | pass |
| Swimsuit Pyra and Mythra | `eflame:7` | pass | pass |
| Swimsuit Pyra and Mythra | `elight:7` | pass | pass |
| Kagamine Rin FT | `daisy:8` | pass | pass |
| The Roaring Knight over Sepiroth | `edge:0` | pass | pass |
| Jirachi | `ness:0` | pass | pass |
| Mr. Krabs | `wario:1` | pass | pass |
| Shadow The Hedgehog (Moveset) - Moveset_ Shadow | `wolf:120` | pass | pass |
| Paper Mario Moveset | `pikmin:64` | pass | stuck loading |
| Sans Moveset | `palutena:120` | **CRASH** | crash |
| Shadow Mewtwo (Moveset) | `mewtwo:24` | pass | stuck loading |
| Blaziken (Moveset) | `captain:120` | pass | pass (crashed 2 in 3 before the unwind fix) |
| Dark Meta Knight Moveset | `metaknight:72` | pass | stuck loading |
| Springtrap | `ganon:16` | pass | stuck loading |
| Sonic Re-Imagined Slotted (Modern Style) | `sonic:0` | pass | pass |

**Sans** still crashes the same way (fighter set-up, main+0x497350: an object's field at +0x10 is null while a weapon is created - `ldr x9, [x19, #0x10]; ldr x0, [x9, #0x18]`), with the real ARCropolis and Smashline too, so neither imitation was the cause. On its path, only the CSK API (still the runtime's) and lib_paramconfig (not running) aren't the real plugins yet; next to look at.

## In short (first report, the morning)

- **The bench works**: `tools/dev/bench.py all` runs every installed character through boot, match load and
  20 s of play with no menu scripting and no controller interference, and writes a pass/fail table.
- **22 characters tested: 17 pass.** All your skin mods pass, and so do Shadow, Blaziken and Sonic Re-Imagined.
  **The 5 that fail all clone weapons through Smashline** (Sans: crash; Paper Mario, Dark Meta Knight,
  Springtrap, Shadow Mewtwo: stuck loading). Our imitation of Smashline's weapon cloning is the problem, so
  the real Smashline is the next thing to run.
- **One real plugin runs as-is: One Slot Effects** (costumes' own effects; Blaziken's fire now shows).
- **Five bugs fixed** that affected plugins generally (boot crashes of Shadow Mewtwo/Dark Meta Knight, Sonic
  Re-Imagined's stop, Blaziken's intermittent crash, the select screen collapsing), see 3.
- Real **Smashline** and real **CSK Collection** were tried: both get partway through their set-up. What's
  missing is concrete (section 4). ARCropolis was not tried (section 4: why it's last).
- Nothing in `~/WindowSmash` changed; nothing pushed. Section 6: what installing it would change.

Work tree: `~/WindowSmash-plugins` (an APFS clone of `~/WindowSmash`, its own git history: `git log`
there lists every change; `git diff 1dff30c` is everything). Nothing in `~/WindowSmash` was changed,
nothing was pushed.

## Bench results (the clone, today's plugins + fixes in 3, 2026-10-24 05:15-06:10)

Each: boot, menus, match load, 20 s of play against a level-9 CPU. Group 1 (Shadow, Sans, Shadow Mewtwo, Blaziken + all your skins), group 2 (Paper Mario, Springtrap, Dark Meta Knight): at most 4 new characters at a time (see 3).

| Mod | Character tested | Result | Notes |
|---|---|---|---|
| Knuckles over Incineroar | `gaogaen:1` | pass |  |
| Fury Bowser + Giga Bowser (Model Port) | `koopa:0` | pass |  |
| Dr. Eggman over Ganondorf | `ganon:6` | pass |  |
| Wolverine - X-Men Mod | `wolf:7` | pass |  |
| Galacta Knight | `metaknight:6` | pass |  |
| The Warden | `pickel:6` | pass |  |
| Uncensored Pyra_Mythra | `eflame:0` | pass |  |
| Uncensored Pyra_Mythra | `elight:0` | pass |  |
| Mr. L | `luigi:0` | pass |  |
| Swimsuit Pyra and Mythra | `eflame:7` | pass |  |
| Swimsuit Pyra and Mythra | `elight:7` | pass |  |
| Kagamine Rin FT | `daisy:8` | pass |  |
| The Roaring Knight over Sepiroth | `edge:0` | pass |  |
| Jirachi | `ness:0` | pass |  |
| Mr. Krabs | `wario:1` | pass |  |
| Shadow The Hedgehog (Moveset) - Moveset_ Shadow | `wolf:120` | pass |  |
| Sans Moveset | `palutena:120` | **CRASH** | crash in fighter set-up right after "13 articles, 4 of them added by plugins": the runtime's imitation of Smashline's weapon cloning |
| Shadow Mewtwo (Moveset) | `mewtwo:24` | **FREEZE-LOAD** | loading screen forever (adds cloned weapons: lightpillar, mewtwom) |
| Blaziken (Moveset) | `captain:120` | pass | with the real One Slot Effects too (its own fire effects show); was crashing 2 runs in 3 until fix 3.5 |
| Paper Mario Moveset | `pikmin:64` | **FREEZE-LOAD** | loading screen forever, also alone and without its plugin; its default costume plays (adds cloned weapons) |
| Springtrap | `ganon:16` | **FREEZE-LOAD** | loading screen forever; config.json also leaves out its camera folders c17-c23 (adds cloned weapons) |
| Dark Meta Knight Moveset | `metaknight:72` | **FREEZE-LOAD** | loading screen forever, also without its plugin; its default costume plays (adds cloned weapons: fourwings) |
| Sonic Re-Imagined Slotted (Modern Style) | `sonic:0` | pass | |



## 1. The character bench (new)

`SSBU_BENCH=<fighter>[:<costume>],<opponent>[:<costume>]` (runtime/hle_bench.c) starts a match with
that fighter and costume for player 1 against a level-9 CPU, whatever the select screen picks, plays
it for `SSBU_BENCH_SECONDS` (20) and exits with one line in the log:

    [bench] RESULT PASS|MENUS|FREEZE-LOAD|FREEZE-MATCH <details>

- It drives player 1 itself and ignores every pad and the keyboard, so your DualSense can't take
  player 1 over in a test window.
- The menus: A through the title and menus as your save needs them, then a pick on the select
  screen, start, the stage, retried every 15 s until the fighters are set up. Which tile it picks
  doesn't matter: the game's fighter set-up (main+0x66dcf0, already hooked for the CSK redirect)
  gets the bench's fighter and costume. New characters work by costume (`wolf:120` = Shadow).
- In the match player 1 jabs, tilts, smashes, uses all four specials, aerials and a grab, over and
  over, so a moveset's scripts run.
- It tells failures apart: no fighter set up in 180 s (MENUS), set up but the match never runs in
  90 s (FREEZE-LOAD: your Pyra/Mythra-style hangs), the fighters' frames stop for 10 s
  (FREEZE-MATCH), or a crash/stop report (CRASH).
- Known cosmetic: the HUD's portrait and name are the select screen's pick (e.g. Game & Watch's for
  the CPU), not the bench's fighter.

`tools/dev/bench.py` runs it: `run <fighter:costume>` once, `list` / `all` for every installed
character (each turned-on mod's first costume, and its first new slot c08+), muted test copies of
the clone, one at a time under `/tmp/windowsmash-test.lock`, never while your own game runs, and it
closes only its own copy. Each run keeps `bench/<tag>/log.txt`, a frame every 6 s
(`shot_*.jpg`) and `result.json`; the table is `bench/results.md`. A run takes about 65-90 s.

## 2. The standard plugin set (collected)

What GameBanana's moveset/new-character mods list as requirements (Shadow, Paper Mario, Shadow
Mewtwo, Blaziken, Sonic Re-Imagined all list the same seven), and the most-downloaded plugins of
GameBanana's Skyline category:

| Plugin | Version | From | Licence | What it needs here |
|---|---|---|---|---|
| ARCropolis `libarcropolis.nro` | 4.1.1 (2026-09-25) | github.com/Raytwo/ARCropolis | GPL-3.0 | file system replacement: `mods.py` does this today (see 4) |
| Smashline 2 `libsmashline_plugin.nro` | 1.6.8 (2026-09-16) | github.com/HDR-Development/smashline | none declared | inline hooks in `lua2cpp_*` found when they load (nn::ro hooks); native version in the runtime today |
| lib_paramconfig `libparam_config.nro` | 6.5 | github.com/CSharpM7/lib_paramconfig | none declared | hooks on param getters; ARCropolis `is_mod_enabled` |
| nro_hook `libnro_hook.nro` | 0.4.0 | github.com/ultimate-research/nro-hook-plugin | none declared | hooks nn::ro::LoadModule (the runtime tells plugins about loaded modules itself) |
| The CSK Collection `libthe_csk_collection.nro` | 5.0.6 | gamebanana.com/mods/499008 | CC (no re-uploads) | ARCropolis file callbacks (rewrites ui_chara_db etc. as they load), inline hooks, code patches |
| One Slot Effects `libone_slot_eff.nro` | 13.0.5 build | gamebanana.com/mods/549058 | CC | ~30 inline hooks + 3 code patches at fixed 13.0.5 addresses |
| Smashline 2 / One Slot Effects compatibility edit | 1.4.1 | gamebanana.com/mods/532968 | CC | (an older Smashline build that worked with older OSE; not needed with the 13.0.5 OSE) |
| One Slot Victory Theme (SL2) | - | gamebanana.com/mods/38834 | CC | nn::ro LookupSymbol + one hook |
| Additional Slots Victory Fix | - | gamebanana.com/mods/510524 | CC | LookupSymbol only (2 KB of logic) |

Downloads and SHA-256s: `tools/std_plugins.py list` (new). It fetches them from those official
release URLs into `<tree>/skyline/plugins/` and checks each hash, rather than WindowSmash shipping
them: the GitHub ones declare no licence, and GameBanana's pages don't allow re-uploads.

## 3. Bugs found and fixed on the way (all in the clone)

1. **Plugins that crashed the game at boot: Shadow Mewtwo, Dark Meta Knight** (and, most likely,
   Z Broly Lite's "format! path length" bug). Rust's integer-to-text code is built with LLVM's
   machine outliner: the compare that ends its digit loop sits in a little helper reached by `bl`,
   and the caller branches on the flags the helper set. The recompiled C kept the flags in each
   function's own variables, so the loop never ended and wrote digits down the stack until it ran
   off the end (bus error at 0x777feefffc, just below the main thread's 1 MB stack).
   Fix (`tools/lift.py`, plugins only, so the game's own C is unchanged): flags go out of a function
   at its returns, calls and tail calls and come back after a call and at its entry, carried in
   bits 28-31 of the CPU context's FPSR (reserved on AArch64), so no change to `recomp.h`, no
   rebuild of the game, and it builds the same with clang-cl. Both plugins' `main` now run to the
   end.
2. **Sonic Re-Imagined stopped the game** ("indirect call to sonic_reimagined+0x120d8, which is not a
   function entry", in its ARCropolis event callback): a jump table whose address was set up before
   an outlined-helper `bl` wasn't recognised, because the analysis assumed any call changes x9.
   `lift.py` now looks into the called helper: a short run to `ret` that calls nothing and doesn't
   write the register keeps it.
3. **New characters broke the select screen** (one column of 4 tiles, nothing selectable; with more,
   the game aborted in the menus). Two causes:
   - a place (`disp_order`) past the last fighter's (86): Sans asks for 106, Springtrap for 88.
     `tools/csk.py` now puts such an entry right after the last fighter. Two entries at one place
     are fine (they sit side by side, as Shadow does next to Sonic).
   - **more than 4 new characters at once**: with 5 (any 5 of Shadow, Sans, Shadow Mewtwo,
     Blaziken, Springtrap, Dark Meta Knight, Paper Mario) the game aborts while loading the menus
     (`abort` from main+0x19f1550). That's a fixed-size table in the game that the real CSK
     Collection patches; the runtime's imitation of CSK doesn't. Until CSK runs for real (see 4),
     turn on at most 4 new-character mods at a time.
4. **`skyline.py probe`** (new): starts the game once for the plugins' select-screen entries and
   applies again if they changed. The Mods app's Apply alone uses the entries from the last start,
   so a newly turned-on new character only shows after a probe (the "Build plugins" button does it).

5. **Blaziken crashed in about 2 of 3 matches** ("terminating with uncaught exception ... L2CExceptionShutdown"):
   when a move is cut short the game throws an exception through the plugin's script, and the unwind
   tables `tools/plugin_eh.py` makes for plugins missed a prologue form (`str d8, [sp, #-0x30]!`,
   a float register saved first), so the unwinder gave up. Fixed; 3 of 3 runs pass. Any plugin
   whose scripts start that way had the same problem, in your game too (Blaziken's moveset is off
   there). A single bench run can miss a crash like this: it depends on which move gets interrupted.
6. `getRegionAddress` now answers for rodata and data too (the CSK Collection asks).

## 4. Running the real plugins: what was done, what's left

New machinery (tools + runtime, all platform-neutral C/Python):
- **Standard plugins**: `.nro`s in `<tree>/skyline/plugins/` load first, before mods' plugins, keep their own
  exports (the runtime's native versions of the same names step aside), and replace any copy a mod bundles.
- **Code patches from plugins**: a plugin's `sky_memcpy` into game code is recorded
  (`dump/skyline/patches.txt`) and lifted into the next build, like the inline hooks already were; the
  module is regenerated when they change. A patched branch into plugin code goes through the dispatch table.
- **ARCropolis file callbacks** (`arcrop_register_callback`, `arcrop_load_file`): they run at the probe on
  each file as the mods make it (`dump/skyline/arc/<hash>.in`, written by `mods.py apply`), and
  `mods.py` puts the result in at the next apply. `skyline.py probe` repeats probe/apply until stable.
  (Tested only as far as the CSK probe below: the callbacks registered; none ran to the end.)

| Plugin | Status | Evidence / what's missing |
|---|---|---|
| One Slot Effects | **runs** | 58 inline hooks + 8 patched instructions baked into `main`; its log says "adding eff data for slot c120"; Blaziken's flame effects show (frames in `bench/ose_blaziken` vs `bench/captain_120_vs_mario_0`). `mods.py` adds costumes' `ef_*_cXX.eff` files when it's installed |
| Smashline 2 (real) | set-up runs half way, then crashes | it hooks the same agent factories as our native version, `nn::ro::LoadModuleInternal`, and 9 inline points, then reads `nn::ro::detail::g_pAutoLoadList` (the list of loaded modules), which the runtime doesn't have. Needs: the RO module lists, LoadModuleInternal called through the hookable guest function, and a second build after a match for the inline hooks it puts in `lua2cpp_*`. **Worth it**: it's what the 5 failing movesets need (weapon cloning) |
| CSK Collection (real) | set-up runs, then crashes | registers 256 file callbacks and its inline hooks, but 8 of its function hooks land mid-function: **CSK 5.0.6 is built for game 13.0.4** (code past main+0x1788000 moved 0x5B0 in 13.0.5). Needs an address map 13.0.4 -> 13.0.5 for it. It's what lifts the 4-new-characters limit |
| lib_paramconfig, nro_hook | not run separately | lib_paramconfig is linked into the movesets themselves; nro_hook's job (telling plugins about loaded modules) is part of the Smashline work |
| ARCropolis | not run | `mods.py` does its job (and passes the bench for every skin mod). Running it would mean its file system hooks into the game's resource loader plus `sd:/ultimate/mods` - a large change to the part that works best today. Do it after Smashline/CSK, and only if `mods.py`'s remaining bugs (unsharing freezes) keep coming |

## 5. Windows

All new C is plain C11 + the Win32 calls the runtime already uses (`CreateThread`, `Sleep`, `ExitProcess`):
`runtime/hle_bench.c` is picked up by the Windows build's `hle*.c` pattern; the plugins' flag passing uses
the CPU context (no thread-locals, no `recomp.h` change, so no game rebuild). **Not built on Windows here.**
`tools/dev/bench.py` is macOS-only (it uses `open -n`); the runtime side works anywhere. Your friend's
`plugin-build.txt` hasn't arrived, so the Windows plugin build failure is still unknown.

## 6. If you install this into your game

Code: `runtime/hle_bench.c` (new), `hle_hid.c`, `hle_skyline.c`, `host.c`; `tools/lift.py`, `recomp.py`,
`plugin_eh.py`, `skyline.py`, `mods.py`, `csk.py`; new `tools/std_plugins.py`, `tools/dev/bench.py`.
What changes for you:
- the plugins you have get rebuilt with the fixes (Blaziken stops crashing; Shadow Mewtwo/Dark Meta Knight
  stop crashing at boot - they'll then hit the weapon-cloning problem instead);
- with `tools/std_plugins.py fetch libone_slot_eff.nro`, costumes' one-slot effects work (a second
  "Build plugins" is needed: the first records its hooks, the second builds them in);
- new characters asking for a select-screen place past the last fighter (Sans) stop breaking the grid;
- `main` is regenerated once (hook points), ~2 min of compiling at 2 jobs.
Unchanged: the 4-new-characters limit, the weapon-cloning failures, the Paper Mario/Springtrap/Dark Meta
Knight/Shadow Mewtwo loading hangs. Keep at most 4 new-character mods on.

## 7. Next steps, in order

1. Real Smashline: RO module lists + LoadModuleInternal through a hookable function + the after-a-match
   rebuild for its `lua2cpp_*` inline hooks. Bench Sans/Paper Mario/etc. after.
2. CSK: a 13.0.4 -> 13.0.5 address map for plugins built for 13.0.4 (start with the 0x5B0 shift past
   main+0x1788000 that Smashline itself uses), then the file callbacks end to end.
3. Mods menu: "Build plugins" runs `std_plugins.py fetch` + two builds; warn at more than 4 new characters.
4. Bench: 2 runs per character by default (intermittent crashes), and a Windows runner.

