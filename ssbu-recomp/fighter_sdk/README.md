# Fighter SDK

Fighters as C++ source, and mods. A fighter library replaces the lifted functions of one script module
(lua2cpp_<name>) with C++ that computes the same thing; a mod hooks any function of the game through
`include/hook.h`. The tools here write that C++ from the game's script modules, build it and test it against the
lifted game.

Nothing of the game is in this folder. The tools read your own copy of the game and public projects about it
(see [Reference data](#reference-data)), and everything they make from them goes to the work folder, never next
to the tools.

```
include/            fighter_sdk.h (L2CValue, L2CAgent, the ACMD macros), hook.h (mods), mod_ga.h, registry.h
src/fighter_rt.cpp  run-time linked into every fighter library
examples/           mod_example.cpp: a complete mod
tools/              decompilers, SDK generator, build and test drivers
fighter_host.c, hook_host.c, newmodule_host.c   the game's side (built into the runtime by nnro.c)
```

## Folders

`tools/sdk_paths.py` finds everything. Each variable below is optional.

| | what | default |
|---|---|---|
| `SSBU_FIGHTER_WORK` | the work folder: `fighters/`, `obj_fighters/`, `fighters_bin/`, `mods/`, `tests/logs/` | the folder `fighter_sdk/` is in (`ssbu-recomp/` in the repository, `build/work/` once `scripts/setup.py` has staged it) |
| `SSBU_REPO` | the repository, for the compiler settings | the first folder above the tools with `tools/cc.py` and `tools/isa.py` |
| `RECOMP_BIN` | the folder with `main.elf` | `../binaries/13.0.4/`, then `../binaries/13.0.5/`, next to the work folder |
| `SSBU_FIGHTER_NRO` | the folder with the script modules | `ro_study/` in the work folder, then `dump/nro/` in the repository |
| `SSBU_FIGHTER_REFERENCE` | the folder with the public projects | `deps/reference/` in the repository, then `../reference/` next to the work folder |
| `SSBU_FIGHTER_USERDATA` | a save folder for `probe.py` | `../userdata/` next to the work folder |
| `S2_EXE`, `PROBE_EXE` | the game program the offline tests start | `ssbu_s2.exe` / `ssbu_s4.exe` in the work folder |

`SSBU_FIGHTER_BIN`, `SSBU_FIGHTERS_ROOT`, `SSBU_SDK_DIR` and `SSBU_FIGHTER_SDK` work as before
(`docs/MODDING.md`). When the tools' notes say `out/`, they mean `fighters/tools_out/` in the work folder.

A tool that misses an input stops and says what it looked for, where, and what makes it. The decompilers and
`census.py` need numpy and capstone; the builds and the path tools need only Python.

## Building

```
python fighter_sdk/tools/build_fighter.py <name>                      fighters/<name>/ -> fighters_bin/<name>.dll
python fighter_sdk/tools/build_mod.py <mod.cpp> [<folder>]            -> <folder>/<mod>.dll (default mods/)
```

The library is `<name>.dll` on Windows, `<name>.so` on Linux and `<name>.dylib` on macOS. Mods are loaded on
Windows only for now (`hook_host.c`).

A fighter's code replaces lifted code, and the fight hash of a match with source fighters is compared with the
lifted game's. So fighters and mods are built by the compiler that builds the game, with its target and
floating-point flags (`tools/fighter_cc.py`, which only imports the root build's modules):

| | before | now |
|---|---|---|
| Windows | `C:\llvm-8.0.0\bin\clang.exe` (LLVM 8), no target flags | clang-cl from `tools/cc.py`, `/winsysroot`, the instruction set `tools/isa.py` picks |
| Linux | not built | clang from `tools/linux/toolchain.py`, its target and instruction set, `-fPIC` |
| macOS | not built | Apple clang from `tools/mac/toolchain.py`: `-arch arm64 -mcpu=apple-m1`, macOS 14 |
| all | `-std=c++14 -O1 -fno-exceptions -fno-rtti -Wall` | the same, with `-fno-strict-aliasing`, then `-fno-fast-math -ffp-contract=off` last |

`-ffp-contract=off` is the one that matters: without it clang fuses `a * b + c` into one multiply-add on arm64
and with `-mfma`, which rounds once where the game rounds twice. `-fno-fast-math` comes before it because it can
put the contraction back to the default. Of the root's flags the SDK leaves out `-w` (it shows warnings),
`-fno-pic` and the runtime's include folder.

`obj_fighters/<name>/compile.json` keeps the compile command. When it changes (another compiler, instruction set
or flag) every object is compiled again.

## Reference data

What each tool reads that is not part of the SDK. None of it is in the repository or may be added to it
(`.github/scripts/check_no_game_data.py` refuses game files, and the ignore files keep the generated sources out).

From your own copy of the game:

| file | version | tools | where it goes |
|---|---|---|---|
| the script modules `lua2cpp_*.nro` (98) | 13.0.4, the build the tools were written against (see below) | `acmd_decomp`, `l2c_decomp`, `status_decomp`, `status_list`, `fighter_status`, `census`, `gen_sdk`, `luaconst`, `try_hash`, `signals`, `nrodis`, and `batch` through them | `ro_study/nro/` with `ro_study/nro_map.json` (`scripts/setup.py`), or `dump/nro/` with `bases.txt` (`tools/build_game.py`), or `SSBU_FIGHTER_NRO` |
| `main.elf`, the main program converted by `scripts/setup.py` (step convert) | 13.0.4 or 13.0.5 | `main_adrp`, the offline tests of `batch`, `fighter_status` and `probe`, `new_fighter` (through the lifter) | `../binaries/<version>/` next to the work folder, or `RECOMP_BIN` |
| `data.arc` | the one your dump has | `new_fighter` (model, motion and parameter files of the donor) | where `arc_extract.py` of `tools/nro/` looks (`romfs/data.arc` beside the staged work folder), or `SSBU_ARC` |
| your save folder | any | `probe` (copied before each run, never written) | `../userdata/` next to the work folder, or `SSBU_FIGHTER_USERDATA` |
| `ga_13_0_5.txt`, a map of 13.0.4 to 13.0.5 function offsets | made from both `main.elf` files | mods that include `mod_ga.h`, when 13.0.5 runs | the mod's folder |
| the game program, built by `scripts/setup.py` | | `batch`, `fighter_status`, `probe` | the work folder, or `S2_EXE` / `PROBE_EXE` |

`luaconst.py` reads the constant key table of `lua2cpp_common` at offset 0x6a5b70, 0x4c0d keys: offsets of the
13.0.4 module. `docs/VERSIONS.md` counts 98 script modules in both builds; whether they are the same bytes has not
been checked, so run the decompilers on 13.0.4's modules.

Public projects (clone them yourself; the SDK does not include them):

| file | project | tools |
|---|---|---|
| `skyline-smash/src/cpp.rs` | ultimate-research/skyline-smash | `gen_sdk` (the game functions' argument types) |
| `skyline-smash/src/lua_const.rs` | ultimate-research/skyline-smash | `luaconst`, `try_hash` (the Lua constants' names) |
| `param-labels/ParamLabels.csv`, `param-labels/motion_list/Labels.txt`, `param-labels/sqb/Labels.txt` | ultimate-research/param-labels | `census` (names of hashes) |

They go in `deps/reference/` in the repository (ignored), `../reference/` next to the work folder, or the folder
`SSBU_FIGHTER_REFERENCE` names.

What the tools make stays in the work folder: `fighters/` (sources, `sdk_generated/`, `tools_out/`, `catalog/`),
`obj_fighters/`, `fighters_bin*/`, `mods/`, `tests/logs/` and `new_fighter`'s characters folder.

The `brawl/` tools read only the files you name on their command line.

## Tests

`tests/test_fighter_sdk.py` at the repository root, with made-up inputs only: the paths, the compile and link
lines for each system, the pure-Python parts (mangled names, hashes), and on a Mac a made-up fighter and the
example mod built and run.
