#!/usr/bin/env python3
"""glslang for the Windows build: the Vulkan back end's shader translator without vkd3d-shader
(docs/TECHNICAL.md, "A second back end: Vulkan"; runtime/gpu_spirv.c's spirv_from_hlsl).

Usage:
  glslang_lib.py [--zip FILE] [--out DLL]   fetch glslang if missing, build the DLL (beside build\\ssbu.exe)
  glslang_lib.py --status                   say what is there

Khronos' Windows release of glslang 16.6.0 (headers and static libraries, SPIRV-Tools with them),
pinned by its SHA-256, is unpacked into deps/glslang (SSBU_GLSLANG_DIR names another folder; --zip
or SSBU_GLSLANG_ZIP a copy of the archive already on this PC, checked the same way). Its libraries
are built against the DLL C runtime (/MD), the program against the static one, so they go into a
library of their own, ssbu_glslang.dll (tools/glslang_shim.c: two C functions), linked with clang-cl
and put beside the program, which loads it when the Vulkan back end starts. It needs the Visual C++
runtime (vcruntime140.dll, msvcp140.dll: on nearly every PC, graphics drivers install it); without
it, or without the DLL, Vulkan says it has no shader translator and Direct3D 11 draws.

tools/build_game.py runs this after the program is built (a failure is said and is not fatal);
recomp.py's link makes the DLL again when deps/glslang is there. glslang is BSD-3-Clause with
parts under other permissive licences, SPIRV-Tools Apache-2.0 (NOTICE.md).
"""
import argparse
import hashlib
import os
import subprocess
import sys
import zipfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
ROOT = os.path.dirname(TOOLS)
DIR = os.environ.get("SSBU_GLSLANG_DIR") or os.path.join(ROOT, "deps", "glslang")
VERSION = "16.6.0"
ASSET = f"glslang-{VERSION}-windows-x86_64-release.zip"
URL = f"https://github.com/KhronosGroup/glslang/releases/download/{VERSION}/{ASSET}"
SHA256 = "82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b"
INCLUDE = os.path.join(DIR, "include")
LIBDIR = os.path.join(DIR, "lib")
LIBS = ("glslang.lib", "glslang-default-resource-limits.lib", "SPIRV-Tools-opt.lib", "SPIRV-Tools.lib")
SHIM = os.path.join(TOOLS, "glslang_shim.c")
HEADERS = [os.path.join(ROOT, "runtime", "gpu_spirv.h")]
DLL_NAME = "ssbu_glslang.dll"
# what the DLL links of the C runtime besides the static libraries the program uses (deps\msvc keeps
# them since tools/msvc_deps.py's VC_LIBS names them; Visual Studio always has them)
DLL_CRT = ("msvcrt.lib", "msvcprt.lib", "vcruntime.lib", "ucrt.lib")


def have():
    """glslang's release is unpacked (what the DLL is built from)."""
    return os.path.isfile(os.path.join(LIBDIR, "glslang.lib")) and \
        os.path.isfile(os.path.join(INCLUDE, "glslang", "Include", "glslang_c_interface.h"))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(zip_path=None, log=print):
    """deps/glslang from the release archive: zip_path (or SSBU_GLSLANG_ZIP) if given, else downloaded
    from GitHub; either checked against SHA256. Nothing is done when it is there already."""
    if have():
        return True
    archive = zip_path or os.environ.get("SSBU_GLSLANG_ZIP") or os.path.join(DIR, "downloads", ASSET)
    if not os.path.isfile(archive):
        if zip_path:
            raise SystemExit(f"{archive}: no such file")
        sys.path.insert(0, os.path.join(TOOLS, "linux"))
        import install_linux as il   # (its download(): shown as it comes, checked by SHA-256)
        try:
            il.download(URL, archive, f"glslang {VERSION} from GitHub", SHA256)
        except il.Stop as e:
            raise SystemExit(str(e))
    got = sha256(archive)
    if got != SHA256:
        raise SystemExit(f"{archive} is not glslang {VERSION}'s release (SHA-256 {got}, expected {SHA256})")
    tmp = DIR + ".partial"
    with zipfile.ZipFile(archive) as z:
        for m in z.infolist():
            parts = m.filename.replace("\\", "/").split("/")
            if m.is_dir() or parts[0] not in ("include", "lib") or ".." in parts:
                continue   # (the headers and the libraries; not glslang.exe)
            dest = os.path.join(tmp, *parts)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with z.open(m) as src, open(dest, "wb") as out:
                out.write(src.read())
    os.makedirs(DIR, exist_ok=True)
    for sub in ("include", "lib"):
        if os.path.isdir(os.path.join(DIR, sub)):
            import shutil
            shutil.rmtree(os.path.join(DIR, sub))
        os.replace(os.path.join(tmp, sub), os.path.join(DIR, sub))
    os.rmdir(tmp)
    log(f"  glslang {VERSION}: {os.path.relpath(DIR, ROOT)}")
    return True


