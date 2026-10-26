#!/usr/bin/env python3
"""Build vkd3d-shader (HLSL -> DXBC, the Linux stand-in for D3DCompile) as a static library.

Usage: build_vkd3d.py [linux|windows]...   (default: linux)

The parsers come from vkd3d's grammar files through bison and flex, generated once into
build-vkd3d/gen (tools: SSBU_BISON / SSBU_FLEX, else the system's bison and flex on Linux,
linux-toolchain/winflexbison on Windows). The windows build is only for checking shaders on a
Windows machine (tools/linux/hlsltest). The linux build is compiled for the game's instruction set
(tools/isa.py), which it writes into build-vkd3d/linux/isa.txt."""
import glob
import os
import subprocess
import sys

from toolchain import CLANG, CFLAGS, HOST_LINUX, ISA, ISA_TOLD, LLVM, LLVM_AR, TOOLCHAIN, VKD3D_BUILD, VKD3D_SRC, compile_each
import isa  # noqa: E402  (tools/, which toolchain puts on the path)

GEN = os.path.join(VKD3D_BUILD, "gen")
SPIRV = (glob.glob(os.path.join(TOOLCHAIN, "src", "SPIRV-Headers-*", "include")) or [""])[0]

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
"""
CONFIG_LINUX = CONFIG + """#define HAVE_DLFCN_H 1
#define HAVE_GETTID 1
#define HAVE_PTHREAD_H 1
#define HAVE_PTHREAD_SETNAME_NP 1
#define HAVE_PTHREAD_SETNAME_NP_2 1
#define HAVE_STRTOF_L 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WCHAR_H 1
#define HAVE_DECL_PROGRAM_INVOCATION_NAME 1
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
"""
CONFIG_WINDOWS = CONFIG + """#define HAVE__STRTOF_L 1
#define HAVE__STRTOD_L 1
#define HAVE_WCHAR_H 1
"""


def sources():
    shader = sorted(glob.glob(os.path.join(VKD3D_SRC, "libs", "vkd3d-shader", "*.c")))
    common = [os.path.join(VKD3D_SRC, "libs", "vkd3d-common", n) for n in ("debug.c", "error.c", "memory.c", "utf8.c")]
    return shader, common


def generate():
    os.makedirs(GEN, exist_ok=True)
    bison = os.environ.get("SSBU_BISON") or ("bison" if HOST_LINUX else os.path.join(TOOLCHAIN, "winflexbison", "win_bison.exe"))
    flex = os.environ.get("SSBU_FLEX") or ("flex" if HOST_LINUX else os.path.join(TOOLCHAIN, "winflexbison", "win_flex.exe"))
    src = os.path.join(VKD3D_SRC, "libs", "vkd3d-shader")
    for name in ("hlsl", "preproc"):
        if not os.path.exists(os.path.join(GEN, name + ".tab.c")):
            subprocess.run([bison, "-d", "-o", name + ".tab.c", os.path.join(src, name + ".y")], cwd=GEN, check=True)
        if not os.path.exists(os.path.join(GEN, name + ".yy.c")):
            subprocess.run([flex, "-o", name + ".yy.c", os.path.join(src, name + ".l")], cwd=GEN, check=True)
    with open(os.path.join(GEN, "vkd3d_version.h"), "w") as f:
        f.write('#define VKD3D_VCS_ID ""\n')


def build(target):
    generate()
    out = os.path.join(VKD3D_BUILD, target)
    inc = os.path.join(out, "include")
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, "config.h"), "w") as f:
        f.write(CONFIG_LINUX if target == "linux" else CONFIG_WINDOWS)
    shader, common = sources()
    gen = [os.path.join(GEN, n) for n in ("hlsl.tab.c", "hlsl.yy.c", "preproc.tab.c", "preproc.yy.c")]
    incs = ["-I", inc, "-I", GEN, "-I", os.path.join(VKD3D_SRC, "include"), "-I", os.path.join(VKD3D_SRC, "include", "private"),
            "-I", os.path.join(VKD3D_SRC, "libs", "vkd3d-shader"), "-I", SPIRV]
    defs = ["-DLIBVKD3D_SHADER_SOURCE", "-DVKD3D_NO_VULKAN_H", "-DVKD3D_DEBUG_ENV=VKD3D_SHADER_DEBUG", "-DNDEBUG"]
    srcs = shader + common + gen
    objs = [os.path.join(out, "obj", os.path.basename(s)[:-2] + ".o") for s in srcs]
    if target == "linux":
        failures = compile_each([(s, o, "-O2", incs + defs + ["-std=gnu11"]) for s, o in zip(srcs, objs)])
    else:
        compat = os.path.join(out, "compat")   # the few POSIX headers vkd3d expects
        os.makedirs(compat, exist_ok=True)
        with open(os.path.join(compat, "unistd.h"), "w") as f:
            f.write("#pragma once\n#include <io.h>\n#include <process.h>\n")
        with open(os.path.join(compat, "pthread.h"), "w") as f:
            f.write("#pragma once\n")
        cl = os.path.join(LLVM, "clang-cl.exe")
        from concurrent.futures import ThreadPoolExecutor
        def one(job):
            s, o = job
            os.makedirs(os.path.dirname(o), exist_ok=True)
            r = subprocess.run([cl, "/nologo", "/c", "/O2", "-w", "/MT", "/D_CRT_SECURE_NO_WARNINGS", "/D_USE_MATH_DEFINES", "/DWIN32_LEAN_AND_MEAN",
                                *[a.replace("-I", "/I") for a in incs], "/I", compat, *[d.replace("-D", "/D") for d in defs], s, "/Fo" + o],
                               capture_output=True, text=True)
            return (s, r.stdout + r.stderr) if r.returncode else None
        with ThreadPoolExecutor(16) as pool:
            failures = [f for f in pool.map(one, zip(srcs, objs)) if f]
    for s, err in failures:
        print(f"FAILED {os.path.basename(s)}:\n{err[:3000]}")
    if failures:
        return False
    lib = os.path.join(out, "libvkd3d-shader.a" if target == "linux" else "vkd3d-shader.lib")
    if os.path.exists(lib):
        os.remove(lib)
    subprocess.run([LLVM_AR, "rcs", lib, *objs], check=True)
    if target == "linux":   # (what it was compiled for: build_game.py builds it again for another instruction set)
        isa.record(out, ISA, ISA_TOLD)
    print(f"{target}: {lib} ({os.path.getsize(lib) / 1048576:.1f} MB)" + (f", for {ISA}" if target == "linux" else ""))
    return True


if __name__ == "__main__":
    targets = sys.argv[1:] or ["linux"]
    if HOST_LINUX and targets != ["linux"]:
        sys.exit("on Linux, only the linux build: the windows one needs Visual Studio's clang-cl")
    ok = all(build(t) for t in targets)
    sys.exit(0 if ok else 1)
