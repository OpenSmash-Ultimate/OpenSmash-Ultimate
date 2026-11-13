# Releasing the shared setup launcher

The player flow is:

1. Download the platform setup asset from GitHub Releases.
2. Pick a parent directory, for example `D:\Games` or `~/Games`.
3. Setup creates `OpenSmash` inside it, downloads this release's source archive,
   verifies its SHA-256 and copies the launcher into the project.
4. The installed launcher opens the existing game installer. The player selects
   their own game dump and keys; the installer obtains the compiler/dependencies
   and builds the game. Later starts open the regular launcher.

The source archive contains committed project code, not game content, compilers,
Python environments or a compiled game. Those dependencies are handled by the
existing platform installers. Existing installations skip location selection.

## Maintainer workflow

`.github/workflows/desktop-release.yml` runs for `desktop-v*` tags. The old Windows-only release workflow has been removed.

Merge and push the launcher/workflow changes to the default branch in a **Git
checkout** of the repository, then create a tag, for example:

```sh
git tag desktop-v0.1.0
git push origin desktop-v0.1.0
```

Alternatively, run **OpenSmash setup release** manually with an existing
`desktop-v*` tag. A folder downloaded as a source ZIP has no `.git` history;
`prepare_desktop_source.py` requires a Git checkout, and cannot publish a release
directly from such an exported folder.

The workflow audits the committed source, creates one `git archive` source ZIP,
records its commit/checksum, and embeds the same source manifest into all three
native builds. It runs the bootstrap/process/render tests on each runner and the
preserved original-image comparisons on Windows. It then creates an **unpublished
draft prerelease**, containing:

| Asset | Purpose |
| --- | --- |
| `OpenSmash.exe` | One executable for Windows players |
| `OpenSmash-linux-x64.tar.gz` | Executable with Unix permissions |
| `OpenSmash-macos-arm64.tar.gz` | Complete Apple silicon `.app` |
| `OpenSmash-Source.zip` | Source downloaded automatically by all setup launchers |
| `bootstrap-source.json` | Exact source commit, asset URL and checksum |
| `OpenSmash.exe.sha256` | Checksum used by the Windows game installer |
| `SHA256SUMS.txt` | Release asset checksums |

Review the draft, then publish it on GitHub. The embedded public source URL
becomes available when the release is published. Before publication, public
downloads of its private draft assets do not work. The workflow refuses to
replace assets on an already published release; corrections require a new tag.
Keep `OpenSmash-Source.zip` attached and unchanged after publication.

Signing/notarization are not configured. The existing Linux/macOS runtime and
visual verification gaps remain documented in `DESKTOP-LAUNCHER-VALIDATION.md`.
This local source export has not pushed a tag or created/published a GitHub release.

## Building a release manually

From the intended commit in a Git checkout:

```sh
python tools/prepare_desktop_source.py --repository OWNER/REPO --tag desktop-v0.1.0
python tools/build_desktop_launcher.py --rid win-x64 --output-root dist/release-build --source-manifest dist/launcher-release-source/bootstrap-source.json
python tools/build_desktop_launcher.py --rid linux-x64 --output-root dist/release-build --source-manifest dist/launcher-release-source/bootstrap-source.json
python tools/build_desktop_launcher.py --rid osx-arm64 --output-root dist/release-build --source-manifest dist/launcher-release-source/bootstrap-source.json
```

Use a fresh output root for each release. Upload the source ZIP and the matching
launchers to the tag's release together. Publishing against an unrelated source
archive breaks the launcher's embedded checksum.

## Local checks

`dotnet run --project tests/desktop -c Release` exercises synthetic archives,
checksums, cancellation, unsafe paths, unrelated destinations, macOS bundle
copying and the first-run screens without downloading any real project.

For an explicit live source-download check with a published standalone launcher:

```sh
dotnet run --project tests/desktop -c Release -- --bootstrap-live /new/test/parent /path/to/published/launcher
```

This downloads/extracts the developer build's pinned source and copies the
specified launcher into the test destination; it does not run the game installer.
Use the published `.app` executable path on macOS, so its complete app can be copied.
