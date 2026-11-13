# Shared launcher behavior audit

Reference: behavior and artwork of the previous Windows launcher.

| Flow | Shared implementation / verification |
| --- | --- |
| Project beside launcher, standalone download, installer vs built home | Project.AtLauncherLocation, Bootstrap; existing relocation and bootstrap regression tests. No global remembered project. |
| Installer files, options, real progress/logs, interactive shader priority, stop/resume | MainWindow.Install, ClassicSurface and installer event parser; existing desktop and Linux installer tests. |
| Mods added while closed | ModChanges snapshots before Python status; startup, focus and Refresh checks; original New Mod Found dialog; enable and acknowledge only after success. Project-local build/work/known_mods.txt is retained. |
| Changed plugins | Parse needs_build and needs_refresh from os_mods.py status. Full game update takes precedence. Launcher-only packaging/updater scripts do not mark the game stale; runtime and recompiler changes still do. Original Update Plugins / Refresh Plugin / Cancel / Play Anyway dialog. Play always rechecks, including a previously dismissed warning. |
| Rebuild manually | Original three-button Rebuild Plugins dialog selects DLL refresh or game update. Real LOG progress feeds original job strip. |
| Add plugins / community plugins | Native multi-file NRO picker copies plugins then checks readiness; pinned backend fetch and download-again retained. Native runtime provides Skyline APIs and ParamConfig. |
| Add local mods | Folder picker followed by copy/link choice; rejects duplicate destination; enable/apply through original backend; acknowledge after success. Failed copies/builds remain detectable. |
| Mod switches, groups, variants, remove, conflicts, updates, shared lists | Shared mod grouping/artwork and Python commands retained; managed imports are acknowledged without hiding failed/external additions. Up/down, Delete, dropdown keyboard selection and scroll dragging restored. |
| Marketplace | Original HTML bridge, install/remove, installed inventory and pinned components retained. Newly installed names are acknowledged only on success. Native WebView smoke test covers ready/installed bridge. |
| Settings, controllers, lobby, vanilla/modded mode | Original definitions and game config files retained. Dropdown keyboard handling, slider arrow keys, section navigation and scrollbar dragging restored. |
| Incomplete shaders | Versioned completion marker checked at boot. Original Later/Low/Medium/High prompt, Python shader compiler and real progress strip. Launcher exit cancels its worker; completed work remains resumable. |
| Source updates | Boot check, original opt-out prompt, commit ancestry comparison and existing updater (Windows UpdateOnly; Unix source-update step only). A different/development branch is not presented as an available update. Update does not rerun game installation. |
| Play | Original readiness choices, settings validation, mod preparation, Starting the Game bar; close after real game readiness (user-requested behavior). Existing independent-process and early-failure fixtures. |

The old Windows-only emulator import/setup routines are unreachable from the reference's
current Plugins screen. They are not exposed as a new flow in the shared launcher.
The Windows shell folder-dialog checkbox is represented by a shared styled copy/link
choice after the platform folder picker. System notification sounds are not reproduced.

Validation is local and offline except the separate native marketplace smoke test.
Desktop tests cover discovery across restarts, status-index mutation, failed-build retry,
full update vs DLL refresh, dismiss/recheck and close while prompting. Fifteen immutable
Windows reference images compare with zero differing pixels; added prompt/progress
screens are captured separately using the original drawing methods. Linux/macOS backend
plugin classification has fixture tests. Cross-published Linux/macOS artifacts still
require native-machine launch/gameplay testing; no real game recompilation, source update
or large shader compilation is performed by this regression suite.

## Integration with main (df947945, 2026-11-12)

The shared native launcher remains the default on all three platforms. Upstream's
alternate browser launcher entry points are superseded; its Linux plugin linking,
archive extraction, desktop authorization and app-window backend tests are retained.
Upstream runtime, GPU, input, rollback, online-arena, mod and build fixes are kept.

Launcher behavior ported from the updated Windows frontend includes selective settings
writes, inherited renderer override cleanup, NVIDIA/Intel automatic-renderer wording,
SPIR-V cache readiness, ready-venv selection and Python setup, vanilla rebuild checks,
and source-update game/launcher rebuilds with pending markers and retry. The launcher
rebuild helper supports the shared project; it will not compile the superseded frontend.
Original reference images are preserved; only the renderer caption/help wording is
masked in the graphics comparison, while the original geometry remains checked.
