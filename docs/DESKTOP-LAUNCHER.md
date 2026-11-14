# Shared desktop launcher

`OpenSmash.Launcher/` uses the original launcher's artwork and layout code in an
Avalonia window on Windows x64, Linux x64 and Apple silicon macOS. The retired Windows Forms launcher has been removed; this is the single launcher
implementation for all three platforms.

## Startup

- When no project is found, show **Choose your folder**. Pick a parent directory;
  setup creates `<parent>/OpenSmash`, downloads the project and opens its copied
  launcher. The next screen is **First-time setup**. No source ZIP or launcher
  build step is needed from the player.
- When the project exists but the platform's game executable is absent, open
  **First-time setup**.
- Setup shows the original file cards, options and username field. **Set up and
  build** runs the platform installer in this window, with its real steps, log,
  percentages, shader-priority question, errors, Stop and Resume controls.
- Once the game is built, **Continue** opens the regular launcher. Subsequent
  launches open the regular launcher immediately.
- `--installer` explicitly opens setup even if the game has already been built.
- `--first-run` opens location selection even when a project was found; this is
  useful for installing another copy or previewing onboarding.
- `--play` starts the built game directly. Lobby/offline settings come from the
  game's existing online configuration.

The game remains experimental on Linux and macOS. No game files are distributed.

## Build for each platform

Use the corresponding file in the project root (Python 3 and the .NET 8 SDK or
newer are required to build):

- **Windows:** `Build Launcher Windows.bat` writes `OpenSmash.exe`.
- **Linux:** `bash "Build Launcher Linux.sh"` writes `OpenSmash`.
- **Apple silicon macOS:** `bash "Build Launcher macOS.command"` writes `OpenSmash.app`.

Each build uses a fresh output directory and replaces only the launcher in the
project root. On macOS/Linux, `chmod +x` the build file to run it directly.
Players downloading the release executable do not need Python or .NET to open
it; the game installer handles its separate build dependencies.

Pressing Play shows **STARTING GAME** while mods are prepared and the game starts.
Once the game window is ready, the launcher closes. Game output goes directly to files (`build/opensmash-play.log`, and the native
`build/play.log` on macOS), so neither the game nor its logging depends on
the launcher staying open. Startup failures keep the launcher open with an error.

## Run and publish

