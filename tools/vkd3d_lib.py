#!/usr/bin/env python3
"""vkd3d-shader for the Windows build: DXBC to SPIR-V for the Vulkan back end in development
(runtime/gpu_spirv.c; docs/TECHNICAL.md, "A second back end: Vulkan").

Usage:
  vkd3d_lib.py [--jobs N] [--force]   fetch, generate and build deps/vkd3d/vkd3d-shader.lib if stale
  vkd3d_lib.py --status               say what is there

The source is the Linux build's (tools/linux/install_linux.py PARTS: vkd3d 2.1 and SPIRV-Headers,
each downloaded and checked against its SHA-256 there), fetched into deps/vkd3d (SSBU_VKD3D_DIR
names another folder). vkd3d's HLSL and preprocessor parsers are made from its grammar files by
bison and flex, once: SSBU_BISON and SSBU_FLEX name Windows ones, otherwise WSL's are used when
WSL has them (nothing is installed). The library is compiled with clang-cl for the program's
instruction set (tools/isa.py) and the static C runtime, as the runtime is; deps/vkd3d/
vkd3d-shader.flags says what it was built with, and a change builds it again.

tools/cc.py links the library into the program, with SSBU_HAVE_VKD3D, when it is there; without it
the program is built as before and the Vulkan back end has no shader translator. Nothing here runs
by itself during a game build: a developer of the Vulkan back end runs it once.

vkd3d is LGPL-2.1 (Wine's; NOTICE.md); the Linux build already links it (HLSL to DXBC for DXVK).
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
ROOT = os.path.dirname(TOOLS)
DIR = os.environ.get("SSBU_VKD3D_DIR") or os.path.join(ROOT, "deps", "vkd3d")
PART_NAMES = ("vkd3d 2.1", "SPIRV-Headers")
SRC = os.path.join(DIR, "src", "vkd3d-2.1")
INCLUDE = os.path.join(SRC, "include")
GEN = os.path.join(DIR, "gen")
LIB = os.path.join(DIR, "vkd3d-shader.lib")
GRAMMARS = ("hlsl", "preproc")

# vkd3d's config.h for Windows (tools/linux/build_vkd3d.py's, which builds the same library for
# checking HLSL on Windows)
CONFIG = """#define HAVE_ATOMIC_EXCHANGE_N 1
#define HAVE_BUILTIN_ADD_OVERFLOW 1
#define HAVE_BUILTIN_CLZ 1
#define HAVE_BUILTIN_CTZ 1
#define HAVE_BUILTIN_POPCOUNT 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYNC_ADD_AND_FETCH 1
#define HAVE_SYNC_BOOL_COMPARE_AND_SWAP 1
#define HAVE_SPIRV_UNIFIED1_SPIRV_H 1
#define HAVE_SPIRV_UNIFIED1_GLSL_STD_450_H 1
#define PACKAGE_NAME "vkd3d"
#define PACKAGE_VERSION "2.1"
#define PACKAGE_STRING "vkd3d 2.1"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_TARNAME "vkd3d"
#define PACKAGE_URL ""
#define VERSION "2.1"
#define HAVE__STRTOF_L 1
#define HAVE__STRTOD_L 1
#define HAVE_WCHAR_H 1
"""
DEFINES = ("LIBVKD3D_SHADER_SOURCE", "VKD3D_NO_VULKAN_H", "VKD3D_DEBUG_ENV=VKD3D_SHADER_DEBUG", "NDEBUG",
           "_CRT_SECURE_NO_WARNINGS", "_USE_MATH_DEFINES", "WIN32_LEAN_AND_MEAN")


def parts():
    """(what, URL, SHA-256, file, where, prefix) of vkd3d and SPIRV-Headers: install_linux.py's pins."""
    sys.path.insert(0, os.path.join(TOOLS, "linux"))
    import install_linux
    found = [p for p in install_linux.PARTS if p[0] in PART_NAMES]
    if len(found) != len(PART_NAMES):
        raise SystemExit("tools/linux/install_linux.py's PARTS no longer name vkd3d 2.1 and SPIRV-Headers")
    return found, install_linux


