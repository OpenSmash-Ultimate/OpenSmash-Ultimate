#!/usr/bin/env python3
"""Publish the shared OpenSmash launcher.

Requires the .NET 8 SDK. Example: python tools/build_desktop_launcher.py --rid win-x64
Output: dist/launcher/<rid> (self-contained; users do not need .NET).
"""
import argparse
import json
import pathlib
import plistlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import re
import platform

ROOT = pathlib.Path(__file__).resolve().parent.parent
RIDS = ("win-x64", "linux-x64", "osx-arm64")


def copy_notices(output):
    """Keep dependency notices from the exact NuGet packages that were restored."""
    assets = json.loads((ROOT / "OpenSmash.Launcher" / "obj" / "project.assets.json").read_text(encoding="utf-8"))
    destination = output / "ThirdPartyNotices"
    destination.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "OpenSmash.Launcher" / "Avalonia-LICENSE.md", destination)
    shutil.copy2(ROOT / "OpenSmash.Launcher" / "Inter-LICENSE.txt", destination)
    shutil.copy2(ROOT / "OpenSmash.Launcher" / "Assets" / "CascadiaMono-LICENSE.txt", destination)
    shutil.copy2(ROOT / "OpenSmash.Launcher" / "Assets" / "README.md", destination / "Font-Versions.md")
    packages = []
    for identity, metadata in assets["libraries"].items():
        if metadata.get("type") != "package":
            continue
        packages.append(identity)
        for cache in assets["packageFolders"]:
            package = pathlib.Path(cache) / metadata["path"]
            if not package.exists():
                continue
            for name in metadata.get("files", []):
                filename = pathlib.PurePosixPath(name).name.lower()
                if filename.endswith(".nuspec") or any(word in filename for word in ("license", "licence", "notice", "copying")):
                    source = package / name
                    if source.is_file():
                        target = destination / identity.replace("/", "-") / name
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(source, target)
            break
    (destination / "packages.txt").write_text("\n".join(sorted(packages)) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rid", required=True, choices=RIDS)
    parser.add_argument("--configuration", choices=("Release", "Debug"), default="Release")
    parser.add_argument("--output-root", type=pathlib.Path, default=ROOT / "dist" / "launcher")
    parser.add_argument("--layout", choices=("single-file", "folder"), default="single-file",
                        help="single-file bundles/compresses the runtime and native libraries (default)")
    parser.add_argument("--source-manifest", type=pathlib.Path,
                        help="embed a checksum-pinned release source manifest prepared by prepare_desktop_source.py")
    args = parser.parse_args()
    rid = args.rid
    source_properties = []
    if args.source_manifest:
        manifest_path = args.source_manifest.resolve()
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        repository = manifest.get("Repository", "")
        if (not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository)
                or not re.fullmatch(r"[a-fA-F0-9]{40,64}", manifest.get("Revision", ""))
                or not re.fullmatch(r"[a-fA-F0-9]{64}", manifest.get("Sha256", ""))
                or not manifest.get("ArchiveUrl", "").startswith(f"https://github.com/{repository}/releases/download/")):
            raise ValueError("Invalid release source manifest")
        source_properties.append(f"-p:BootstrapSourceManifest={manifest_path}")
    output = args.output_root.resolve() / rid
    target = output
    if rid.startswith("osx"):
        target = output / "OpenSmash.app" / "Contents" / "MacOS"
    # Publish into a fresh staging directory: an earlier folder build must never
    # leave loose runtime DLLs beside what is advertised as a single-file build.
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() and any(output.iterdir()):
        raise OSError(f"Output is not empty: {output}. Choose a fresh --output-root; existing files are not deleted.")
    with tempfile.TemporaryDirectory(prefix="opensmash-publish-") as temporary:
        staging = pathlib.Path(temporary)
        subprocess.run(["dotnet", "restore", str(ROOT / "OpenSmash.Launcher"), "-r", rid], check=True)
        notices = staging / "notices"
        notices.mkdir()
        copy_notices(notices)
        for source in (ROOT / "LICENSE", ROOT / "NOTICE.md", ROOT / "OpenSmash.Launcher" / "Assets" / "Anton-OFL.txt"):
            shutil.copy2(source, notices / source.name)
        shutil.copy2(ROOT / "docs" / "DESKTOP-LAUNCHER.md", notices / "README.md")
        notice_archive = shutil.make_archive(str(staging / "ThirdPartyNotices"), "zip", notices)
        single = args.layout == "single-file"
        published = staging / "app"
        subprocess.run([
            "dotnet", "publish", str(ROOT / "OpenSmash.Launcher"), "-c", args.configuration,
            "-r", rid, "--self-contained", "true", "--no-restore",
            f"-p:PublishSingleFile={str(single).lower()}",
            f"-p:IncludeNativeLibrariesForSelfExtract={str(single).lower()}",
            f"-p:EnableCompressionInSingleFile={str(single).lower()}",
            "-p:DebugType=embedded", "-p:DebugSymbols=false",
            f"-p:EmbeddedNoticesArchive={notice_archive}",
            *source_properties,
            "-o", str(published),
        ], check=True)
        executable = "OpenSmash.exe" if rid.startswith("win") else "OpenSmash"
        if single:
            # Native debug symbols are not runtime dependencies and need not ship.
            extras = [p for p in published.iterdir() if p.name != executable and p.suffix.lower() != ".pdb"]
            # macOS native packages deliberately keep their dylibs outside the
            # .NET binary. They stay inside the one Finder-visible .app bundle.
            unexpected = [p for p in extras if not (rid.startswith("osx") and p.is_file() and p.suffix == ".dylib")]
            if unexpected:
                raise OSError("Single-file publish left unexpected runtime files: " + ", ".join(p.name for p in unexpected))
            target.mkdir(parents=True, exist_ok=True)
            shutil.copy2(published / executable, target / executable)
            for library in extras:
                shutil.copy2(library, target / library.name)
        else:
            shutil.copytree(published, target, dirs_exist_ok=True)
        legal = target.parent / "Resources" if rid.startswith("osx") else output
        shutil.copytree(notices, legal, dirs_exist_ok=True)
    if rid.startswith("osx"):
        contents = target.parent
        with (contents / "Info.plist").open("wb") as stream:
            plistlib.dump({
                "CFBundleName": "OpenSmash", "CFBundleDisplayName": "OpenSmash",
                "CFBundleIdentifier": "org.opensmash.launcher",
                "CFBundleExecutable": "OpenSmash", "CFBundlePackageType": "APPL",
                "CFBundleShortVersionString": "0.1.0", "CFBundleVersion": "1",
                "LSMinimumSystemVersion": "14.0", "NSHighResolutionCapable": True,
            }, stream)
        # Cross-publishing on Windows cannot set the Unix executable bit; tar preserves it
        # when built on macOS/Linux. CI publishes each artifact on its target OS.
    # On native build runners, inspect the actual published host (including the
    # single-file bundle), not an intermediate DLL, before releasing its archive.
    native = ((rid == "win-x64" and sys.platform == "win32")
              or (rid == "linux-x64" and sys.platform.startswith("linux") and platform.machine().lower() in ("x86_64", "amd64"))
              or (rid == "osx-arm64" and sys.platform == "darwin" and platform.machine().lower() in ("arm64", "aarch64")))
    if native:
        binary = target / ("OpenSmash.exe" if rid.startswith("win") else "OpenSmash")
        actual = subprocess.run([str(binary), "--source-info"], check=True, capture_output=True, text=True)
        expected_path = args.source_manifest or ROOT / "OpenSmash.Launcher" / "BootstrapSource.json"
        expected = json.loads(pathlib.Path(expected_path).read_text(encoding="utf-8"))
        if json.loads(actual.stdout) != expected:
            raise ValueError("Published launcher does not contain the intended bootstrap source manifest")
        print("Verified source manifest in the published executable")
    if rid.startswith("win"):
        archive = shutil.make_archive(str(output), "zip", output)
    else:
        archive = str(output) + ".tar.gz"

        def permissions(entry):
            # Windows cross-publish cannot store Unix modes on disk. Restore them
            # in the archive so the extracted Linux executable/macOS app can start.
            entry.mode = 0o755 if entry.isdir() or pathlib.PurePosixPath(entry.name).name == "OpenSmash" else 0o644
            return entry

        with tarfile.open(archive, "w:gz") as bundle:
            bundle.add(output, arcname=".", filter=permissions)
    print(f"Published {rid}: {output}")
    print(f"Archive: {archive}")


if __name__ == "__main__":
    try:
        main()
    except (subprocess.CalledProcessError, OSError, ValueError) as error:
        print(f"Publish failed: {error}", file=sys.stderr)
        sys.exit(1)
