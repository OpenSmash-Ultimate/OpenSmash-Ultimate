# Ultrawide: left before the first PR

Everything that must be done before branch `ultrawide` becomes a pull request. When this list is
empty, the file is deleted and the PR opened. Words as in [ULTRAWIDE-CONTEXT.md](ULTRAWIDE-CONTEXT.md);
how things work and what was learned in [ULTRAWIDE.md](ULTRAWIDE.md); HUD rulings in its
"HUD decisions" table.

Order agreed with the owner (2026-11-11): HUD work first, then the Vulkan port.

## Menu and settings

- [x] **"16:9 (original)"**: the 16:9 preset's label; help text "The game as made for 16:9. The View setting has no effect."
- [x] **View greyed at 16:9**: shown, with "only with a picture wider than 16:9".
- [x] **Presets snap to the monitor** (43:18 dropped from the menu: on a 21:9 monitor it was the same as 21:9): 21:9 and 43:18 take the monitor's exact shape when it is within
      2.22-2.45 (2560x1080, 3440x1440, 3840x1600), 32:9 within 3.38-3.73; otherwise the preset's own
      shape. No bars in fullscreen inside those ranges; bars outside them are expected.
- [x] **Window size and render scale change without a restart**: at once offline; in an online match,
      after it (as the picture shape). Window size: any renderer on Windows (Linux and macOS: next start).
      Render scale: Direct3D 11 (Vulkan and Metal: next start); only the targets whose size changes are
      made again.
- [x] **Window size options fit the picture shape and the monitor**: the menu offers heights 720 to 2160
      whose width (height x shape) fits the monitor's work area; display.ini keeps width and height.
- [x] **Render scale auto follows the shape**: chosen again whenever the window's picture changes size
      (not while the window or its edge is being dragged: at the end of the drag).
- [x] **Windowed: the window follows the shape**: same height, new width, around the same centre; shrunk to
      fit the monitor if wider.
- [x] **The online hold ends at the main menu**: the match block's online melee mode (Online 14, Battle
      Arena 20) counts only while the game talks to other players (15 s), so the menus go back to the
      picture shape once they are gone. Not yet seen in a real online session.
- [x] **Every display setting persists**: `tests/test_renderer_option.py` reads back every value the Display
      page writes. Found: the launcher before upstream #318 wrote its old copy of the settings back when the
      game closed, undoing the in-game menu's changes; fixed by the current launcher (not yet installed here).

## Experimental tab (the maintainer agreed to a new tab)

Rule for every row: once turned on it stays on; any change takes effect at the next start
([ADR 0001](adr/0001-visual-features-never-change-the-fight.md)).

- [x] **Ultrawide online** (display.ini `experimental_ultrawide_online`; the shape still never changes inside an online fight): the picture shape kept in online matches. Disclaimer: "Experimental. A mistake
      in this feature changes the fight for you AND your opponent, and can make the match go out of sync."