Developers need the .NET 8 SDK or newer (a recent SDK also enables Avalonia's newer compiler analyzers):

```
dotnet run --project OpenSmash -- --project "/path/to/OpenSmash"
python tools/build_desktop_launcher.py --rid win-x64
python tools/build_desktop_launcher.py --rid linux-x64
python tools/build_desktop_launcher.py --rid osx-arm64
```

The default publish creates a compressed, self-contained **single executable**
for Windows/Linux and a macOS **OpenSmash.app** bundle. No .NET installation or
loose DLLs are required. Windows/Linux users may move just the executable into
their project; macOS users move the complete `.app` as one item in Finder.
Archives and readable notices are in `dist/launcher/<rid>/` and beside that folder.
License notices are also embedded in the executable; `--licenses` opens a cached
copy. `--layout folder` retains the unpacked publish option for development.
Use a fresh `--output-root <folder>` for a new build: nonempty outputs are refused
to prevent mixing old DLLs with the standalone package.

Native libraries unpack automatically into the .NET bundle cache on first run
(`%TEMP%/.net` on Windows, `$HOME/.net` on Linux/macOS). The game/project and system
GUI/web-engine dependencies remain external. This bundles the launcher, not the
game, compiler, Python installation or your dumped files.

Open `OpenSmash.exe`, `OpenSmash.app`, or the Linux `OpenSmash`
executable. Startup uses only the folder containing that executable (or the
folder containing the macOS app bundle). It does not consult remembered projects,
parent projects or the working directory. An empty folder opens download setup;
an unbuilt local project opens the installer; a built local project opens the
regular launcher. An explicit `--project` argument remains available as an override.
Existing valid projects are reused; unrelated nonempty `OpenSmash` folders are
refused. Cancellation and failed downloads remove only their unique staging
folder. The source is checked before an atomic rename into the final location.
Windows copies the launcher as `OpenSmash.exe`, Linux as `OpenSmash`, and
macOS copies the complete `OpenSmash.app`, including its native libraries.

Release builds embed a source URL, commit and SHA-256 from `--source-manifest`.
They download the source asset from that exact release and verify it before
extraction. Developer builds without a supplied manifest use the commit-pinned
GitHub archive in `OpenSmash.Launcher/BootstrapSource.json`; that development
fallback relies on HTTPS plus the pinned commit rather than a release checksum.
See [GitHub release instructions](DESKTOP-RELEASE.md).

Unix archives restore executable permissions even when built on Windows. The
macOS app is unsigned; signing/notarization are not configured. Native builds
and testing on each destination OS are required before promoting a release.

Linux needs Avalonia's desktop native dependencies, including X11 and fontconfig.
Its GUI installer uses `pkexec` for system dependency authorization when available;
without it, passwordless/cached sudo authorization is required. Passwords are
never collected by the launcher. A denied authorization stops setup with its log
preserved. Terminal installations keep their existing sudo behavior.

## Original artwork

`Visuals/InstallerArtwork.cs`, `LauncherArtwork.cs`, `DetailsArtwork.cs`, and the
shared theme retain the original drawing layouts. Their fonts, emblem and
marketplace page now live in this project's `Assets/` folder. The old source
extraction tool and Windows Forms project were retired after the assets and
fifteen reference screenshots were preserved.
The port preserves the original drawing coordinates, fonts, colours, gradients,
scanlines, grain, slashes, outlines, cards, buttons, animation curves and timing.
The body and mono fonts are pinned; see `OpenSmash.Launcher/Assets/README.md`.
The canvas renders at the monitor's physical pixel density.

Windows uses GDI+ to preserve the original raster output. Linux and macOS use
Skia with the same artwork and fonts. Their native rasterization has not yet
been compared on hardware. `OPENSMASH_RENDERER=skia` also selects the portable
renderer on Windows for development checks.

The Home, Settings, Mods and Plugins layouts and the mod-list/conflict dialogs use
the original drawing methods. The marketplace embeds the original
`MarketplaceHtml.html` with its existing CSS and a portable message transport.
Settings -> Developer offers mod asset hot reload, disabled by default. Enable it
before launching the game, then press L + R + A in training mode to refresh changed
mod data in loaded buffers. Files that outgrow those buffers and models/textures
already uploaded to the GPU still need a new match. The preference is stored as
`hot_reload_mod_assets = 1` (enabled) or `0` (disabled) in `launcher.ini`'s
`[opensmash]` section, under `config/` once migrated. A game built before this
setting was added must be rebuilt; updating the launcher alone cannot change its runtime.
Settings -> Display -> Preload all shaders is a Compile shaders button, not a
saved preference. It runs the installer's `tools/shader_aot.py --priority medium`
step, reusing completed work. The bottom progress bar shows listing/compilation
counts and the estimated time remaining; while running, the button offers Cancel
compilation. Duplicate starts are ignored, and closing the launcher cancels its
compiler process. Failures are reported with the log path, and the next run resumes.
This prepares the disk shader cache; the game's existing startup loading policy
and memory budgets are unchanged.
Watched mod extensions are `.numshb`, `.nusktb`, `.nuhlpb`, `.numatb`, `.numdlb`,
`.numshexb`, `.nutexb`, `.nuanmb`, and `.btnx` (case-insensitive).
On the verified 13.0.5 build, the experimental vector-only `.numatb` path
(including CustomVector8) queues edits for the material's normal rendering update,
then uses the game's setter and shader dirty flags there. Materials are
tracked from construction to destruction and indexed by material name; reloading
does not scan the game heap. Validation has a 200 ms limit and defers the edit
without applying it if that limit is exceeded. This also permits
a re-export whose serialized size changed. Material/shader names and parameter
types must stay the same. Matching checks the previous values of edited vectors while preserving unrelated
parameters changed by gameplay or animation, and edits
are rejected if another watched asset has the same material identity. Unsupported
or currently unloaded material edits stay pending and are retried on the next
L + R + A; the log explains why. File snapshots advance only after rendering confirms
all queued writes. Destruction or invalidation cancels pending writes; a partial
application requires restarting before another attempt. The game copies each
material's animation baseline back into the live value every frame (main+36a8320),
which used to restore the old color on the next frame. The applied value now replaces
that baseline on this path only while the baseline still equals the replaced value,
so animated keys still win; it is dropped when the material is destroyed. This has
not yet been confirmed in a live match; the path remains experimental.
Other asset formats still use raw buffer replacement.
Installs/removals, progress, installed-state synchronization, shared lists,
conflict checks and local folder imports use the existing Python backend.
Plugin requirements use the project's checksum-pinned `std_plugins.py` releases;
Skyline and ParamConfig loader APIs are provided by the native game runtime.
Local imports currently copy a folder; the original optional symbolic-link import
has not been ported.

The marketplace uses Avalonia.Controls.WebView 12.1.1 (MIT), with Avalonia 12.1.6.
Windows needs the Edge WebView2 Runtime; macOS uses the system WKWebView. Linux
needs GTK 3, WebKitGTK 4.1 and libsoup 3 (Debian/Ubuntu:
`sudo apt install libgtk-3-0 libwebkit2gtk-4.1-0 libsoup-3.0-0`). See
[Avalonia's platform prerequisites](https://docs.avaloniaui.net/docs/app-development/embedding-web-content#platform-prerequisites).
Missing or failed browser startup displays a message in the original browser pane.
Native browser behavior still needs end-to-end testing on each OS. Do not run
another launcher or installer against the same project while modifying it here.

## Verification

```
dotnet run --project tests/desktop -c Release
```

Set `OPENSMASH_TEST_SCREENSHOT` to an output PNG path to save the individual
installer and launcher renders in that directory. The tests use synthetic
projects and exercise startup routing, settings, installer arguments, live
progress, process input/output, cancellation and rendering.

On Windows, the desktop tests automatically compare fifteen renders with the
original PNG baselines in `tests/desktop/References/`. Only live elapsed-clock
text is excluded. The retired Windows Forms source and separate reference
project are no longer needed. CI runs these comparisons as part of the normal
Windows test run.
See [local validation results](DESKTOP-LAUNCHER-VALIDATION.md) for coverage and gaps.
