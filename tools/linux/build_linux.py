#!/usr/bin/env python3
"""Build the game for x86-64 Linux: cross-compiled on Windows, or on Linux itself (the toolchain:
tools/linux/toolchain.py).

Usage: build_linux.py [--port <dir>] [--out <dir>] [--jobs N] [--runtime-only] [--link-only] [--replace] [--game-dll]

The recompiled game is the C the Windows build already generated (tools/recomp.py writes it into
<port>/build/{main,subsdk0,sdk,nro/*}); this compiles each file again for Linux, next to nothing
of the Windows objects, into <out>/<module>/. <port> is a game tree, a folder with dump/ and
build/ at its top: the port's own (toolchain.default_port). Files with exception landing sites (guest C++ try/catch) are
compiled as C++ (see recomp.h). The runtime is runtime/*.c with the Linux platform layer
(runtime/linux), and the program links against DXVK (Direct3D 11 on Vulkan), SDL2, vkd3d-shader
(built by build_vkd3d.py), libopus and libvpx, the last three statically (on Linux, libopus and
libvpx as the system has them). Built on Linux, the DXVK libraries go into <out>/lib, so the
program runs from its own folder. With --game-dll the game's modules are linked into a library of
their own instead, <out>/ssbu_game.so, which the program loads at start (tools/game_dll.py): the
program is then the runtime alone.
Incremental: a file is compiled again when its source, the runtime headers it is built with, its
compile command or the compiler changed (tools/objcache.py: <out>/rt/ and each module's folder keep
an objcache.json); --link-only links only when nothing is to be compiled, so that <out>/isa.txt, which
the link writes (tools/isa.py), says what all of it is compiled for. <out> holds one tree's game
(port.txt): build another tree's into another <out>, or give --replace to start this one over. A
progress bar shows how far it has got and the time left (tools/progress.py). A file the compiler
crashes on, or a generated one that runs over its time limit, is compiled again another way
(tools/ladder.py: <out>/ladder_report.txt lists them); <out>/compile_times.json keeps what each
generated file took, for the next build's time left and time limits (its order too, only with
SSBU_LONGEST_FIRST=1)."""
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from collections import Counter

from toolchain import (CFLAGS, CLANG, DXVK_INCLUDE, DXVK_LIBS, HOST_LINUX, ISA, ISA_TOLD, ISA_WHY, LLVM, LLVM_NM, REPO,
                       RUNTIME, SYSROOT, TARGET, TOOLCHAIN, VKD3D_BUILD, VKD3D_SRC, compile_command, default_port,
                       read_port_stamp, run, same_path, wrapped, write_port_stamp)

sys.path.insert(0, os.path.join(REPO, "tools"))
import game_dll  # noqa: E402
import isa  # noqa: E402
import ladder  # noqa: E402
import objcache  # noqa: E402
import progress  # noqa: E402


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


PORT = os.path.abspath(arg("--port") or default_port())
OUT = os.path.abspath(arg("--out", os.path.join(REPO, "build-linux")))
JOBS = int(arg("--jobs", str(max(4, (os.cpu_count() or 8) - 2))))
GEN = os.path.join(OUT, "gen")

RUNTIME_INCLUDES = ["-I", os.path.join(RUNTIME, "linux", "include"), "-I", os.path.join(DXVK_INCLUDE, "windows"),
                    "-I", os.path.join(DXVK_INCLUDE, "directx"), "-I", os.path.join(SYSROOT, "usr", "include", "SDL2"),
                    "-I", os.path.join(VKD3D_SRC, "include"), "-I", GEN]
