#!/usr/bin/env python3
"""Epic Online Services beside the program (runtime/net_eos.c): EOSSDK-Win64-Shipping.dll and eos.txt, from
OpenSmash-Ultimate's release EOS_TAG, checked against its SHA256SUMS. Neither is in the repository: the DLL is
Epic's (its SDK license lets a product ship it), eos.txt the product's client credentials, written into the
release by .github/workflows/eos-release.yml from the repository's secrets. Without them the game runs as
before (the lobby's relay is the last resort), so a failure here is never fatal.

    python tools/eos_fetch.py [build folder]

tools/build_game.py runs it after a Windows build. Only the standard library is used.
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runtime_fetch   # noqa: E402  (download: GitHub only, Windows' own HTTPS when Python's is refused)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EOS_TAG = "eos-1"
FILES = ("EOSSDK-Win64-Shipping.dll", "eos.txt")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def current(build, want):
    """True: every file is there with the release's SHA-256 (nothing to fetch)."""
    for name in FILES:
        path = os.path.join(build, name)
        if not os.path.isfile(path):
            return False
        with open(path, "rb") as f:
            if sha256(f.read()) != want.get(name):
                return False
    return True


def ensure(build=os.path.join(ROOT, "build"), get=runtime_fetch.download, log=print):
    """Puts both files into build; True when they are there. Never raises."""
    if os.environ.get("SSBU_EOS") == "0":
        return False
    url = runtime_fetch.URL
    try:
        want = runtime_fetch.sums(get(url.format(tag=EOS_TAG, name="SHA256SUMS")).decode("utf-8", "replace"))
        if all(n in want for n in FILES) and current(build, want):
            return True
        for name in FILES:
            if name not in want:
                raise runtime_fetch.Error(f"the release {EOS_TAG} has no {name}")
            data = get(url.format(tag=EOS_TAG, name=name))
            if sha256(data) != want[name]:
                raise runtime_fetch.Error(f"{name} does not match the release's SHA-256")
            tmp = os.path.join(build, name + ".part")
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, os.path.join(build, name))
        log(f"  Epic Online Services ({EOS_TAG}): online play without a direct path goes through Epic's relays")
        return True
    except (runtime_fetch.Error, OSError, ValueError) as e:
        log(f"  (Epic Online Services not fetched: {e}; online play uses the lobby's relay where no direct path forms)")
        return False


def main(argv):
    build = argv[1] if len(argv) > 1 else os.path.join(ROOT, "build")
    return 0 if ensure(build) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
