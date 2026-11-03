#!/usr/bin/env python3
"""Put a prebuilt runtime (.github/workflows/runtime-release.yml) into this build: a runtime fix without
compiling the game again.

    python tools/runtime_fetch.py runtime-v<n>

Only for a build made with tools/build_game.py --game-dll (Windows): the game is build\\ssbu_game.dll and
build\\ssbu.exe is the runtime alone, which this replaces. The release named is downloaded from
OpenSmash-Ultimate's releases (no "latest": the tag says which), for the instruction set the build is
compiled for (build\\isa.txt), and checked against the release's SHA256SUMS. The old ssbu.exe is kept as
ssbu.exe.old; if the new one does not accept this build's game library (ssbu --game-library), the old
one is put back. Only the standard library is used.
"""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "OpenSmash-Ultimate/OpenSmash-Ultimate"
URL = "https://github.com/" + REPO + "/releases/download/{tag}/{name}"
FILES = ("ssbu.exe", "SDL3.dll", "isa.txt", "commit.txt")


class Error(Exception):
    pass


def download(url):
    if not url.startswith("https://github.com/"):
        raise Error(f"not a GitHub address: {url}")
    try:
        with urllib.request.urlopen(url, timeout=60) as r:
            return r.read()
    except OSError as e:   # (Python refused the certificate: through curl.exe, Windows' own HTTPS - msvc_deps.cert_refused)
        import msvc_deps
        if not msvc_deps.cert_refused(e):
            raise
        try:
            return msvc_deps.curl_get(url)
        except msvc_deps.Failed as f:
            raise Error(str(f))


def isa_of(build):
    try:
        with open(os.path.join(build, "isa.txt"), encoding="utf-8-sig") as f:
            first = f.readline().strip()
    except OSError:
        first = ""
    if first not in ("avx2", "x86-64-v2"):
        raise Error(f"{build}\\isa.txt does not say avx2 or x86-64-v2: build the game first")
    return first


def sums(text):
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2 and len(parts[0]) == 64:
            out[parts[1].lstrip("*")] = parts[0].lower()
    return out


def check(exe):
    """The new program loads this build's game library (0: it can be used)."""
    r = subprocess.run([exe, "--game-library"], capture_output=True, text=True, timeout=120)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def fetch(tag, build=os.path.join(ROOT, "build"), get=download, run_check=check, log=print):
    if not tag.startswith("runtime-v"):
        raise Error(f"{tag}: a release tag is runtime-v<n>")
    if os.name != "nt":
        raise Error("the prebuilt runtime is for Windows")
    if not os.path.isfile(os.path.join(build, "ssbu_game.dll")):
        raise Error("this build has no ssbu_game.dll: the prebuilt runtime needs a build made with tools/build_game.py --game-dll")
    name = f"opensmash-runtime-windows-{isa_of(build)}.zip"
    want = sums(get(URL.format(tag=tag, name="SHA256SUMS")).decode("utf-8", "replace")).get(name)
    if not want:
        raise Error(f"{tag}'s SHA256SUMS does not list {name}")
    data = get(URL.format(tag=tag, name=name))
    if hashlib.sha256(data).hexdigest() != want:
        raise Error(f"{name} does not match its SHA-256 in {tag}'s SHA256SUMS: nothing changed")
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        names = set(z.namelist())
        if "ssbu.exe" not in names:
            raise Error(f"{name} holds no ssbu.exe")
        exe = os.path.join(build, "ssbu.exe")
        old = exe + ".old"
        if os.path.exists(exe):
            shutil.copy2(exe, old)
        for f in FILES:
            if f in names:
                with z.open(f) as src, open(os.path.join(build, f), "wb") as dst:
                    shutil.copyfileobj(src, dst)
    ok, said = run_check(exe)
    if not ok:
        if os.path.exists(old):
            shutil.copy2(old, exe)
        raise Error(f"the {tag} runtime does not accept this build's game library, so the old one is back:\n{said}")
    log(f"{tag}: the runtime is in place ({exe}; the old one is {os.path.basename(old)})")
    return exe


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    try:
        fetch(argv[1])
    except (Error, OSError) as e:
        print(e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