RUNTIME_DEFINES = ["-D_GNU_SOURCE", "-D_REENTRANT", "-DSSBU_HAVE_OPUS", "-DSSBU_HAVE_VPX", "-DSSBU_HAVE_SDL2"]
# --game-dll (tools/game_dll.py): the game's modules in LIBRARY, which the program's runtime, compiled to
# (game_dll.DEFINE), loads; the library's objects of its own in LIBRARY_OBJ. A shared library's code is
# position-independent (-fPIC, after toolchain.py's -fno-pic: the last one counts), and the game's
# functions are hidden, so that a call between two of them stays a direct call, not one through the
# library's symbol table. The program stays as it is (-fno-pic, -no-pie). The cost, which only the
# game can measure: each switch's jump table is offsets added to its address, not addresses, and the
# loader relocates the modules' tables of functions when it loads the library.
GAME_DLL = "--game-dll" in sys.argv
GAME_DLL_FLAGS = ["-fPIC", "-fvisibility=hidden"]
LIBRARY = os.path.join(OUT, game_dll.SO)
LIBRARY_OBJ = os.path.join(OUT, "game")
if GAME_DLL:
    RUNTIME_DEFINES = RUNTIME_DEFINES + ["-D" + game_dll.DEFINE]
# Each job's estimated compile in seconds, by its source, for the progress bar (tools/progress.py).
# A game file's grows with its size and with the square of its biggest function, as on Windows
# (progress.clang_cost, measured with this build), or is what it took last time (compile_times.json:
# ladder.Times). A runtime file's goes by its size.
ESTIMATES = {}
RUNTIME_PER_BYTE = 1e-5   # (gpu_d3d11.c, 210 KB: about 2 s)
LINK = 10.0               # the link and the symbol list (4 s on a fast PC)
INFOS = {}                # object -> its ladder.Info, of each generated file to compile (game_jobs)
TIMES = None              # <out>/compile_times.json (ladder.Times), once game_jobs has read it
STEPS = None              # the build's ladder.Ladder (main): one pace, one report


def modules():
    """(name, generated C directory, output directory) for every module the Windows build has."""
    build = os.path.join(PORT, "build")
    found = [(n, os.path.join(build, n), os.path.join(OUT, n)) for n in ("main", "subsdk0", "sdk")]
    nro = os.path.join(build, "nro")
    if os.path.isdir(nro):
        found += [(n, os.path.join(nro, n), os.path.join(OUT, "nro", n)) for n in sorted(os.listdir(nro))
                  if os.path.exists(os.path.join(nro, n, "manifest.tsv"))]
    return [m for m in found if os.path.exists(os.path.join(m[1], "manifest.tsv"))]


def use_tree():
    """<out>'s game objects must all be from one tree: of another tree's, one newer than this
    tree's source would be kept, and one this tree has no source for linked. So another tree's
    are only started over with --replace."""
    built = read_port_stamp(OUT)
    game = [os.path.join(OUT, n) for n in ("main", "subsdk0", "sdk", "nro")]
    if (built is None or not same_path(built, PORT)) and any(os.path.isdir(d) for d in game):
        if "--replace" not in sys.argv or "--link-only" in sys.argv:
            raise SystemExit(f"{OUT} holds the game compiled from {built or 'another tree'}: build {PORT}'s into another "
                             f"--out, or give --replace (without --link-only) to start this one over")
        print(f"{OUT} holds the game compiled from {built or 'another tree'}: compiling {PORT}'s from the start")
        for path in game:
            shutil.rmtree(path, ignore_errors=True)
        for name in ("ssbu", "ssbu.syms", "ssbu.map", "ssbu.rsp"):
            if os.path.exists(os.path.join(OUT, name)):
                os.remove(os.path.join(OUT, name))
    write_port_stamp(OUT, PORT)


def needs(obj, *srcs):
    return not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(s) for s in srcs)


# object -> (its folder's objcache.Record, the key it is compiled with, key_for(step): its key compiled a
# ladder step's way), for each job to compile
KEYS = {}
LANDING_SITES = b"__try"   # (what a generated file with exception landing sites has)


