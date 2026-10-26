# Contributing

Thanks for wanting to help. OpenSmash-Ultimate is a static recompilation of *Super Smash Bros. Ultimate* to a native PC program: the tools that translate the game, the runtime that replaces the console, and the tooling around them. It is the merge of two projects, WindowSmash and ssbu-recomp, and the merge is still going on:

- **The repository root is WindowSmash**: its recompiler, runtime, installer, tests and guides, at their original paths. This is the base, and the version that players install today.
- **[`ssbu-recomp/`](ssbu-recomp/) is ssbu-recomp**, staged with its history. Its parts get ported into the base one area at a time (renderer, installer, menus, modding, rollback), and each moves out of the folder when it is ported.

It is early, it moves fast, and there is a lot that a few people cannot test alone.

Please read the three rules below before anything else. They are not style preferences; a contribution that breaks one of them cannot be merged, however good the code is.

## The three rules

### 1. Nothing derived from the game goes into the repository

No game code, no lifted or decompiled game functions, no assets, no parameter files, no shader binaries, no save data, no keys, no firmware. Everything of that kind is generated on each person's PC from their own dump and stays there. The `.gitignore` files list the generated paths. CI's no-game-data check fails on Switch file types, key files and any file over 10 MB, and ssbu-recomp's release script audits each package it makes (`audit_package` in `ssbu-recomp/scripts/make_release.py`).

In practice:

