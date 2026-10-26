# Third-party notices

[← README](README.md)

## OpenSmash-Ultimate's own source

OpenSmash-Ultimate is licensed under the GNU General Public License, version 3 or (at your option) any later version: **GPL-3.0-or-later**. [`LICENSE`](LICENSE) holds the license text. This covers both projects it brings together:

- **WindowSmash**, at the repository root, was published under GPL-3.0 only until it was merged here. Its author, SNO2207, relicensed it as GPL-3.0-or-later for OpenSmash-Ultimate.
- **ssbu-recomp**, in [`ssbu-recomp/`](ssbu-recomp/), was already GPL-3.0-or-later. [`ssbu-recomp/LICENSE`](ssbu-recomp/LICENSE) is the same text as [`LICENSE`](LICENSE).

The separately identified third-party material below keeps its own license. The GPL covers this project's own source only: it gives no rights to Super Smash Bros. Ultimate, its files, or anything built from them.

## WindowSmash (repository root)

### libvpx

The VP9 decoder of the WebM Project's libvpx, release v1.15.2, is included as source in [`third_party/libvpx/`](third_party/libvpx/) and compiled into the game when it is built (the game's movies are VP9). [`third_party/README.md`](third_party/README.md) says exactly which files and how they were taken from the upstream release; none is modified. The upstream license and additional patent grant are kept there:

- [BSD license and copyright notice](third_party/libvpx/LICENSE)
- [Additional IP rights grant](third_party/libvpx/PATENTS)

[`runtime/gpu_vp9_tables.h`](runtime/gpu_vp9_tables.h) also contains default VP9 probability tables derived from libvpx, as its header states.

### libopus

