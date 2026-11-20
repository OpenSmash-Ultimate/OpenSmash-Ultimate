from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import csk
import prc


class CskPositions(unittest.TestCase):
    def database(self):
        rows = []
        for chara, order in ((1, 0), (2, 86), (3, 99), (4, -1)):
            rows.append((prc.STRUCT, [
                (prc.hash40("ui_chara_id"), (prc.HASH, chara)),
                (prc.hash40("disp_order"), (prc.I8, order)),
            ]))
        return prc.write((prc.STRUCT, [(csk.DB_ROOT, (prc.LIST, rows))]))

    def entry(self, chara, order):
        return {"kind": "ui_chara_db", "ui_chara_id": hex(chara),
                "set": {"disp_order": ["sbyte", order]}}

    def positions(self, data):
        rows = dict(prc.read(data)[1])[csk.DB_ROOT][1]
        return {dict(fields)[prc.hash40("ui_chara_id")][1]:
                dict(fields)[prc.hash40("disp_order")][1] for _, fields in rows}

    def test_post_vanilla_positions_are_preserved(self):
        entries = [self.entry(5, 111), self.entry(6, 100), self.entry(7, 87),
                   self.entry(8, 111)]
        data, _ = csk.apply(self.database(), entries, "ui_chara_db")
        self.assertEqual(self.positions(data),
                         {1: 0, 2: 86, 3: 99, 4: -1, 5: 111, 6: 100, 7: 87, 8: 111})

    def test_updated_plugin_changes_an_existing_position(self):
        before, _ = csk.apply(self.database(), [self.entry(5, 100)], "ui_chara_db")
        after, _ = csk.apply(before, [self.entry(5, 111)], "ui_chara_db")
        self.assertEqual(self.positions(before)[5], 100)
        self.assertEqual(self.positions(after)[5], 111)


if __name__ == "__main__":
    unittest.main()
