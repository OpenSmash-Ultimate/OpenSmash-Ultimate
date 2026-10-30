# Mod sweep, night of 2026-10-23

What was checked: tonight's two problems in `logs/game-log-20261023-012352.txt`, every installed fighter mod
in a test copy of the game (muted, offline, its own copy of the save and of `romfs.cfg`; your own mod
state and save weren't touched), and the mod menu.

**To do when you're back:** open WindowSmash Mods and press **Apply to game** once (with the game closed).
The fixed `mods.py` is installed but your game still reads the last apply, so the Pyra/Mythra freeze is
still there until you apply. Your mods are on and off exactly as you left them. (The Skyline session
applied once at 08:55 to add the Shadow moveset, the two "shadow - ..." mods. Shadow's tile sits next to
Sonic's, so the fighters after it moved one place along; Pyra/Mythra is still on the roster.)

## (a) The freeze loading a Pyra/Mythra match

**What the log shows.** Launches 1 and 2 each loaded Pyra/Mythra's code (`lua2cpp_eflame/elight/element`)
and then never started the match: the game kept drawing at 60 fps for 90 s and more with the loader
threads idle, no crash and no error, until the window was closed. Both times you were in an online room,
but the same freeze happens offline (below), so it isn't the network.

**Cause: a bug in `mods.py`'s "unsharing" (patch 0012), set off by Uncensored Pyra_Mythra, not the physics
mod.** Most of a fighter's costumes share files: c01-c07 read most of theirs from c00's folder or from the
fighter's common folder. When a mod changes a shared file for one costume, `mods.py` gave that costume its
own copy ("unsharing"). In some layouts that copy makes matches never finish loading. Uncensored changes
7 of c00's files that live in the common folder; their copies went into c00's folder, which the other
costumes read, and **every costume except c00 froze** (you play the swimsuit, c07).

Proof, offline in test copies (the VS screen's art was checked to know which costume loaded):

| Pyra/Mythra test | Old mods.py | Fixed mods.py |
|---|---|---|
| no mods, c00 | plays | - |
| your 14 mods, c00 | plays (2 runs) | plays (2 runs) |
| your 14 mods, c07 | **stuck** (4 runs) | plays (2 runs) |
| Swimsuit only, c07 | plays | - |
| Swimsuit + Exaggerated Breast Physics, c07 | plays | - |
| Swimsuit + Uncensored, c07 | **stuck** (3 runs) | plays |
| same, Uncensored's 7 files kept shared | plays | - |

The same bug hit other fighters, which I found by testing their other costumes:

| Mod | Froze | Its unshared files |
|---|---|---|
| Mr. L | **Luigi c01** (a vanilla costume) | 14 c00 files in c00's folder |
| Blaziken | **Captain Falcon c00** | one texture, a different copy for each of c01-c06 |
| Jotaro Kujo | **Terry c00** | 3 c04 textures |
| Dr. Eggman | Ganondorf c06 (earlier tonight) | already worked around by its `unshare-blacklist` |

**Fix (`tools/mods.py`):** a file stays shared, so the mod's version is used by every costume that shares
it, as in the older `mods.py` your friends have, when
1. the costume's folder is one other costumes read from (c00's, usually), or
2. one mod has different versions of the file for two or more costumes (Blaziken, Exaggerated Physics).

One costume's own copy is still made otherwise. That works for Swimsuit c07, Ultimate Shadow c07,
Wolverine c07 and Knuckles c01: those costumes load, and so do Pyra/Mythra's, Sonic's, Wolf's and
Incineroar's c00 next to them. **But it isn't safe everywhere**, and I couldn't find the pattern before
morning:
- Jotaro's 3 c04 textures still froze Terry's c00 with the fix;
- Galacta Knight's 12 c06 files froze Meta Knight's c00 (Galacta's own c06 loaded).

Both mods now keep those files shared through their `config.json` `unshare-blacklist`, as Dr. Eggman's
already did (Jotaro: backup `config.json.before-sweep`; Galacta Knight had no config.json, so it's new).
Terry c00, Meta Knight c00 and Galacta c06 play with that. **A new mod that unshares files can still
freeze its fighter's other costumes.** If a fighter's match won't finish loading, turn its mods off one by
one and apply; that finds it. A real fix needs the game's folder loader understood (follow-up). The apply
log says how many files were kept shared, and the mod's row in the menu shows an "N on all costumes"
badge.

Side effects, looks only: Uncensored's Mythra body shows on Pyra/Mythra's other costumes too (with their
own colours), Mr. L's and Fury Bowser's shared files show on Luigi's and Bowser's other costumes, Blaziken
uses one version of that texture on all of its costumes, and Jotaro's c04 uses its other costumes' versions
of those 3 textures. If you add a c08+ Pyra/Mythra slot mod next to Uncensored, the invisible Mythra that
unsharing was meant to fix can come back.

## (b) Stuck on the results screen after the online Joker match

**What the log shows.** Launch 3 (about 01:35): online match, Joker (you, with BULLY MAGUIRE over Joker
on) vs Bowser (sonicoftheday). The match ran, then the results screen never ended: the game kept drawing
at a steady 60 fps for 90+ s with no crash, no error and no network trouble logged, until you closed it.
The next launch had 449 fewer RomFS layers: Bully Maguire turned off.

**Cause.** Bully Maguire changes far more than Joker's look: his victory poses (`j02win*` motions), the
results camera, the results screen's stage (`stage/resultstage_jack/.../render_param.prc`), his victory
theme, Final Smash movie and results voice. In an online room every player's game plays the same match
and results screen in step; a player without the mod has a different results screen, so the games wait on
each other forever. Offline nothing waits on another game, which is why it worked offline before (it still
does: a test match with it loaded and played fine). You confirmed tonight's online errors came from
mismatched mods.

## The mod menu (WindowSmash Mods)

- **"Everyone needs it online" (new, orange 👥 badge):** mods that change more than the fighter's look get
  this badge, a clash-check entry and a line in the sidebar notice while they're on. That covers
  results-screen poses and camera, the results stage, any stage, movies, victory themes and body
  animations. The message: every player in an online room needs the same mod, or the match or results
  screen can hang. Yours: BULLY MAGUIRE over Joker. Victory poses used to count as "online safe". Swing
  physics count as looks only.
- **Overlap warning (new):** a physics/animation mod on the same costume as another mod's skin is now a
  yellow warning instead of an "info" line, one per pair of mods. Yours: Exaggerated Breast Physics on the
  costumes Uncensored and Swimsuit reskin. It says the physics was made for the game's own model and to
  turn the physics mod off first if a costume looks wrong or won't load.
- **"N on all costumes" badge** for files kept shared (above), after the next apply.
- **Fixed:**
  - Jotaro's and Bully's different Kirby copy hats were reported as "both skins for kirby c00".
  - A mod whose config.json only holds an `unshare-blacklist` (Dr. Eggman) was labelled "new costume
    slots".
- **The server ends with its window** (your friend's Windows report: two servers left running):
  - the page tells the server when its window closes, and the server exits 15 s later (a reload pings
    first);
  - it really exits (`os._exit`), so nothing keeps `manager.log` open;
  - opening the app while one is running shows the running one instead of starting a second
    (`dump/mods/server.json`).

  Tested on macOS; not run on Windows.

Backups of every changed file: `<file>.before-sweep` (tools/mods.py, tools/modmanager/packs.py,
server.py, ui/app.js, ui/app.css, mods/Jotaro Kujo/config.json). The Mods app's Swift window didn't
change, so it wasn't rebuilt; the new menu shows the next time WindowSmash Mods opens (quit it first if it's
open). Patch for teammates: `~/Downloads/windowsmash-mac-patches/0014-Mods-sweep-*.patch` (made against patch 0012, without the Skyline session's 0013; where both change `tools/mods.py`, apply 0013 first and merge by hand if a hunk doesn't fit).

## Each mod (test copy, offline, your 14 mods on, fixed mods.py unless it says old)

| Mod | Tested | Result |
|---|---|---|
| Uncensored Pyra_Mythra + Swimsuit + Exaggerated Physics | Pyra/Mythra c00, c07 | play (c07 froze with the old mods.py) |
| Mr. L | Luigi c00, c01 | play (c01 froze with the old mods.py) |
| Fury Bowser + Giga Bowser | Bowser c00, c01 | play |
| Dr. Eggman over Ganondorf | Ganondorf c06 | plays (2 runs) |
| Galacta Knight | Meta Knight c06, c00 | c06 played twice, and once **crashed** while loading (GPU thread, bus error reading a texture: `reports/crash_20261023_043155.txt`; not repeatable so far). c00 **froze** until Galacta's files were kept shared (its new config.json); now c00 and c06 play |
| Ultimate Shadow the Hedgehog | Sonic c07 (+ c00) | play (2 runs) |
| Knuckles over Incineroar | Incineroar c01 (+ c00) | play |
| Jotaro Kujo | Terry c00 | plays with the 3-file blacklist; froze without |
| Blaziken | Captain Falcon c00 | plays (froze with the old mods.py) |
| The Warden | Steve c06 | plays (2 runs) |
| Wolverine | Wolf c07 (+ c00) | play (2 runs) |
| Kagamine Rin FT | Daisy c08 | plays |
| BULLY MAGUIRE over Joker (off; turned on in the test only) | Joker c00 | match plays offline; results screen not reached by the script |
| Jokaro Kujo, Wolverine - X-Men Mod | off | not tested |

Not tested: other costumes of all-slot mods (Jotaro c01-c07, Blaziken c01-c07), results screens (the
script's matches don't end), and anything online.

## Your friend's Windows crash (null read on guest thread 7)

The 4 identical crashes (`crash_20261022_2239*` to `2309*`) are on one of the game's resource-loader worker
threads (the same thread stack as the loader threads in your log), inside a per-object update called while
loading (main+0x3602380, a loop over 0x80-byte entries). So it happens while loading data and is most
likely a mod's file. The reports don't say which fighter or file was loading, and their play.log was
overwritten by later launches, so it can't be pinned down from what we have. Of their mods, only
**Jotaro Kujo** was on in both crash windows (22:39/22:40 with only the "ellp" pack, Jotaro + Jokaro;
22:51/23:09 with 15 mods). They're on the older mods.py without unsharing, so it isn't tonight's freeze.
Their minidumps (`reports\crash_*.dmp`) would show what was loading.

## How the tests were done (for next time)

- Test tree in the session scratchpad: symlinks to the game, copies of the save and shader cache, its own
  `mods/mods.json` and `romfs.cfg`; `mods.py apply --tree <test tree>`. One copy at a time under
  `/tmp/windowsmash-test.lock`, with a minute's gap after each run for the other session.
- **Picking any fighter dependably:** a test-only `ui_chara_db.prcxml` (a mod only in the test tree) swaps
  the fighter's `disp_order` with Pyra/Mythra's (80), so the fighter sits on Pyra/Mythra's tile. P1's cursor
  reaches that tile from its start with two `SUP/0.21` taps at about 56 s (character select is open by
  then). Costume: R presses while hovering, or L to go back (c07 = one L, c06 = two). Many R presses make
  the cursor drift, and taps sometimes overshoot, so each run checks the log for the fighter's
  `lua2cpp_<name>` and retries.
- The VS screen's art shows which costume was picked (the panel art can change after the pick).
- Keep the display awake (`caffeinate -d -w <pid>`): when the Mac's display sleeps, the test copy stops
  presenting frames and gets closed. One lost run tonight looked like a crash because of that
  (`reports/freeze_20261023_040059.txt` is from a test copy, not your game).
