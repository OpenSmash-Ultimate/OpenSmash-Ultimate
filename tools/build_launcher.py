#!/usr/bin/env python3
"""Build the shared launcher and place OpenSmash in the project root."""
import argparse
import pathlib
import shutil
import subprocess
import sys
import uuid

ROOT = pathlib.Path(__file__).resolve().parent.parent


def install(folder, rid):
    if rid.startswith("osx"):
        target = ROOT / "OpenSmash.app"
        staging = ROOT / (".OpenSmash-build-" + uuid.uuid4().hex + ".app")
        backup = ROOT / (".OpenSmash-backup-" + uuid.uuid4().hex + ".app")
        assert staging.resolve().parent == ROOT and backup.resolve().parent == ROOT and target.resolve().parent == ROOT
        installed = False
        try:
            shutil.copytree(folder / "OpenSmash.app", staging)
            if target.exists():
                target.rename(backup)
            staging.rename(target)
            installed = True
        except OSError:
            if backup.exists() and not target.exists():
                backup.rename(target)
            raise
        finally:
            for owned in (staging, backup) if installed else (staging,):
                if owned.exists():
                    assert owned.resolve().parent == ROOT and owned.name.startswith((".OpenSmash-build-", ".OpenSmash-backup-"))
                    shutil.rmtree(owned)
        return target
    name = "OpenSmash.exe" if rid.startswith("win") else "OpenSmash"
    target = ROOT / name
    staging = ROOT / (".OpenSmash-build-" + uuid.uuid4().hex + (".exe" if rid.startswith("win") else ".tmp"))
    try:
        shutil.copy2(folder / name, staging)
        staging.replace(target)
    finally:
        if staging.exists():
            staging.unlink()
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rid", required=True, choices=("win-x64", "linux-x64", "osx-arm64"))
    parser.add_argument("--configuration", default="Release", choices=("Release", "Debug"))
    args = parser.parse_args()
    if not shutil.which("dotnet"):
        parser.error("Install the .NET 8 SDK or newer, then run this build file again.")
    output = ROOT / "dist" / "launcher" / ("build-" + uuid.uuid4().hex[:12])
    subprocess.run([sys.executable, str(ROOT / "tools/build_desktop_launcher.py"), "--rid", args.rid,
                    "--configuration", args.configuration, "--output-root", str(output)], check=True)
    try:
        installed = install(output / args.rid, args.rid)
    except PermissionError as error:
        raise OSError("Close OpenSmash before replacing it. The finished package is in " + str(output)) from error
    print(f"Built {installed}")
    print(f"Package: {output / args.rid}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Launcher build failed: {error}", file=sys.stderr)
        sys.exit(1)
