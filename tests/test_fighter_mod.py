"""A new fighter's files under its own name (tools/fighter_mod.py, tools/mods.py's new-fighters): the hashes of
paths known only in part (a name stored as its hash, a donor's path with the new name in place) agree with the
hashes of the whole texts; the search section gets the copy of a donor's folder and file; a fighter folder
becomes a turned-on mod with its config.json. Names and files made up here."""
from pathlib import Path
import json
import os
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import mods  # noqa: E402
import fighter_mod  # noqa: E402

H = mods.hash40


class Hashes(unittest.TestCase):
    def test_rename(self):
        for rest in ("", "/c00", "/motion/body/c00/a_walk.nuanmb", "/model/x/c00/" + "q" * 120 + ".nutexb"):
            self.assertEqual(mods.rename_hash(H("fighter/zed" + rest), "fighter/zed", "fighter/quillon"),
                             H("fighter/quillon" + rest))
            self.assertEqual(mods.rename_hash(H("effect/fighter/longname" + rest), "effect/fighter/longname", "effect/fighter/ab"),
                             H("effect/fighter/ab" + rest))

    def test_join_and_hashed_names(self):
        self.assertEqual(mods.join_hash(H("fighter/zed/motion/"), H("jump.nuanmb")), H("fighter/zed/motion/jump.nuanmb"))
        name = H("jump.nuanmb")
        hashed = f"fighter/zed/motion/0x{name:010x}.nuanmb"
        h, ext, parent, leaf = mods.path_parts(hashed)
        self.assertEqual((h, ext, parent, leaf), (H("fighter/zed/motion/jump.nuanmb"), H("nuanmb"), H("fighter/zed/motion/"), name))
        self.assertEqual(mods.path_parts("ui/a/b.bntx"), (H("ui/a/b.bntx"), H("bntx"), H("ui/a/"), H("b.bntx")))
        self.assertEqual(mods.path_hash(f"x/0x{H('noext'):010x}"), H("x/noext"))

    def test_swap_token(self):
        self.assertEqual(mods.swap_token("sound/bank/fighter/se_Zed_c00.nus3bank", "zed", "ab"), "sound/bank/fighter/se_ab_c00.nus3bank")
        self.assertEqual(mods.swap_token("fighter/zedd/zed/x", "zed", "ab"), "fighter/zedd/ab/x")


def a_search(folders):
    """A search section (zstd) with the folder "fighter" only, read back as mods.Search reads data.arc's."""
    from arc import LOW40  # noqa: F401
    body = struct.pack("<Q", H("fighter") | 0 << 40) + struct.pack("<4Q", H("fighter"), 0, H("fighter"), mods.NO_CHILD)
    blob = struct.pack("<QIII", 0x14 + len(body), 1, 0, 0) + body
    z = mods.zstd_compress(blob)
    data = struct.pack("<4I", 0x10, len(blob), len(z), 0) + z
    s = mods.Search(lambda off, n: data[off:off + n], 0)
    for f in folders:
        s.folder(f)
    return s


class SearchCopy(unittest.TestCase):
    def test_clone(self):
        s = a_search(["fighter/zed/motion/body"])
        s.add_file("fighter/zed/motion/body/walk.nuanmb")
        old = H("fighter/zed/motion/body/walk.nuanmb")
        new = mods.rename_hash(old, "fighter/zed", "fighter/quillon")
        s.add_clone(new, old, "fighter/zed", "fighter/quillon")
        self.assertIn(H("fighter/quillon/motion/body/walk.nuanmb"), s.path_at)
        for f in ("fighter/quillon", "fighter/quillon/motion", "fighter/quillon/motion/body"):
            self.assertIn(H(f), s.folder_at, f)
        f = s.folder_at[H("fighter/quillon/motion/body")]
        _path, parent, name, _child = struct.unpack_from("<4Q", s.folders, f * 32)
        self.assertEqual((parent & mods.LOW40, name & mods.LOW40, parent >> 40), (H("fighter/quillon/motion"), H("body"), 1))
        # (the section written and read again has both fighters' paths)
        blob = s.to_bytes()
        z = mods.zstd_compress(blob)
        data = struct.pack("<4I", 0x10, len(blob), len(z), 0) + z
        again = mods.Search(lambda off, n: data[off:off + n], 0)
        self.assertIn(new, again.path_at)
        self.assertIn(old, again.path_at)


class FolderToMod(unittest.TestCase):
    def test_make_mod(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = os.path.join(tmp, "quillon")
            for rel, data in (("fighter/quillon/model/body/c00/model.numdlb", b"m"), ("sound/bank/fighter/se_quillon_c00.nus3bank", b"s"),
                              ("src/quillon/acmd/game.cpp", b"code"), ("quillon.dll", b"MZ"), ("README.txt", b"r")):
                p = os.path.join(folder, *rel.split("/"))
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as f:
                    f.write(data)
            with open(os.path.join(folder, "fighter.json"), "w") as f:
                json.dump({"name": "Quillon", "donor": "zed", "dll": "quillon.dll"}, f)
            tree = mods.Tree(os.path.join(tmp, "tree"))
            name, donor, n = fighter_mod.make_mod(folder, tree)
            self.assertEqual((name, donor, n), ("quillon", "zed", 2))
            out = os.path.join(tree.mods, "quillon")
            self.assertEqual(sorted(p for p, _src in mods.mod_files(out)),
                             ["fighter/quillon/model/body/c00/model.numdlb", "sound/bank/fighter/se_quillon_c00.nus3bank"])
            self.assertTrue(tree.load()["mods"]["quillon"]["enabled"])
            self.assertEqual(mods.mod_config(tree, ["quillon"])["new-fighters"], {"quillon": "zed"})

    def test_needs_a_donor(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, "fighter.json"), "w") as f:
                json.dump({"name": "x"}, f)
            with self.assertRaises(mods.Error):
                fighter_mod.make_mod(tmp, mods.Tree(os.path.join(tmp, "tree")))


if __name__ == "__main__":
    unittest.main()