def stale(record, job, inputs, headers, why, info=None):
    """Whether a job's object is to be compiled (objcache.py): built from other `inputs` (files),
    headers (objcache.digest_files), command or compiler; why[reason] counts it. info: a generated
    file's ladder.Info (its object may be one the ladder compiled another way: ladder.check)."""
    src, obj, opt = job[:3]
    flags = job[3] if len(job) > 3 else []

    def key(level, header):
        command = compile_command(src, obj, level, flags, header=ladder.header_name(obj) if header else None)
        return record.entry(inputs, ladder.key_command(command, header), objcache.compiler_id(CLANG), headers)

    def key_for(step):
        return ladder.ladder_entry(key(step.level, step.header), step, info.above)
    entry = key(opt, info.header if info else None)
    reason = ladder.check(record, obj, entry, key_for, info.c, info.above) if info else record.check(obj, entry)
    if reason:
        KEYS[obj] = (record, entry, key_for)
        why[reason] += 1
    return reason is not None


def game_jobs():
    global TIMES
    # what the generated files include: recomp.h, one with hook slots hookable.h too, g_module.c
    # module.h (tools/recomp.py; objcache.game_headers)
    headers = {h: objcache.digest_files(objcache.header_files(RUNTIME, *h)) for h in objcache.GAME_HEADERS}
    jobs, why = [], Counter()
    # (a folder an older version built, without a record: its objects that the date said were current are kept -
    # only if they were compiled with today's flags, for today's instruction set: isa.adopts, which no target is today)
    adopt = (objcache.header_files(RUNTIME, "recomp.h", "module.h")
             if isa.adopts(OUT, ISA, isa.CLANG[ISA], isa.LEGACY_CLANG) else None)
    TIMES, above = ladder.Times(OUT), ladder.optnone_setting()
    for name, src_dir, out_dir in modules():
        record = objcache.Record(out_dir, scan=(LANDING_SITES, objcache.HOOK_SLOTS), adopt_by_date=adopt,
                                 adopt_found={objcache.HOOK_SLOTS: objcache.header_files(RUNTIME, "hookable.h")})
        with open(os.path.join(src_dir, "manifest.tsv")) as f:
            rows = [line.split("\t") for line in f.read().splitlines() if line]
        for fname, insns, biggest in rows + [["g_module.c", "0", "0"]]:
            src = os.path.join(src_dir, fname)
            obj = os.path.join(out_dir, fname[:-2] + ".o")
            insns, biggest = int(insns), int(biggest)
            own = ladder.eager(src, biggest, above) if fname != "g_module.c" else None   # (SSBU_OPTNONE_ABOVE's)
            left = own.left if own else biggest
            # very large functions make the optimiser superlinear; build those at a lower level (as on Windows)
            opt = "-O1" if left > 20000 or fname == "g_module.c" else "-O2"
            job, inputs = (src, obj, opt), [src]
            digest, found = record.source(src)
            cxx = fname != "g_module.c" and LANDING_SITES in found
            if cxx:
                # As C++, a huge function full of landing sites (lua2cpp item/fighter tables: up to
                # 210k instructions, 41k sites) takes the optimiser hours even at -O1 - and seconds
                # at -O0. Those are one-off set-up code; 38k-instruction ones still take 20 s at -O1.
                if left > 50000:
                    opt = "-O0"
                wrapper = os.path.join(out_dir, fname[:-2] + "_cxx.cpp")
                os.makedirs(out_dir, exist_ok=True)
                text = '#include "recomp.h"\nextern "C" {\n#include "%s"\n}\n' % src.replace("\\", "/")
                if not os.path.exists(wrapper) or open(wrapper).read() != text:
                    with open(wrapper, "w") as f:
                        f.write(text)
                job, inputs = (wrapper, obj, opt), [wrapper, src]
            if GAME_DLL:
                job = (*job, GAME_DLL_FLAGS)
            model = progress.TABLE if fname == "g_module.c" else progress.clang_cost(insns, left, opt, cxx)
            rel = os.path.relpath(obj, OUT)[:-2].replace(os.sep, "/") + ".c"
            measured = TIMES.measured(rel, digest, opt)
            info = ladder.Info(src, rel, insns, biggest, cxx, model, model if measured is None else measured, digest,
                               *((own.above, own.header, own.names, own.left) if own else ()), measured=measured is not None)
            if not stale(record, job, inputs, headers[objcache.game_headers(fname, record.source(src)[1])], why, info):
                continue
            jobs.append(job)
            ESTIMATES[job[0]], INFOS[obj] = info.estimate, info
        record.save()   # (the sources' digests: the next build reads only the files that changed)
    if objcache.summary(why):
        print(f"game: {objcache.summary(why)}")
    return jobs


