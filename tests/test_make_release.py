#!/usr/bin/env python3
"""Tests for tools/release/make_release.py: the manifest the launcher reads, and the audit that keeps game data out
of a release.

    python -I tests/test_make_release.py [-v]"""
import json
import os
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "release"))
import make_release as M   # noqa: E402

COMMIT = "0123456789abcdef0123456789abcdef01234567"


class Release(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dist = self.tmp.name
        with open(os.path.join(self.dist, "OpenSmash.exe"), "wb") as f:
            f.write(b"MZ not a real launcher")
        self.zip("opensmash-runtime-windows-avx2.zip", {"ssbu.exe": b"MZ", "isa.txt": b"avx2"})

    def tearDown(self):
        self.tmp.cleanup()

    def zip(self, name, entries):
        with zipfile.ZipFile(os.path.join(self.dist, name), "w") as z:
            for n, data in entries.items():
                z.writestr(n, data)

    def test_versions(self):
        self.assertEqual(M.version("v0.5.0"), "0.5.0")
        self.assertEqual(M.version("v0.5.0-rc.1"), "0.5.0-rc.1")
        for bad in ("main", "v0.4", "v04.0.0", "0.5.0.1"):
            with self.assertRaises(SystemExit):
                M.version(bad)

    def test_manifest(self):
        m = M.manifest(self.dist, "v0.5.0", COMMIT, lobby="lobby.example.org:39010", min_level=1)
        with open(os.path.join(self.dist, M.MANIFEST)) as f:
            self.assertEqual(json.load(f), m)
        self.assertEqual(m["version"], "0.5.0")
        self.assertEqual(m["launcher"]["windows-x64"]["file"], "OpenSmash.exe")
        self.assertEqual(m["runtime"]["windows-avx2"]["sha256"], M.sha256(os.path.join(self.dist, "opensmash-runtime-windows-avx2.zip")))
        self.assertEqual((m["lobby"], m["min_client_level"]), ("lobby.example.org:39010", 1))
        for numbers in ("192.0.2.7:39421", "3232235777:39421", "localhost:39421"):   # (the official lobby is a DNS name)
            with self.assertRaises(SystemExit):
                M.manifest(self.dist, "v0.5.0", COMMIT, lobby=numbers)
        with open(os.path.join(self.dist, "SHA256SUMS")) as f:
            self.assertEqual(len(f.read().splitlines()), 2)

    def tar(self, name, entries, link=None):
        import io
        import tarfile
        with tarfile.open(os.path.join(self.dist, name), "w:gz") as t:
            for n, data in entries.items():
                info = tarfile.TarInfo(n)
                info.size, info.mode = len(data), 0o755
                t.addfile(info, io.BytesIO(data))
            if link:
                info = tarfile.TarInfo(link)
                info.type, info.linkname = tarfile.SYMTYPE, "/etc/passwd"
                t.addfile(info)

    def test_unix_launchers(self):
        """(v0.5.0) the Linux and macOS launchers (OpenSmash.Avalonia) are listed beside OpenSmash.exe, audited inside"""
        self.tar("OpenSmash-linux-x64.tar.gz", {"./OpenSmash.Avalonia": b"ELF"})
        self.tar("OpenSmash-macos-arm64.tar.gz", {"./OpenSmash.Avalonia.app/Contents/MacOS/OpenSmash.Avalonia": b"Mach-O"})
        self.tar("OpenSmash-macos-x64.tar.gz", {"./OpenSmash.Avalonia.app/Contents/MacOS/OpenSmash.Avalonia": b"Mach-O x64"})
        m = M.manifest(self.dist, "v0.5.0", COMMIT)
        self.assertEqual(sorted(m["launcher"]), ["linux-x64", "macos-arm64", "macos-x64", "windows-x64"])
        self.assertEqual(m["launcher"]["linux-x64"], {"file": "OpenSmash-linux-x64.tar.gz",
                                                      "sha256": M.sha256(os.path.join(self.dist, "OpenSmash-linux-x64.tar.gz"))})
        self.tar("OpenSmash-linux-x64.tar.gz", {"./OpenSmash.Avalonia": b"ELF", "./prod.keys": b"x"})
        with self.assertRaises(SystemExit):
            M.manifest(self.dist, "v0.5.0", COMMIT)
        self.tar("OpenSmash-linux-x64.tar.gz", {"./OpenSmash.Avalonia": b"ELF"}, link="./OpenSmash.Avalonia.lnk")
        self.assertTrue(any("not a plain file" in p for p in M.audit([os.path.join(self.dist, "OpenSmash-linux-x64.tar.gz")])))
        os.remove(os.path.join(self.dist, "OpenSmash.exe"))
        self.tar("OpenSmash-linux-x64.tar.gz", {"./OpenSmash.Avalonia": b"ELF"})
        with self.assertRaises(SystemExit):   # (the Windows launcher is still required: v0.4.x launchers update from it)
            M.manifest(self.dist, "v0.5.0", COMMIT)

    def test_prerelease_only_testing(self):
        with self.assertRaises(SystemExit):
            M.manifest(self.dist, "v0.5.0-rc.1", COMMIT)
        self.assertEqual(M.manifest(self.dist, "v0.5.0-rc.1", COMMIT, channel="testing")["channel"], "testing")

    def test_unknown_file_refused(self):
        with open(os.path.join(self.dist, "notes.txt"), "w") as f:
            f.write("x")
        with self.assertRaises(SystemExit):
            M.manifest(self.dist, "v0.5.0", COMMIT)

    def test_audit_finds_game_data(self):
        self.zip("opensmash-runtime-windows-x86-64-v2.zip", {"main.nso": b"NSO0", "keys/prod.keys": b"x",
                                                           "a.txt": b"header_key = " + b"0" * 64})
        bad = M.audit([os.path.join(self.dist, "opensmash-runtime-windows-x86-64-v2.zip")])
        self.assertEqual(len(bad), 3, bad)
        with self.assertRaises(SystemExit):
            M.manifest(self.dist, "v0.5.0", COMMIT)

    def test_service(self):
        out = os.path.join(self.dist, "svc.json")
        os.environ["OPENSMASH_SUPABASE_URL"] = "https://example.supabase.co"
        os.environ["OPENSMASH_SUPABASE_KEY"] = "sb_publishable_test"
        try:
            self.assertEqual(M.service(out)["url"], "https://example.supabase.co")
            os.environ["OPENSMASH_SUPABASE_KEY"] = "sb_secret_nope"
            with self.assertRaises(SystemExit):
                M.service(out)
        finally:
            del os.environ["OPENSMASH_SUPABASE_URL"], os.environ["OPENSMASH_SUPABASE_KEY"]

    def test_service_committed_and_required(self):
        out = os.path.join(self.dist, "svc2.json")
        with open(out, "w", encoding="utf-8") as f:
            f.write('{"url": "https://committed.supabase.co", "publishable_key": "sb_publishable_c", "sign_in_page": "", "account_page": ""}')
        self.assertEqual(M.service(out)["publishable_key"], "sb_publishable_c")   # (the committed values, no variables)
        with open(out, "w", encoding="utf-8") as f:
            f.write('{"url": "", "publishable_key": ""}')
        with self.assertRaises(SystemExit):   # (a release without the accounts service is refused)
            M.service(out)

    def test_repository_service_file(self):
        with open(os.path.join(os.path.dirname(__file__), "..", "OpenSmash.Core", "account_service.json"), encoding="utf-8") as f:
            s = json.load(f)
        self.assertTrue(s["url"].startswith("https://") and s["publishable_key"].startswith("sb_publishable_"))


if __name__ == "__main__":
    unittest.main()
