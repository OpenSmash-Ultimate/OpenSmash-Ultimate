#!/usr/bin/env python3
"""The whole runtime (runtime/ and this system's platform layer) compiled and linked into the game's
program, as a player's build does it here, with no game files: the game is tools/mac/boottest's
made-up one (main, subsdk0 and sdk, hand-written in the form tools/recomp.py writes a module's C),
laid out as a game tree. What .github/workflows/runtime-compile.yml runs on Windows, Linux and macOS.
  Windows  tools/recomp.py compile for each module, then tools/recomp.py link: <tree>/build/ssbu.exe
  Linux    tools/linux/build_vkd3d.py linux when vkd3d-shader is not built for this instruction set
           (as build_game.py), then tools/linux/build_linux.py: <tree>/build-linux/ssbu
  macOS    tools/mac/build_mac.py: <tree>/build-mac/WindowSmash.app
The program is not started (tools/mac/boottest/boottest.py starts it on a Mac), except with
--game-dll, to load its library (below). SSBU_ISA chooses the instruction set on Windows and
Linux, as for a player's build (tools/isa.py).

Then every runtime source must be in the program, by its link's list of objects, unless LEFT_OUT
(below) says why it is not, and no source may be where no build looks for one (only the top of
runtime/, runtime/linux and runtime/macos): a file left out of a build's list, or moved into a
subfolder, fails the check rather than dropping out of the program unseen.

With --game-dll the build is build_game.py --game-dll's: the program is the runtime alone and the
game's modules are a library of their own, which it loads (tools/game_dll.py). Then the program
must have only the runtime's objects, the library each module's and none of a runtime source's,
and the program, started as  ssbu --game-library, must load the library and accept it: every
runtime function the game's C calls found, the interface's version, structures and instruction
set the same.

Usage: runtime_compile_check.py [--tree <dir>] [--jobs N] [--game-dll]
  --tree      the made-up game's folder (default: windowsmash-runtime-check in the system's
              temporary folder; not this checkout, whose build/ is the real game's). Kept: a
              second run compiles only what changed
  --jobs      files compiled at a time (default: each build's own)
  --game-dll  the game in a library of its own, the program the runtime alone (above)

The tools are a player's: on Windows clang-cl (tools/cc.py finds it) and SDL3 in deps\\SDL3 if
there; on Linux install-windowsmash.sh's packages and deps/linux-toolchain; on macOS the Command
Line Tools and SDL3.framework in deps/SDL3 (SSBU_SDL3_DIR names another folder).
Exit status: 0 the program linked with every source (and with --game-dll loaded the library), 1 a
step failed (its output says why), a source is missing or misplaced, or the library is wrong, 2 a
wrong argument."""
import importlib.util
import os
import subprocess
import sys
import tempfile
import time

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import game_dll  # noqa: E402
RUNTIME = os.path.join(REPO, "runtime")
BOOTTEST = os.path.join(TOOLS, "mac", "boottest", "boottest.py")
MODULES = ("main", "subsdk0", "sdk")
TREE = os.path.join(tempfile.gettempdir(), "windowsmash-runtime-check")
SOURCES = (".c", ".m", ".mm", ".cpp", ".S")
# The platform layers, each a folder of runtime/: Linux's program is built with runtime/linux, macOS's
# with runtime/macos and parts of runtime/linux; Windows' layer is in runtime/ itself. The builds take
# sources from these folders and the top of runtime/, none from a subfolder of them.
FOLDERS = ("linux", "macos")
PLATFORMS = {"windows": (), "linux": ("linux",), "macos": ("linux", "macos")}

# The sources (paths in runtime/) each system's program is built without on purpose, and why. Any other
# source at the top of runtime/ or in that system's PLATFORMS that its program has no object of fails
# the check, and so does one listed here that it has, or that is gone.
TESTS = {"difftest.c": "the lifter's differential test: tools/dev/difftest.py builds it with the instructions' C",
         "functest.c": "the whole-function test: tools/dev/functest.py builds it with functions lifted from the game",
         "smoke.c": "a first end-to-end test: tools/dev/smoke.ps1 builds it with a function from the game"}
