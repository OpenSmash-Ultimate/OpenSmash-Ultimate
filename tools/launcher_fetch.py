#!/usr/bin/env python3
"""The OpenSmash launcher for Linux and macOS (OpenSmash.Avalonia: the same launcher as Windows', with the account
sign-in the official lobbies need) put beside this folder's game:

    python3 tools/launcher_fetch.py [--release | --build] [--force] [--dry-run]

Linux: ./OpenSmash.Avalonia (one program). macOS: ./OpenSmash.Avalonia.app. opensmash-launcher.sh and
"OpenSmash Launcher.command" start it when it is there, else the web launcher (tools/weblauncher).

Where it comes from (default: the first that works):
  1. built here from this folder's sources (tools/build_avalonia.py) when the .NET SDK is installed - the launcher
     then matches this folder exactly; again only when its sources changed (build/cache/launcher_built.txt);
  2. the newest release's build for this computer (release.yml: OpenSmash-linux-x64.tar.gz,
     OpenSmash-macos-arm64.tar.gz, OpenSmash-macos-x64.tar.gz), named with its SHA-256 in the release's
     opensmash-release.json ("launcher"), checked before anything is unpacked; again only for a newer release
     (build/cache/launcher_release.txt). Only the program (or the .app) is taken from the archive.
Nothing here is needed to play: when neither works the web launcher stays. Only the standard library is used.
"""
import argparse
import hashlib
import io
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "OpenSmash-Ultimate/OpenSmash-Ultimate"
LATEST = "https://github.com/" + REPO + "/releases/latest/download/opensmash-release.json"
ASSET = "https://github.com/" + REPO + "/releases/download/v{version}/{name}"
PROGRAM = "OpenSmash.Avalonia"
APP = "OpenSmash.Avalonia.app"
# the manifest's launcher keys (tools/release/make_release.py LAUNCHERS) and the dotnet runtime ids
KEYS = {"linux-x64": "linux-x64", "macos-arm64": "osx-arm64", "macos-x64": "osx-x64"}
SOURCES = ("OpenSmash.Avalonia", "OpenSmash.Core", os.path.join("OpenSmash.Launcher", "SettingDefaults.cs"), "locales")


class Error(Exception):
    pass


def platform_key(system=None, machine=None):
    """this computer's launcher build: "linux-x64", "macos-arm64", "macos-x64"; None: none is made for it"""
    system = (system or platform.system()).lower()
    machine = (machine or platform.machine()).lower()
    if system == "linux" and machine in ("x86_64", "amd64"):
        return "linux-x64"
    if system == "darwin":
        return "macos-arm64" if machine in ("arm64", "aarch64") else "macos-x64" if machine in ("x86_64", "amd64") else None
    return None


def installed(root, key):
    """the launcher's path in root for key (it may not be there)"""
    return os.path.join(root, APP if key.startswith("macos") else PROGRAM)


