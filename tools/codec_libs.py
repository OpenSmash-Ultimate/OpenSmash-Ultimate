#!/usr/bin/env python3
"""The audio and video codecs, built from source: libopus (the game's music) and libvpx's VP9
decoder (its movies).

Usage:
  codec_libs.py [--jobs N] [--force]     build build/codecs/opus.lib and libvpx.lib if stale
  codec_libs.py --status                 say what is there
On macOS: build-mac/codecs/libopus.a and libvpx.a instead (--out <dir> names another folder
than build-mac for codecs/).

Their sources live under third_party/opus and third_party/libvpx (third_party/README.md says
which upstream versions and how they were taken). `build()` compiles them with clang-cl into
build/codecs/ - some 200 small C files, a few seconds - and skips a library that is newer than
its sources. tools/cc.py adds the libraries and their defines to every link whose sources are
there, so music and movies work without the developer setting SSBU_OPUS_LIBS or SSBU_VPX_DIR;
those two still win when they are set.

Why plain C for libvpx: its x86 assembly needs nasm, one more tool to install, and the
intrinsics-only files depend on symbols from that assembly. Measured on the game's 1280x720
title movie (1,069 frames, four decoder threads): about 570 frames a second in plain C against
1,100 with the assembly - either far above the 30 the game shows. libopus keeps its SSE, SSE4.1
and AVX2 intrinsics, which are plain C for a compiler told the instruction set the whole program
is built for (tools/isa.py): all of them for avx2; for x86-64-v2, SSE4.1 at most, without opus's
AVX2 files. A library is built again when its sources or its compile command change (another
instruction set): build/codecs/<library>.flags says what it was built with.

The macOS build (arm64, tools/mac/build_mac.py) has `build_static()` make them with clang and ar
into <its folder>/codecs: both in plain C there - opus without its x86 code (its ARM code is not
vendored), libvpx with config-arm64/, the same configuration for arm64 in C only.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)   # (for cc, also when this runs on its own under a Python that does not add it: recomp.py)
ROOT = os.path.dirname(TOOLS)
THIRD_PARTY = os.path.join(ROOT, "third_party")
OPUS = os.path.join(THIRD_PARTY, "opus")
VPX = os.path.join(THIRD_PARTY, "libvpx")
OUT = os.path.join(ROOT, "build", "codecs")
OPUS_LIB = os.path.join(OUT, "opus.lib")
VPX_LIB = os.path.join(OUT, "libvpx.lib")

# The source folders of each library, as its own build lists them for a float, x86-64 build
# without run-time CPU detection (opus) and a VP9-decoder-only build in plain C (libvpx).
OPUS_FOLDERS = ("celt", "celt/x86", "silk", "silk/x86", "silk/float", "silk/float/x86", "src")
OPUS_INCLUDES = ("include", "celt", "silk", "silk/float", "src", "")
# opus's build-system defines: no config.h; alloca for its scratch buffers (no C99 VLAs with
# clang-cl); the x86 intrinsics present and presumed (the program needs the instruction set it is
# built for anyway, so there is nothing to detect at run time): avx2's, then x86-64-v2's (SSE4.1 at most)
OPUS_DEFINES = ("OPUS_BUILD", "USE_ALLOCA", "HAVE_LRINT", "HAVE_LRINTF", 'PACKAGE_VERSION="1.5.2"',
                "OPUS_X86_MAY_HAVE_SSE", "OPUS_X86_MAY_HAVE_SSE2", "OPUS_X86_MAY_HAVE_SSE4_1", "OPUS_X86_MAY_HAVE_AVX2",
                "OPUS_X86_PRESUME_SSE", "OPUS_X86_PRESUME_SSE2", "OPUS_X86_PRESUME_SSE4_1", "OPUS_X86_PRESUME_AVX2")
OPUS_DEFINES_V2 = tuple(d for d in OPUS_DEFINES if not d.endswith("_AVX2"))
VPX_FOLDERS = ("vp9", "vp9/common", "vp9/decoder", "vpx/src", "vpx_dsp", "vpx_mem", "vpx_ports",
               "vpx_scale", "vpx_scale/generic", "vpx_util")
VPX_INCLUDES = ("config", "")   # config/: the headers libvpx's configure and make generate
VPX_DEFINES = ("_CRT_SECURE_NO_WARNINGS",)
# The macOS (arm64) build's: opus's portable C, with the defines of its build system for that
# (no x86 folders or OPUS_X86_*), and libvpx's same sources with config-arm64/ in place of config/
OPUS_FOLDERS_ARM64 = ("celt", "silk", "silk/float", "src")
OPUS_DEFINES_ARM64 = ("OPUS_BUILD", "USE_ALLOCA", "HAVE_LRINT", "HAVE_LRINTF", 'PACKAGE_VERSION="1.5.2"')
VPX_INCLUDES_ARM64 = ("config-arm64", "")
VPX_DEFINES_ARM64 = ()


def have_opus():
    return os.path.isfile(os.path.join(OPUS, "src", "opus_decoder.c"))


def have_vpx(config="config"):
    return os.path.isfile(os.path.join(VPX, "vp9", "vp9_dx_iface.c")) and \
        os.path.isfile(os.path.join(VPX, config, "vpx_config.h"))


def sources(root, folders):
    """The .c files of the given folders under `root` (not their subfolders), sorted. (Not the
    ._* files macOS leaves beside each file on a volume without its metadata, e.g. exFAT.)"""
    out = []
    for folder in folders:
        d = os.path.join(root, folder) if folder else root
        if os.path.isdir(d):
            out += sorted(os.path.join(d, n) for n in os.listdir(d) if n.endswith(".c") and not n.startswith("._"))
    return out


def inputs(root, folders, includes):
    """Everything a library is built from: its sources, the headers next to them and in its
    include folders, and this file (its flags)."""
    files = sources(root, folders)
    for folder in set(folders) | set(includes):
        d = os.path.join(root, folder) if folder else root
        if os.path.isdir(d):
            files += [os.path.join(d, n) for n in os.listdir(d) if n.endswith(".h") and not n.startswith("._")]
    return files + [os.path.abspath(__file__)]


def is_current(lib, root, folders, includes):
    """The library is there and newer than everything it is built from."""
    if not os.path.isfile(lib):
        return False
    return os.path.getmtime(lib) >= max(os.path.getmtime(p) for p in inputs(root, folders, includes))


def archiver(clang):
    """llvm-lib next to clang-cl (Visual Studio's Clang tools and LLVM both ship it), or lib.exe."""
    candidate = os.path.join(os.path.dirname(clang), "llvm-lib.exe")
    if os.path.isfile(candidate):
        return [candidate]
    for name in ("llvm-lib", "lib"):
        if shutil.which(name):
            return [shutil.which(name)]
    return None


def wanted(src, defines):
    """Whether a library's source is built with these defines: opus's AVX2 files only with its AVX2
    code (they use AVX2 intrinsics unguarded, which a compiler not told AVX2 refuses)."""
    return "OPUS_X86_MAY_HAVE_AVX2" in defines or not re.search(r"_avx2?\.c$", os.path.basename(src))


def windows_flags(root, includes, defines, target):
    """The Windows build's flags for one library: the program's own for the instruction set `target`
    (cc.base_cflags), /O2, the library's include folders and defines."""
    import cc
    flags = [*cc.base_cflags(target), "/O2"]
    for inc in includes:
        flags += ["/I", os.path.join(root, inc) if inc else root]
    return flags + ["/D" + d for d in defines]


def windows_command(root, includes, defines, target):
    """What a Windows library's .flags records: the compiler and its flags."""
    import cc
    return subprocess.list2cmdline([cc.CLANG, *windows_flags(root, includes, defines, target)]) + "\n"


def build_lib(name, lib, root, folders, includes, defines, jobs, log, target):
    import cc
    objdir = os.path.join(OUT, name)
    os.makedirs(objdir, exist_ok=True)
    flags = windows_flags(root, includes, defines, target)
    srcs = [s for s in sources(root, folders) if wanted(s, defines)]
    t0 = time.time()

    def one(src):
        obj = os.path.join(objdir, os.path.relpath(src, root).replace(os.sep, "_")[:-2] + ".obj")
        r = subprocess.run([cc.CLANG, "/c", *flags, src, "/Fo" + obj], capture_output=True, text=True)
        return (src, r.stdout + r.stderr) if r.returncode else obj

    with ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(one, srcs))
    failures = [r for r in results if isinstance(r, tuple)]
    for src, err in failures[:3]:
        log(f"FAILED {os.path.relpath(src, root)}:\n{err[:2000]}")
    if failures:
        raise RuntimeError(f"{name}: {len(failures)} of {len(srcs)} files did not compile")
    objs = [r for r in results if not isinstance(r, tuple)]
    ar = archiver(cc.CLANG)
    if not ar:
        raise RuntimeError("no llvm-lib.exe or lib.exe to make a static library with")
    rsp = lib + ".rsp"
    with open(rsp, "w") as f:
        f.write("\n".join(f'"{o}"' for o in objs))
    partial = lib + ".partial"
    r = subprocess.run([*ar, "/nologo", "/OUT:" + partial, "@" + rsp], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{name}: making the library failed\n{(r.stdout + r.stderr)[:2000]}")
    os.replace(partial, lib)
    log(f"  {os.path.relpath(lib, ROOT)}: {len(objs)} files in {time.time() - t0:.1f}s")


def libraries(target="avx2"):
    """name -> (library, root, folders, includes, defines, have) of the Windows build's libraries for
    an instruction set (tools/isa.py)."""
    return {
        "opus": (OPUS_LIB, OPUS, OPUS_FOLDERS, OPUS_INCLUDES, OPUS_DEFINES if target == "avx2" else OPUS_DEFINES_V2, have_opus),
        "libvpx": (VPX_LIB, VPX, VPX_FOLDERS, VPX_INCLUDES, VPX_DEFINES, have_vpx),
    }


LIBRARIES = libraries()


def build(which=("opus", "libvpx"), jobs=16, force=False, log=print, target=None):
    """Build the named libraries whose sources are there and whose library is stale or was built with
    other flags (another instruction set: `target`, cc.ISA unless given). Returns the libraries that
    are there afterwards."""
    if target is None:
        import cc
        target = cc.ISA
    libs = []
    for name in which:
        lib, root, folders, includes, defines, have = libraries(target)[name]
        if not have():
            continue
        command = windows_command(root, includes, defines, target)
        if force or not static_is_current(lib, root, folders, includes, command):
            log(f"  building {name} from {os.path.relpath(root, ROOT)} for {target}")
            build_lib(name, lib, root, folders, includes, defines, jobs, log, target)
            with open(lib + ".flags", "w", encoding="utf-8", newline="\n") as f:
                f.write(command)
        libs.append(lib)
    return libs


# ---- the macOS build: clang and ar ------------------------------------------------------------
def static_libraries(out, arch="arm64"):
    """name -> (library, root, folders, includes, defines, have) of the static libraries a
    clang build for `arch` makes in out/codecs (macOS: arm64)."""
    if arch != "arm64":
        raise ValueError(f"no codec configuration for {arch}")
    codecs = os.path.join(out, "codecs")
    return {
        "opus": (os.path.join(codecs, "libopus.a"), OPUS, OPUS_FOLDERS_ARM64, OPUS_INCLUDES, OPUS_DEFINES_ARM64, have_opus),
        "libvpx": (os.path.join(codecs, "libvpx.a"), VPX, VPX_FOLDERS, VPX_INCLUDES_ARM64, VPX_DEFINES_ARM64,
                   lambda: have_vpx("config-arm64")),
    }


def static_flags(cflags, root, includes, defines):
    """The compile flags of one library: the program's own (`cflags`), -O2, its include folders and defines."""
    flags = [*cflags, "-O2"]
    for inc in includes:
        flags += ["-I", os.path.join(root, inc) if inc else root]
    return flags + ["-D" + d for d in defines]


def build_static_lib(name, lib, root, folders, flags, clang, ar, jobs, log):
    objdir = os.path.join(os.path.dirname(lib), name)
    os.makedirs(objdir, exist_ok=True)
    srcs = sources(root, folders)
    t0 = time.time()

    def one(src):
        obj = os.path.join(objdir, os.path.relpath(src, root).replace(os.sep, "_")[:-2] + ".o")
        r = subprocess.run([clang, "-c", *flags, src, "-o", obj], capture_output=True, text=True)
        return (src, r.stdout + r.stderr) if r.returncode else obj

    with ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(one, srcs))
    failures = [r for r in results if isinstance(r, tuple)]
    for src, err in failures[:3]:
        log(f"FAILED {os.path.relpath(src, root)}:\n{err[:2000]}")
    if failures:
        raise RuntimeError(f"{name}: {len(failures)} of {len(srcs)} files did not compile")
    objs = [r for r in results if not isinstance(r, tuple)]
    partial = lib + ".partial"
    if os.path.exists(partial):   # (ar adds to an archive that is there)
        os.remove(partial)
    r = subprocess.run([ar, "rcs", partial, *objs], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{name}: making the library failed\n{(r.stdout + r.stderr)[:2000]}")
    os.replace(partial, lib)
    log(f"  {os.path.basename(lib)}: {len(objs)} files in {time.time() - t0:.1f}s")


def static_is_current(lib, root, folders, includes, command):
    """The library is newer than everything it is built from, and was built with this command."""
    try:
        with open(lib + ".flags", encoding="utf-8") as f:
            built_with = f.read()
    except OSError:
        return False
    return built_with == command and is_current(lib, root, folders, includes)


def build_static(out, clang, cflags, ar, which=("opus", "libvpx"), jobs=16, force=False, log=print, arch="arm64"):
    """The macOS build's codecs: out/codecs/libopus.a and libvpx.a, compiled with `clang` and
    the program's `cflags` (tools/mac/toolchain.py), archived with `ar`, each when stale or
    built with other flags. Returns the libraries that are there afterwards."""
    libs = []
    for name in which:
        lib, root, folders, includes, defines, have = static_libraries(out, arch)[name]
        if not have():
            continue
        flags = static_flags(cflags, root, includes, defines)
        command = subprocess.list2cmdline([clang, *flags]) + "\n"
        if force or not static_is_current(lib, root, folders, includes, command):
            log(f"  building {name} from {os.path.relpath(root, ROOT)}")
            build_static_lib(name, lib, root, folders, flags, clang, ar, jobs, log)
            with open(lib + ".flags", "w", encoding="utf-8", newline="\n") as f:
                f.write(command)
        libs.append(lib)
    return libs


def mac_toolchain():
    sys.path.insert(0, os.path.join(TOOLS, "mac"))
    import toolchain
    return toolchain


def main_mac(a):
    tc = mac_toolchain()
    out = os.path.abspath(a.out or os.path.join(ROOT, "build-mac"))
    for name, (lib, root, folders, includes, defines, have) in static_libraries(out).items():
        command = subprocess.list2cmdline([tc.CLANG, *static_flags(tc.CFLAGS, root, includes, defines)]) + "\n"
        state = "no sources" if not have() else "up to date" if static_is_current(lib, root, folders, includes, command) \
            else "to build"
        print(f"{name}: {len(sources(root, folders))} sources, {lib} {state}")
    if a.status:
        return 0
    try:
        build_static(out, tc.CLANG, tc.CFLAGS, tc.AR, jobs=a.jobs, force=a.force)
    except RuntimeError as e:
        print(f"ERROR: {e}")
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description="Build the codec libraries from third_party/.")
    ap.add_argument("--jobs", type=int, default=16)
    ap.add_argument("--force", action="store_true", help="build even when the libraries are up to date")
    ap.add_argument("--status", action="store_true", help="only say what is there")
    if sys.platform == "darwin":
        ap.add_argument("--out", help="the build folder whose codecs/ they go into (default: build-mac)")
    a = ap.parse_args()
    if sys.platform == "darwin":
        return main_mac(a)
    import cc
    print(f"instruction set: {cc.ISA} ({cc.ISA_WHY})")
    for name, (lib, root, folders, includes, defines, have) in libraries(cc.ISA).items():
        current = static_is_current(lib, root, folders, includes, windows_command(root, includes, defines, cc.ISA))
        state = "no sources" if not have() else "up to date" if current else "to build"
        srcs = [s for s in sources(root, folders) if wanted(s, defines)]
        print(f"{name}: {len(srcs)} sources, {os.path.relpath(lib, ROOT)} {state}")
    if a.status:
        return 0
    try:
        build(jobs=a.jobs, force=a.force)
    except RuntimeError as e:
        print(f"ERROR: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
