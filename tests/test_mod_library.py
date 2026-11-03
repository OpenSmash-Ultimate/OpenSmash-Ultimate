"""tools/mods.py: conflicts between turned-on mods, and shareable mod lists (export-list / import-list)."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import mods  # noqa: E402


def make_mod(tree, name, files, **meta):
    for rel in files:
        path = os.path.join(tree.mods, name, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(b"x")
    state = tree.load()
    state["mods"][name] = dict({"enabled": True}, **meta)
    tree.save(state)


class ModLibrary(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.tree = mods.Tree(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_conflicts_name_the_mod_the_game_reads(self):
        make_mod(self.tree, "a", ["fighter/mario/model/body/c00/def_mario_001_col.nutexb", "ui/message/msg_name.msbt"])
        make_mod(self.tree, "b", ["fighter/mario/model/body/c00/def_mario_001_col.nutexb"])
        make_mod(self.tree, "c", ["ui/message/msg_name.msbt"], enabled=False)
        state = self.tree.load()
        state["order"] = ["a", "b", "c"]
        self.tree.save(state)
        found = mods.conflicts(self.tree)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["mods"], ["a", "b"])
        self.assertEqual(found[0]["wins"], "b")

    def test_list_round_trip_keeps_order_and_switches(self):
        make_mod(self.tree, "one", ["ui/a.bntx"], title="One")
        make_mod(self.tree, "two", ["ui/b.bntx"], title="Two")
        state = self.tree.load()
        state["mods"]["two"]["enabled"] = False
        state["order"] = ["two", "one"]
        self.tree.save(state)
        path = os.path.join(self.tmp.name, "list.json")
        mods.export_list(self.tree, path)
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        self.assertEqual(data["format"], mods.LIST_FORMAT)
        self.assertEqual([m["name"] for m in data["mods"]], ["two", "one"])
        state = self.tree.load()
        state["order"] = ["one", "two"]
        state["mods"]["two"]["enabled"] = True
        self.tree.save(state)
        got = mods.import_list(self.tree, path, log=lambda s: None)
        self.assertEqual(sorted(got["already"]), ["one", "two"])
        state = self.tree.load()
        self.assertEqual(state["order"][:2], ["two", "one"])
        self.assertFalse(state["mods"]["two"]["enabled"])

    def test_a_list_without_sources_reports_what_is_missing(self):
        path = os.path.join(self.tmp.name, "list.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"format": mods.LIST_FORMAT, "version": 1, "mods": [{"name": "elsewhere", "enabled": True}]}, f)
        got = mods.import_list(self.tree, path, log=lambda s: None)
        self.assertEqual(got["missing"], ["elsewhere"])

    def test_not_a_list(self):
        path = os.path.join(self.tmp.name, "x.json")
        with open(path, "w", encoding="utf-8") as f:
            f.write("{}")
        with self.assertRaises(mods.Error):
            mods.import_list(self.tree, path, log=lambda s: None)


if __name__ == "__main__":
    unittest.main()