def hle_names():
    names = set()
    for n in os.listdir(RUNTIME):
        if n.endswith(".c"):
            with open(os.path.join(RUNTIME, n), encoding="utf-8") as f:
                names |= set(re.findall(r"^HLE\((\w+)\)", f.read(), re.M))
    return sorted(names)


def hle_wraps():
    """The guest functions the runtime wraps (HLE_WRAP(<address>), as tools/recomp.py finds them)."""
    found = set()
    for n in os.listdir(RUNTIME):
        if n.endswith(".c"):
            with open(os.path.join(RUNTIME, n), encoding="utf-8") as f:
                found |= {int(a, 16) for a in re.findall(r"^HLE_WRAP\(([0-9a-fA-F]+)\)", f.read(), re.M)}
    return sorted(found)


def write_if_changed(path, text):
    if os.path.exists(path) and open(path, encoding="utf-8").read() == text:
        return False
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return True


def overlay_font():
    """gen/overlay_font.h: the debug overlay's glyphs, drawn by GDI here as the Windows build does
    (on Linux, with Pillow from the system's monospace font: make_overlay_font.py)."""
    os.makedirs(GEN, exist_ok=True)
    header = os.path.join(GEN, "overlay_font.h")
    tool_src = os.path.join(REPO, "tools", "linux", "make_overlay_font.py" if HOST_LINUX else "make_overlay_font.c")
    if not needs(header, tool_src):
        return True
    if HOST_LINUX:
        r = subprocess.run([sys.executable, tool_src, header], capture_output=True, text=True)
        print((r.stdout + r.stderr).strip())
        if r.returncode:   # (only the debug overlay's text needs it: not worth stopping the build for)
            print("could not draw the overlay font: the overlay (F1, F2) shows no text")
            with open(header, "w", newline="\n") as f:
                f.write("static const unsigned char overlay_font[%d] = {0};\n" % (24 * 16 * 48 * 6))
        return True
    exe = os.path.join(GEN, "make_overlay_font.exe")
    code, out = run([os.path.join(LLVM, "clang-cl.exe"), "/nologo", "/O1", "-w", "-fuse-ld=lld", tool_src, "/Fe" + exe, "/link", "gdi32.lib", "user32.lib"])
    if code or subprocess.run([exe, header]).returncode:
        print("could not make the overlay font:\n" + out)
        return False
    return True


