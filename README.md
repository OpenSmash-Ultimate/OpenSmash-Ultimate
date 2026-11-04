# OpenSmash-Ultimate

[![Discord](https://img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white)](https://discord.gg/pkgyYGf3YB)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)

**[Website](https://denkishi.github.io/opensmash-site/) · [Discord](https://discord.gg/pkgyYGf3YB) · [Releases](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/releases) · [Installation guide](docs/INSTALL.md) · [Issues](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/issues)**

OpenSmash-Ultimate turns *Super Smash Bros. Ultimate* into a native PC program by static recompilation. It translates the game's ARM64 code into C once, ahead of time, compiles it, and links it with a runtime that stands in for the Nintendo Switch. The game's code and data come from a copy you dumped from your own console.

It is the merge of two projects, [WindowSmash](https://github.com/OpenSmash-Ultimate/WindowSmash) and [ssbu-recomp](https://github.com/OpenSmash-Ultimate/ssbu-recomp): one repository, one runtime (WindowSmash's, at game version 13.0.5) and one community. New work, issues and pull requests go here; the two old repositories are kept for their history.

> [!IMPORTANT]
> **This repository does not contain the game.** It holds no game files, keys, firmware or saves, and never will. You need your own copy of the game, dumped from your own Switch.

## Install

1. Download **[OpenSmash Setup.exe](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/releases/latest/download/OpenSmash-Setup.exe)** from the latest [release](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/releases) and run it. It does the rest.
2. Choose where OpenSmash goes (about 60 GB free; not the Desktop or Documents) and click **Download and set up**. It downloads the project from GitHub into that folder, makes the **OpenSmash** and **Play SSBU** shortcuts (desktop and Start menu), and opens the launcher there.
3. In the installer window, check that it found your game files (game plus its 13.0.5 update), keys and optionally firmware; fix any folder with **Browse**. Click **Set up and build** and wait: 30 minutes to a few hours.
4. Press **Play** in the launcher, or use the **Play SSBU** shortcut.
5. To update, open **Settings -> Update** in the launcher. Mods are in the launcher's **Mods** view; online is in **Settings -> Online**.

You need your own copy of the game, version 13.0.5, dumped from your own Switch, and a 64-bit Windows 10 or 11 PC; AVX2 is recommended, SSE4.2 works too. Linux and macOS (Apple silicon) are experimental. The [installation guide](docs/INSTALL.md) explains each step; [docs/BUILDING.md](docs/BUILDING.md) is the step-by-step setup and building from source.

## What's in it

| Area | What you get | Where |
| :--- | :--- | :--- |
| The game | The whole game recompiled for x86-64: menus, every mode, DLC fighters and stages, movies (VP9), music. | here |
| Online | Rooms with a code (`XXXXX-XXXXX`), LAN rooms, a lobby, relay and direct paths; rollback netplay for fights (`SSBU_RB`, from WindowSmash #103 and ssbu-recomp #116). | [#55](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/55), [#59](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/59), [#63](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/63) |
| Mods | The OpenSmash launcher and mod manager, Skyline plugins, ARCropolis mods, a switch for every mod in the game's settings and in the launcher; native mods written against `hook.h`. | [#62](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/62), [#63](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/63), [#67](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/67) |
| New fighters | Fighters with a kind and files of their own (up to 16), all eight colours; fighters written as C++ source. | [#67](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/67), [#68](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/68) |
| Graphics | Direct3D 11 by default; Vulkan as a choice in `display.ini` and the settings menu; Metal on macOS; shader prewarm and Prepare Shaders. | [#64](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/64), [#26](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/26) |
| Settings | Game language, master volume, controllers (dead zones, invert, ignore), GameCube adapters, keyboard keys, low-latency display mode, the album. | [#56](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/56), [#57](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/57), [#60](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/60), [#3](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/3) |
| Memory | Caps on the shader prewarm and on the copy kept for mod reloads; a memory line in the log. | [#66](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/pull/66) |

Rows that link to pull requests are being reviewed and land in `main` as they are merged.

## Guides

| Guide | Read it to |
| :--- | :--- |
| [Installation guide](docs/INSTALL.md) | install, update, add mods and go online. |
| [Setup guide](docs/BUILDING.md) | do every setup step by hand, and build from source. |
| [Playing](docs/PLAYING.md) | change the controls or the window, and find saves, screenshots and problem reports. |
| [Multiplayer](docs/MULTIPLAYER.md) | play with other people online or in a private room. |
| [Mods and Skyline plugins](docs/SKYLINE.md) | install mods and plugins and turn them on or off. |
| [DLC and unlocks](docs/DLC-AND-UNLOCKS.md) | use your DLC, or unlock every base-game fighter on your save. |
| [The 13.0.5 update](docs/UPDATES.md) | see which copies of the game count as 13.0.5 and how the update is applied. |
| [Linux](docs/LINUX.md) · [macOS](docs/MACOS.md) | install, build and run the experimental versions. |
| [How it works](docs/TECHNICAL.md) | understand the recompiler, the runtime and every tool. |

## `ssbu-recomp/`

[`ssbu-recomp/`](ssbu-recomp/) holds ssbu-recomp as it was imported, with its history, so `git log` and `git blame` still show who wrote what. Its parts move out of the folder as they are ported to the root runtime, and the folder goes when it is empty. Material taken from the game was removed from every commit of its history on import.

## Getting help

- **Questions and finding players:** the OpenSmash-Ultimate Discord, **[discord.gg/pkgyYGf3YB](https://discord.gg/pkgyYGf3YB)** (WindowSmash's and ssbu-recomp's communities in one server).
- **Setup problems:** see [If something goes wrong](docs/BUILDING.md#if-something-goes-wrong) in the setup guide.
- **Bugs:** open an [issue](https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/issues) with your operating system, graphics card and driver version, what you did, what happened instead, and the end of the log (`build/play.log`, plus the `.txt` report from `reports/` after a crash).
- **Security problems:** report them privately, as [SECURITY.md](SECURITY.md) explains. Don't open a public issue.

> [!WARNING]
> Never post game files, keys, saves or generated game code in issues or pull requests. Logs and reports can contain your folder names, player names and internet addresses, so read them before posting. A crash report's `.dmp` file holds part of the game's memory: share it only privately, with someone who asked for it.

## For developers

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Its three rules hold for every change: nothing derived from the game goes into the repository, clean sources only, and every claim is measured.

WindowSmash's source-only tests use made-up data and need no game files. Run them from the repository root, in PowerShell or a Unix shell:

```
python -m pip install -r tests/requirements-dev.txt
python -m unittest discover -s tests -v
cd tools
python -m nex.selftest
```

CI runs these workflows from [`.github/workflows/`](.github/workflows/):

| Workflow | Runs on | What it checks |
| :--- | :--- | :--- |
| WindowSmash tests | Windows, Linux, macOS | The unit tests and the NEX server self-test. |
| Runtime compile | Windows, Linux, macOS | The whole runtime, with its platform layer, compiles and links into the game's program as a player's build makes it, on a made-up game (`tools/dev/runtime_compile_check.py`); on Windows and Linux for both instruction sets. Every runtime source is in the program unless the script lists it as left out on purpose, and none is in a subfolder no build looks in. |
| ssbu-recomp Linux runtime | Linux | ssbu-recomp's host runtime compiles; its synthetic differential test, platform self-test and compile-scheduler test pass; `setup.py --tools-only` finds every tool and library the build needs; its log collector leaves game files out of a report. Its installer script, `install_deps.sh`, is only checked for syntax: the workflow installs the packages itself. |
| ssbu-recomp macOS runtime | macOS (Apple silicon) | The same on macOS, where `install_mac.sh` does run. It also builds the glslang shim, checks that `setup.py` stops at the dump check when given an empty dump, and reports what MoltenVK offers on the runner. |
| No game data | Linux | No Switch files (`.nca`, `.nsp`, `.nsz`, `.xci`, `.nso`, `.nro`, `.arc`), no key files, and no file over 10 MB, in any commit. |

## License

OpenSmash-Ultimate is free software under the GNU General Public License, version 3 or (at your option) any later version: **GPL-3.0-or-later**. [`LICENSE`](LICENSE) holds the license text. Third-party parts keep their own licenses, listed in [NOTICE.md](NOTICE.md).

The license covers this project's own source. It gives no rights to Super Smash Bros. Ultimate, its files, or anything built from them.

Super Smash Bros. Ultimate and related names belong to their respective owners. OpenSmash-Ultimate is an independent project and is not affiliated with or endorsed by Nintendo.
