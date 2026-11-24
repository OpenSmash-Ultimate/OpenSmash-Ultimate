#!/usr/bin/env python3
"""The release's RecompLauncher package: OpenSmash-<tag>-Windows-x64.zip.

RecompLauncher (https://recomplauncher.com, issue #543) installs a release from a zip that has recomplauncher.json at
its top, next to the program. This zip is the project's source at the release's tag (git archive), the release's
OpenSmash.exe and recomplauncher.json, all at the top (no folder around them). recomplauncher.json gets the tag as
prepare's OPENSMASH_RELEASE_TAG, so the setup step builds the game the way a release install does (the game as its own
library, the release's prebuilt runtime: tools/install_opensmash.ps1).

    python tools/make_recomplauncher_zip.py --tag v0.5.0 --exe dist/OpenSmash.exe [--out dist]
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", required=True, help="the release tag (vX.Y.Z), also the source's git revision")
    ap.add_argument("--exe", required=True, help="the release's OpenSmash.exe")
    ap.add_argument("--out", default=str(ROOT / "dist"), help="the folder the zip goes into (default dist)")
    a = ap.parse_args(argv)
    exe = pathlib.Path(a.exe)
    if not exe.is_file():
        ap.error(f"no such file: {exe}")
    source = subprocess.run(["git", "-C", str(ROOT), "archive", "--format=zip", a.tag], capture_output=True, check=True).stdout
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    target = out / f"OpenSmash-{a.tag}-Windows-x64.zip"
    with zipfile.ZipFile(io.BytesIO(source)) as src, zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as dst:
        for info in src.infolist():
            if info.filename in ("recomplauncher.json", "OpenSmash.exe"):
                continue
            dst.writestr(info, src.read(info))
        dst.writestr("recomplauncher.json", manifest(a.tag))
        dst.write(exe, "OpenSmash.exe")
    print(target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