def runtime_jobs():
    os.makedirs(GEN, exist_ok=True)
    rt_out = os.path.join(OUT, "rt")
    font = os.path.join(GEN, "overlay_font.h")
    headers = objcache.digest_files([os.path.join(RUNTIME, n) for n in os.listdir(RUNTIME) if n.endswith(".h")]
                                    + [os.path.join(RUNTIME, "linux", "include", "windows.h")]
                                    + ([font] if os.path.exists(font) else []))
    record, why = objcache.Record(rt_out), Counter()
    sources = [os.path.join(RUNTIME, n) for n in sorted(os.listdir(RUNTIME))
               if re.fullmatch(r"(host|kernel|gpu\w*|hle\w*|ipc\w*|net\w*|debug_watchdog)\.c", n)]
    sources += [os.path.join(RUNTIME, "linux", n) for n in sorted(os.listdir(os.path.join(RUNTIME, "linux")))
                if n.endswith((".c", ".cpp"))]
    jobs = []

    def want(job):
        if stale(record, job, [job[0]], headers, why):
            jobs.append(job)
    for src in sources:
        want((src, os.path.join(rt_out, os.path.basename(src).rsplit(".", 1)[0] + ".o"), "-O2", RUNTIME_INCLUDES + RUNTIME_DEFINES))
    # the processor check, for any x86-64 processor (it runs before the rest: runtime/cpu_check.c)
    want((os.path.join(RUNTIME, "cpu_check.c"), os.path.join(rt_out, "cpu_check.o"), "-O2", ["baseline"]))
    names = hle_names()
    table = os.path.join(GEN, "hle_table.c")
    write_if_changed(table, '#include "host.h"\n\n' + "".join(f"GuestFn hle_{n};\n" for n in names)
                     + "const HleEntry hle_table[] = {\n" + "".join(f'    {{"{n}", hle_{n}}},\n' for n in names)
                     + f"    {{0, 0}}\n}};\nconst u32 hle_count = {len(names)};\n")
    want((table, os.path.join(rt_out, "hle_table.o"), "-O1", RUNTIME_INCLUDES + RUNTIME_DEFINES))
    # (the program is linked from every object in rt/: the other kind of build's go)
    for name in ("wrap_missing.o", "nro_table.o") if GAME_DLL else ("game_funcs.o",):
        if os.path.exists(os.path.join(rt_out, name)):
            os.remove(os.path.join(rt_out, name))
    if GAME_DLL:   # (the originals of the wrapped functions and the NROs' list are the library's: library_jobs)
        funcs = os.path.join(GEN, game_dll.FUNCS)
        write_if_changed(funcs, game_dll.funcs_text(RUNTIME))
        want((funcs, os.path.join(rt_out, "game_funcs.o"), "-O1", RUNTIME_INCLUDES + RUNTIME_DEFINES))
        record.save()
        if objcache.summary(why):
            print(f"runtime: {objcache.summary(why)}")
        return jobs + library_jobs(headers)
    # A wrapped function no module of this build has (a made-up game's): the original its wrapper names
    # (f_<address>_orig, which nothing reaches) is rt_wrap_missing. The Windows link says so
    # with /alternatename (tools/recomp.py); here it is a weak stub a module's own replaces.
    missing = os.path.join(GEN, "wrap_missing.c")
    write_if_changed(missing, '#include "host.h"\n\nvoid rt_wrap_missing(Cpu *c);\n'
                     + "".join(f"__attribute__((weak)) void f_{a:x}_orig(Cpu *c) {{ rt_wrap_missing(c); }}\n"
                               for a in hle_wraps()))
    want((missing, os.path.join(rt_out, "wrap_missing.o"), "-O1", RUNTIME_INCLUDES + RUNTIME_DEFINES))
    nros =[n for n, src, _o in modules() if os.path.basename(os.path.dirname(src)) == "nro"]
    nro_table = os.path.join(GEN, "nro_table.c")
    write_if_changed(nro_table, '#include "module.h"\n\n' + "".join(f"extern const RtModule rt_module_{n};\n" for n in nros)
                     + "const RtModule *const rt_nro_modules[] = {" + "".join(f"&rt_module_{n}, " for n in nros) + "0};\n"
                     + f"const u32 rt_nro_count = {len(nros)};\n")
    want((nro_table, os.path.join(rt_out, "nro_table.o"), "-O1", ["-I", RUNTIME]))
    record.save()
    if objcache.summary(why):
        print(f"runtime: {objcache.summary(why)}")
    return jobs


def nro_table_text():
    nros = [n for n, src, _o in modules() if os.path.basename(os.path.dirname(src)) == "nro"]
    return ('#include "module.h"\n\n' + "".join(f"extern const RtModule rt_module_{n};\n" for n in nros)
            + "const RtModule *const rt_nro_modules[] = {" + "".join(f"&rt_module_{n}, " for n in nros) + "0};\n"
            + f"const u32 rt_nro_count = {len(nros)};\n")