- Tools may *read* the game's files and may contain names (function names, constant names, file paths inside the archive) and addresses. They may not contain the content those names point to: no instruction words, no data tables, no captures.
- A fix for one game function is expressed as a rule in a tool (the lifter, a decompiler, a patch list generated from the player's dump), not as a pasted copy of the function.
- Screenshots and clips of the running port are fine in issues. Dumps, extracted files and generated sources are not: do not attach them, do not link to them.
- Do not ask for, offer or link to copies of the game, keys or firmware anywhere in this project.

### 2. Clean sources only

Work only from: your own legally obtained dump of the game, public documentation and headers, and open-source projects under their licences (credited in [NOTICE.md](NOTICE.md)). Do not use leaked or confidential material of any kind, in particular no proprietary console SDK, its headers, its documentation, or notes derived from them. If you have seen such material for the part you want to work on, please work on a different part.

If you bring technique or code from another open-source project, say which one in the pull request and keep to its licence.

### 3. Every claim is measured

The README and the guides say what was tested and what was not. Keep it that way:

- "Works" means you ran it and can say how: which GPU, which operating system, which mode, which test, how long.
- "Equal to the original" means a comparison said so (see *Testing* below), not that it looked right.
- "Faster" means you timed it before and after, on hardware you name.
- Say what you did not verify. A pull request that lists its own gaps is easier to merge than one that claims everything.
- If you find that something documented as working does not, open an issue. That is a contribution.

## Using AI

Both projects were built largely with AI coding agents: ssbu-recomp's README says so, and almost every WindowSmash commit names Claude as co-author. AI-assisted contributions are welcome on the same terms as any other: you understand what you submit, you ran it, and the three rules hold. Tell us in the pull request what was AI-written and how you checked it. An unverified AI answer about how the game or the console works is not evidence; a test is.

## Ways to help

You do not need to be a reverse engineer for most of these.

- **Testing on your hardware.** Other GPUs (AMD above all, and older integrated graphics), other controllers, other Windows versions, Linux distributions and Macs. Report the GPU and driver version, the options you used, and the log.
- **Controllers.** Reports with physical pads (which pad, how connected, what happened) are scarce.
- **Game modes.** Many modes beyond 1-v-1 against the CPU are untested. A report "mode X: reached this screen, then this happened, log attached" is useful.
- **Performance.** Frame-time logs from a first run and a second run on your machine, and anything that still hitches in a fight.
- **Porting from `ssbu-recomp/`.** Each area that moves into the base needs its own tests and a working build on every platform the base supports. Open an issue first for anything large: it saves you from building on a part that is being rewritten.
- **Documentation.** If a step in a guide did not work for you, fix the guide.

## Building

- **The base (repository root):** follow the [setup guide](docs/BUILDING.md), or the [Linux](docs/LINUX.md) and [macOS](docs/MACOS.md) guides. You need your own dump of game version 13.0.5.
- **ssbu-recomp (staged):** [`ssbu-recomp/BUILDING.md`](ssbu-recomp/BUILDING.md) and its Linux and macOS guides beside it. It takes version 13.0.4 or 13.0.5.

Both builds generate several gigabytes of files that git ignores.

## Testing

The base's source-only tests use made-up data, need no game files and run in CI on Windows, Linux and macOS. Run them before you open a pull request:

```
python -m pip install -r tests/requirements-dev.txt
python -m unittest discover -s tests -v
cd tools
python -m nex.selftest
```

Tests that start the game should be scripted, not played by hand. The base presses buttons on a timetable (`SSBU_INPUT`) or in sequences counted in frames (`SSBU_SEQ`), saves frames (`SSBU_CAPTURE`), runs fast-forward (`SSBU_SPEED`), cuts movies short (`SSBU_SKIP_MOVIES`), logs a hash of every fight frame (`SSBU_FIGHT_HASH`), makes the fight repeat (`SSBU_DETERMINISTIC`, `SSBU_FIXED_SEED`), picks the fighters and stage (`SSBU_DEBUG_MATCH`) and ends after a number of fight frames (`SSBU_DEBUG_END`): see [Test runs](docs/TESTING-RUNS.md). It has run scripts in `tools/dev/` such as `autobattle.sh` and `menurun.sh` (see the [tool reference](docs/TECHNICAL.md#7-tool-reference)). Frame-time measurements run at normal speed.

Two checks carry most of the weight:

- **Lifter and decompiler changes:** the differential tests against the Unicorn emulator (the base: [Verification and debugging](docs/TECHNICAL.md#verification-and-debugging); ssbu-recomp: `ssbu-recomp/tools/lifter/difftest.py`, and the offline and in-match differentials of its fighter SDK for decompiled scripts). A function counts as equal when it was compared, not when it compiles.
- **Anything that could change game behaviour:** run the same scripted fight before and after your change and compare. Where a deterministic mode with a per-frame state log exists (ssbu-recomp has one), the two logs must be identical; the first differing line tells you the frame and the fighter.

Renderer changes additionally need a run under the Vulkan validation layer (where the renderer is Vulkan) with no new messages, and, for anything that can cost time in a fight, frame-time numbers before and after, on the GPU you have.

Say in the pull request which of these you ran.

## Reporting a problem

Include:

- what you did, in steps, and what you expected;
- your operating system, GPU and driver version, and any settings you changed;
- the log of the run, and after a crash or a freeze the report the runtime wrote;
- whether it happens every time.

Before attaching a log, check that it contains nothing personal (user names in paths, player names, internet addresses). Never attach game files, keys, saves or generated game code. Security problems go to [SECURITY.md](SECURITY.md), not to a public issue.

## Code

- **Languages.** The runtime is mostly C, in clang's default GNU dialect (it uses GNU builtins and extensions). The macOS window and Metal renderer are Objective-C, the macOS fiber switch is assembly, and Linux exception handling (`runtime/linux/eh.cpp`) and ssbu-recomp's settings menu are C++. ssbu-recomp's Linux build needs clang 19 or newer, for C23's `typeof_unqual`. The tools are Python 3.10 or newer; ssbu-recomp's fighter SDK is C++. No new dependencies without discussion; the runtime builds with clang and a handful of official libraries.
- **Style.** Match the file you are editing: its naming, its comment density, its layout. Comments explain *why* and record what was measured, not what the next line does.
- **Switches.** Anything experimental or test-only goes behind an option or an environment variable that defaults to the old behaviour, with one line of documentation where the other switches are listed.
- **Game-specific knowledge** (an address, a structure layout, a table size) is written with how it was found: seen in the binary at an address, taken from a public source, or inferred. An inference is marked as one.
- **No silent fakes.** If a system call is answered with a simplification instead of the real behaviour, say so where it is implemented, so it shows up when someone looks. Do not make something "work" by returning success from a function that does nothing.
- **One change per pull request**, with a description of what it does, how it was tested, and what was not tested.

## Commits and licence

- Write commit messages that say what changed and why. Mention the test you ran.
- By contributing you agree that your contribution is licensed under the project's licence, the GNU General Public License version 3 or later (GPL-3.0-or-later, see [`LICENSE`](LICENSE)), and that you have the right to contribute it.
- Keep personal data out of commits: no local paths with user names, and no e-mail addresses in code, docs, tests or logs. The exceptions are `.mailmap`, which joins one person's commit identities, and the unchanged `AUTHORS` files of the libraries in `third_party/`.

## Conduct

Be direct and be kind. Disagree with the idea, not the person. People arrive here from modding, emulation, graphics, competitive play and plain curiosity, with very different backgrounds; explain rather than assume. Harassment, and anything that puts the project at legal risk (requests for game files, links to them), gets removed.

## Legal

This project is not affiliated with Nintendo, Bandai Namco or Sora Ltd. It contains none of their code or assets and does not help anyone obtain them. If you are unsure whether something you want to contribute is acceptable, ask in an issue before sending it.
