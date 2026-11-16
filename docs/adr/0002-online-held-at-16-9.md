---
status: accepted
---

# Online matches are held at 16:9 until lobby matches show ultrawide is safe

Ultrawide is fight-identical offline (ADR 0001, replays in every view and with the shape and view
switched mid-fight), and its design is client-side: a player at 32:9 never changes what a 16:9
opponent sees or computes. But online play is where a mistake would cost two players a match, so an
online match is played at 16:9 until real matches say otherwise. The maintainers and the community
decide when that evidence is enough; there is no fixed number.

## How it is held

Per match, not per session: OpenSmash.exe starts every session connected to the lobby, so a
per-session lock held offline play at 16:9 too. A match counts as online when rollback has agreed an Online
or Local Wireless match, a relay room's match is set up, a Local Wireless room has other players, or the
game's melee mode is an online one (Online, Battle Arena) while the game is talking to other players;
that mode stays set after the match, so without the players it no longer counts (`net_match_on`,
`hle_rb_core.c`). The shape goes to 16:9
before GO! and never changes inside an online fight, where a hitch would be both players'.

## The evidence

Players who turn on **Ultrawide online** (Experimental tab, ADR 0001) and play in the lobby. Rollback
already compares both games' fight hashes every frame, and players' own choices of characters, stages,
rules and views give the variety; fighters go off-screen in every match, since that is how a stock is
taken. Each match's log names the player's picture shape and view, and a desync report carries that
line, so a desync can be traced to ultrawide or ruled out (desyncs happen today without it).
Collection is by players sending their logs ("Open log folder"); no log is sent to the server.

## Considered

- Unlocking online at once, on the strength of the offline proof: rejected, a rollback re-runs frames
  and the online code paths differ from replays.
- A fixed count of matches as the bar: rejected for "enough variety, judged by the maintainers".
- Sending match summaries to the lobby automatically: not now; the owner will raise it with the
  maintainer.
