# Skyline plugins: moveset and new-character mods

Status, 2026-10-23: plugins are part of the mod manager's flow (any mod's plugin.nro), and new characters work: Shadow the Hedgehog (GameBanana 578451) plays in the user's build. Tested on macOS; Windows untested. Prototype tree: `~/WindowSmash-skyline`
(an APFS clone of `~/WindowSmash`). Nothing here is in the playable tree yet.

GameBanana's moveset mods and new characters for Super Smash Bros. Ultimate are two things in one
download:

- **files** for ARCropolis (models, animations, effects, sounds, UI, `config.json` for new costume
  slots), which WindowSmash already installs (`tools/mods.py`, the Mods app), and
- **code**: a Skyline plugin, `plugin.nro`, Switch arm64 code built with skyline-rs. It replaces the
  fighter's scripts through **Smashline 2** and often uses **lib_paramconfig** (fighter params),
  and, for new characters, the **CSK Collection** (new character-select entries) and ARCropolis'
  API.

WindowSmash already recompiles the game's own NROs (the `lua2cpp_*` fighter modules) to C ahead
of time and loads them natively (`tools/recomp.py`, `runtime/hle_ro.c`). A plugin is the same
kind of file, so the plan is: recompile each plugin the same way, and provide what Skyline,
Smashline and the other dependency plugins provide on the console natively in the runtime.

## Survey (GameBanana, 2026-10-23)

