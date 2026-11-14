"""tools/mods.py's walk_once: the launcher's status reads every mod folder several times (describe, online
safety, plugin state); inside walk_once each folder is walked once, with the same results as without it."""
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import mods  # noqa: E402


def write(path, data=b"x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


class WalkOnce(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.folder = os.path.join(tmp.name, "(Moveset) Example")
        for rel in ("fighter/mario/model/body/c08/model.numdlb", "ui/message/msg_name.msbt", "plugin.nro",
                    "code/sub/extra.nro", "prebuilt;/nro/release/lua2cpp_mario.nro", "flags/flag.txt",
                    "config.json", "victory.toml", ".hidden/fighter/x.bin", "fighter/.dot.bin"):
            write(os.path.join(self.folder, *rel.split("/")))

    def test_same_results(self):
        files, code = mods.mod_files(self.folder), mods.plugins(self.folder)
        with mods.walk_once():
            self.assertEqual(mods.mod_files(self.folder), files)
            self.assertEqual(mods.plugins(self.folder), code)
        self.assertEqual(code, ["code/sub/extra.nro", "plugin.nro"])
        self.assertEqual([p for p, _ in files], ["fighter/mario/model/body/c08/model.numdlb",
                                                 "prebuilt:/nro/release/lua2cpp_mario.nro",
                                                 "ui/message/msg_name.msbt"])

    def test_one_walk_per_folder(self):
        real = os.walk
        with mock.patch.object(mods.os, "walk", side_effect=real) as walk, mods.walk_once():
            for _ in range(3):
                mods.mod_files(self.folder)
                mods.plugins(self.folder)
        self.assertEqual(walk.call_count, 1)

    def test_no_cache_outside(self):
        mods.plugins(self.folder)
        write(os.path.join(self.folder, "late.nro"))
        self.assertIn("late.nro", mods.plugins(self.folder))

    def test_callers_get_their_own_lists(self):
        with mods.walk_once():
            mods.mod_files(self.folder).clear()
            self.assertTrue(mods.mod_files(self.folder))


if __name__ == "__main__":
    unittest.main()