def spirv_include():
    return (glob.glob(os.path.join(DIR, "src", "SPIRV-Headers-*", "include")) or [""])[0]


def have():
    """The library and its headers are there (what tools/cc.py links)."""
    return os.path.isfile(LIB) and os.path.isfile(os.path.join(INCLUDE, "vkd3d_shader.h"))


def fetch(log=print):
    """vkd3d's and SPIRV-Headers' sources into deps/vkd3d/src, each archive checked by its SHA-256."""
    found, il = parts()
    for what, url, sha, name, where, prefix in found:
        dest = os.path.join(DIR, where)
        if os.path.isdir(dest):
            continue
        archive = os.path.join(DIR, "downloads", name)
        import hashlib
        if not (os.path.isfile(archive) and hashlib.sha256(open(archive, "rb").read()).hexdigest() == sha):
            try:
                il.download(url, archive, what, sha)
            except il.Stop as e:
                raise SystemExit(str(e))
        log(f"  {what}: {os.path.relpath(dest, ROOT)}")
        il.extract_part(archive, dest, prefix)


def wsl_path(path):
    r = subprocess.run(["wsl", "-e", "wslpath", "-a", path.replace("\\", "/")], capture_output=True, text=True)
    if r.returncode or not r.stdout.strip():
        raise SystemExit(f"WSL cannot name {path}: {r.stderr.strip()}")
    return r.stdout.strip()


def tools_for_parsers():
    """How to run bison and flex: [bison...], [flex...], and how paths are given to them."""
    bison, flex = os.environ.get("SSBU_BISON"), os.environ.get("SSBU_FLEX")
    if bison and flex:
        return [bison], [flex], lambda p: p
    if shutil.which("wsl"):
        r = subprocess.run(["wsl", "-e", "sh", "-c", "command -v bison && command -v flex"], capture_output=True, text=True)
        if r.returncode == 0:
            return ["wsl", "-e", "bison"], ["wsl", "-e", "flex"], wsl_path
    raise SystemExit("vkd3d's parsers need bison and flex: set SSBU_BISON and SSBU_FLEX (winflexbison), "
                     "or install them in WSL (sudo apt install bison flex)")


def generate(log=print):
    """vkd3d's HLSL and preprocessor parsers from its grammar files (once; without #line paths)."""
    os.makedirs(GEN, exist_ok=True)
    src = os.path.join(SRC, "libs", "vkd3d-shader")
    todo = [g for g in GRAMMARS if not all(os.path.isfile(os.path.join(GEN, g + s)) for s in (".tab.c", ".tab.h", ".yy.c"))]
    if todo:
        bison, flex, path = tools_for_parsers()
        gen = path(GEN)
        for g in todo:
            for cmd in (bison + ["--no-lines", "-d", "-o", f"{gen}/{g}.tab.c", path(os.path.join(src, g + ".y"))],
                        flex + ["--noline", "-o", f"{gen}/{g}.yy.c", path(os.path.join(src, g + ".l"))]):
                r = subprocess.run(cmd, cwd=GEN, capture_output=True, text=True)
                if r.returncode:
                    raise SystemExit(f"{' '.join(cmd[:3])} failed:\n{(r.stdout + r.stderr)[:2000]}")
            log(f"  {g}: parser made")
    with open(os.path.join(GEN, "vkd3d_version.h"), "w") as f:
        f.write('#define VKD3D_VCS_ID ""\n')
    with open(os.path.join(GEN, "config.h"), "w") as f:
        f.write(CONFIG)
    with open(os.path.join(GEN, "static_api.h"), "w") as f:   # (linked into the program: nothing exported from it)
        f.write("#pragma once\n#include <vkd3d_types.h>\n#undef VKD3D_EXPORT\n#define VKD3D_EXPORT\n")
    compat = os.path.join(GEN, "compat")   # the few POSIX headers vkd3d expects
    os.makedirs(compat, exist_ok=True)
    with open(os.path.join(compat, "unistd.h"), "w") as f:
        f.write("#pragma once\n#include <io.h>\n#include <process.h>\n")
    with open(os.path.join(compat, "pthread.h"), "w") as f:
        f.write("#pragma once\n")


