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

1. Download **OpenSmash.exe** (Windows), **OpenSmash** (Linux), or **OpenSmash.app** (Apple silicon macOS) from the project's release assets.
2. Open it and choose where to save the project. The launcher downloads the required source into an **OpenSmash** folder and opens setup there.
3. Choose your own game files, keys and optional firmware, enter your online name, and start setup. Dependencies, game compilation, progress and logs are handled by the launcher. Completed work is kept if you stop and resume.
4. Once setup finishes, use **Play** in OpenSmash. Later launches beside that built project open the regular launcher.

If you already downloaded the source, place the matching launcher in that folder, or build it with **Build Launcher Windows.bat**, **Build Launcher Linux.sh**, or **Build Launcher macOS.command**.

The root installer/play shortcuts are removed. Internal setup remains under `tools/`:

| Platform | Setup backend |
| --- | --- |
| Windows | `tools/install_windowsmash.ps1` |
| Linux | `tools/linux/install.sh` and `install_linux.py` |
| macOS | `tools/mac/install.sh` and `install_mac.py` |

Developers can run those scripts directly, but users normally open OpenSmash. Keep the `tools/` folder: the standalone launcher downloads it with the project and then calls these backends.

## Updates

Use **Settings ? Update ? Check for updates**. The updater replaces changed project files while preserving game files, settings, saves and builds. Then use **Play** to prepare anything that changed. Copies with local changes or a different branch may need a manual update with git.

To replace the launcher itself, download its matching release asset or use the platform's Build Launcher script.

## Mods

Mods (skins, stages, music, movesets, new fighters from GameBanana, or a `.zip` you have) are managed in the launcher's **Mods** view: install, turn on and off, and apply them to the game. ARCropolis mods and Skyline plugins work; the runtime already provides ARCropolis, Smashline 2, lib_paramconfig and the CSK Collection, so do not install those. **Check** lists mods that change the same files and mods with a newer file; **Share** saves your mods as a list or installs a friend's list. Details: [Mods and Skyline plugins](SKYLINE.md) and [Playing](PLAYING.md#mods-on-and-off).

## Online

OpenSmash starts the game joined to the online lobby; if the lobby does not answer, the game plays offline. Set your name and lobby in the launcher's **Settings → Online** tab, or in the game (<kbd>Esc</kbd> → **Online**). Players only meet when they run the same game version (13.0.5) and the same mods. Rooms with a code, LAN rooms and rollback netplay are explained in the [multiplayer guide](MULTIPLAYER.md). Allow the game through Windows Firewall (the installer option above) or direct connections will fail and matches use the relay.

## If something goes wrong

- Run the installer again; it resumes.
- Read `install.log` (installer) and `build\play.log` (game). They can hold folder names and your online name, so read them before posting.
- See [If something goes wrong](BUILDING.md#if-something-goes-wrong) in the setup guide, ask on [Discord](https://discord.gg/pkgyYGf3YB), or open an [issue](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/issues).

## Linux, macOS and building from source

Linux and macOS (Apple silicon) are experimental: see [Linux](LINUX.md) and [macOS](MACOS.md). Doing each step by hand, building the launcher, and the developer tools are in the [setup guide](BUILDING.md) and [CONTRIBUTING.md](../CONTRIBUTING.md).
