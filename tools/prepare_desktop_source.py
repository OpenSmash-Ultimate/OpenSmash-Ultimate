#!/usr/bin/env python3
"""Create the tracked source archive and checksum manifest for a desktop release."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def prepare(root, output, repository, tag, revision="HEAD"):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("repository must be owner/name")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", tag):
        raise ValueError("release tag must contain only letters, digits, dots, dashes or underscores")
    if not re.fullmatch(r"HEAD|[0-9a-fA-F]{40,64}", revision):
        raise ValueError("revision must be HEAD or a full commit ID")
    commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "--verify", revision + "^{commit}"], text=True).strip()
    output = pathlib.Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    archive = output / "OpenSmash-Source.zip"
    if archive.exists():
        raise FileExistsError(f"Source archive already exists: {archive}")
    # git archive includes only the committed source, never local dumps/builds/keys.
    subprocess.run(["git", "-C", str(root), "archive", "--format=zip", "--output", str(archive), commit], check=True)
    with zipfile.ZipFile(archive) as zip:
        names = set(zip.namelist())
        for required in ("tools/os_mods.py", "tools/recomp.py", "tools/install_windowsmash.ps1", "tools/linux/install.sh", "tools/mac/install.sh"):
            if required not in names:
                raise ValueError(f"Release source is missing {required}")
        if not any(name.startswith("runtime/") for name in names):
            raise ValueError("Release source is missing runtime/")
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    manifest = {"Repository": repository, "Revision": commit,
                "ArchiveUrl": f"https://github.com/{repository}/releases/download/{tag}/{archive.name}",
                "Sha256": digest}
    (output / "bootstrap-source.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (output / "OpenSmash-Source.zip.sha256").write_text(f"{digest}  {archive.name}\n", encoding="ascii")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--revision", default="HEAD")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "dist" / "launcher-release-source")
    args = parser.parse_args()
    manifest = prepare(ROOT, args.output, args.repository, args.tag, args.revision)
    print(f"Release source: {manifest['Revision']}")
    print(f"SHA-256: {manifest['Sha256']}")


if __name__ == "__main__":
    main()
