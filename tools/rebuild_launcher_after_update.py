#!/usr/bin/env python3
"""Rebuild and reopen the shared Unix launcher after its current process exits."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform not in ("linux", "darwin"):
        parser.error("Unix helper only")
    root = args.root.resolve()
    marker = root / "config/update-launcher-build.pending"
    log = root / "build/logs/launcher_update.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + 180
    while True:
        try:
            os.kill(args.pid, 0)
        except ProcessLookupError:
            break
        if time.monotonic() > deadline:
            raise RuntimeError("Previous launcher did not close; rebuild remains pending")
        time.sleep(0.25)
    rid = "osx-arm64" if sys.platform == "darwin" else "linux-x64"
    command = ["open", "-n", str(root / "OpenSmash.app")] if sys.platform == "darwin" else [str(root / "OpenSmash")]
    with log.open("a", encoding="utf-8") as output:
        try:
            subprocess.run([sys.executable, str(root / "tools/build_launcher.py"), "--rid", rid], cwd=root,
                           stdout=output, stderr=subprocess.STDOUT, check=True)
            subprocess.Popen(command, cwd=root, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            marker.unlink(missing_ok=True)
        except (OSError, subprocess.CalledProcessError) as error:
            print(f"Launcher rebuild remains pending: {error}", file=output, flush=True)
            # The builder preserves the previous executable on failure. Reopen it
            # with the pending marker intact so the user can retry or postpone.
            subprocess.Popen(command, cwd=root, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
