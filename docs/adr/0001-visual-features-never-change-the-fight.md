---
status: accepted
---

# Visual features never change the fight

A feature that changes only what a player sees (picture shape, views, render scale, HUD placement)
must leave every fight exactly as the unchanged game computes it: **fight-identical**, frame for frame,
proven by the fight hash, not argued from the code. Online play depends on it: two players' games run
the same fight independently and rollback compares them every frame, so a feature that moves one
number in one player's fight desyncs both players, not just the one who turned it on.

## Why this is not obvious

The tempting way to make a wider picture is to tell the game its screen is wider. The game then
frames the fight for that shape itself, and it looks right. But the game's camera is part of the
fight: what is off-screen decides who takes off-screen damage and who is shown in a magnifying bubble.
Measured (docs/ULTRAWIDE.md, "The game: game camera"): widening the game's own camera made a scripted
32:9 fight differ from 16:9 at frame 578, in damage. So every view is built on top of the game's 16:9
camera, on copies, in what the renderer and the HUD receive, never in what the fight reads. That is
why the code takes the long way round.

## How a feature is shown to be fight-identical

1. **Offline, by replay.** A saved replay plays the same fight every time. Play it with the feature
   off and on (`SSBU_FIGHT_HASH=<file>` writes each frame's fight hash), then compare:
   `python tools/dev/fight_hash_diff.py <off>.fh.txt <on>.fh.txt`. The files must match up to GAME!
   (after it the results screen's victory pose is random, and fighters jump to x 1100 / 0). Use
   replays, not scripted live fights: scripted menu input lands a frame early or late now and then,
   which parts the fights on its own. The scripts are in `build\aspect_test\auto\`
   (docs/ULTRAWIDE.md, "The fight stays the same: how to check").
2. **Switching while playing.** If the feature can change mid-fight, the replay must also be played
   with it changing on a timer (`SSBU_TEST_VIEW_CYCLE`, `SSBU_TEST_ASPECT_CYCLE`), and still match.
3. **Every renderer.** The fight hash does not depend on the renderer, so the same comparison proves
   each back end.
4. **Online, by rollback.** In an online match each game sends its fight hash with its inputs and
   rollback compares them; a difference is a desync, written to `reports\rollback_<date>_<time>.txt`.
   A feature is declared online-safe from matches played with it on (ADR 0002 says how for ultrawide).

## Experimental features

A visual feature not yet shown online-safe can be offered only in the settings' **Experimental** tab:
once turned on it stays on, a change takes effect at the next start (never inside a match), and its
text says that a mistake in it affects the opponent as well as the player who turned it on.