def library_jobs(headers):
    """--game-dll: the library's own objects, of gen/game_tables.c (tools/game_dll.py) and gen/nro_table.c, compiled
    as the game is (GAME_DLL_FLAGS), into LIBRARY_OBJ."""
    record, why, jobs = objcache.Record(LIBRARY_OBJ), Counter(), []
    for name, text in ((game_dll.TABLES, game_dll.tables_text(RUNTIME)), ("nro_table.c", nro_table_text())):
        src = os.path.join(GEN, name)
        write_if_changed(src, text)
        job = (src, os.path.join(LIBRARY_OBJ, name[:-2] + ".o"), "-O1", GAME_DLL_FLAGS)
        if stale(record, job, [src], headers, why):
            jobs.append(job)
    record.save()
    if objcache.summary(why):
        print(f"game library: {objcache.summary(why)}")
    return jobs


def in_order(jobs):
    """Biggest first: a few files take minutes, and started last they would hold up the end. With
    SSBU_LONGEST_FIRST=1, once a build has measured them, the longest first by what they took
    (ladder.longest_first: compile_times.json)."""
    measured = ladder.longest_first(jobs, INFOS)
    if measured:
        return measured

    def source_size(job):   # (a C++ wrapper's size is that of the generated file it includes)
        src = job[0]
        if src.endswith("_cxx.cpp"):
            src = re.findall(r'#include "([^"]+)"', open(src).read())[-1]
        return os.path.getsize(src)
    return sorted(jobs, key=source_size, reverse=True)


def forget(jobs):
    """Before the jobs compile: their objects and recorded keys gone (objcache.py), so that neither a
    build stopped half-way nor a file that fails to compile leaves an old object to be taken as
    current, by the next build or by a link (--link-only)."""
    records = {}
    for job in jobs:
        records.setdefault(KEYS[job[1]][0], []).append(job[1])
    for record, objs in records.items():
        record.forget(objs)
    for job in jobs:
        if os.path.exists(job[1]):
            os.remove(job[1])


def new_ladder():
    """A build's compile ladder (tools/ladder.py), its compilers at the priority run() gives them."""
    return ladder.Ladder(progress.clang_cost, "clang", popen=wrapped([])[1])


def compile_all(jobs, what, shown=None):
    """Compile the jobs (from stale: each one's key is recorded once it compiles), a crash compiled again
    and a generated file over its time limit compiled another way (STEPS, else a ladder of its own,
    whose report is said at the end: tools/ladder.py); `shown`, a progress.Build with a module named
    `what`, follows them."""
    if not jobs:
        print(f"{what}: up to date")
        return True
    t0 = time.time()
    jobs = in_order(jobs)
    forget(jobs)
    for job in jobs:
        os.makedirs(os.path.dirname(job[1]), exist_ok=True)
    if shown:
        shown.begin("compile", what, [(job[0], estimate(job)) for job in jobs])
    done, lock = [0], threading.Lock()

    def finished(job, seconds):
        if shown:
            shown.finished(job, seconds)
        with lock:
            done[0] += 1
            if done[0] % 64 == 0 or done[0] == len(jobs):
                print(f"{what}: {done[0]}/{len(jobs)} compiled, {time.time() - t0:,.0f}s", flush=True)

    def compiled(job, _level, step=None):
        record, key, key_for = KEYS[job[1]]
        record.compiled(job[1], key if step is None else key_for(step))

    def argv(job, level, header):
        return wrapped(compile_command(job[0], job[1], level, job[3] if len(job) > 3 else [], header=header))[0]
    steps = STEPS or new_ladder()
    failures = steps.compile(jobs, JOBS, argv, INFOS, shown, None, finished, compiled, TIMES)
    if shown:
        shown.end("compile", what)
    for src, err in failures[:8]:
        print(f"FAILED {src}:\n{err[:3000]}")
    if STEPS is None:
        steps.end(OUT)
    return not failures


def estimate(job):
    """A job's estimated compile in seconds (ESTIMATES), for one not listed there by its source's size."""
    if job[0] not in ESTIMATES:
        ESTIMATES[job[0]] = RUNTIME_PER_BYTE * os.path.getsize(job[0])
    return ESTIMATES[job[0]]