LEFT_OUT = {
    "windows": TESTS,
    "linux": {**TESTS,
              "debug_profile.c": "Windows' profiler: linux/profile.c is Linux's"},
    "macos": {**TESTS,
              "cpu_check.c": "the x86-64 processor check (CPUID): the Mac program is arm64",
              "debug_profile.c": "Windows' profiler: macos/profile.c is macOS's",
              "gpu_d3d11.c": "the Direct3D 11 renderer: macos/gpu_metal.m is macOS's",
              "gpu_overlay.c": "the overlay, drawn with Direct3D 11: macos/gpu_metal_overlay.m is macOS's",
              "gpu_menu.c": "the settings menu, drawn with that overlay: not on macOS yet (macos/platform.c)",
              "linux/d3dcompile_vkd3d.c": "D3DCompile through vkd3d-shader, for DXVK: the Metal renderer takes no DXBC",
              "linux/profile.c": "Linux's profiler: macos/profile.c is macOS's",
              "linux/window.c": "Linux's SDL2 window: macos/window.m is macOS's"},
}


def arg(argv, name, default=None):
    return argv[argv.index(name) + 1] if name in argv and argv.index(name) + 1 < len(argv) else default


def this_host():
    return "windows" if os.name == "nt" else "macos" if sys.platform == "darwin" else "linux"


def make_tree(tree):
    """The made-up game's tree: boottest.py's (build/<module>/ with the C and manifest.tsv, dump/modules/
    with the images). An unchanged file keeps its date."""
    spec = importlib.util.spec_from_file_location("boottest", BOOTTEST)
    boottest = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(boottest)
    boottest.make_tree(tree)


def program(tree, host):
    """Where the build puts the program: where a player's build puts it in its tree."""
    return {"windows": os.path.join(tree, "build", "ssbu.exe"),
            "linux": os.path.join(tree, "build-linux", "ssbu"),
            "macos": os.path.join(tree, "build-mac", "WindowSmash.app", "Contents", "MacOS", "ssbu")}[host]


def runtime_folder(tree, host):
    """The folder the build compiles the runtime's objects into."""
    return {"windows": os.path.join(tree, "build", "rt"), "linux": os.path.join(tree, "build-linux", "rt"),
            "macos": os.path.join(tree, "build-mac", "obj", "rt")}[host]


def library(tree, host):
    """Where a --game-dll build puts the game's library: beside the program (macOS: in the app's Frameworks)."""
    return {"windows": os.path.join(tree, "build", game_dll.DLL), "linux": os.path.join(tree, "build-linux", game_dll.SO),
            "macos": os.path.join(tree, "build-mac", "WindowSmash.app", "Contents", "Frameworks", game_dll.DYLIB)}[host]


def module_folder(tree, host, module):
    """The folder the build compiles a module's objects into."""
    return {"windows": os.path.join(tree, "build", module), "linux": os.path.join(tree, "build-linux", module),
            "macos": os.path.join(tree, "build-mac", "obj", module)}[host]


def link_list(tree, host, of_library=False):
    """The objects the program was linked from, as its link listed them for the linker (Windows' and
    Linux's response file, macOS's file list), or None without one. of_library: the game library's
    (--game-dll), as its own link listed them."""
    if of_library:
        path = os.path.join(tree, "build-mac", "ssbu_game.filelist") if host == "macos" else library(tree, host) + ".rsp"
    else:
        path = {"windows": program(tree, host) + ".rsp", "linux": os.path.join(tree, "build-linux", "ssbu.rsp"),
                "macos": os.path.join(tree, "build-mac", "ssbu.filelist")}[host]
    try:
        with open(path, errors="replace") as f:   # (in the encoding the build wrote it in: this Python's own)
            return [line.strip().strip('"') for line in f.read().splitlines() if line.strip()]
    except OSError:
        return None


def inside(path, folder):
    """path relative to folder, when it is in it (else None)."""
    try:
        rel = os.path.relpath(os.path.abspath(path), folder)
    except ValueError:   # (another drive)
        return None
    return rel if rel not in (os.curdir, os.pardir) and not rel.startswith(os.pardir + os.sep) else None


def linked(objects, rt):
    """{(folder in rt, name without its extension)} of the objects in rt (the runtime's) among objects."""
    found = set()
    for obj in objects:
        rel = inside(obj, rt)
        if rel:
            folder, name = os.path.split(rel)
            found.add((folder.replace(os.sep, "/"), os.path.splitext(name)[0]))
    return found


def runtime_sources(runtime):
    """Every source file under runtime/ (SOURCES), as its path in runtime/ with /, in order."""
    found = []
    for folder, dirs, files in os.walk(runtime):
        dirs.sort()
        rel = os.path.relpath(folder, runtime).replace(os.sep, "/")
        found += [n if rel == "." else f"{rel}/{n}" for n in sorted(files) if n.endswith(SOURCES) and not n.startswith("._")]
    return found


def misplaced(sources):
    """The sources in a folder no build takes sources from: neither the top of runtime/ nor FOLDERS."""
    return [rel for rel in sources if rel.rpartition("/")[0] not in ("",) + FOLDERS]


