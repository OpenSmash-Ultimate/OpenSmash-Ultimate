# Ultrawide: what is known, what was changed

Working notes for the ultrawide mode (branch `ultrawide`). Read this before touching the camera, the
HUD or the renderer's squeeze for it. Addresses are game version 13.0.5, `main` loaded at `0x7100000000`
(`main+0x1234` = `0x7100001234`). Every claim says how it was found; **measured** means a run showed it.

Goals agreed with the owner: the fight must stay exactly what 16:9 computes (online play); the HUD
stays in the middle 16:9, unstretched; the P1/CPU tags and the magnifying glasses sit on their fighters;
the camera should feel like 16:9's (centred, pulling in, not showing much past the stage).

**Every change reaches both renderers and every supported system** (owner, 2026-11-14). A change to the
picture, the views or the HUD lands in Direct3D 11 and Vulkan together, and on Windows and Linux together.
The decisions belong in shared code (`gpu_aspect.c` for the renderers, `host.c` for the game's side), and
each back end only applies them. A change that one renderer or system can't carry yet is not done: it stays
under [What's next](#whats-next) until it can. macOS (Metal) stays held at 16:9 until its port.

## HUD decisions

One row per HUD element: its kind (static, dynamic or custom), what it does in each view, and why. When
an element shows is always the game's 16:9 rule; the kind is about where it is drawn. A custom row says
what its rule depends on. The words are [ULTRAWIDE-CONTEXT.md](ULTRAWIDE-CONTEXT.md)'s; what is left to
do is under [What's next](#whats-next).

| Element | Kind | Wide | Zoomed | Hybrid | Status | Decided, and why |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Tags (P1, CPU) | dynamic | over the fighter | over the fighter | over the fighter | done | 2026-11-11: they follow the view the picture shows (fixed: the HUD read the camera's own matrix, not the moved one). |
| Magnifying bubbles | custom: by view | at the 16:9 edge | at the picture's edge | at the picture's edge | done | 2026-11-11/12: they show at the 16:9 off-screen boundary in every view (the game's rule, a useful cue near the blast zone). In the wide view the picture shows past that boundary, so the bubble stays on it: at the picture's far edge it would look like the fighter jumped. In zoomed and hybrid the picture's edge is where the fighter leaves the picture. |
| Fighter in a bubble | custom: by view | not drawn in the side area (known limitation) | hidden (the game's) | hidden (the game's) | known limitation | 2026-11-12 (owner): in the wide view the bubble at the 16:9 edge is right as it is: it marks a fighter off the standard view. 2026-11-14 (owner): the fighter disappearing from the side area is a known limitation until there is a proper UX solution; see "Known limitations". |
| Triple arrows | dynamic | by the picture's edge | same | same | done | 2026-11-14 (owner): they only say a fighter is off that side, so one rule in every view, unlike the bubble, which marks a place (`set_parts_arrow_*`, host.c `pane_place`); their size is the game's. |
| TIME! (a timed match's end) | dynamic | its two copies meet in the middle | same | same | done | 2026-11-14 (owner: two words, "TIMTIME!"): the `timeup` layout slides one copy in from the right and one from the left, to collide at x 0. Each trails its blur lines on its side, so the side rules took it for a panel against that edge and stopped it 960 x (widening - 1) short. Its x is widened instead: the copies start past the picture's edges and meet in the middle. GAME! and SUDDEN DEATH! were right already. |
| Yellow frame (`set_parts_edge`, `edge_y`) | static | hidden | hidden | hidden | done | Its border sits just past the 16:9 screen's edges, where the console never shows it, and its drawing keeps its 16:9 shape, so on a wider picture it stood at the 16:9 edges. Hidden in a fight's scene at every shape (2026-11-14, owner: hidden only while wider, it flashed for a moment after a switch from 16:9, in the frames the game had placed before it). |
| Damage meters | static | 16:9 view | 16:9 view | 16:9 view | done | 2026-11-11: left with the 16:9 view. 2026-11-15 (owner, 8 fighters): with 5 to 8 fighters the row is `p1_2` .. `p4_2`, `p5` .. `p8` (`info_melee`); only `p1` .. `p8` were known, so the side rules took P1 to the picture's left edge. All of them are static now. |
| Online waiting room's player slots (`info_waiting`, `trans_players`) | static | 16:9 view | 16:9 view | 16:9 view | done | 2026-11-15 (owner): a row like the damage meters. The side rules took its two end slots to the picture's edges and left the middle two (P1's slot also cut off at the 16:9 edge). The status ("Waiting...") and the score boxes at the right follow the side rules, as Training's damage panel does. |
| Timer | custom: player setting | right, center, left or hidden | same | same | done | 2026-11-12 (owner): "Timer position" on the Display page (display.ini `timer`), at any shape. Right is the game's place, at the picture's top-right corner (the default); left keeps the same margin from the picture's left edge; center centres it; hidden folds it away. Placed by its own 256 px box, which holds a two-digit minute: checked with a 15-minute match (14:52) in all four places, 32:9 and 16:9, nothing cut off. |
| Radar (the minimap square) | dynamic | its corner of the picture | same | same | done | 2026-11-12 (owner: "keep it in the corner"): it shows when a fighter is off-screen by the 16:9 rule, on that fighter's side; the side rule takes it to the picture's top-left or top-right corner (both seen). Its inner box stays the 16:9 view (owner: widening it may overlap). |
| Pause menu, pause camera controls | dynamic | at the picture's sides | same | same | done | 2026-11-12 (owner): same size as at 16:9, never stretched, only moved: each panel goes to the side it is laid out against (the pause menu flush with the picture's right edge, PAUSE with its left, the camera controls' lists and buttons to their sides). The fight's layouts mark no anchors, so the side is read from where a panel sits against the screen (host.c, `pane_place`). |
| Popup dimming ("End the game?" and other popups in a fight, `system/popup`'s `set_bg_00`) | dynamic | the whole picture | same | same | done | 2026-11-15 (owner): it dimmed only the 16:9 middle, the side areas and the pause menu's outer part stayed bright. The popup's dimming pane is stretched across the picture, by name: stretching every screen-sized fill also stretched the results wipe's still of the fight's last frame (it already fills the picture), and the damage meters spread out for a few frames after GAME! (owner, 2026-11-15). |
| Training mode elements | dynamic | at the picture's sides | same | same | done | 2026-11-12 (owner): as the pause menu: the damage panel at the right edge, the + menu (Quit Training, the settings list) at the left; the help line stays centred. The damage meters stay static (an exception by name). |
| Battle Camera frame (the radar's inner box) | static | as the game draws it | same | same | done | 2026-11-12 (owner): left as the game draws it; widening it may overlap other things. |
| Vs screen before a fight (`system/matchup`) | static | 16:9 view, the side areas open | same | same | done (open ends for now) | 2026-11-15 (owner): each fighter's background (`set_panel_bg_*`, 960 px each with two fighters) stays in its 16:9 half; the side areas show what is behind until there is a better idea. Moved to the sides by the side rules they left a gap in the middle (and the left one was cut at the 16:9 edge, below); widened to meet in the middle and reach the edges, they stretched. The cut: a scissor's left is unsigned, so the game gives one past the screen's left edge as 0; in a fight's scene a scissor from 0 now reaches the picture's left edge, as one past 1920 reaches the right (`gpu_aspect.c`), which was also the waiting room's P1 slot's cut. |
| Menus, loading screens | static | black side areas | same | same | done (first PR) | 2026-11-12 (owner): black for the first PR; a blurred fill after it. |

## Known limitations

- **A fighter disappears past the 16:9 edge** (the wide view; the hybrid view now and then, when it shows
  past the 16:9 view). Once a fighter is fully outside the game's 16:9 view, the game draws them only inside
  their magnifying bubble: their model is moved out of the main picture and into the bubble. At 16:9 nobody
  sees this, because that area is off the screen; in a wider picture the fighter is still in view, so they
  vanish from the side area while the bubble shows them. The zoomed view does not show past the 16:9 view, so
  it is not affected. Left as it is for now (owner, 2026-11-14): it needs a proper UX solution, not only a
  rendering change. What was tried is under "Tried and dropped".

## What's next

None of these blocked the first PR (#359).

- **Test on Linux** (Direct3D 11 through DXVK, and Vulkan): ported 2026-11-14 and shipped untested (owner).
  The views and the HUD (`host.c`) are out of its Windows-only part (the camera hunt stays Windows only);
  `auto` and the 21:9 / 32:9 snapping read the monitor through SDL; the windowed SDL window takes the
  picture's shape. Checked only by compiling the touched files against DXVK's, the DirectX, Vulkan and SDL2
  headers and resolving their symbols. Done when a Linux player has run 21:9 or 32:9 on both renderers,
  windowed and fullscreen.

- **An Assist Trophy that takes the camera** (owner, 2026-11-14: one zooms in on itself and pulls the blast
  zones in; which one is not known, and there is no case to test on yet). The fight should be the same in every view (nothing the fight reads is
  written), but the zoomed view's shadow camera only moves on frames that run the game's normal framing: if the
  trophy's camera skips it, the zoomed view keeps a stale offset and zoom. To be established with the owner
  (a case of their own); the plan so far: log the fight frames that skip the normal framing, fall back to the
  game's camera on them, CPU-only fights with Assist Trophies only on high, each view's hash against 16:9.

- **A fighter disappears past the 16:9 edge** (wide view; hybrid now and then): see "Known limitations"
  above. Needs a UX decision first (what the side area should
  show for a fighter the game has put in a bubble), then the rendering.

- **Menus and loading screens**: a blurred fill of the side areas instead of black.
- **Metal** (macOS): the port; until then macOS stays at 16:9.
- **Online unlocked by default**, by [ADR 0002](adr/0002-online-held-at-16-9.md)'s evidence.

## Settings and switches (Direct3D 11 and Vulkan, on Windows and Linux)

The words used here (picture shape, wide / zoomed / hybrid view, stage range, ...) are defined in
[ULTRAWIDE-CONTEXT.md](ULTRAWIDE-CONTEXT.md).

| Setting | What it does |
| :--- | :--- |
| `display.ini` `aspect = 16:9 \| auto \| W:H` (`SSBU_ASPECT` over it) | The picture shape, at most 32:9. 16:9 (the default): nothing changes. auto: the primary monitor's. Also in the settings menu ("Aspect ratio"): taken at the next frame, in fights too. 16:9 while an online match is set up or under way (taken before its GO!, never inside an online fight), and for the whole session on macOS (Metal not ported yet). On Linux, auto and the 21:9 / 32:9 snapping read the window's monitor through SDL. |
| `display.ini` `view = wide \| zoomed \| hybrid` (`SSBU_VIEW` over it) | The view, with a picture wider than 16:9. Default hybrid. Also in the settings menu ("Viewtype"), at once, mid-fight too. |
| `SSBU_DEBUG_REFERENCE_VIEW=1` | The reference view: tells the game itself the picture shape. Changes fights: offline comparison only (`build\aspect_test\liked.bat`). |
| `SSBU_DEBUG_CAMERA_HUNT`, `SSBU_DEBUG_CAM_TRACE=<n>` (`SSBU_DEBUG_CAM_TRACE_OBJ`), `SSBU_DEBUG_FRAMING_DUMP=<n>`, `SSBU_DEBUG_PANES=<text>`, `SSBU_DEBUG_PANES_MOVED=1`, F9 | Research and measurement: who writes the camera's matrices, both cameras every n fight frames, the framing camera object, the HUD's panes by name, each pane the HUD's side, arrow and TIME! rules move, which 2D draw is which. |
| `SSBU_GFX_LOG_CB=all` | F4's draw log dumps every constant buffer bank in full. |
| `SSBU_TEST_ASPECT_CYCLE=<s>` | Test: 16:9, 21:9, 32:9 asked for in turn every s seconds (in fights too). |
| `SSBU_TEST_VIEW_CYCLE=<n>` | Test: the next view every n fight frames, as the menu would (switching mid-fight, checked by replay hash). |

## What the runtime changes (by file)

- `runtime/display.h`, `runtime/gpu_display.c`: the settings. `display_aspect()` is the shape the picture
  has now; the menu or `display.ini` asks for one (`display_set_aspect`), and the renderer takes it at
  its next present (`display_aspect_take`: at once offline, before GO! for an online match, never inside
  an online fight). `display_shape_locked()`: 16:9 for the session on macOS only.
  `display_view()` / `display_set_view()`: the view, read every frame. `display_fit` fits the picture
  at the shape.
- `runtime/gpu_aspect.c` (`gpu_aspect.h`): the renderers' decisions, in one place so the two cannot drift.
  Each back end hands it a draw (`AspectDraw`) and applies the answer (`AspectDecision`).
  - `aspect_widening`: a 16:9 render target is made `shape / (16/9)` times wider (each back end's
    `scaled_size` keeps it as `Surface.wide`).
  - The squeeze (`aspect_draw`): the game draws 3D wide itself; a 2D draw (no depth test) that is not a
    full-screen pass has its clip-space x divided by the widening (`params.fix[0]`), so the HUD sits in
    the middle 16:9, scissored to it (the game's own scissor squeezed with it; in a fight the whole picture).
  - Magnifying bubbles: known by the portrait's vertex program and the two draws after it (ring, arrow),
    moved to the picture's edge in the zoomed and hybrid views (HUD decisions), in a fight only: menus draw
    replay thumbnails and selection frames with the same programs. The back end shifts the viewport.
  - Research: the F9 picker (`aspect_pick_next`).
- `runtime/gpu_d3d11.c`, `runtime/gpu_vk.c`: apply them. `scaled_size` widens with `aspect_widening`; the draw
  fills an `AspectDraw` (the target's widening, depth test, whether it samples a widened target, the game's
  scissor, the vertex bank 3) and sets `params.fix[0]`, the scissor and the viewport from the answer. At a
  shape change (`display_aspect_take`, at a present) every target whose size changes is released and made
  again on its next use (they are looked up by the game's address). Both also change the render scale
  live, the same way. Direct3D 11's research: `cam_hunt_cb`, the `aspect:` line
  in F4's draw log.
- `runtime/gpu_menu.c`: the settings menu's Aspect ratio and Viewtype rows (both take effect without a
  restart).
- `runtime/gpu_window.c`, `runtime/linux/window.c`: F9 passed to the renderer. Linux: the windowed SDL window
  takes the picture's shape (at the start, and at a shape change or a new window size from the menu, never
  inside an online fight: `display_take_window_size`); `rt_monitor_shape` gives `auto` the monitor's shape.
- `runtime/hle_rb_core.c`: `net_match_on()`, whether an online match is set up or under way (ADR 0002).
- `runtime/hle_fight_test.c`: `fight_test_frame0()`, so the camera trace's lines join the fight hash log's.
- `runtime/host.c` (Windows and Linux; the camera hunt, `SSBU_DEBUG_CAMERA_HUNT`, Windows only):
  - `wide_camera` (each frame's end): the render camera's width `+0x164` (and `+0x16c`) set from the
    shape when wider than 16:9, written back to 16:9's once when 16:9 returns.
  - `pane_place` (nn::ui2d's Pane::CalculateGlobalMatrixSelf, `f_71000589a0`): in a fight's scene (the game's camera
    manager exists, from the fight's loading on), a panel laid out against one side of the screen goes to the same side
    of the picture, whole; parked panels and screen-sized containers scale out with the picture; the edge frame is
    hidden; the damage meters stay. The timer's place (display.ini `timer`) is set there too, at any shape.
    `pane_calc` (Pane::Calculate, `f_7100058700`): after a shape change inside a fight's scene, every layout is
    placed again for half a second (x3 = 1 at its root), so panels that never change follow at once.
    `SSBU_DEBUG_PANES=<text>` logs the panes whose names hold the text (the pane's name is at `+0xb0`).
  - `hud_call` and its 12 wrappers: while the HUD's world-to-screen functions run they get 16:9's
    projection and the view the picture shows, so tags land on their fighters (straight through at 16:9).
  - The views, one hook on the game's framing (`framing_hook`, `f_71004fe4f0`) that reads the view every
    frame. Zoomed: the framing and camera-move calls are recorded (`rec_call`) and replayed with the
    picture's shape on a shadow camera of this file's own (`shadow_update`), which the picture follows.
    Wide: the game's own camera, untouched (2026-11-12: it had re-run the framing for the picture's shape,
    held at 12% of it by a bug; the owner chose the game's framing). Hybrid: the framing left alone,
    the picture's camera slid along the stage range and zoomed in only as needed (`wide_copy`). Nothing
    the fight reads is written.
  - `net_match_watch` (each frame's end, on the game's thread): caches `net_match_on()` for the renderer.
  - Research: the hunts, the camera trace and dumps (behind their switches, kept as listed under "Settings and switches"; dropped: the cull hunt, `RCAM_WATCH`, `ASPECT_HYBRID`, `ASPECT_SCREEN`, `NO_2D_CLAMP`, `SHADOW_LOG`, `WIDE_FRAMING_STORED`, `FRAMING_SHADOW`).

## The game: render camera

The camera the picture is drawn with. Found: `f_7103747820` (scene set-up, calls `tanf`) reads it and
copies its matrices to the scene globals; dumped live with `SSBU_CAM_DUMP` (**measured**).

Address: `*(*(main+0x5337ce8) + 0x490)` is a manager `m`; the camera is `*(m + 0xe0)` when
`*(m + 0xd8)` is set, else the default at `main+0x5334060`.

| Offset | Value at 16:9 | Meaning |
| :--- | :--- | :--- |
| +0x140 | l, r, t, b | the orthographic box (Training: about -44.6, 44.6, 25.1, -25.1: 16:9); the projection is built from it when the kind is 1 (**measured**) |
| +0x150 | 0 | projection kind (0 perspective; 1 orthographic: Training) |
| +0x15c | 30, 41 | field of view, degrees (41: the entry; 30: the fight) |
| +0x160 | 0.5236 | field of view, radians |
| +0x164 | 1920 | width: **the projection is built from this** (widening it alone widened the projection, measured) |
| +0x168 | 1080 | height |
| +0x16c | 1.77778 | width / height, computed as 1920 x (1/1080) - not bit-equal to 16/9, which is why searches for 16/9 missed it |
| +0x170, +0x174 | 1920, 1080 | a second size; widening it changed nothing seen |
| +0x178, +0x17c | 1/1920, 1/1080 | reciprocals of the second size |
| +0x1d0 | 4x4 | view, row-vector (`p * V`), translation row at +0x200 |
| +0x210 | 4x4 | identity seen |
| +0x250 | 4x4 | inverse view; camera position at +0x280 |
| +0x290 | 4x4 | projection: +0x290 x scale (cot(fov/2) / aspect), +0x2a4 y scale (cot(fov/2)) |
| +0x2d0 | 4x4 | inverse projection |
| +0x310 | 1920 1080 1/1920 1/1080 | another size |

Widening the width at +0x164 each frame fixed the culling of floors and background pieces at the
picture's sides (**measured**) and does not change the fight (**measured**, below).

Copies on the way to the GPU: the scene constant buffer (`cb3`, field of view 41) is written by
`f_7103553980` (a loop that flushes CPU-side buffers to GPU ones with memcpy); per-draw `cb8` copies
(field of view 30) by `f_710355a980` / `f_710355ad30` and `main+0xa5c80`. Found with `SSBU_CAMERA_HUNT` (now `SSBU_DEBUG_CAMERA_HUNT`)
(page write-watch), **measured**.

## The game: game camera (`app::`) - the one that frames the fight

The camera logic: where the camera goes, what is off screen. Its state feeds the fight.

- Camera manager: `*(*(main+0x52b8f00))`, call it `M` (`app::camera::*` and `sv_camera_manager::*` read it):
  - `M+0xc20` .. `+0xc2c`: the stage's camera range, left, right, top, bottom (`sv_camera_manager::camera_range`,
    `main+0x2282ea0`; left/right order from the framing code's min/max).
  - `M+0xc30` .. `+0xc3c`: the dead area, the blast zones (`app::camera::get_dead_area`, `main+0x15cb9e0`).
  - `M+0xeb0`: the visible range's centre; `M+0xf38` x `M+0xf68`: its half height; `M+0xf58`: its
    width / height. `app::camera::get_camera_range` (`main+0x15cb960`) returns centre +- (half height x
    aspect, half height). Read from code, not yet dumped live.
- The framing: `f_71004fe4f0(camera, out, ..., s0 = aspect)` (calls `tanf`) fits the fighters' camera
  ranges, clamped to the stage's (`+0x224..+0x230` against `M+0xc20..`). Its caller copies the aspect
  from `*(camera + 0x8) + 0x128` to `camera + 0x134` every update; `app::item::is_clip_in_camera`
  (`main+0x15c46a0`) reads `+0x134..+0x140`.
- `main+0x4472c5c`: the only 16:9 float in `main` (rodata). Read by the framing code's other branch and
  the main menu's 3D camera, not by the fight's projection (**measured**: patching it widened the main
  menu's panels only).

**Widening this camera's aspect (both places) changes the fight** (**measured**: a scripted 32:9 fight
with it differs from 16:9 at frame 578, in damage - fighters past the old 16:9 edge took the
magnifying glass's off-screen damage). With it widened, though, the camera framed the wide picture
the way the owner wanted: it pulled in, stayed inside the stage, and fighters past the old 16:9 edge
were drawn (the game hides a fighter in the magnifying glass) with the glass at the real edge.

### How the 16:9 camera behaves (measured, 2026-11-11)

A 4-player replay (frames 10-4170; camera range x -170..170, y -50..130; blast zones x +-230) watched at
16:9 with `SSBU_CAM_TRACE=10` (now `SSBU_DEBUG_CAM_TRACE`) and `SSBU_FIGHT_HASH` (`build\aspect_test\replay169.bat`):

- The game camera is a 16:9 rectangle on the fighters' plane: centre `M+0xeb0`, half height
  `M+0xf38 x M+0xf68`, half width = half height x 16/9.
- The render camera follows it: same x, a little above it (pitched ~5 degrees down), at the distance
  that shows that half height (half height = distance x tan(fov / 2)); the field of view itself moves too
  (26-30 degrees seen).
- The rectangle stays inside the stage's camera range: its left edge stopped at -169, -166, -167 against
  the range's -170 (frames 2110, 3010, 3310). That is the 16:9 feel: it slides with the fighters and stops
  at the range.
- Off screen = outside that rectangle: at frame 2110 two fighters at x -197 and -211 (past the range,
  inside the blast zone).
- At 32:9 the same height is twice as wide: frame 2110's rectangle would run -261..109, 91 past the range
  and 31 past the blast zone. Zoomed out (half height ~75 and more) a 32:9 view is wider than the whole
  camera range, so no sideways slide can keep it inside: it has to zoom in, as the game's own framing did
  when its aspect was widened.

The framing camera object (x0 of `f_71004fe4f0`; dumped with `SSBU_FRAMING_DUMP` (now `SSBU_DEBUG_FRAMING_DUMP`), a hook that only
reads; same replay):

| Offset | Meaning (measured against the trace and the fighters' positions) |
| :--- | :--- |
| +0x70 (x, y) | where the camera looks now |
| +0x90 (x, y, z) | where it is now (z: its distance) |
| +0x80, +0xa0 | the same two as its goal: it eases from now to goal |
| +0xb0 | 30, 30, 0.6 (field of view, ...) |
| +0x1f8 (left, right, top, bottom) | the goal rectangle, already 16:9 (fighters' box plus margins, stretched to 16:9) |
| +0x208 (left, right, top, bottom) | the visible rectangle now (16:9, inside the camera range) |
| +0x224..+0x230 | the slot the framing clamps against the range: +-FLT_MAX here (empty) |

The fighters' own box is not kept: the game works it out and stores only the 16:9 result. A wide
framing needs it (spread sideways: zoom in; spread up and down: cannot).

### The liked mode against the visual-only framing (measured 2026-11-11)

`SSBU_GAME_FRAMING_WIDE=1` (now `SSBU_DEBUG_REFERENCE_VIEW`) is the mode the owner liked: the game's own camera
framing told the wide aspect. It changes the fight (kept for reference: `build\aspect_test\liked.bat`,
offline only). A live 1v1 on Bridge of Eldin in it, against the owner's 16:9 replay of the same match-up
and the visual-only framing (`SSBU_WIDE_FRAMING`), view half height by the fighters' spread:

| spread | 16:9 | liked | visual-only |
| :--- | :--- | :--- | :--- |
| 0-49 | 42 | 41 | 42 |
| 150-199 | 64 | 61 | 59 |
| 300+ | 95 | 48 | 84 |

The liked mode keeps a close shot when the fighters spread out (the wide picture covers them), and
slides off their middle to stay inside the stage's range; past the range at spread 300+: 0. The
visual-only framing zooms out as 16:9 does: 259 past. So the zoom is not decided by `f_71004fe4f0`.

The camera update (`f_71004fdc10`) then calls `f_71004fec80` twice: with s0 = the stored aspect
(+0x134), x2 = +0x70 (the camera's now), x3 = +0x110; and with s0 = main+0x4472c5c (16:9), x2 = +0xc0,
x3 = +0x118. These move the camera's state; the zoom is likely there. Setting the stored aspect only during the
second `f_71004fe4f0` call did not change the picture; the hash difference seen then (frame 2474) was
the results screen's random victory pose, not the fight. Compare hash logs up to the "GAME!" frame (the
fighters jump to x 1100 / 0 for the results).

### Decisions (owner, 2026-11-11)

(The styles below were renamed: **full** is now the wide view, **stage** the hybrid view; the zoomed view came later.)

- Two camera styles, both kept as options: **full** (the visual-only framing as it is: everything the
  fight could need is in view, past the stage if need be) and **stage** (the liked mode's look: the view
  clamped to the stage's camera range at the full width, zoom fitted to the wide picture), the latter
  to be made fight-identical with a shadow camera (the game's own camera update run on a copy).
- The "stage" style's zoom exists only to hide what lies past the stage (owner, 2026-11-11): stay as
  zoomed out as the full style, slide along the stage first, zoom in only as far as needed to keep
  the picture inside the stage's camera range. The zoomed-out look is preferred.
- The magnifying glass stays where the 16:9 rules put it: it marks a fighter nearing the blast zone,
  which is when it shows anyway. Drawing the fighter instead is for later.
- Stages whose floor runs to the blast zones (Bridge of Eldin) show the oddities most; competitive
  rulesets ban them.

## Driving the game without a person

`build\aspect_test\auto\*.bat` (made by `mk.py` there): `SSBU_SEQ` walks the menus - title (A at tick
600), main menu (RIGHT is Vault, A), Vault (RIGHT is Replays, A), Replay Data (A), the newest replay (A),
Play (A) - with screenshots every few seconds (`SSBU_CAPTURE`, `sheet.py` makes a contact sheet). Each
run takes a copy of the save (`SSBU_DATA_ROOT`). A saved replay plays the same fight every time:
`framing169` (scripted) and a watch by hand hashed the same over 16536 lines.

The same replay at 32:9 (`replay329.bat`, the native mode) hashed the same as at 16:9 over 4039 fight
frames (16153 lines): the second proof that the mode does not change a fight, this time on real play.

## The game: HUD

Drawn by a few vertex programs, all with depth testing off, on the final 1920x1080 target. Found from an
F4 draw log of a fight (`SSBU_GFX_LOG_CB=all`), **measured**:

- Placed elements, `vs_e0758c3ac3507774` (text `ps_340268462afc028d`, images `ps_547eb1f52eda5794`):
  vertex bank 3, first 0x40 bytes, is the element's transform with the screen's: x row `(2/1920, 0, 0, x)`,
  y row `(0, -2/1080, 0, y)`; the corners come in the element's pixels (a storage buffer).
- Shapes, `vs_1669b7d183575986` and 7 others: bank 3 is the screen's mapping (no position) and, at +0x40,
  the element's transform (x row `(scale, 0, 0, x in layout pixels from the middle)`); the vertex buffer
  is a unit square.
- The P1/CPU tags and the magnifying glasses use the same programs as the rest of the HUD: no program
  tells them apart. Their positions come from the render camera's projection through the HUD's
  world-to-screen code. Narrowing each element around its own anchor (instead of squeezing all to the
  middle) put tags right but broke widgets made of pieces (clock and damage digits spread apart) and
  pulled a full-screen element's edges (the yellow bars) into view.
- The 12 functions given 16:9's projection (`main+0x1341670`, `+0x134c690`, `+0x135ccb0`, `+0x13955d0`,
  `+0x13a3f50`, `+0x13a53d0`, `+0x13a67c0`, `+0x13adb10`, `+0x13c8460`, `+0x13ca580`, `+0x13e29d0`,
  `+0x142aed0`): the ones that load the render camera and read +0x290, in that part of `main`. Tags land
  on their fighters with all 12 (**measured**); which of them are needed is not known. In a fight 7 run,
  once a frame each (`+0x135ccb0`, `+0x13955d0`, `+0x13a3f50`, `+0x13adb10`, `+0x13c8460`, `+0x13ca580`,
  `+0x142aed0`); `+0x1341670` and `+0x134c690` not at all.
- They take the **view** from the render camera's own matrix (`cam+0x0`, translation `+0x30`), not from
  the drawing block (`cam+0x1d0`) the views move, and the projection through `*(main+0x53375a8)` (the same
  camera). So the zoom and slide never reached the tags (owner, 2026-11-11: "tags broken on zoomed and
  hybrid"). Fixed: while one runs, the moved translation is put in `cam+0x0` too and put back after (only
  when its rotation is the block's). Bridge of Eldin replay: wide, zoomed, hybrid the same as 16:9.
  `+0x1341670` with bit 0 of x2 builds its view from the camera manager instead (`M+0xeb0`, `+0xec0`, up
  `+0xee0`; look-at in `f_7103559a30`): not seen in fights.
- `sv_camera_manager::world_to_screen` (`main+0x22832c0` -> `f_7101341280`): layout pixels, centre
  (960, 540); in a fight only ever returned the centre (**measured**). Not what places the tags.
- `kozukatasight::screen_width` (`main+0x1646f00`) reads a global at `main+0x744701c` (1920 when 0).
- Yellow bars: an element just past the 16:9 edges; the 2D clip hides it.

## The fight stays the same: how to check

`build\aspect_test\verify\run169.bat` and `run329b.bat` (and `run329.bat`): the same scripted fight
(`SSBU_SEQ`, `SSBU_DETERMINISTIC`, `SSBU_FIXED_SEED`, `SSBU_FIGHT_HASH`; Mario against a level 1 CPU
Donkey Kong, `stage=end`), each with a fresh copy of the save. The menus are walked with the keyboard
(`SSBU_SEQ_KEYBOARD=1`); the fight plays itself and the run ends after 30 s. Then:

```
python tools/dev/fight_hash_diff.py build/aspect_test/verify/run169.fh.txt build/aspect_test/verify/run329b.fh.txt
```

Results (2026-11-10): 16:9 vs 32:9 native with the 16:9 framing: the same over 4036 lines. 16:9 vs 32:9
with the game camera widened: first difference frame 578, damage. A stage not named in
`SSBU_DEBUG_MATCH` comes from the menus, so two runs can differ in stage: name it.

**Scripted live fights jitter** (2026-11-11, `build\aspect_test\auto\mktags.py`, the menu route of
`cmp169`): the same `SSBU_SEQ` polls land the first stick press a frame early or late now and then
(frame 328 or 329, any view, even the same build twice), and the fights part from there. Good for
looking at tags (replays draw none), not for proving a fight the same: use the replays. Tags fade a
few seconds into a fight (16:9 too).

## Tried and dropped

- Patching `main+0x4472c5c`: only the main menu's camera and the framing's other branch.
- Searching memory for 16:9 floats, for perspective matrices of 16:9, for frustum planes (6 or 4,
  either layout): the fight's 16:9 is computed each frame; culling keeps no planes.
- Telling the game the screen is wider (`vi`): ignored, the game's sizes are its own.
- A vertex-shader squeeze (perspective vertices only): right picture, but every vertex program's cache
  is new (18,222 programs, ~1 h of compiling); short effects were skipped while compiling.
- `f_710364b620`'s visible rectangle (+0x950 = height x 16 / 9): 0 x 0 in fights.
- Panning the render camera to keep the stage's range in view, with the rest dimmed (2026-11-11):
  no dimming seen; reverted.
- Drawing a fighter in a bubble in the main picture too (2026-11-14, Mario on Bridge of Eldin, 32:9 wide):
  the bubble's start (`main+0x490d10`, the model module's vtable +0x2a0: player slot, in a bubble) moves the
  render model's group to `0x7650000 | slot`, which lib's node lookup (`main+0x35db540`) turns into
  `NODE_LOUPE_<slot>` instead of the main view's node. Keeping the main group left the bubble empty and the
  fighter still not drawn; a second node slot on the drawable (`main+0x35fb5c0`, slot 2) drew nothing (the
  slot seems to pick the pass: 1 is the shadow's); widening every 1920x1080 camera the renderer copies only
  stretched the bubble's portrait. Something else hides the fighter at the same boundary; not found. Reverted.

## Done (history)

What is left is under [What's next](#whats-next). Done, with what was learned:

1. **The hybrid view, fight-identical** (2026-11-11; first called the "stage" style, `SSBU_WIDE_STYLE=stage`):
   the game's camera widened, slid along the stage range and zoomed in only as far as needed to keep the
   picture inside it, never past the 16:9 view. Both replays hash the same as 16:9 up to GAME!; past the
   range: Town and City 0, Bridge of Eldin at most 7 (the game's own 16:9 view). The shadow camera (the
   game's framing replayed on a copy) was kept as `SSBU_WIDE_STYLE=shadow` for research (removed 2026-11-12).
2. **Magnifying bubbles at the picture's edge** (2026-11-11): the renderer knows a bubble by its portrait
   (`vs_545fbedf170d65dd`, the fighter's picture from a 128x128 target) and the two draws right after it
   at the same x (ring `vs_38e6664f…`, arrow `vs_1669b7d1…`; x in vertex bank 3 at +0x4c, the ring's at
   +0x5c, in HUD pixels from the middle), and shifts the three by the viewport to the same side of the
   picture, as far from its edge as from the 16:9 one, past the 2D clip box. Not in the wide view (HUD
   decisions).
3. **Settings** (2026-11-11): `display.ini` `aspect` and `view`, both in the settings menu; all three
   views fight-identical on the Bridge of Eldin replay.
4. **The view changes live** (2026-11-11): every view's hooks are in whenever the picture may be wider
   than 16:9 and each reads the view every frame (`SSBU_VIEW_CYCLE=200`, now `SSBU_TEST_VIEW_CYCLE`, on the Bridge of Eldin replay:
   the same as 16:9).
5. **The picture shape changes without a restart** (2026-11-11): the renderer takes the shape asked for
   at its next present, then every 16:9-shaped render target is released and made again at the new width
   on its next use. Everything else reads the shape live: the camera's +0x164/+0x16c, the HUD wrappers
   (straight through at 16:9), the views (no offset at 16:9). First made to wait for the fight's end;
   the owner tried it in a fight and saw nothing happen, and only online fights need the wait (a hitch
   there is both players'): offline it is taken at once (5 switches mid-fight, the same fight). Online:
   16:9 per match, not per session (OpenSmash.exe starts every session on the lobby; ADR 0002).
6. **A second replay, the whole match** (2026-11-12): the owner's Bridge of Eldin replay, Ganondorf against
   Pichu, recorded at 32:9 zoomed (`build\aspect_test\auto\e2_*.bat`, stopped at fight frame 5300: the
   replay viewer goes on to the next replay by itself, and SSBU_DEBUG_END counts no replay frames). Against
   16:9: 32:9 zoomed, 32:9 wide, the view switched every 200 frames and the shape every 7 s (21 switches)
   the same up to GAME! (frame 5153, the winner's pose); 21:9 hybrid the same throughout.
7. **Upstream merged** (2026-11-12, 52 commits): `auto` draws with Vulkan on AMD, NVIDIA and Intel cards
   since, so wider pictures needed `renderer = direct3d11` until the Vulkan port.
8. **Vulkan and Linux** (2026-11-14): the renderers' decisions moved into `gpu_aspect.c`, which Vulkan now
   applies too; the views and the HUD made portable for Linux. Vulkan, on the Eldin replay: 32:9 zoomed the
   same as 16:9 up to GAME!; the shape switched every 7 s, the same over the whole log; the same frames
   captured on both back ends show the same layout (`build\aspect_test\auto\vkz32`, `d3z32`, `vkh32`, `d3h32`,
   `vkscyc`). Linux: compiled against the headers, not run.