- [x] **Open log folder** (Windows): Explorer on `build\` with `play.log` selected (rollback's desync reports too).
- [x] **`play.prev.log`**: the previous session's log kept at each start (the launcher, `Program.cs`; reaches players with the next launcher build).
- [x] **The log names the shape and view** at each online match's start, and the rollback desync report
      carries that line ([ADR 0002](adr/0002-online-held-at-16-9.md)).

## HUD (see ULTRAWIDE.md, "HUD decisions")

- [x] Pause menu elements: panels to the side their layout puts them against (the game's own layouts; see ULTRAWIDE.md, `pane_place`)
- [x] Training mode elements (the + menu too)
- [x] Timer placement: the "Timer position" setting (right, center, left, hidden), two-digit minutes checked
- [x] Radar (the minimap square): it shows on the side of the off-screen fighter; seen on both (2026-11-12): the
      picture's top-left corner for a fighter off the left, top-right (under the timer) for one off the right
- [x] Radar's inner box (the Battle Camera frame): left as the game draws it (owner, 2026-11-12: widening it may
      overlap other things)
- [x] A shape changed mid-fight reaches every HUD panel at once (owner, 2026-11-12): the layouts are placed again for
      half a second after the change; the replay with the shape cycling every 7 s matches 16:9 up to GAME!
- [ ] Triple arrows (the blue arrows) follow the bubbles out, in all three views (owner: what makes them appear is
      still to be found; not blocking the first PR)
- [x] Wide view: the bubble stays at the 16:9 edge, marking a fighter off the standard view (owner, 2026-11-12: as it
      should be; no change)
- [x] Menus and loading screens: black side areas for the first PR (owner); the blurred fill after it

## Bugs to reproduce

- [x] **Training mode camera stretches** in a wider picture (owner's test): Training's camera is
      orthographic (`+0x150` = 1), its projection built from a 16:9 box (`+0x140`), not from the width
      the fight's camera takes, so the 3D was drawn 2x wide (grid cells 61 x 31 px; 16:9: square). Its
      drawing projection is widened after the copy now (cells square, side areas drawn, tags on their
      fighters). Wide and zoomed look the same there: an orthographic camera has no distance to zoom by.
      Scripted route: title A, RIGHT, UP, A (Games & More), DOWN, LEFT, A (Training), wait, A, SUP x3, A,
      PLUS, A (`build/aspect_test/auto/train_*.bat`).
- [x] **A menu's selection frame strays into the side area** (owner, 2026-11-12, 32:9 zoomed or hybrid,
      Vault > Replays > Replay Data): the magnifying bubble mover took the menu's replay thumbnails and
      selection frame for a bubble (the same vertex programs as a bubble's portrait and ring; the left tile
      sits where a left bubble would) and moved the frame to the picture's edge. Bubbles move only in a
      fight now (the game's ready-go flag, on in replays too).
- [x] **Pause camera jumps** (owner, 2026-11-12, 32:9 zoomed, worst near the stage's edge): a pause stops
      the fighters' frame line, and a quarter second later the views took that for the fight's end and
      dropped their offset, so the picture jumped to the game's own camera (and the zoomed view's shadow
      camera started afresh on resume). A pause is the fight now (the ready-go flag stays on); the pause
      shot starts from the picture's view, and the offset fades as the player moves the pause camera
      away. The pause menu box at the 16:9 view's right edge is the pause menu's HUD decision.
- [x] **Wide view at 12% of its framing** (found 2026-11-12): the wide view re-ran the game's framing
      for the picture's shape, but the renderer's copies for other cameras cleared that offset every
      frame, so it sat at 12% of it (100% zooms in until the fighters fill the picture, like the zoomed
      view). Owner: the game's own framing, no shift; matches the glossary's wide view.
- [x] **Zoomed view tighter than 16:9?** Checked 2026-11-12 on the owner's Ganondorf vs Pichu replay
      (Bridge of Eldin, 32:9 zoomed), every frame to GAME!: the picture is always shorter than the 16:9
      view (zooming in is the view), and it stops at the stage's edge, so a fighter standing on the ledge
      has its centre up to 1.3 units past the picture's edge (half the body shown); once, for 2 frames,
      a fighter high up was 1.4 units above the picture's top. No fighter is hidden without a bubble.

- [x] **Zoomed pause zooms out** (owner, 2026-11-12, Bridge of Eldin): the renderer's copies for other
      cameras cleared the views' zoom each frame, and the zoomed view's shadow camera, which set it again
      every fight frame, stops in a pause, so the zoom fell to 1 the moment the game paused. The other
      cameras' copies leave the views alone now.

- [ ] **An Assist Trophy that takes the camera** (owner, 2026-11-14: one zooms in on itself and pulls the blast
      zones in; which one is not known). The fight should be the same in every view (nothing the fight reads is
      written), but the zoomed view's shadow camera only moves on frames that run the game's normal framing: if the
      trophy's camera skips it, the zoomed view keeps a stale offset and zoom. To be established with the owner
      (a case of their own); the plan so far: log the fight frames that skip the normal framing, fall back to the
      game's camera on them, CPU-only fights with Assist Trophies only on high, each view's hash against 16:9.

## Renderers

- [x] **Vulkan port** (2026-11-14, owner: go ahead without waiting for upstream): the squeeze, 2D clip, bubble
      shift and F9 picker in `gpu_aspect.c`, which both back ends call; each applies the answer (`params.fix[0]`,
      scissor, viewport) and rebuilds its 16:9-shaped targets at a shape change. The lock is now macOS only.
      Proven on the owner's Eldin replay (NVIDIA RTX 2070): Vulkan 32:9 zoomed the same fight as 16:9 up to
      GAME! (frame 5153, as Direct3D 11); Vulkan with the shape switched every 7 s (21 switches) the same over
      the whole log; 32:9 zoomed and 32:9 hybrid captured at the same fight frames on both back ends: the same
      layout (HUD, bubbles, tags), the pictures a few game frames apart (each back end's capture lags
      differently: mean difference 0.75-1.5 at hybrid). Vulkan's render scale still waits for the next start.
- [ ] **Linux** (Direct3D 11 through DXVK, and Vulkan): done 2026-11-14, **untested on Linux**. The views and the
      HUD (`host.c`) are out of its Windows-only part (the camera hunt stays Windows only); `auto` and the
      21:9 / 32:9 snapping read the monitor through SDL; the windowed SDL window takes the picture's shape.
      Checked here only by compiling the touched files against DXVK's, the DirectX, Vulkan and SDL2 headers
      and resolving their symbols (no Linux toolchain on this machine; the branch could not have linked on
      Linux before). To tick: a Linux player runs 21:9 or 32:9, both renderers, windowed and fullscreen.

## Bugs to reproduce (found 2026-11-14)

- [x] **Results screen: the cards pushed apart** (owner, 2026-11-14, Vulkan; Direct3D 11 the same): the HUD's
      "panels to the picture's sides" rule keyed on the fight's camera manager, which the results screens still
      have, so the cards went to the sides. Now the rule ends with the fight (the ready-go flag off and the
      fighters' line stopped; the finish after GAME! keeps the whole picture) until the next scene: the results
      stay in the middle 16:9, as menus do. Checked by a debug match run into the results at 21:9 on both back
      ends (`build\aspect_test\auto\vkres21b`, `d3res21b`; 16:9: `vkres169b`). The winner's pose was framed as
      at 16:9 there (Donkey Kong); the owner's "zoomed in" not seen yet. Not checked: Sudden Death.
- [ ] **A tag off its fighter right after a mid-fight shape change** (both back ends, so the HUD's side,
      `host.c`): with `SSBU_TEST_ASPECT_CYCLE=7` (hybrid), a capture less than a second after 21:9 -> 32:9 has
      P1 well away from Ganondorf (Vulkan fight frame 3836; Direct3D 11's e2_scycle frame 4041). With the
      shape fixed from the start the tag is on him on both back ends. How long it stays off: to measure.

## Clean-up

- [x] Research switches: kept as `SSBU_DEBUG_CAMERA_HUNT`, `SSBU_DEBUG_CAM_TRACE` (`_OBJ`), `SSBU_DEBUG_FRAMING_DUMP`,
      `SSBU_DEBUG_REFERENCE_VIEW`, `SSBU_DEBUG_PANES`, F9; `SSBU_TEST_ASPECT_CYCLE`, `SSBU_TEST_VIEW_CYCLE`; removed: the cull
      hunt, `RCAM_WATCH`, `ASPECT_HYBRID`, `ASPECT_SCREEN`, `NO_2D_CLAMP`, `SHADOW_LOG`, `WIDE_FRAMING_STORED`,
      `FRAMING_SHADOW` (the wide view's old framing re-run).
- [x] `docs/PLAYING.md`: the settings text (aspect, view, timer) matches what ships.

## After the first PR (not blocking)

- Menus and loading screens: a blurred fill of the side areas instead of black.

- Metal.
- Online unlocked by default, by [ADR 0002](adr/0002-online-held-at-16-9.md)'s evidence.