def download(url):
    if not url.startswith("https://github.com/"):
        raise Error(f"not a GitHub address: {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "OpenSmash-launcher-fetch"})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.read()
    except OSError as e:
        raise Error(f"{url}: {e}") from None


def pick(manifest, key):
    """(version, file name, sha256) of the release's launcher for key; Error when the release has none"""
    if not isinstance(manifest, dict) or manifest.get("schema") != 1:
        raise Error("the release's opensmash-release.json is not one this tool reads")
    entry = (manifest.get("launcher") or {}).get(key)
    version = manifest.get("version")
    if not isinstance(entry, dict) or not isinstance(version, str):
        raise Error(f"the newest release has no launcher for {key}")
    name, sha = entry.get("file"), entry.get("sha256")
    if not isinstance(name, str) or "/" in name or "\\" in name or not isinstance(sha, str) or len(sha) != 64:
        raise Error("the release's launcher entry is not valid")
    return version, name, sha.lower()


def _safe_members(tar, prefix):
    """the archive's regular files and folders under prefix ("./OpenSmash.Avalonia", "./OpenSmash.Avalonia.app/");
    anything else (a link, a device, an absolute path, "..") stops it"""
    out = []
    for m in tar.getmembers():
        name = m.name[2:] if m.name.startswith("./") else m.name
        if name.startswith("/") or ".." in name.replace("\\", "/").split("/"):
            raise Error(f"an unsafe path in the archive: {m.name}")
        if not (name == prefix or name.startswith(prefix + "/")):
            continue
        if not (m.isfile() or m.isdir()):
            raise Error(f"not a plain file in the archive: {m.name}")
        out.append((name, m))
    if not out:
        raise Error(f"the archive has no {prefix}")
    return out


def unpack(data, key, root):
    """the program (Linux) or the .app (macOS) from the archive's bytes into root, replacing the one there in one
    step (a staging copy renamed); the path put in place"""
    target = installed(root, key)
    prefix = os.path.basename(target)
    stage = os.path.join(root, ".OpenSmash-fetch-" + uuid.uuid4().hex)
    try:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
            members = _safe_members(tar, prefix)
            os.makedirs(stage)
            for name, m in members:
                dest = os.path.join(stage, *name.split("/"))
                if m.isdir():
                    os.makedirs(dest, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with tar.extractfile(m) as src, open(dest, "wb") as out:
                    shutil.copyfileobj(src, out)
                os.chmod(dest, 0o755 if m.mode & 0o111 else 0o644)
        built = os.path.join(stage, prefix)
        if not os.path.exists(built):
            raise Error(f"the archive has no {prefix}")
        if os.path.isdir(target) and not os.path.islink(target):
            old = os.path.join(root, ".OpenSmash-old-" + uuid.uuid4().hex)
            os.rename(target, old)
            try:
                os.rename(built, target)
            except OSError:
                os.rename(old, target)
                raise
            shutil.rmtree(old, ignore_errors=True)
        else:
            os.replace(built, target)   # (a running launcher keeps its old file open: replaced, not overwritten)
        return target
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def stamp(root, name):
    return os.path.join(root, "build", "cache", name)


def read_stamp(root, name):
    try:
        with open(stamp(root, name), encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def write_stamp(root, name, text):
    os.makedirs(os.path.dirname(stamp(root, name)), exist_ok=True)
    with open(stamp(root, name), "w", encoding="utf-8", newline="\n") as f:
        f.write(text + "\n")


def fetch_release(root, key, get=download, force=False, log=print, dry_run=False):
    """the newest release's launcher for key into root; the path, or None when it was already there"""
    manifest = json.loads(get(LATEST).decode("utf-8"))
    version, name, sha = pick(manifest, key)
    if not force and read_stamp(root, "launcher_release.txt") == f"{version} {sha}" and os.path.exists(installed(root, key)):
        log(f"the OpenSmash launcher {version} is already here")
        return None
    if dry_run:
        log(f"would download the OpenSmash launcher {version} ({name})")
        return None
    data = get(ASSET.format(version=version, name=name))
    if hashlib.sha256(data).hexdigest() != sha:
        raise Error(f"{name} does not match the SHA-256 its release names: not used")
    path = unpack(data, key, root)
    write_stamp(root, "launcher_release.txt", f"{version} {sha}")
    log(f"the OpenSmash launcher {version}: {path}")
    return path


def sources_hash(root):
    """what the launcher is built from, as one hash (a change means: build it again)"""
    h = hashlib.sha256()
    for top in SOURCES:
        path = os.path.join(root, top)
        files = [path] if os.path.isfile(path) else sorted(
            os.path.join(d, f) for d, dirs, fs in os.walk(path) for f in fs
            if not ({"bin", "obj"} & set(os.path.relpath(d, path).split(os.sep))))
        for f in files:
            h.update(os.path.relpath(f, root).replace(os.sep, "/").encode() + b"\0")
            with open(f, "rb") as fh:
                h.update(hashlib.sha256(fh.read()).digest())
    return h.hexdigest()


def can_build(root):
    if not os.path.isfile(os.path.join(root, "OpenSmash.Avalonia", "OpenSmash.csproj")) or not shutil.which("dotnet"):
        return False
    try:
        r = subprocess.run(["dotnet", "--list-sdks"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return False
    return r.returncode == 0 and any(line[:1].isdigit() for line in r.stdout.splitlines())


def build(root, key, force=False, log=print, dry_run=False, run=subprocess.run):
    """the launcher built from root's sources; the path, or None when it was already built from them"""
    want = sources_hash(root)
    if not force and read_stamp(root, "launcher_built.txt") == want and os.path.exists(installed(root, key)):
        log("the OpenSmash launcher is already built from these sources")
        return None
    if dry_run:
        log("would build the OpenSmash launcher from this folder's sources (.NET SDK)")
        return None
    log("building the OpenSmash launcher from this folder's sources (.NET SDK; a few minutes the first time)")
    r = run([sys.executable, os.path.join(root, "tools", "build_avalonia.py"), "--rid", KEYS[key]])
    if r.returncode != 0:
        raise Error("the launcher's build did not finish")
    write_stamp(root, "launcher_built.txt", want)
    return installed(root, key)


def ensure(root=ROOT, how="auto", force=False, log=print, dry_run=False, get=download):
    """the launcher beside the game, by how ("auto", "build", "release"); True when one is there after it"""
    key = platform_key()
    if key is None:
        log(f"no OpenSmash launcher is made for this computer ({platform.system()} {platform.machine()}): the web launcher stays")
        return False
    if dry_run:   # (nothing asked of the network, nothing built)
        log("would put the OpenSmash launcher here: " + ("built with the .NET SDK" if how != "release" and can_build(root)
                                                          else "the newest release's") + f" ({key})")
        return os.path.exists(installed(root, key))
    errors = []
    if how in ("auto", "build") and can_build(root):
        try:
            build(root, key, force, log, dry_run)
            return True
        except Error as e:
            errors.append(str(e))
    elif how == "build":
        errors.append("the .NET SDK is not installed")
    if how in ("auto", "release"):
        try:
            fetch_release(root, key, get, force, log, dry_run)
            return True
        except (Error, ValueError) as e:
            errors.append(str(e))
    for e in errors:
        log(f"the OpenSmash launcher: {e}")
    there = os.path.exists(installed(root, key))
    log("the one already here stays" if there else "the web launcher stays (tools/weblauncher): sign in there is not possible")
    return there


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--build", dest="how", action="store_const", const="build", help="only build it here (.NET SDK)")
    g.add_argument("--release", dest="how", action="store_const", const="release", help="only download the newest release's")
    ap.add_argument("--force", action="store_true", help="again, even when it is already up to date")
    ap.add_argument("--dry-run", action="store_true", help="say what would be done")
    ap.add_argument("--root", default=ROOT, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    return 0 if ensure(a.root, a.how or "auto", a.force, dry_run=a.dry_run) else 1


if __name__ == "__main__":
    sys.exit(main())