MISPLACED = ("the builds take the runtime's sources only from the top of runtime/, runtime/linux and runtime/macos: "
             "no program has it")


def runtime_keys(rel, host):
    """The (folder in the runtime's object folder, name) an object of runtime source rel may have (linked's)."""
    folder, _, name = rel.rpartition("/")
    stem, ext = os.path.splitext(name)
    sub = folder if host == "macos" else ""   # (macOS's build keeps each folder's objects apart; fiber.c, fiber.S)
    return {(sub, stem), (sub, f"{stem}_{ext[1:]}")}


def source_problems(runtime, host, have):
    """What is wrong with the program's runtime sources, in words ([]: nothing). have: linked's, of its
    link list. A source misplaced; one at the top of runtime/ or in this system's PLATFORMS that the
    program has no object of, unless LEFT_OUT lists it; one LEFT_OUT lists that it has, or that is gone."""
    sources, left_out = runtime_sources(runtime), LEFT_OUT[host]
    problems = [f"runtime/{rel}: {MISPLACED}" for rel in misplaced(sources)]
    for rel in sources:
        folder = rel.rpartition("/")[0]
        if folder and folder not in PLATFORMS[host]:
            continue   # (another system's platform layer, or misplaced)
        has = bool(runtime_keys(rel, host) & have)
        if rel in left_out and has:
            problems.append(f"runtime/{rel}: in the {host} program, though LEFT_OUT says it is left out ({left_out[rel]}): "
                            "take it off LEFT_OUT")
        elif rel not in left_out and not has:
            problems.append(f"runtime/{rel}: not in the {host} program: add it to the build's runtime sources, or to LEFT_OUT "
                            "in tools/dev/runtime_compile_check.py with why")
    problems += [f"runtime/{rel}: in LEFT_OUT for {host}, but there is no such file: take it off LEFT_OUT"
                 for rel in sorted(set(left_out) - set(sources))]
    return problems


def library_problems(runtime, tree, host, program_objects, library_objects):
    """--game-dll: what is wrong with how the program and the game's library were linked, in words ([]: nothing).
    The program has only the runtime's objects (in its object folder), the library each module's and no
    runtime source's (the tables tools/game_dll.py writes for it are its own)."""
    rt = runtime_folder(tree, host)
    problems = [f"the program has {os.path.basename(o)}, which is not the runtime's (not in {rt}): the game's objects "
                "are the library's" for o in program_objects if not inside(o, rt)]
    have = linked(library_objects, rt)
    problems += [f"the game's library has runtime/{rel}'s object: it is linked without the runtime's (tools/game_dll.py)"
                 for rel in runtime_sources(runtime) if runtime_keys(rel, host) & have]
    for m in MODULES:
        folder = module_folder(tree, host, m)
        if not any(inside(o, folder) for o in library_objects):
            problems.append(f"the game's library has none of {m}'s objects (in {folder})")
    return problems


def steps(tree, host, jobs=None, vkd3d_ready=True, python=sys.executable, dll=False):
    """[(what, command)] of the build, in order; jobs None: each build's own default; dll: --game-dll."""
    more = ["--jobs", str(jobs)] if jobs else []
    split = ["--game-dll"] if dll else []
    dirs = [os.path.join(tree, "build", m) for m in MODULES]
    if host == "windows":
        recomp = os.path.join(TOOLS, "recomp.py")
        link = [python, recomp, "link", "--dirs", ",".join(dirs), "--out", program(tree, host), *split]
        return [(f"compile {m}", [python, recomp, "compile", d, *more]) for m, d in zip(MODULES, dirs)] + \
            [("compile the runtime and link", link)]
    if host == "linux":
        vkd3d = [("vkd3d-shader", [python, os.path.join(TOOLS, "linux", "build_vkd3d.py"), "linux"])]
        build = [python, os.path.join(TOOLS, "linux", "build_linux.py"), "--port", tree,
                 "--out", os.path.join(tree, "build-linux"), *more, *split]
        return ([] if vkd3d_ready else vkd3d) + [("compile and link", build)]
    return [("compile and link", [python, os.path.join(TOOLS, "mac", "build_mac.py"), "--port", tree,
                                  "--out", os.path.join(tree, "build-mac"), *more, *split])]


def vkd3d_ready():
    """Linux: whether vkd3d-shader is built for the instruction set the build compiles for (build_game.py's
    question). tools/linux/toolchain.py stops here when clang and lld are missing."""
    sys.path.insert(0, os.path.join(TOOLS, "linux"))
    sys.path.insert(1, TOOLS)
    import isa
    import toolchain
    folder = os.path.join(toolchain.VKD3D_BUILD, "linux")
    return os.path.isfile(os.path.join(folder, "libvkd3d-shader.a")) and (isa.recorded(folder) or (None,))[0] == toolchain.ISA