def dll_current(dll):
    if not os.path.isfile(dll):
        return False
    t = os.path.getmtime(dll)
    return all(os.path.getmtime(p) < t for p in [SHIM, *HEADERS, *(os.path.join(LIBDIR, n) for n in LIBS)])


def build(dll, log=print):
    """ssbu_glslang.dll at `dll` (made again when the shim, the binding map or glslang changed)."""
    if dll_current(dll):
        return True
    import cc
    os.makedirs(os.path.dirname(os.path.abspath(dll)), exist_ok=True)
    obj_dir = os.path.join(os.path.dirname(os.path.abspath(dll)), "glslang_obj")
    os.makedirs(obj_dir, exist_ok=True)
    flags = ["/nologo", "/MD", "/O2", "/LD", "-w", *cc.LINK_FLAGS,   # (LINK_FLAGS: lld, and deps\msvc's root)
             "/I", INCLUDE, "/Fo" + obj_dir + os.sep]
    new = dll[:-4] + ".new.dll"   # (made beside it, then put in its place: a failed link leaves the old one)
    cmd = [cc.CLANG, *flags, SHIM, "/Fe" + new, "/link", *cc.LINK_OPTIONS, "/LIBPATH:" + LIBDIR, *LIBS]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if not r.returncode and os.path.isfile(new):
        try:
            os.replace(new, dll)
        except OSError:   # (a running game holds it open: renamed aside, which Windows allows, then replaced)
            old = dll + ".old"
            try:
                if os.path.isfile(old):
                    os.remove(old)
                os.replace(dll, old)
                os.replace(new, dll)
            except OSError as e:
                log(f"  {DLL_NAME} could not be replaced ({e}): the old one stays")
                return False
    if r.returncode or not os.path.isfile(dll):
        text = (r.stdout + r.stderr)[-2000:]
        hint = ""
        if any(n in text for n in DLL_CRT):
            hint = ("\n(the DLL C runtime's libraries are missing: deps\\msvc from before they were kept. "
                    "Run tools\\msvc_deps.py again, or build with Visual Studio's Build Tools.)")
        log(f"  ssbu_glslang.dll could not be built: Vulkan will have no shader translator.\n{text}{hint}")
        return False
    for leftover in (new[:-4] + ".lib", new[:-4] + ".exp", dll + ".old"):
        try:
            if os.path.isfile(leftover):
                os.remove(leftover)
        except OSError:
            pass
    log(f"  {DLL_NAME}: {os.path.relpath(dll, ROOT) if dll.startswith(ROOT) else dll}")
    return True


def ensure(dll, zip_path=None, log=print):
    """fetch() and build(), for tools/build_game.py: False (said) when either fails."""
    try:
        fetch(zip_path, log)
        return build(dll, log)
    except (SystemExit, Exception) as e:   # (never fatal to the game's build: Direct3D 11 draws without it)
        log(f"  glslang could not be set up ({e}): Vulkan will have no shader translator")
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--zip", help="a copy of " + ASSET + " already on this PC")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", DLL_NAME), help="the DLL to make")
    ap.add_argument("--status", action="store_true")
    a = ap.parse_args()
    if a.status:
        print(f"glslang {VERSION} in {DIR}: {'there' if have() else 'missing'}")
        print(f"{a.out}: {'current' if have() and dll_current(a.out) else 'missing or stale'}")
        return 0
    fetch(a.zip)
    return 0 if build(a.out) else 1


if __name__ == "__main__":
    sys.exit(main())
