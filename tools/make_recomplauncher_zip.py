#!/usr/bin/env python3
"""The release's RecompLauncher package: OpenSmash-<tag>-Windows-x64.zip.

RecompLauncher (https://recomplauncher.com, issue #543) installs a release from a zip that has recomplauncher.json at
its top, next to the program. This zip is the project's source at the release's tag (git archive), the release's
OpenSmash.exe and recomplauncher.json, all at the top (no folder around them). recomplauncher.json gets the tag as
prepare's OPENSMASH_RELEASE_TAG, so the setup step builds the game the way a release install does (the game as its own
library, the release's prebuilt runtime: tools/install_opensmash.ps1).

    python tools/make_recomplauncher_zip.py --tag v0.5.0 --exe dist/OpenSmash.exe [--out dist] [--rev HEAD]

The release workflow (.github/workflows/release.yml) makes it for every stable release, beside OpenSmash.exe. A release
without it breaks RecompLauncher's installs: an update from a release that had it to one that has only OpenSmash.exe
takes away every file of the old zip the new release no longer has - the whole project, tools/install_opensmash.ps1
with it - and OpenSmash.exe then shows its first-start setup (v0.5.1, whose zip was missing at first).
"""
import argparse
import io
import json
import pathlib
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def manifest(tag):
    """recomplauncher.json with the release tag in prepare's environment."""
    data = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))
    data["prepare"].setdefault("env", {})["OPENSMASH_RELEASE_TAG"] = tag
    return (json.dumps(data, indent=2) + "\n").encode("utf-8")


def build(tag, exe, out, rev=None):
    """OpenSmash-<tag>-Windows-x64.zip in out: the source at rev (the tag when None) with exe and the tag's
    recomplauncher.json at the top. Its path."""
    exe = pathlib.Path(exe)
    source = subprocess.run(["git", "-C", str(ROOT), "archive", "--format=zip", rev or tag], capture_output=True, check=True).stdout
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    target = out / f"OpenSmash-{tag}-Windows-x64.zip"
    with zipfile.ZipFile(io.BytesIO(source)) as src, zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as dst:
        for info in src.infolist():
            if info.filename in ("recomplauncher.json", "OpenSmash.exe"):
                continue
            dst.writestr(info, src.read(info))
        dst.writestr("recomplauncher.json", manifest(tag))
        dst.write(exe, "OpenSmash.exe")
    return target


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", required=True, help="the release tag (vX.Y.Z), also the source's git revision")
    ap.add_argument("--exe", required=True, help="the release's OpenSmash.exe")
    ap.add_argument("--out", default=str(ROOT / "dist"), help="the folder the zip goes into (default dist)")
    ap.add_argument("--rev", help="the source's git revision when it is not the tag (the release workflow: HEAD, the tag's commit checked out)")
    a = ap.parse_args(argv)
    if not pathlib.Path(a.exe).is_file():
        ap.error(f"no such file: {a.exe}")
    print(build(a.tag, a.exe, a.out, a.rev))
    return 0


if __name__ == "__main__":
    sys.exit(main())
