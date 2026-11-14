# Desktop launcher validation, 2026-11-11

Checked locally on Windows 11 x64 (build 26200), .NET SDK 8.0.403.

## Original artwork follow-up

The initial sidebar preview was replaced with the original drawing layouts.
The shared launcher now automatically selects the installer when no platform
build exists and the regular launcher when a build exists.

- `dotnet run --project tests/desktop -c Release`: passed. Checks synthetic
  project discovery, platform builds, config migration, setup routing, settings,
  original progress parsing, captured output, exit codes, interactive stdin,
  cancellation and all main screen renders.
- The same tests with `OPENSMASH_RENDERER=skia`: passed on Windows, exercising
  the portable renderer independently of GDI+.
- Original Windows comparison harness (now retired; its PNGs are preserved in `tests/desktop/References/`):
  passed. **Zero differing artwork pixels** at 1440×810 for these controlled states:
  - active installer build progress;
  - settled regular launcher Home;
  - all seven Settings tabs, and the open Window dropdown;
  - empty Mods view;
  - Plugins view;
  - marketplace heading (without a native browser in the headless harness);
  - Share mod-list and Check Mods dialogs.
  There are 15 controlled comparisons.
- Reference images come from the original Windows forms, rendered offscreen.
  Animation time and particle seed are controlled; real elapsed-clock text is
  masked in the automated comparison. The five original local comparisons also
  matched without those masks.
- Linux installer tests: 45 passed on Windows using their existing stand-ins.
- GUI authorization bridge tests: three passed, with every external command mocked.
- Python syntax checks passed for the publisher, artwork extraction tool and
  modified Linux/macOS installers.
- macOS installer tests are skipped on Windows because they require POSIX.

The offline native browser smoke test (`dotnet run --project tests/desktop-native -c Release`)
passed on Windows: the original page sent `ready`, accepted installed state and
returned that state through the JavaScript bridge. It uses a restrictive CSP,
opens no project/game and installs no mods. It caught the missing supported-OS
manifest required by Windows native control hosting; the manifest is now included.
The smoke test is local and is not part of the headless CI matrix.

The native Computer Use helper was unavailable for the follow-up (`failed to
connect native pipe`, Windows error 2). This revision's Windows verification is
offscreen rendering plus interaction/process tests, not a new desktop click-through.

Not verified: native Linux/macOS startup, pixel equivalence on those platforms,
other window sizes and every animation/input/modal state, game launch, full
installer completion, remote marketplace content/downloads end to end, or macOS
signing. The original marketplace page is embedded using Avalonia.Controls.WebView
12.1.1. Its transport and installed-state synchronization passed the separate
Windows native smoke test; Back navigation passed the headless interaction check.
The installer username field has the original physical bounds and Windows font
height, but native text editing is not covered by the pixel oracle. The original
optional symbolic-link mod import is not implemented; local folders are copied.
The GitHub workflow has been configured but not executed in this local folder.

## Standalone packaging follow-up

The default publisher now bundles and compresses managed assemblies, the .NET
runtime and native libraries. The Windows prototype is 46.9 MiB. Its EXE was
copied alone into a fresh temporary folder and started against the built project;
it stayed running for five seconds with no stderr and no companion files. The
owned test process was then stopped without starting a game or installer.

The offline native WebView smoke harness was separately published with the same
single-file/native-extraction/compression flags. Copied alone into another fresh
folder, it passed its ready/installed-state round trip. This covers the native
browser loader after bundling as well as ordinary startup. License notices are
embedded as a ZIP resource and remain readable through `--licenses`.

The launcher now prefers a project beside its executable over a remembered
project, so moving the standalone EXE to another built tree selects that tree.
Linux/macOS standalone artifacts are cross-published; their native execution
and signing status remain subject to the limitations above.

## First-run download follow-up

The shared launcher now renders a location/download/retry screen before the game
installer when no project is found. Headless checks cover those three states and
all 15 prior Windows pixel comparisons still pass.

The bootstrap tests pass for release and GitHub ZIP layouts, SHA-256 mismatch,
network failure, unknown download length, cancellation, incomplete source,
traversal/rooted paths, archive symlinks, preservation of unrelated destinations,
reuse without download, and copying a complete synthetic macOS app.

A live Windows backend check fetched the pinned public GitHub archive (8,804,980
bytes, 1,656 entries), extracted it into a fresh `OpenSmash` folder and installed
the published standalone launcher there as `OpenSmash.exe`. The copied launcher
stayed running for five seconds with no stderr against that downloaded project;
its owned test process was then stopped. No compiler, game installer or game was
started. This verifies the live download/extract/copied-launcher startup path;
it is not a native mouse-driven onboarding or full game-build test.

Compiled-resource checks also passed when switching development -> release ->
development manifests. This caught and fixed an MSBuild incremental-resource
cache issue; native publishers now inspect the actual bundled host with
`--source-info` before archiving it.

The release-source tests use a real temporary Git repository: `git archive`
includes the intended committed files, excludes untracked data, and produces a
manifest whose commit/checksum match the archive. The draft release workflow YAML
was parsed locally. GitHub Actions has not run, and no remote tag or release has
been created. This working folder is a source export without `.git` history.

## Earlier repository-wide check

The previous full run completed 1,177 tests with 72 skipped and two failures in
unchanged code outside the launcher:

- `test_installer.InstallerDecisionTests.test_source_version_filled_in_by_downloads`:
  this downloaded tree contains a substituted commit/date rather than the literal
  Git export placeholder expected by the test.
- `test_shader_aot.EmptyFilesTests.test_empty_files_are_not_unreadable`:
  a truncated compressed frame counts as zero unreadable files instead of one.
  It also reproduces when `test_shader_aot.py` runs alone.

The earlier `python -m nex.selftest` passed. The unrelated runtime-wide suite was
not repeated for the subsequent UI port and GUI installer adapters.

## Launcher consolidation and Play handoff

The shared project is now `OpenSmash.Launcher/OpenSmash.csproj`, assembly/output
`OpenSmash`. The retired Windows Forms source, extraction generator, separate
reference project and its obsolete implementation-specific tests were removed.
Fonts/artwork/marketplace HTML were moved into the shared project's own assets.
All fifteen original screenshot comparisons pass using the preserved PNG fixtures.

Synthetic native process tests verify readiness detection, early failure, literal
Windows argument quoting (including quotes, percent signs and ampersands), and
continued game output after launcher-owned handles are released. No real game
was launched for this change. Windows uses inherited file handles; Linux/macOS
redirect output to files before exec, avoiding launcher-owned output pipes.
Linux/macOS runtime handoff still requires native hardware validation.

## Portable startup location

Startup no longer reads/writes remembered project paths or searches parent/current
working directories. Tests pass for an empty subfolder inside a project, a local
built project, a local unbuilt project and the macOS bundle-parent location.
An explicit --project argument remains an intentional override.
