Download the setup launcher for your platform:

- **Windows x64:** `OpenSmash.exe`
- **Linux x64:** extract `OpenSmash-linux-x64.tar.gz` and run `OpenSmash`.
- **Apple silicon macOS:** extract `OpenSmash-macos-arm64.tar.gz` and open `OpenSmash.app`.

Choose where to install. Setup creates an **OpenSmash** folder, downloads the
matching project source, checks its SHA-256, and opens the game installer from
that folder. Existing projects open directly. You do not need to download the
source manually or build the launcher.

You provide your own game dump and keys on the next screen. No game files are
included or downloaded. The installer obtains the tools/dependencies needed to
build the game. Linux and macOS game support is experimental; the macOS app is
unsigned. The marketplace requires the platform web-engine dependencies described
in the [desktop launcher guide](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/blob/main/docs/DESKTOP-LAUNCHER.md).

`OpenSmash-Source.zip` is downloaded automatically by setup. `SHA256SUMS.txt`
contains the checksums of the release assets. Keep all source and setup assets
on this release: the launchers are tied to this source archive.