def built_for(tree, host):
    """The instruction set the program is compiled for (the isa.txt its link writes), or arm64 on macOS."""
    if host == "macos":
        return "arm64"
    sys.path.insert(0, TOOLS)
    import isa
    return (isa.recorded(os.path.dirname(program(tree, host))) or ("?",))[0]


def same_path(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def main(argv=None, run=subprocess.run, host=None):
    argv = sys.argv[1:] if argv is None else argv
    if "-h" in argv or "--help" in argv:
        print(__doc__)
        return 0
    host = host or this_host()
    tree = os.path.abspath(arg(argv, "--tree") or TREE)
    jobs = arg(argv, "--jobs")
    dll = "--game-dll" in argv
    if jobs is not None and not (jobs.isdigit() and int(jobs) > 0):
        print(f"--jobs {jobs}: not a number of files")
        return 2
    if same_path(tree, REPO):
        print(f"--tree {tree}: this checkout, whose build/ is the real game's: name another folder")
        return 2
    for rel in misplaced(runtime_sources(RUNTIME)):   # (said now too: its build may stop for want of it)
        print(f"runtime/{rel}: {MISPLACED}", flush=True)
    print(f"the made-up game (tools/mac/boottest's modules) in {tree}", flush=True)
    make_tree(tree)
    if host == "windows":
        sys.path.insert(0, TOOLS)
        import cc
        if not cc.SDL3_DLL:
            print("(no SDL3 in deps\\SDL3: the runtime is compiled without its SDL3 controllers)", flush=True)
    t0 = time.time()
    for what, command in steps(tree, host, jobs, vkd3d_ready() if host == "linux" else True, dll=dll):
        print(f"\n{what}:\n  {subprocess.list2cmdline(command)}", flush=True)
        code = run(command, cwd=REPO).returncode
        if code:
            print(f"\nFAILED: {what} (exit status {code})")
            return 1
    exe = program(tree, host)
    if not os.path.isfile(exe):
        print(f"\nFAILED: the build ended without {exe}")
        return 1
    seconds = time.time() - t0
    objects = link_list(tree, host)
    if objects is None:
        problems = ["the build left no list of the objects it linked: which sources the program has is not known"]
    else:
        problems = source_problems(RUNTIME, host, linked(objects, runtime_folder(tree, host)))
    lib, loaded = library(tree, host), None
    if dll and not os.path.isfile(lib):
        problems.append(f"the build ended without the game's library, {lib}")
    elif dll and link_list(tree, host, of_library=True) is None:
        problems.append("the build left no list of the objects it linked into the game's library")
    elif dll:
        problems += library_problems(RUNTIME, tree, host, objects or [], link_list(tree, host, of_library=True))
    if dll and not problems:   # (the program started only to load the library and check it: runtime/host.c)
        command = [exe, "--game-library"]
        print(f"\nload the game's library:\n  {subprocess.list2cmdline(command)}", flush=True)
        r = run(command, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        print(r.stdout, end="", flush=True)
        loaded = next((line for line in r.stdout.splitlines() if line.startswith("[boot] game library ")), None)
        if r.returncode or not loaded:
            problems.append(f"the program did not load the game's library (exit status {r.returncode}): its output says why")
    if problems:
        lines = [f"FAILED ({host}{', --game-dll' if dll else ''}): {p}" for p in problems]
    elif dll:
        lines = [f"{host}, {built_for(tree, host)}, --game-dll: the runtime compiled and linked into {os.path.basename(exe)} "
                 f"({os.path.getsize(exe) / 1048576:,.1f} MB) and the made-up game into {os.path.basename(lib)} "
                 f"({os.path.getsize(lib) / 1048576:,.1f} MB) in {seconds:,.0f} s, which the program loaded "
                 f"({loaded.rsplit(': ', 1)[-1]}), every source in it but those left out on purpose: {', '.join(LEFT_OUT[host])}"]
    else:
        lines = [f"{host}, {built_for(tree, host)}: the runtime compiled and linked into {os.path.basename(exe)} "
                 f"({os.path.getsize(exe) / 1048576:,.1f} MB) in {seconds:,.0f} s, every source in it but those left out "
                 f"on purpose: {', '.join(LEFT_OUT[host])}"]
    print("\n" + "\n".join(lines))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:   # (the job's page on GitHub)
        with open(summary, "a", encoding="utf-8") as f:
            f.write("".join(f"{line}\n" for line in lines))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
