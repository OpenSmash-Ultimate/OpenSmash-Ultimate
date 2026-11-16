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
        self.assertEqual(M.version("v0.6.0"), "0.6.0")
        self.assertEqual(M.version("v0.6.1-rc.1"), "0.6.1-rc.1")
        for bad in ("main", "v0.6", "v06.0.0", "0.6.0.1"):
            with self.assertRaises(SystemExit):
                M.version(bad)

    def test_manifest(self):
        m = M.manifest(self.dist, "v0.6.0", COMMIT, lobby="lobby.example.org:39010", min_level=1)
        with open(os.path.join(self.dist, M.MANIFEST)) as f:
            self.assertEqual(json.load(f), m)
        self.assertEqual(m["version"], "0.6.0")
        self.assertEqual(m["launcher"]["windows-x64"]["file"], "OpenSmash.exe")
        self.assertEqual(m["runtime"]["windows-avx2"]["sha256"], M.sha256(os.path.join(self.dist, "opensmash-runtime-windows-avx2.zip")))
        self.assertEqual((m["lobby"], m["min_client_level"]), ("lobby.example.org:39010", 1))
        with open(os.path.join(self.dist, "SHA256SUMS")) as f:
            self.assertEqual(len(f.read().splitlines()), 2)

    def test_prerelease_only_testing(self):
        with self.assertRaises(SystemExit):
            M.manifest(self.dist, "v0.6.0-rc.1", COMMIT)
        self.assertEqual(M.manifest(self.dist, "v0.6.0-rc.1", COMMIT, channel="testing")["channel"], "testing")

    def test_unknown_file_refused(self):
        with open(os.path.join(self.dist, "notes.txt"), "w") as f:
            f.write("x")
        with self.assertRaises(SystemExit):
            M.manifest(self.dist, "v0.6.0", COMMIT)

    def test_audit_finds_game_data(self):
        self.zip("opensmash-runtime-windows-x86-64-v2.zip", {"main.nso": b"NSO0", "keys/prod.keys": b"x",
                                                           "a.txt": b"header_key = " + b"0" * 64})
        bad = M.audit([os.path.join(self.dist, "opensmash-runtime-windows-x86-64-v2.zip")])
        self.assertEqual(len(bad), 3, bad)
        with self.assertRaises(SystemExit):
            M.manifest(self.dist, "v0.6.0", COMMIT)

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


if __name__ == "__main__":
    unittest.main()
