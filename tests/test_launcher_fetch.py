"""tools/launcher_fetch.py: the OpenSmash launcher (OpenSmash.Avalonia) for Linux and macOS put beside the game, from
the newest release's build (named with its SHA-256 in opensmash-release.json) or built here. Made-up archives and a
made-up release only: no network, no .NET. Runs on Windows, Linux and macOS."""
import hashlib
import io
import json
import os
import sys
import tarfile
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import launcher_fetch as lf  # noqa: E402


def archive(entries):
    """a .tar.gz as tools/publish_avalonia.py makes it: entries (name, bytes or None for a folder, mode)"""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as t:
        for name, data, mode in entries:
            info = tarfile.TarInfo(name)
            info.mode = mode
            if data is None:
                info.type = tarfile.DIRTYPE
                t.addfile(info)
            else:
                info.size = len(data)
                t.addfile(info, io.BytesIO(data))
    return buf.getvalue()


LINUX = archive([(".", None, 0o755), ("./OpenSmash.Avalonia", b"ELF launcher", 0o755), ("./LICENSE", b"gpl", 0o644),
                 ("./NOTICE.md", b"notices", 0o644)])
MAC = archive([(".", None, 0o755), ("./OpenSmash.Avalonia.app", None, 0o755), ("./OpenSmash.Avalonia.app/Contents", None, 0o755),
               ("./OpenSmash.Avalonia.app/Contents/Info.plist", b"plist", 0o644),
               ("./OpenSmash.Avalonia.app/Contents/MacOS", None, 0o755),
               ("./OpenSmash.Avalonia.app/Contents/MacOS/OpenSmash.Avalonia", b"Mach-O", 0o755),
               ("./OpenSmash.Avalonia.app/Contents/MacOS/libSkiaSharp.dylib", b"dylib", 0o644)])