def sources():
    shader = sorted(glob.glob(os.path.join(SRC, "libs", "vkd3d-shader", "*.c")))
    common = [os.path.join(SRC, "libs", "vkd3d-common", n) for n in ("debug.c", "error.c", "memory.c", "utf8.c")]
    return shader + common + [os.path.join(GEN, g + s) for g in GRAMMARS for s in (".tab.c", ".yy.c")]


def flags(target):
    import cc
    incs = [GEN, INCLUDE, os.path.join(INCLUDE, "private"), os.path.join(SRC, "libs", "vkd3d-shader"), spirv_include(),
            os.path.join(GEN, "compat")]
    return [*cc.base_cflags(target), "/O2", "/MT", *[a for i in incs for a in ("/I", i)], *["/D" + d for d in DEFINES],
            "/FI" + os.path.join(GEN, "static_api.h")]


def command(target):
    import cc
    return subprocess.list2cmdline([cc.CLANG, *flags(target)]) + "\n"


def is_current(target):
    stamp = LIB[:-4] + ".flags"
    if not have() or not os.path.isfile(stamp) or open(stamp).read() != command(target):
        return False
    return os.path.getmtime(LIB) >= max(os.path.getmtime(s) for s in sources())


def build(target=None, jobs=16, force=False, log=print):
    """Fetch, generate and build the library (if stale); True when it is there afterwards."""
    import cc
    import codec_libs
    target = target or cc.ISA
    fetch(log)
    generate(log)
    if not force and is_current(target):
        log(f"  {os.path.relpath(LIB, ROOT)}: current")
        return True
    objdir = os.path.join(DIR, "obj")
    os.makedirs(objdir, exist_ok=True)
    f = flags(target)
    srcs = sources()
    t0 = time.time()

    def one(src):
        obj = os.path.join(objdir, os.path.basename(src)[:-2] + ".obj")
        r = subprocess.run([cc.CLANG, "/c", *f, src, "/Fo" + obj], capture_output=True, text=True)
        return (src, r.stdout + r.stderr) if r.returncode else obj

    with ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(one, srcs))
    failures = [r for r in results if isinstance(r, tuple)]
    for src, err in failures[:3]:
        log(f"FAILED {os.path.basename(src)}:\n{err[:2000]}")
    if failures:
        raise SystemExit(f"vkd3d-shader: {len(failures)} of {len(srcs)} files did not compile")
    ar = codec_libs.archiver(cc.CLANG)
    if not ar:
        raise SystemExit("no llvm-lib.exe or lib.exe to make a static library with")
    rsp = LIB + ".rsp"
    with open(rsp, "w") as out:
        out.write("\n".join(f'"{o}"' for o in results))
    r = subprocess.run([*ar, "/nologo", "/OUT:" + LIB + ".partial", "@" + rsp], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"vkd3d-shader: making the library failed\n{(r.stdout + r.stderr)[:2000]}")
    os.replace(LIB + ".partial", LIB)
    with open(LIB[:-4] + ".flags", "w") as out:
        out.write(command(target))
    log(f"  {os.path.relpath(LIB, ROOT)}: {len(results)} files in {time.time() - t0:.1f}s, "
        f"{os.path.getsize(LIB) / 2**20:.1f} MB, for {target}")
    return True


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", type=int, default=16)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--status", action="store_true")
    a = ap.parse_args(argv)
    if a.status:
        print(f"{LIB}: " + (f"{os.path.getsize(LIB) / 2**20:.1f} MB" if have() else "not built (tools/vkd3d_lib.py builds it)"))
        return 0
    return 0 if build(jobs=a.jobs, force=a.force) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
