# Installing OpenSmash-Ultimate

[← README](../README.md) · [Playing](PLAYING.md) · [Multiplayer](MULTIPLAYER.md) · [Mods](SKYLINE.md) · [Step-by-step setup and building](BUILDING.md)

This is the short path from nothing to playing, on Windows 10 or 11. The game is built on your PC from your own game files; nothing in this repository or in its releases contains the game.

> [!IMPORTANT]
> You need your own copy of *Super Smash Bros. Ultimate*, dumped from your own Nintendo Switch, with your console's keys (`prod.keys`, `title.keys`). It must be **version 13.0.5**: the game plus its 13.0.5 update, side by side, or an installation in yuzu or Ryujinx that has the update ([which copies count](UPDATES.md#what-counts-as-1305)). Firmware is optional: it gives the console's own fonts, with Chinese, Japanese and Korean text. Never post game files, keys or saves anywhere.

## What you need

- 64-bit Windows 10 or 11. A processor with AVX2 is recommended; SSE4.2 works but is slower.
- About 60 GB of free disk space for the build, and 16 GB of RAM is comfortable.
- An internet connection for the first install (Python, a compiler and SDL3 are fetched if you do not have them).

## Install

1. Download **[OpenSmash Setup.exe](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/releases/download/v0.3.0-alpha.3/OpenSmash-Setup.exe)** from the [latest release](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/releases) (GitHub names the file `OpenSmash-Setup.exe`; `OpenSmash-Setup.exe.sha256` beside it is its checksum) and run it. That one file is all you need: it does the rest.

   Windows may warn that the publisher cannot be verified: choose **Run** (or **More info**, then **Run anyway**).
2. The setup asks where OpenSmash goes. The default is an `OpenSmash` folder next to the setup file (or the setup's own folder, when the file is alone in a folder of its own); **Browse** picks another. Choose a drive with about 60 GB free, not the Desktop or Documents (cloud sync and long paths cause trouble). A folder that already has other files in it is never filled without asking: the setup offers a new folder inside it instead. **Download and set up** then:
   - downloads the project from GitHub (the newest `main`, about 20 MB) and unpacks it into the folder;
   - copies the launcher into it as `OpenSmash.exe`;
   - makes the shortcuts, if their boxes are ticked: **OpenSmash** (the launcher) and **Play SSBU** (the game straight away once it is built) on the desktop and in a Start menu folder named OpenSmash;
   - opens the launcher from the folder, which shows the installer window below while no game is built.

   Already have a copy (a ZIP from the **Code** button, extracted, or a `git clone`)? Press **I already have it** and choose its folder, or run `Install WindowSmash.bat` in it. The setup remembers the folder, so running the setup file again later opens the launcher there.
3. In the installer window, point it at your files. It looks in yuzu's and Ryujinx's folders by itself and shows a line under each entry saying what it found.
   - **Game files**: the folder with your game (`.xci`, `.nsp`, `.nca`), its 13.0.5 update, and any DLC.
   - **Keys**: the folder with `prod.keys` and `title.keys`.
   - **Firmware** (optional).
   - **Username**: your online name.

   **Check files** re-reads the folders. If the game is not 13.0.5, the line turns red and tells you what to add; **Set up and build** will not go on until it is fixed.
4. Choose the options on the right: a desktop shortcut, the Windows Firewall rule (asks for administrator rights once; needed to play online), preparing shaders after the build (you pick low, medium or high; it stops the first-time stutter), checking for updates first, accepting Microsoft's license terms for the compiler download (about 570 MB), and the number of compiler jobs (fewer keeps the PC usable). Click **Set up and build**.
5. Wait: **30 minutes to a few hours**. You can keep using the PC. If it stops or you close it, run the installer again: finished steps are skipped and it carries on. Everything it prints goes to `install.log`.
6. When it finishes, the launcher opens. Press **Play**, or later use the **OpenSmash** or **Play SSBU** shortcut, `OpenSmash.exe`, or `Play SSBU.bat`.

## Update

OpenSmash is updated from GitHub's `main` branch, not from release files.

- The launcher checks when it opens. Open **Settings**, then the **Update** tab, to check again. If a newer version exists it says how many new commits there are; apply it from there.
- Applying runs the repository's installer in update mode: changed files are replaced, and your game files, settings, save and builds are left as they are. Then press **Play**, which rebuilds what changed (minutes, not hours).
- A copy with local changes is not updated automatically; update it with `git`, or use a clean download. A development checkout whose commit is not on `main` cannot be checked.
- Running `Install WindowSmash.bat` again also brings the folder up to date and builds again.
- To replace an old `OpenSmash.exe`, take the one from the newest release (compare its SHA-256 with `OpenSmash.exe.sha256`), or run `Build Launcher.bat` with the .NET 8 SDK installed.

## Mods

Mods (skins, stages, music, movesets, new fighters from GameBanana, or a `.zip` you have) are managed in the launcher's **Mods** view: install, turn on and off, and apply them to the game. ARCropolis mods and Skyline plugins work; the runtime already provides ARCropolis, Smashline 2, lib_paramconfig and the CSK Collection, so do not install those. **Check** lists mods that change the same files and mods with a newer file; **Share** saves your mods as a list or installs a friend's list. Details: [Mods and Skyline plugins](SKYLINE.md) and [Playing](PLAYING.md#mods-on-and-off).

## Online

`Play SSBU.bat` and the launcher start the game joined to the online lobby; if the lobby does not answer, the game plays offline. Set your name and lobby in the launcher's **Settings → Online** tab, or in the game (<kbd>Esc</kbd> → **Online**). Players only meet when they run the same game version (13.0.5) and the same mods. Rooms with a code, LAN rooms and rollback netplay are explained in the [multiplayer guide](MULTIPLAYER.md). Allow the game through Windows Firewall (the installer option above) or direct connections will fail and matches use the relay.

## If something goes wrong

- Run the installer again; it resumes.
- Read `install.log` (installer) and `build\play.log` (game). They can hold folder names and your online name, so read them before posting.
- See [If something goes wrong](BUILDING.md#if-something-goes-wrong) in the setup guide, ask on [Discord](https://discord.gg/pkgyYGf3YB), or open an [issue](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/issues).

## Linux, macOS and building from source

Linux and macOS (Apple silicon) are experimental: see [Linux](LINUX.md) and [macOS](MACOS.md). Doing each step by hand, building the launcher, and the developer tools are in the [setup guide](BUILDING.md) and [CONTRIBUTING.md](../CONTRIBUTING.md).