Most downloaded in Gameplay (category 26521) are movesets; Skyline (3322) holds the dependency
plugins. The three studied (downloads kept in this session's scratchpad, not in `mods/`):

| Mod | GameBanana | Plugin | What it is |
| --- | --- | --- | --- |
| Sonic Re-Imagined v3.2 (Modern Mode, hotfix) | 443018 | 172 KB, `sonic_reimagined` | moveset over Sonic, all costumes |
| Waluigi Moveset | 443150 | 664 KB, `Waluigi_Moveset` | new character: Terry (`dolly`) costumes c120-c127 + a CSS entry |
| Silver Moveset | 470041 | 610 KB, `Silver_Mewtwo` | new character: Mewtwo costumes c128-c135 + a CSS entry |

A "new character" is not a new fighter kind: it is extra costume slots of an existing fighter
(c120+, which `mods.py` already installs as new slots from `config.json`), a new
character-select entry made by the CSK Collection plugin, and the moveset code limited to those
costumes (Smashline's `*_costume` calls).

### What the plugins import

| | Sonic | Waluigi | Silver |
| --- | --- | --- | --- |
| game `main` exports (`app::sv_animcmd::*`, `app::lua_bind::*Module__*_impl`, `lib::L2CValue`, `lib::L2CAgent`, malloc/free) | 124 | 129 | 136 |
| `lua2cpp_common` exports (`L2CFighterBase::change_status`, ...) | 1 | 3 | 1 |
| SDK exports (nn::fs, nn::os, nn::ro::LookupSymbol, pthread, memcpy...) | 23 | 43 | 44 |
| Skyline (`A64HookFunction`, `getRegionAddress`, `sky_memcpy`, `skyline_tcp_send_raw`) | 3 | 4 | 3 |
| CSK Collection (`add_chara_db_entry_info`, `add_chara_layout_db_entry_info`, `add_bgm_db_entry_info`, `add_narration_characall_entry`, `set_fighter_jingle`, ...) | - | 10 | 10 |
| ARCropolis (`arcrop_register_event_callback`), Skyline `get_program_id` | - | 2 | 2 |

Smashline 2 is not imported by symbol: each plugin finds `smashline_install_acmd_script`,
`smashline_install_status_script`, `smashline_install_line_callback` and
`smashline_install_state_callback` (and the `_costume` variants) at run time with
`nn::ro::LookupSymbol`. lib_paramconfig's code is linked into the plugins themselves (its hooks
on `FighterParamModule` getters and some vtables are the plugin's own `A64HookFunction` calls).
Plugins check that their dependencies exist (`rom:/skyline/plugins/libsmashline_plugin.nro`,
`libnro_hook.nro`, `libarcropolis.nro`, `libparam_config.nro`, `libthe_csk_collection.nro`).

The dependency plugins themselves (latest releases):

| Plugin | Imports | How it works on the console |
| --- | --- | --- |
| Smashline 2 v1.6.8 | Skyline 5 (incl. `A64InlineHook`), main 11, lua2cpp_common 12, SDK 59 | hooks the game's agent factories (main+0x64c310 ...), lua2cpp_common's coroutine call and status lines with **inline** hooks at single instructions |
| lib_paramconfig 6.5 | Skyline 3, ARCropolis 2, main 20 | hooks param getters; reads `config_param.toml` from each mod folder |
| CSK Collection 5.0.6 | Skyline 5 (incl. `A64InlineHook`), ARCropolis 6 (`arcrop_register_callback`: rewrites files as the game loads them) | patches `ui_chara_db` and friends at load time |

## How plugins run in WindowSmash (prototype)

1. **Recompile the plugin** like a game NRO: copy `plugin.nro` into `dump/nro/`
   (`recomp.py` names and places it, `bases.txt`), then `recomp.py image` and `recomp.py build`
   for it, and `tools/mac/build_mac.py` compiles and links it in. Changes the recompiler needed
   for Rust plugins (all in `tools/`):
   - functions: plugins are built with `panic=abort` and have **no unwind table**, which the
     recompiler used for function bounds; `recomp.tail_entries` finds functions reached only by
     tail branches (a branch that leaves its function's stretch makes a new entry, to a fixpoint).
     Coverage went from 84% of functions to 99.9% (the rest: 128-bit atomic copy helpers).
   - jump tables: LLVM's small tables (`adr` anchor, 1- or 2-byte entries, `add x, x, w, lsl #2`)
     in `lift.analyze_tables`.
   - registers: Rust code uses LLVM's **machine outliner** (shared helper sequences reached by
     `bl` that read and write the caller's callee-saved registers), so the register cache
     (`tools/regcache.py`) is off for modules without an unwind table.
2. **Load it** as Skyline does: when the game mounts its RomFS (`nn::fs::MountRom`), every module
   named in `dump/skyline/plugins.txt` is loaded (`rt_load_nro_named`) and its `main` export run
   (`runtime/hle_skyline.c`).
3. **Skyline's API** natively (`hle_skyline.c`):
   - `A64HookFunction(target, replace, &original)`: the recompiled game reaches a function either
     directly (a C call compiled in) or through the dispatch table (`rt_lookup`: virtual calls,
     function pointers, calls between modules). `rt_hook_function` (host.c) swaps the dispatch
     table's entry, so every indirect call goes to the plugin, and hands back a native alias of
     the original. Functions that are only called indirectly (the agent factories, status
     functions, vtable entries) hook this way with **no rebuild of the game**. Directly called
     ones need the function wrapped at build time (the existing `HLE_WRAP` mechanism, made from
     a list of hook targets) - see the plan.
   - `A64InlineHook` (a callback at one instruction, with the registers): not possible at run
     time in recompiled code; logged and left out. Smashline's own inline hooks are replaced by
     natively hooking the functions around them (below).
   - `getRegionAddress`, `sky_memcpy` (data writes work; code writes mean nothing to recompiled
     code - logged), `skyline_tcp_send_raw` (the plugin's log), `get_program_id`.
   - `nn::ro::LookupSymbol` answers from the runtime's symbol table, which includes the native
     Smashline functions; `nn::fs::GetEntryType` reports the natively provided dependency plugins
     as present.
4. **Smashline 2 natively** (`hle_skyline.c`), instead of recompiling Smashline itself (which is
   built on inline hooks):
   - ACMD (game/effect/sound/expression scripts): the eight agent factories in main are hooked
     through the dispatch table; after the game makes a fighter's agent, the plugin's scripts for
     that fighter and category go in with `lib::L2CAgent::sv_set_function_hash`. The game runs
     ACMD functions as `fn(agent, variadic)`, and a plugin's `fn(agent)` is call-compatible.
   - status scripts: the status agent factory (main+0x64bbd0) is hooked; each agent gets a copy
     of its class's vtable whose `set_status_scripts` (slot 9) runs the original and then
     `L2CAgentBase::sv_set_status_func(status, line, function)` for the plugin's scripts.
   - line callbacks of the main loop (per-frame "opff" code): lua2cpp_common+0x3250 (the status
     line call) is hooked when lua2cpp_common loads; after it, each callback for that fighter (or
     for every fighter) runs.
   - not yet: costume-limited scripts are applied to every costume; other status lines' callbacks;
     state callbacks; new status kinds beyond the fighter's count (global table entry 0xC);
     weapons (articles); `smashline_get_original_*`.

5. **Unwind tables for the plugin** (`tools/plugin_eh.py`): the game's ACMD coroutines clean up
   a script that is cut short (a move interrupted) by throwing a C++ exception from
   `L2CAgentBase::coroutine_yield` through the script, and the game's libunwind needs a frame
   description for every guest frame in between - it finds a module's `.eh_frame_hdr` through
   its MOD0 header (`nn::ro::detail::GetExceptionInfo`). Rust plugins have none. The recompiler
   now makes one from each function's prologue (stack size, frame pointer, saved registers;
   outlined prologue helpers followed), puts it after the plugin's image and points MOD0 at it.
   (On the console Smashline avoids this with its own interpreter and a hook on libunwind's
   step function.)

## Results (prototype, `~/WindowSmash-skyline`, Sonic Re-Imagined v3.2 Modern Mode + hotfix)

Verified in test runs (muted copies, scripted input; logs and screenshots in that session's
scratchpad `runs/sky*`):

- the plugin is recompiled (1,253 functions, 29,245 instructions; 14 untranslated: the 128-bit
  atomic copy helpers, never called) and loaded at the RomFS mount; its `main` runs to the end.
- it finds Smashline through `LookupSymbol`, passes its dependency check, hooks one game
  function (main+0x20876e0 -> its `sonic_airboost_once`, which has no direct callers, so the
  dispatch-table hook catches every call), and installs **103 ACMD scripts** and 8 status Main line callbacks (39 game, 23 effect, 16 sound and 17 expression scripts go into each of Sonic's
  agents of that kind; the other 8 are for other agents, not applied: weapons are not done yet). It installs no status scripts.
- in a match (Sonic vs a level-9 CPU, random stages): the factory
  hooks put the scripts into Sonic's agents (`sonic's game agent ...: 39 acmd scripts from
  plugins`); a plugin script runs inside the game's ACMD coroutine (seen in a backtrace:
  `lua2cpp_common call_coroutine -> sonic_reimagined+0x6d38 -> app::sv_animcmd::frame`); all 8
  per-frame callbacks run; the match plays to the results screen at 60 fps with jabs,
  specials, aerials and dash attacks pressed.
- before the unwind tables, the first interrupted move stopped the game (svcBreak from
  libunwind: no frame description for sonic_reimagined+0x6d38); with them, no stop in two
  full runs.

Not verified yet: that each modded move looks and hits as on the console (needs a person
playing it next to a video of the mod); status scripts (Sonic uses none - the vtable slot 9
for `set_status_scripts` is from Smashline's source and untested); costume-limited scripts;
the small-jump-table and function-finding changes on the game's own modules (they only run
for modules without an unwind table, except the jump-table form, which also applies to the
game's modules the next time they are regenerated - check that a full regeneration of `main`
gives the same C before shipping).


## Using it (any mod)

1. Install the mod as usual (Mods app, or `tools/mods.py install` / `gamebanana`). Its `plugin.nro`
   stays in the mod's folder.
2. `tools/mods.py apply` (the Mods app's Apply) puts the files in and runs `tools/skyline.py sync`:
   each turned-on mod's plugin is copied into `dump/nro/`, placed and recompiled to C, and listed in
   `dump/skyline/plugins.txt`. The dependency plugins mods ship (Smashline 2, lib_paramconfig,
   ARCropolis, nro_hook, the CSK Collection) are left out: the runtime provides them.
3. `tools/skyline.py build [--jobs 2]` makes them part of the game: regenerates the C of any game
   module that gained hook targets (`dump/skyline/hooks.txt`; main takes ~1 min), compiles and
   links (runtime + plugins + changed files, a minute or two), starts the game once in probe mode
   (`SSBU_SKYLINE_PROBE=1`: exits as soon as the plugins have loaded) to record what they add to
   the select screen (`dump/skyline/csk.json`), and applies the mods again if that changed.
   `tools/skyline.py status` lists what is listed and built in.

## New characters (the CSK Collection, natively)

A new character is a new select-screen entry for extra costume slots of a fighter (Shadow: Wolf
c120-c127). The runtime implements the CSK Collection's API: `add_chara_db_entry_info` and
`add_chara_layout_db_entry_info` structs are decoded (the_csk_collection_api's Rust layouts;
hashbrown maps for the cXX/characall fields) into `csk.json`, and `tools/csk.py` (run by
`mods.py apply`) adds them to `ui_chara_db.prc` / `ui_layout_db.prc` (a copy of the fighter's
entry, the plugin's fields set). CSK's `color_start_index` is not a game field: the runtime does
CSK's redirect itself - at the fighter set-up (main+0x66dcf0) a player whose record names a new
character gets costume start+k for color k, and the portrait-path function (main+0x3238a70) maps
c120+k back to the character's own portraits (the HUD). Both are called directly by the game, so
they are on the hookable list (`NATIVE_HOOKS` in skyline.py).

The CSK Collection itself (5.0.6) is built for game 13.0.4: in 13.0.5 the code from about
main+0x1788000 up moved by 0x5B0 (Smashline's `+ 0x5B0` offsets), and most of its work is code
patches and inline hooks - it can't run as a plugin here, hence the native version.

ARCropolis' API: event callbacks (run once the plugins' mains have: Shadow registers its entries
from one), `arcrop_is_mod_enabled`, versions. `mods:/` (ARCropolis' view of all turned-on mods'
files, a later mod winning; the runtime reads the order from `dump/skyline/mods.txt`) and `sd:`
(with `sd:/ultimate/mods` = the mods folder, for lib_paramconfig's config_param.toml) are mounted.

## Recompiler and runtime changes made for plugins (beyond the first prototype)

- hookable functions: `recomp.py` makes each hook target in `dump/skyline/hooks.txt` reachable
  through the dispatch table from direct calls too (`f_X` -> `rt_lookup(X)`, the body as `f_X_orig`);
  per-module fingerprints, so only the modules concerned are generated again.
- CRC32 instructions (crc32b/h/w/x, crc32c*: plugins compute Hash40s with them) and the
  `fcvtn`/`fcvtl` 64-bit-destination forms.
- getting the order of hashbrown maps right, Smashline costume rules (a slot a plugin claimed
  gets only that slot's scripts), weapons' agents, new status kinds (the status count),
  `smashline_get_original_acmd/status`, `smashline_get_target_function`.

## Known limits

- inline hooks (`A64InlineHook`) and code patches (`sky_memcpy` to code) are logged and left out;
  so are ARCropolis file callbacks, Smashline state callbacks and status-line callbacks other than
  Main, one-slot effects (`ef_<fighter>_cXX.eff`: the costume uses the fighter's effects), CSK's BGM,
  stage and narration tables.
- `mods.py`'s unsharing breaks match loads when a mod with unshared files (here Mr. L: 14 Luigi
  files) is on together with a new character's new files: the match stays on the loading screen.
  Workaround used for the user's mods: `mods/Mr. L/config.json` with an `unshare-blacklist` of
  Mr. L's files (as for Dr. Eggman). The real fix is in `Table.unshare` (see the earlier notes).
- Windows: the same code builds with clang-cl in principle (`skyline.py build` runs
  `recomp.py all` there), but nothing of this was run on Windows.
- Online: everyone in a room needs the same plugins and mods.

## Plan (first version; steps 1, 2, 4 and part of 3 done since)

1. **Tooling**: `tools/skyline.py` (or part of `mods.py apply`): for each enabled mod with a
   `plugin.nro`, copy it into `dump/nro/`, generate its C, write `dump/skyline/plugins.txt`, and
   rebuild only the runtime and the plugins (minutes, `--jobs 2`). The Mods app shows "needs a
   rebuild" for code mods. Plugin builds can be cached by build id like the game's NROs.
2. **Hooks on directly called functions**: run each plugin once (or read its hook calls from a
   log) to list its `A64HookFunction` targets; `recomp.py` wraps those functions at build time
   (as `HLE_WRAP` does: callers reach a dispatcher that checks the run-time hook). That rebuilds
   the C files holding those functions only.
3. **Smashline completeness**: costume filtering (needed for new characters), status-count
   growth for new statuses, weapons/articles, the other status lines (needs the call sites
   inside lua2cpp_common wrapped, as Smashline's inline hooks are), state callbacks, the
   `smashline_get_original_*` calls.
4. **New characters**: the CSK Collection API natively - `add_chara_db_entry_info` and friends
   are data (the new character-select entry, layout, BGM, narration); the runtime records them
   while plugins load, and `mods.py` (which already merges `.prcxml` into `ui_chara_db.prc`) or
   the file layer applies them. ARCropolis' `arcrop_register_event_callback` /
   `arcrop_register_callback` need a hook in the runtime's file reads.
5. **lib_paramconfig**: works as part of each plugin once its hooks reach their targets (it
   hooks `FighterParamModule` getters - check whether those are called directly) and it can read
   `sd:/ultimate/mods/<mod>/config_param.toml` (an `sd:` mount pointing at the mods folder).
6. **Windows**: the same C and runtime build with clang-cl; `rt_hook_function` and the loader
   are platform-independent.
7. **Online**: plugins change gameplay; everyone in a room needs the same plugins (the mod
   manager's NOT ONLINE SAFE label already applies to code mods).

Mods it unlocks, in order: movesets over an existing fighter that use only ACMD + status +
main-loop callbacks (Sonic Re-Imagined and most mRama movesets: Knuckles, Funky Kong, Kamek,
Bandana Dee, Silver/Waluigi once costume filtering and CSK are in), then new characters
(CSK), then mods that depend on inline hooks.