libopus 1.5.2 (Xiph.Org Foundation and contributors) is included as source in [`third_party/opus/`](third_party/opus/) and compiled into the game when it is built (the game's music is Opus), under its [BSD license](third_party/opus/COPYING). [`third_party/README.md`](third_party/README.md) says which files. One line is changed: in `silk/x86/NSQ_del_dec_avx2.c`, a test for Microsoft's compiler now leaves clang out, so older clang-cl versions can build the file ([Local changes](third_party/README.md#local-changes)).

### Open fonts

[`third_party/fonts_open/`](third_party/fonts_open/) holds seven TrueType fonts that both runtimes use in place of a firmware font that isn't there ([`runtime/hle_pl.c`](runtime/hle_pl.c), font by font; [`ssbu-recomp/runtime/nnpl.c`](ssbu-recomp/runtime/nnpl.c)). They are open-source fonts the yuzu project put together for the same purpose, taken unmodified (only renamed) from the suyu repository at commit `c0dd2ff554327657f0c6318f29b740ac7e329ea0`. [Their README](third_party/fonts_open/README.md) lists each file with the SHA-256 of its source, and the license texts are beside them. They are separate files the game reads when it runs, not compiled into it; the Linux package (`tools/linux/package_linux.py`) carries the folder as it is, with its README and license texts. The files stay as they are: WindowSmash's runtime adds U+25A1 to the character map of the copy it loads, in memory.

| Font | Copyright | License |
| :--- | :--- | :--- |
| Open Sans 1.10 (Regular, Light) | 2010-2011 Google Corporation | [Apache-2.0](third_party/fonts_open/LICENSE-Apache-2.0.txt) |
| Source Sans Pro 2.021 | 2010, 2012, 2014 Adobe Systems Incorporated, with Reserved Font Name "Source" | [OFL-1.1](third_party/fonts_open/LICENSE-OFL-1.1.txt) |
| YuzuOSSExtension 1.000 | 2019 yuzu Emulator Project | [GPL-2.0-or-later](third_party/fonts_open/LICENSE-GPL-2.0-or-later.txt) |

Open Sans is a trademark of Google. Source is a trademark of Adobe Systems Incorporated.

### SDL3

Controller input can use [SDL3](https://www.libsdl.org/) (Simple DirectMedia Layer), copyright Sam Lantinga, under the [zlib license](https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt).

SDL3 is an external dependency: this repository contains no SDL source files or binaries. The runtime is compiled against the headers of the official SDL3 Windows development package (`deps/SDL3`, which `Install WindowSmash.bat` downloads from SDL's GitHub releases, or `SSBU_SDL3_DIR`) and loads `SDL3.dll` from the executable's folder at run time. A package that includes `SDL3.dll` should include that package's `LICENSE.txt` too.

[`runtime/hle_padusb.c`](runtime/hle_padusb.c) reads Nintendo's GameCube adapter and the Switch 2 GameCube controller over USB without SDL. It is new code, written for Windows' own USB and HID libraries, but it follows SDL's HIDAPI GameCube and Switch 2 drivers, as its header says: from the Switch 2 driver it takes the controller's start-up commands, flash addresses and stick-calibration layout. SDL's drivers are under the zlib license above.

### Python

`Install WindowSmash.bat` may put a copy of [Python](https://www.python.org/) (the embeddable package from python.org, under the [PSF license](https://docs.python.org/3/license.html)) into `deps/python`, with [pip](https://pip.pypa.io/) and the packages below, when the PC has no Python it can use. Nothing from it is distributed with OpenSmash-Ultimate.

### The compiler: LLVM, and Microsoft's C runtime and Windows SDK

`tools/msvc_deps.py` (which `Install WindowSmash.bat` and `launchers\Build WindowSmash.bat` run when the PC has no compiler) downloads two external dependencies onto the PC that builds the game. Neither is part of this repository or distributed with OpenSmash-Ultimate.

- **LLVM** (`clang-cl`, `lld-link`, `llvm-lib` and clang's headers) into `deps/llvm`, from LLVM's official release on GitHub, under the [Apache License 2.0 with LLVM exceptions](https://llvm.org/LICENSE.txt).
- **Microsoft's C runtime and Windows SDK** (headers and 64-bit libraries) into `deps/msvc`, from Microsoft's servers, under Microsoft's license terms for the Visual Studio Build Tools, which the tool shows and asks you to accept first. They are Microsoft's software, used to build the game on that PC; they are not for passing on. The game built with them links Microsoft's static C runtime, as it does when built with Visual Studio.

### Other external dependencies

These are external dependencies, not bundled implementations. Their own licenses apply when you obtain, link or distribute them.

| Dependency | Used for | How it's obtained |
| :--- | :--- | :--- |
| SDL3 | Controllers other than Xbox pads | Optional, the official Windows development package. |
| PyCryptodome (`pycryptodomex`) | Reading game packages | `requirements.txt`. |
| zstandard | Reading `data.arc` on Pythons older than 3.14 | `requirements.txt` (Python 3.14 has `compression.zstd` built in). |
| Capstone, Unicorn | Disassembly and differential tests | `tests/requirements-dev.txt`, optional. |

A developer can link their own libopus or libvpx builds instead of the included sources (`SSBU_OPUS_LIBS`, `SSBU_VPX_DIR`: [`tools/cc.py`](tools/cc.py)). No binaries of any library are checked into this repository.

## ssbu-recomp (`ssbu-recomp/`)

### Code and data derived from yuzu and suyu

- [`ssbu-recomp/runtime/mx_ops.h`](ssbu-recomp/runtime/mx_ops.h) is generated from suyu's `src/shader_recompiler/frontend/maxwell/maxwell.inc` (the GPU's instruction bit patterns), copyright 2021 yuzu Emulator Project, under GPL-2.0-or-later.
- **Mii data** is not in the repository. At build time, [`ssbu-recomp/tools/lifter/gen_sdktest.py`](ssbu-recomp/tools/lifter/gen_sdktest.py) reads the default-Mii and colour tables from two files of a yuzu-family source tree into the SDK self-test's reference data: `src/core/hle/service/mii/types/raw_data.cpp` (Ryujinx Team and Contributors, MIT) and `src/core/hle/service/mii/mii_types.h` (yuzu Emulator Project, GPL-2.0-or-later). ssbu-recomp's installer downloads them from suyu at the commit above and checks their SHA-256 ([`ssbu-recomp/scripts/install.py`](ssbu-recomp/scripts/install.py)); CI takes them from a sparse checkout of eden.

### Libraries the installer or CI downloads

None of these is in the repository. Their own licenses apply when you obtain, link or distribute them.

| Dependency | License | Used for | How it's obtained |
| :--- | :--- | :--- | :--- |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | The settings menu | v1.91.9b source archive, pinned by SHA-256 in `ssbu-recomp/scripts/install.py`; cloned at that tag in CI. |
| [SDL3](https://www.libsdl.org/) | zlib | Controllers; the window on Linux and macOS | Windows: SDL3-devel-3.4.18-VC.zip from SDL's releases, pinned by SHA-256. macOS: Homebrew. Linux CI: built from the release-3.4.14 tag. |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others, as its `LICENSE.txt` lists | Compiling shaders to SPIR-V | Windows: the glslang 16.6.0 release, pinned by SHA-256. Linux and macOS: system packages. |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | Apache-2.0 | Linked with a static glslang | With glslang. |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Apache-2.0 (some files Apache-2.0 or MIT, as its `LICENSE.md` says) | Compiling the Vulkan renderer | Windows: the v1.4.329 source archive, pinned by SHA-256 in `ssbu-recomp/scripts/install.py`. CI on Linux and macOS: cloned at the v1.4.351 tag. |
| [libopus](https://opus-codec.org/) | BSD-3-Clause | The game's music: the runtime loads `libopus-0.dll` | Windows: MSYS2's ucrt64 package of Opus 1.6.1, fetched by `ssbu-recomp/scripts/fetch_media.py` with the package and the DLL pinned by SHA-256; the package's `COPYING` is kept beside the DLL as `LICENSE-opus.txt`. Linux and macOS: system packages. |

### FFmpeg

`ssbu-recomp/scripts/fetch_media.py` downloads `ssbu-ffmpeg-8.1.3-lgpl-win64.zip` from ssbu-recomp's `ffmpeg-8.1.3-lgpl` release: the five FFmpeg 8.1.3 DLLs that `ssbu-recomp/runtime/mf_ffmpeg.c` loads when Windows' Media Foundation can't play the game's movies, with FFmpeg's headers and `COPYING.LGPLv2.1`. FFmpeg is licensed under the GNU Lesser General Public License, version 2.1 or later (LGPL-2.1-or-later); this build has no GPL or non-free parts. It was built from FFmpeg's release source with [`ssbu-recomp/scripts/ffmpeg/build_ffmpeg_win.sh`](ssbu-recomp/scripts/ffmpeg/build_ffmpeg_win.sh). It includes an H.264 decoder, and H.264 is covered by patents in some countries.

### LLVM 9

`ssbu-recomp/scripts/install.py` downloads `llvm-9.0.0-win64-portable.zip` from ssbu-recomp's `llvm-9.0.0-portable` release, pinned by SHA-256: the files of LLVM's official, signed `LLVM-9.0.0-win64.exe`, unchanged, repacked as a zip. LLVM 9 is licensed under the Apache License 2.0 with LLVM Exceptions, with the legacy University of Illinois/NCSA license for its older code.

## Game content and names

The Git source tree contains no Nintendo game binaries, translated game functions, assets, firmware fonts, keys, firmware or personal saves. Each player builds the game on their own PC from their own copy, and game-data archives, firmware fonts, keys and saves stay with the player. Material from the game was stripped from every commit of ssbu-recomp's history when it was imported (three files, and the game's values in an example in its README); [the README](README.md#ssbu-recomp-staged-code) names them.

Super Smash Bros. Ultimate and associated names are the property of their respective owners. OpenSmash-Ultimate is an independent project and is not affiliated with Nintendo.
