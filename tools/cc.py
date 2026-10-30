"""Host toolchain wrapper: compiles generated C with clang-cl."""
import glob
import json
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

import codec_libs   # (this folder, which every importer of cc puts on sys.path)
import isa
import ladder
import progress
from ladder import CRASH_SIGNS, RETRY, crash_cause, crash_report, crashed  # noqa: F401 (the crash retry's, here before)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME = os.path.join(ROOT, "runtime")
DEPS_LLVM = os.path.join(ROOT, "deps", "llvm")   # tools/msvc_deps.py sets these two up
DEPS_MSVC = os.path.join(ROOT, "deps", "msvc")


def _deps_record(folder):
    """A deps folder's windowsmash.json (msvc_deps.py writes it last: no record, not finished)."""
    try:
        with open(os.path.join(folder, "windowsmash.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _find_toolchain():
    """(clang-cl, the /winsysroot folder or None, which toolchain that is, in words):
    SSBU_CLANG if set; else deps\\llvm's with deps\\msvc as the system root, when tools/msvc_deps.py
    has set both up (no Visual Studio needed); else Visual Studio's own (the "C++ Clang tools"
    component), newest edition first, else a standalone LLVM install, else whatever is on PATH.
    The last three find the C runtime and the Windows SDK themselves, through Visual Studio."""
    if os.environ.get("SSBU_CLANG"):
        return os.environ["SSBU_CLANG"], None, "SSBU_CLANG"
    llvm, msvc = _deps_record(DEPS_LLVM), _deps_record(DEPS_MSVC)
    clang = os.path.join(DEPS_LLVM, "bin", "clang-cl.exe")
    if llvm is not None and msvc is not None and os.path.isfile(clang):
        return clang, DEPS_MSVC, (f"deps\\llvm (LLVM {llvm.get('version')}) with deps\\msvc (MSVC {msvc.get('msvc')}, "
                                  f"Windows SDK {msvc.get('sdk')}): no Visual Studio")
    def vs_major(path):   # the folder is a year up to 2022 (2022 = 17), a major version since (18)
        name = path.split(os.sep + "Microsoft Visual Studio" + os.sep)[1].split(os.sep)[0]
        return {"2017": 15, "2019": 16, "2022": 17}.get(name, int(name) if name.isdigit() else 0)
    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        found = sorted(glob.glob(os.path.join(root, "Microsoft Visual Studio", "*", "*", "VC", "Tools", "Llvm", "x64", "bin", "clang-cl.exe")),
                       key=vs_major, reverse=True)
        if found:
            return found[0], None, "Visual Studio's C++ Clang tools"
    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"),):
        for clang in glob.glob(os.path.join(root, "LLVM", "bin", "clang-cl.exe")):
            return clang, None, "LLVM's installer (Program Files), with Visual Studio's C runtime and Windows SDK"
    return "clang-cl", None, "clang-cl on the PATH"


CLANG, WINSYSROOT, TOOLCHAIN = _find_toolchain()
# The instruction set (tools/isa.py): avx2, or x86-64-v2 for older processors; why, in words; and
# whether SSBU_ISA chose it (build/isa.txt keeps that for the next build).
ISA, ISA_TOLD, ISA_WHY = isa.target(os.path.join(ROOT, "build"))
# Guest FP semantics need IEEE-exact arithmetic: no contraction, no fast-math. (BASE_CFLAGS is
# also what the codec libraries are compiled with: same instruction set, same C runtime.) With
# deps\msvc, /winsysroot is where the C runtime's and the Windows SDK's headers and libraries are.
def base_cflags(target):
    """BASE_CFLAGS for an instruction set (tools/isa.py)."""
    return ["/nologo", *isa.CLANG_CL[target], "-fno-strict-aliasing", "/clang:-ffp-contract=off", "-w",
            *(["/winsysroot", WINSYSROOT] if WINSYSROOT else [])]


BASE_CFLAGS = base_cflags(ISA)
CFLAGS = [*BASE_CFLAGS, "/I", RUNTIME]
# runtime/cpu_check.c's: for any x64 processor, as it checks the processor before anything else runs
CHECK_CFLAGS = [*base_cflags(isa.ANY), "/I", RUNTIME]
# The generated C's flags: it includes recomp.h or module.h, which read none of the runtime's defines and
# include folders added to CFLAGS below (SDL3, libopus, libvpx). So their coming and going compiles the
# runtime again (minutes), not the game (hours: tools/objcache.py keys each object on its command).
GAME_CFLAGS = [*BASE_CFLAGS, "/I", RUNTIME]
# Linking through clang-cl: with lld-link when it is next to clang-cl (else Visual Studio's link.exe),
# and LINK_OPTIONS after /link. With deps\msvc both are given the system root. clang-cl, given
# /winsysroot, hands lld-link the root's library folders (-libpath:). lld-link, given /winsysroot:,
# does not add the folders of a Visual Studio it finds on its own. Without the root, clang-cl hands
# lld-link no library folders on a PC without Visual Studio. lld-link would then add the root's
# folders only once an object file has told it the machine type, and clang-cl puts the libraries
# named after /link before the objects. So none of SYSTEM_LIBS was found (players' builds stopped
# at the link; on a PC with Visual Studio, clang-cl hands lld-link VS's folders instead, which
# hides this).
def link_flags(root):
    """(LINK_FLAGS, LINK_OPTIONS) for a system root, or for None."""
    lld = ["-fuse-ld=lld"] if os.path.exists(os.path.join(os.path.dirname(CLANG), "lld-link.exe")) else []
    return (lld + (["/winsysroot", root] if root else []), ["/winsysroot:" + root] if root else [])


LINK_FLAGS, LINK_OPTIONS = link_flags(WINSYSROOT)
# the Windows libraries the game links (all from the Windows SDK)
SYSTEM_LIBS = ("winmm.lib", "user32.lib", "gdi32.lib", "d3d11.lib", "dxgi.lib", "d3dcompiler.lib", "dxguid.lib",
               "ws2_32.lib", "ole32.lib", "oleaut32.lib")

# The audio and video codecs: libopus for the hwopus service (music) and libvpx for the video
# decode engine's VP9 (movies). Normally built from the sources under third_party/ by
# tools/codec_libs.py when link() runs. A developer can supply libraries instead: SSBU_OPUS_LIBS
# is a semicolon-separated list of Windows x64 .lib files, SSBU_VPX_DIR a folder with include/
# and lib/Win64/Release/libvpx.lib.
OPUS_LIBS = [p for p in os.environ.get("SSBU_OPUS_LIBS", "").split(";") if p]
if not all(os.path.exists(p) for p in OPUS_LIBS):
    OPUS_LIBS = []
VPX_DIR = os.environ.get("SSBU_VPX_DIR", "")
VPX_INCLUDE = os.path.join(VPX_DIR, "include")
VPX_LIBS = [os.path.join(VPX_DIR, r"lib\Win64\Release\libvpx.lib")] if VPX_DIR else []
if not all(os.path.exists(p) for p in VPX_LIBS) or not os.path.exists(os.path.join(VPX_INCLUDE, "vpx", "vpx_decoder.h")):
    VPX_LIBS = []
# (supplied libraries were built against the DLL C runtime; this program uses the static one)
EXTERNAL_LIBS = OPUS_LIBS + VPX_LIBS
BUILT_CODECS = []   # the libraries codec_libs builds for this link, from third_party/
if not OPUS_LIBS and codec_libs.have_opus():
    OPUS_LIBS, BUILT_CODECS = [codec_libs.OPUS_LIB], BUILT_CODECS + ["opus"]
if not VPX_LIBS and codec_libs.have_vpx():
    VPX_LIBS, VPX_INCLUDE, BUILT_CODECS = [codec_libs.VPX_LIB], codec_libs.VPX, BUILT_CODECS + ["libvpx"]
if OPUS_LIBS:
    CFLAGS.append("/DSSBU_HAVE_OPUS")
if VPX_LIBS:
    CFLAGS += ["/DSSBU_HAVE_VPX", "/I", VPX_INCLUDE]
# SDL3 for controllers (zlib licence): the runtime loads SDL3.dll at run time, so the build needs only the headers,
# and the link step copies the DLL beside the exe. SSBU_SDL3_DIR overrides where (include/, lib/x64/SDL3.dll).
SDL3_DIR = os.environ.get("SSBU_SDL3_DIR", os.path.join(ROOT, "deps", "SDL3"))
SDL3_DLL = os.path.join(SDL3_DIR, r"lib\x64\SDL3.dll")
if os.path.exists(os.path.join(SDL3_DIR, "include", "SDL3", "SDL_gamepad.h")) and os.path.exists(SDL3_DLL):
    CFLAGS += ["/DSSBU_HAVE_SDL3", "/I", os.path.join(SDL3_DIR, "include")]
else:
    SDL3_DLL = None
EXTRA_LIBS = OPUS_LIBS + VPX_LIBS + (["/NODEFAULTLIB:msvcrt.lib"] if EXTERNAL_LIBS else [])

# Compiler crashes, and compiles that run too long (tools/ladder.py). A player's build stopped on two files
# that clang-cl crashed on (access violations, one in the parser, one in the optimiser) where the very same
# clang-cl compiles them here: a PC short of memory, or one not quite stable, does that, and it rarely
# happens twice to the same file. So compile_each compiles a file the compiler crashed on again, on its own
# once the others are done, at its own level, then /O1 (RETRY: fewer optimiser passes; the giant functions
# are built at /O1 anyway). A crash is never retried at /Od: that, after the longest functions unoptimised,
# is for a generated file that runs over its time limit. Compiled without a bug report's files: after a
# crash clang would preprocess the file again, into %TEMP%.
NO_CRASH_FILES = "-fno-crash-diagnostics"


def compile_one(src, obj, opt="/O2"):
    r = subprocess.run([CLANG, "/c", opt, *CFLAGS, src, "/Fo" + obj], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"compile failed: {src}\n{(r.stdout + r.stderr)[:4000]}")
    return obj


def compile_many(sources, jobs=16, opt="/O2"):
    """Compile each source next to itself; returns the object paths."""
    with ThreadPoolExecutor(jobs) as pool:
        return list(pool.map(lambda s: compile_one(s, os.path.splitext(s)[0] + ".obj", opt), sources))


def compile_command(src, obj, opt, flags=None, header=None):
    """The command compile_each compiles a file with (and what objcache.py keys its object on);
    flags: CFLAGS unless given (GAME_CFLAGS for the generated C); header: the ladder's optnone header,
    given before the file (/FI), when it has one."""
    return [CLANG, "/c", opt, *(CFLAGS if flags is None else flags), NO_CRASH_FILES, *(["/FI" + header] if header else []),
            src, "/Fo" + obj]


def compile_each(jobs_list, jobs=16, started=None, finished=None, log=print, compiled=None, flags=None, shown=None,
                 infos=None, steps=None, times=None):
    """Compile (src, obj, opt) triples in parallel, in order, with flags (CFLAGS unless given); returns
    [(src, error text)] for failures. started(job) and finished(job, seconds), if given, are called
    (from the worker threads) as each one starts and ends (shown: a progress.Build, which also hears
    of a file compiled again), and compiled(job, level[, step]) as each one compiles, with the level
    it compiled at. A file the compiler crashed on is compiled again once the others are done, on its
    own (RETRY); log() says so. infos {obj: ladder.Info}: the generated files, which have a time limit
    and the rest of the ladder (steps: the build's ladder.Ladder, else one of its own); times: the
    build folder's ladder.Times."""
    steps = steps or ladder.Ladder(progress.level_cost, "clang-cl", log)
    return steps.compile(jobs_list, jobs, lambda job, level, header: compile_command(job[0], job[1], level, flags, header),
                         infos, shown, started, finished, compiled, times)


def link_command(rsp, exe, options=(), libs=()):
    """clang-cl's command that links the objects listed in the response file `rsp` into `exe`, with
    SYSTEM_LIBS and `libs`; `options` go to the linker. (build_game.py's compiler check links its
    test program with it too, so that the check tries what the build does.)"""
    return [CLANG, "/nologo", *LINK_FLAGS, "@" + rsp, "/Fe" + exe, "/link", *LINK_OPTIONS, *options, *SYSTEM_LIBS, *libs]


# The program's manifest: file names are UTF-8 (the "A" file calls - fopen, FindFirstFileA - take
# mods.txt's and the mods' names as they are: an accented or Japanese mod name opens) and paths may
# pass 260 characters (when Windows' LongPathsEnabled is on). Windows 10 1903 and newer.
MANIFEST = b"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>
</assembly>
"""


def manifest_res(path):
    """MANIFEST as a compiled resource file (.res: RT_MANIFEST 24, id 1), which the linker takes as an
    input like an object - no rc.exe or mt.exe needed."""
    import struct
    def entry(type_id, name_id, data, flags, lang):
        head = struct.pack("<IIHHHHIHHII", len(data), 32, 0xFFFF, type_id, 0xFFFF, name_id, 0, flags, lang, 0, 0)
        return head + data + b"\0" * (-len(data) % 4)
    with open(path, "wb") as f:
        f.write(entry(0, 0, b"", 0, 0) + entry(24, 1, MANIFEST, 0x1030, 0x0409))
    return path


def link(objs, exe, options=()):
    """Link objects into an executable; `options` go to the linker. The codec libraries built
    from third_party/ are made first if they are missing or stale (a few seconds)."""
    if BUILT_CODECS:
        codec_libs.build(BUILT_CODECS, log=lambda m: print(m, flush=True), target=ISA)
    rsp = exe + ".rsp"
    res = manifest_res(exe[:-4] + ".res")
    for inputs in ([*objs, res], objs):   # (a linker that takes no .res still links: without the manifest)
        with open(rsp, "w") as f:
            f.write("\n".join(f'"{o}"' for o in inputs))
        r = subprocess.run(link_command(rsp, exe, ["/HIGHENTROPYVA:NO", "/MANIFEST:NO", "/OPT:REF", "/OPT:NOICF",
                                                   "/MAP:" + exe[:-4] + ".map", *options], EXTRA_LIBS),
                           capture_output=True, text=True)
        if not r.returncode:
            break
        if inputs is objs or ".res" not in r.stdout + r.stderr:
            break
        print("the link didn't take the manifest (" + exe[:-4] + ".res): linking without it", flush=True)
    if r.returncode:
        lines = (r.stdout + r.stderr).splitlines()
        errors = [l for l in lines if "error" in l.lower()] or lines   # (the third-party libraries' CRT warnings drown them)
        raise RuntimeError("link failed\n" + "\n".join(errors)[:4000])
    return exe


def library_command(rsp, dll):
    """clang-cl's command that links the objects listed in `rsp` into the DLL `dll`: the game's modules
    (--game-dll: tools/game_dll.py), as link() links the program but with none of its libraries, which only
    the runtime calls (the game's C reaches the runtime through the tables the library hands it). Placed as
    the program is (/HIGHENTROPYVA:NO); its one export is the function that hands over those tables."""
    return [CLANG, "/nologo", "/LD", *LINK_FLAGS, "@" + rsp, "/Fe" + dll, "/link", *LINK_OPTIONS, "/HIGHENTROPYVA:NO",
            "/MAP:" + dll[:-4] + ".map"]


def link_library(objs, dll):
    """Link objects into a DLL (library_command)."""
    rsp = dll + ".rsp"
    with open(rsp, "w") as f:
        f.write("\n".join(f'"{o}"' for o in objs))
    r = subprocess.run(library_command(rsp, dll), capture_output=True, text=True)
    if r.returncode:
        lines = (r.stdout + r.stderr).splitlines()
        errors = [l for l in lines if "error" in l.lower()] or lines
        raise RuntimeError("the game library's link failed\n" + "\n".join(errors)[:4000])
    return dll