class LauncherFetch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        for name in ("LICENSE", "NOTICE.md"):
            with open(os.path.join(self.root, name), "w") as f:
                f.write("the repository's own")

    def tearDown(self):
        self.tmp.cleanup()

    def read(self, *parts):
        with open(os.path.join(self.root, *parts), "rb") as f:
            return f.read()

    def test_platform_keys(self):
        self.assertEqual(lf.platform_key("Linux", "x86_64"), "linux-x64")
        self.assertEqual(lf.platform_key("Darwin", "arm64"), "macos-arm64")
        self.assertEqual(lf.platform_key("Darwin", "x86_64"), "macos-x64")
        self.assertIsNone(lf.platform_key("Linux", "aarch64"))   # (no build for it: the web launcher)
        self.assertIsNone(lf.platform_key("Windows", "AMD64"))   # (Windows has OpenSmash.exe)
        self.assertEqual(set(lf.KEYS), {"linux-x64", "macos-arm64", "macos-x64"})

    def test_linux_program_only(self):
        path = lf.unpack(LINUX, "linux-x64", self.root)
        self.assertEqual(path, os.path.join(self.root, "OpenSmash-Launcher"))
        self.assertEqual(self.read("OpenSmash-Launcher"), b"ELF launcher")
        self.assertEqual(self.read("LICENSE"), b"the repository's own")   # (nothing else of the archive)
        if os.name != "nt":
            self.assertEqual(os.stat(path).st_mode & 0o777, 0o755)
        lf.unpack(archive([("./OpenSmash.Avalonia", b"newer", 0o755)]), "linux-x64", self.root)
        self.assertEqual(self.read("OpenSmash-Launcher"), b"newer")
        self.assertEqual([n for n in os.listdir(self.root) if n.startswith(".OpenSmash")], [])

    def test_linux_source_folder_kept(self):
        src = os.path.join(self.root, "OpenSmash.Avalonia")
        os.makedirs(src)
        with open(os.path.join(src, "OpenSmash.csproj"), "w") as f:
            f.write("<Project/>")
        lf.unpack(LINUX, "linux-x64", self.root)
        self.assertEqual(self.read("OpenSmash.Avalonia", "OpenSmash.csproj"), b"<Project/>")
        self.assertEqual(self.read("OpenSmash-Launcher"), b"ELF launcher")
        os.remove(os.path.join(self.root, "OpenSmash-Launcher"))
        os.makedirs(os.path.join(self.root, "OpenSmash-Launcher", "keep"))
        with self.assertRaises(lf.Error):
            lf.unpack(LINUX, "linux-x64", self.root)
        self.assertTrue(os.path.isdir(os.path.join(self.root, "OpenSmash-Launcher", "keep")))

    def test_mac_app_replaced_whole(self):
        old = os.path.join(self.root, "OpenSmash.Avalonia.app", "Contents", "stale.txt")
        os.makedirs(os.path.dirname(old))
        with open(old, "w") as f:
            f.write("old")
        path = lf.unpack(MAC, "macos-arm64", self.root)
        self.assertEqual(path, os.path.join(self.root, "OpenSmash.Avalonia.app"))
        self.assertEqual(self.read("OpenSmash.Avalonia.app", "Contents", "MacOS", "OpenSmash.Avalonia"), b"Mach-O")
        self.assertFalse(os.path.exists(old))
        if os.name != "nt":
            self.assertEqual(os.stat(os.path.join(path, "Contents", "MacOS", "OpenSmash.Avalonia")).st_mode & 0o777, 0o755)
            self.assertEqual(os.stat(os.path.join(path, "Contents", "Info.plist")).st_mode & 0o777, 0o644)
        self.assertEqual([n for n in os.listdir(self.root) if n.startswith(".OpenSmash")], [])

    def test_unsafe_archives_refused(self):
        for bad in (archive([("./OpenSmash.Avalonia/../../evil", b"x", 0o644)]), archive([("/OpenSmash.Avalonia", b"x", 0o755)])):
            with self.assertRaises(lf.Error):
                lf.unpack(bad, "linux-x64", self.root)
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as t:
            link = tarfile.TarInfo("./OpenSmash.Avalonia")
            link.type, link.linkname = tarfile.SYMTYPE, "/bin/sh"
            t.addfile(link)
        with self.assertRaises(lf.Error):
            lf.unpack(buf.getvalue(), "linux-x64", self.root)
        with self.assertRaises(lf.Error):
            lf.unpack(archive([("./README.md", b"x", 0o644)]), "linux-x64", self.root)   # (no program in it)
        self.assertFalse(os.path.exists(os.path.join(self.root, "OpenSmash-Launcher")))

    def release(self, data, sha=None, key="linux-x64", name="OpenSmash-linux-x64.tar.gz"):
        manifest = {"schema": 1, "version": "0.5.0", "commit": "0" * 40, "channel": "stable",
                    "launcher": {"windows-x64": {"file": "OpenSmash.exe", "sha256": "1" * 64},
                                 key: {"file": name, "sha256": sha or hashlib.sha256(data).hexdigest()}}}
        asked = []

        def get(url):
            asked.append(url)
            if url == lf.LATEST:
                return json.dumps(manifest).encode()
            self.assertEqual(url, f"https://github.com/{lf.REPO}/releases/download/v0.5.0/{name}")
            return data
        return get, asked

    def test_release_checked_and_kept(self):
        get, asked = self.release(LINUX)
        logs = []
        path = lf.fetch_release(self.root, "linux-x64", get, log=logs.append)
        self.assertEqual(self.read("OpenSmash-Launcher"), b"ELF launcher")
        self.assertEqual(path, os.path.join(self.root, "OpenSmash-Launcher"))
        self.assertEqual(len(asked), 2)
        self.assertIsNone(lf.fetch_release(self.root, "linux-x64", get, log=logs.append))   # (already this release)
        self.assertEqual(len(asked), 3)   # (only the manifest asked again)
        self.assertIsNotNone(lf.fetch_release(self.root, "linux-x64", get, force=True, log=logs.append))

    def test_release_sha_mismatch_refused(self):
        get, _ = self.release(LINUX, sha="f" * 64)
        with self.assertRaises(lf.Error):
            lf.fetch_release(self.root, "linux-x64", get, log=lambda s: None)
        self.assertFalse(os.path.exists(os.path.join(self.root, "OpenSmash-Launcher")))

    def test_release_without_this_platform(self):
        get, _ = self.release(MAC, key="macos-arm64", name="OpenSmash-macos-arm64.tar.gz")
        with self.assertRaises(lf.Error):
            lf.fetch_release(self.root, "linux-x64", get, log=lambda s: None)
        with self.assertRaises(lf.Error):
            lf.pick({"schema": 1, "version": "0.5.0", "launcher": {"linux-x64": {"file": "../x", "sha256": "0" * 64}}}, "linux-x64")

    def test_build_again_only_when_sources_change(self):
        src = os.path.join(self.root, "OpenSmash.Avalonia")
        os.makedirs(os.path.join(src, "obj"))
        with open(os.path.join(src, "OpenSmash.csproj"), "w") as f:
            f.write("<Project/>")
        with open(os.path.join(src, "obj", "noise"), "w") as f:
            f.write("1")
        runs = []

        class Done:
            returncode = 0

        def run(cmd):
            runs.append(cmd)
            return Done()
        before = lf.sources_hash(self.root)
        with open(os.path.join(src, "obj", "noise"), "w") as f:
            f.write("2")
        self.assertEqual(lf.sources_hash(self.root), before)   # (build output is not a source)
        lf.build(self.root, "macos-arm64", log=lambda s: None, run=run)
        self.assertEqual(runs[0][-2:], ["--rid", "osx-arm64"])
        os.makedirs(os.path.join(self.root, "OpenSmash.Avalonia.app"))
        self.assertIsNone(lf.build(self.root, "macos-arm64", log=lambda s: None, run=run))
        with open(os.path.join(src, "MainWindow.cs"), "w") as f:
            f.write("changed")
        lf.build(self.root, "macos-arm64", log=lambda s: None, run=run)
        self.assertEqual(len(runs), 2)

    def test_dry_run_asks_nothing(self):
        from unittest import mock

        def get(url):
            raise AssertionError("a dry run asked the network: " + url)
        logs = []
        with mock.patch.object(lf, "platform_key", return_value="linux-x64"), mock.patch.object(lf, "can_build", return_value=False):
            self.assertFalse(lf.ensure(self.root, dry_run=True, log=logs.append, get=get))
        self.assertIn("the newest release's", logs[0])
        self.assertFalse(os.path.exists(os.path.join(self.root, "OpenSmash-Launcher")))
        with mock.patch.object(lf, "platform_key", return_value=None):   # (no build for this computer: the web launcher)
            self.assertFalse(lf.ensure(self.root, log=logs.append, get=get))

    def test_launch_scripts_prefer_it(self):
        for path, needle in (("opensmash-launcher.sh", '"$launcher" --project "$root"'),
                             ("OpenSmash Launcher.command", 'open "$root/OpenSmash.Avalonia.app" --args --project "$root"')):
            with open(os.path.join(ROOT, path), encoding="utf-8") as f:
                text = f.read()
            self.assertIn(needle, text, path)
            if path == "opensmash-launcher.sh":   # (the new name first; an older install's program file as the fallback)
                self.assertIn('launcher="$root/OpenSmash-Launcher"', text)
                self.assertIn('[ ! -f "$launcher" ] && [ -f "$root/OpenSmash.Avalonia" ]', text)
            self.assertIn("--web", text, path)                       # (the web launcher stays, as the fallback)
            self.assertIn("tools/weblauncher/launcher.py", text, path)
        for path in (os.path.join("tools", "linux", "install_linux.py"), os.path.join("tools", "mac", "install_mac.py")):
            with open(os.path.join(ROOT, path), encoding="utf-8") as f:
                self.assertIn("launcher_fetch.ensure(ROOT", f.read(), path)


if __name__ == "__main__":
    unittest.main()