def link():
    objs = []
    rt_out = os.path.join(OUT, "rt")
    objs += sorted(os.path.join(rt_out, n) for n in os.listdir(rt_out) if n.endswith(".o"))
    library = sorted(os.path.join(LIBRARY_OBJ, n) for n in os.listdir(LIBRARY_OBJ) if n.endswith(".o")) if GAME_DLL else None
    for _name, _src, out_dir in modules():
        (objs if library is None else library).extend(sorted(os.path.join(out_dir, n) for n in os.listdir(out_dir) if n.endswith(".o")))
    if os.path.exists(LIBRARY):   # (a --game-dll build's: made again below, or not wanted)
        os.remove(LIBRARY)
    exe = os.path.join(OUT, "ssbu")
    rsp = exe + ".rsp"
    with open(rsp, "w") as f:
        f.write("\n".join('"%s"' % o.replace("\\", "/") for o in objs))
    t0 = time.time()
    code, out = run([CLANG, *TARGET, "-fuse-ld=lld", "-no-pie", "@" + rsp, "-o", exe,
                     os.path.join(VKD3D_BUILD, "linux", "libvkd3d-shader.a"), codec_lib("opus"), codec_lib("vpx"),
                     "-L" + DXVK_LIBS, "-l:libdxvk_d3d11.so.0", "-l:libdxvk_dxgi.so.0", "-l:libSDL2-2.0.so.0",
                     "-lstdc++", "-lm", "-lpthread", "-ldl", "-lrt",
                     "-Wl,-rpath,$ORIGIN/lib", "-Wl,--allow-shlib-undefined", "-Wl,-z,noexecstack", "-Wl,--build-id",
                     "-Wl,-Map=" + exe + ".map"])
    if code:
        errors = [l for l in out.splitlines() if "error" in l.lower()] or out.splitlines()
        print("link failed\n" + "\n".join(errors)[:6000])
        return False
    isa.record(OUT, ISA, ISA_TOLD)   # (what build_game.py's build_is_current compares)
    syms = subprocess.run([LLVM_NM, "-n", "--defined-only", exe], capture_output=True, text=True)
    with open(os.path.join(OUT, "ssbu.syms"), "w", newline="\n") as f:
        f.write(syms.stdout)
    if HOST_LINUX:   # (the program looks for them in lib/ beside it: -rpath $ORIGIN/lib)
        for so in ("libdxvk_d3d11.so.0", "libdxvk_dxgi.so.0"):
            src, dst = os.path.join(DXVK_LIBS, so), os.path.join(OUT, "lib", so)
            if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(src):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
    print(f"linked {len(objs)} objects into {exe} ({os.path.getsize(exe) / 1048576:,.0f} MB) in {time.time() - t0:,.0f}s")
    return library is None or link_library(library)


def link_library(objs):
    """--game-dll: the game's modules (objs, with library_jobs') linked into LIBRARY, a shared library whose one
    exported name is game_dll.ENTRY (the rest is hidden: GAME_DLL_FLAGS). It reaches the program's runtime only
    through its tables (tools/game_dll.py), so -z defs fails the link on any symbol neither it nor the C and C++
    libraries define."""
    rsp = LIBRARY + ".rsp"
    with open(rsp, "w") as f:
        f.write("\n".join('"%s"' % o.replace("\\", "/") for o in objs))
    t0 = time.time()
    code, out = run([CLANG, *TARGET, "-fuse-ld=lld", "-shared", "@" + rsp, "-o", LIBRARY, "-lstdc++", "-lm",
                     "-Wl,-z,defs", "-Wl,-z,noexecstack", "-Wl,--build-id", "-Wl,-Map=" + LIBRARY + ".map"])
    if code:
        errors = [l for l in out.splitlines() if "error" in l.lower()] or out.splitlines()
        print("the game library's link failed\n" + "\n".join(errors)[:6000])
        return False
    print(f"linked {len(objs)} objects into {LIBRARY} ({os.path.getsize(LIBRARY) / 1048576:,.0f} MB) in {time.time() - t0:,.0f}s")
    return True


