# OpenSmash-Ultimate

OpenSmash-Ultimate turns *Super Smash Bros. Ultimate* into a native PC program by static recompilation. It translates the game's ARM64 code into C once, ahead of time, compiles it, and links it with a runtime that stands in for the Nintendo Switch. The game's code and data come from a copy you dumped from your own console.

It brings two projects together: [WindowSmash](https://github.com/OpenSmash-Ultimate/WindowSmash) and [ssbu-recomp](https://github.com/OpenSmash-Ultimate/ssbu-recomp).

> [!IMPORTANT]
> **This repository does not contain the game.** It holds no game files, keys, firmware or saves, and never will. You need your own copy of the game, dumped from your own Switch.

## The merge is in progress

Both projects' full histories are in this repository. WindowSmash's recompiler and runtime are the base; ssbu-recomp's features will be ported into it one area at a time.

| Where | What it is | State |
| :--- | :--- | :--- |
| Repository root | WindowSmash: recompiler, runtime, installer, tests and guides, at their original paths | Builds and plays today. Its unit tests run in CI on Windows, Linux and macOS. |
| [`ssbu-recomp/`](ssbu-recomp/) | ssbu-recomp's tree, with its history | Staged, waiting to be ported. The root build and installer don't use it. Its runtime still compiles in CI on Linux and macOS. |

This first step changes nothing about how either project builds or runs. Later steps bring in ssbu-recomp's Vulkan renderer, installer and prebuilt runtime, settings menu, modding and rollback netcode, and move files into their final places as each area is ported.

## Play today

WindowSmash's install steps are the path that works today. Until OpenSmash-Ultimate's first release, download WindowSmash from **[github.com/OpenSmash-Ultimate/WindowSmash](https://github.com/OpenSmash-Ultimate/WindowSmash)** and follow its guides. The same guides are here in [`docs/`](docs/):

| Guide | Read it to |
| :--- | :--- |
| [Setup guide](docs/BUILDING.md) | install WindowSmash and build the game, step by step. |
| [Playing](docs/PLAYING.md) | change the controls or the window, and find saves, screenshots and problem reports. |
| [Multiplayer](docs/MULTIPLAYER.md) | play with other people online or in a private room. |
| [DLC and unlocks](docs/DLC-AND-UNLOCKS.md) | use your DLC, or unlock every base-game fighter on your save. |
| [The 13.0.5 update](docs/UPDATES.md) | see which copies of the game count as 13.0.5 and how the update is applied. |
| [Linux](docs/LINUX.md) | install, build and run the experimental Linux version. |
| [macOS](docs/MACOS.md) | install, build and run the experimental version for Macs with Apple silicon. |
| [How it works](docs/TECHNICAL.md) | understand the recompiler, the runtime and every tool. |

You need game version 13.0.5 and a 64-bit Windows 10 or 11 PC. A processor with AVX2 is recommended; one with only SSE4.2 (x86-64-v2: Intel since 2008, AMD since 2011) works too, with a build for it that is expected to run slower. Linux and macOS (Apple silicon) are experimental. The [setup guide](docs/BUILDING.md) lists everything.

## `ssbu-recomp/`: staged code

[`ssbu-recomp/`](ssbu-recomp/) holds ssbu-recomp as it was when it was imported, with its history, so `git log` and `git blame` still show who wrote what. Since then, its two CI workflows have moved to [`.github/workflows/`](.github/workflows/), where GitHub runs them, and its README has lost the image line for the removed `media/menus.webp`. Each part moves out of the folder when it is ported, and the folder goes when it is empty.

If you play ssbu-recomp, keep using [its own repository](https://github.com/OpenSmash-Ultimate/ssbu-recomp) for now: its installer, updates and prebuilt runtime still come from there. Its scripts still name the repository's old address, `Denkishi/ssbu-recomp`, which GitHub forwards to the same place.

Material taken from the game was removed from every commit of ssbu-recomp's history on import:

- the file `ssbu-recomp/media/menus.webp`, a capture of the running game;
- the files `ssbu-recomp/tools/lifter/patches/kinds.txt` and `kinds_1305.txt`: instruction words from the game's code, in patches for new fighter kinds that are off by default. [`kinds_scan.py`](ssbu-recomp/tools/lifter/patches/kinds_scan.py), in the same folder, makes `kinds.txt` again from your own 13.0.4 dump. Nothing makes `kinds_1305.txt` again yet;
- in `ssbu-recomp/README.md`, the game's values in its example of a decompiled fighter script (Mario's `game_attack11`): its frame, wait and attack numbers are placeholders in every version.

## Getting help

- **Setup problems:** see [If something goes wrong](docs/BUILDING.md#if-something-goes-wrong) in the setup guide.
- **Questions:** ask on Discord, on [recomps.wtf](https://discord.gg/pkgyYGf3YB) (WindowSmash) or the [OpenSmash-Ultimate server](https://discord.gg/RyZpx3u2t4) (ssbu-recomp).
- **Bugs:** open an issue with your operating system, graphics card and driver version, what you did, what happened instead, and the end of the log (`build/play.log`, plus the `.txt` report from `reports/` after a crash).
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