def link_only():
    """--link-only: link(), when no object is to be compiled (runtime_jobs, game_jobs: each one compiled
    with this build's command, so for its instruction set). link writes <out>/isa.txt, which must name
    what every object it links is compiled for: avx2 objects linked as x86-64-v2 would pass the
    processor check on a processor without AVX2 (it checks for what the runtime is compiled for), and
    build_game.py would take the program as current."""
    stale = runtime_jobs() + game_jobs()
    if stale:
        built = isa.built_for(OUT)
        print(f"--link-only: {len(stale)} object{'s' if len(stale) != 1 else ''} not compiled for this build"
              + (f" (the folder is built for {built}, this build is for {ISA})" if built != ISA else "")
              + ": build without --link-only")
        return False
    return link()


def codec_lib(name):
    """libopus or libvpx for the link: cross-compiling, the sysroot's static library; on Linux,
    the system's static one if it has one (then the program needs no copy at run time), else its
    shared one."""
    if not HOST_LINUX:
        return os.path.join(SYSROOT, "usr", "lib", "x86_64-linux-gnu", f"lib{name}.a")
    r = subprocess.run([CLANG, f"-print-file-name=lib{name}.a"], capture_output=True, text=True)
    path = r.stdout.strip()
    return path if os.path.isabs(path) and os.path.isfile(path) else f"-l{name}"


def dxvk_libs():
    """The DXVK libraries under their sonames (the release's symbolic links do not survive on Windows)."""
    for so in ("libdxvk_d3d11.so", "libdxvk_dxgi.so"):
        target = os.path.join(DXVK_LIBS, so + ".0")
        if not os.path.exists(target):
            real = [n for n in os.listdir(DXVK_LIBS) if n.startswith(so + ".0.")]
            if not real:
                raise SystemExit(f"{so}: not in {DXVK_LIBS}")
            with open(os.path.join(DXVK_LIBS, real[0]), "rb") as a, open(target, "wb") as b:
                b.write(a.read())


def main():
    global STEPS
    ladder.settings()   # (SSBU_COMPILE_LIMIT, SSBU_OPTNONE_ABOVE, SSBU_LONGEST_FIRST: one that is not a number stops the build now)
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(os.path.join(VKD3D_BUILD, "linux", "libvkd3d-shader.a")):
        raise SystemExit("vkd3d-shader is not built: run tools/linux/build_vkd3d.py linux")
    vkd3d = isa.recorded(os.path.join(VKD3D_BUILD, "linux"))   # (none: an older version built it, with its own flags)
    if not vkd3d or vkd3d[0] != ISA:
        raise SystemExit(f"vkd3d-shader is built for {vkd3d[0] if vkd3d else 'an older version'}, the game for {ISA}: "
                         "run tools/linux/build_vkd3d.py linux")
    print(f"instruction set: {ISA} ({ISA_WHY})")
    dxvk_libs()
    if not modules():
        raise SystemExit(f"no recompiled game in {os.path.join(PORT, 'build')} (tools/recomp.py all)")
    print(f"game tree: {PORT}")
    if "--runtime-only" not in sys.argv:
        use_tree()
    if "--link-only" in sys.argv:
        return link_only()
    if not overlay_font():
        return False
    runtime_only = "--runtime-only" in sys.argv
    rt, game = in_order(runtime_jobs()), [] if runtime_only else in_order(game_jobs())
    shown = progress.Build(JOBS, link=None if runtime_only else LINK)
    shown.module("runtime", 0, [(job[0], estimate(job)) for job in rt])
    shown.module("game", 0, [(job[0], estimate(job)) for job in game])
    STEPS = new_ladder()
    with progress.Line(shown.status, title="building ssbu"):
        try:
            if not compile_all(rt, "runtime", shown):
                return False
            if runtime_only:
                return True
            if not compile_all(game, "game", shown):
                return False
        finally:
            STEPS.end(OUT)
        shown.begin("link")
        ok = link()
        shown.end("link")
        return ok


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
