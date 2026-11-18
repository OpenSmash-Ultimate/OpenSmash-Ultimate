"""The launcher's "Save disk space" setting (config/save_disk.txt, OpenSmash.Core DiskSpace.cs) as the build reads it
(tools/build_game.py save_disk_apply; tools/install_windowsmash.ps1 reads the same file for the disk it asks for)."""
from pathlib import Path
import os
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import build_game as B  # noqa: E402


def tree(text):
    d = tempfile.TemporaryDirectory()
    if text is not None:
        os.makedirs(os.path.join(d.name, "config"))
        Path(d.name, "config", "save_disk.txt").write_text(text, encoding="utf-8")
    return d


class SaveDiskTests(unittest.TestCase):
    def test_off_without_the_file_and_when_it_says_off(self):
        for text in (None, "off\n", "", "no\n"):
            with tree(text) as d:
                env = {}
                self.assertFalse(B.save_disk_setting(d))
                self.assertEqual(B.save_disk_apply(d, env), {})
                self.assertEqual(env, {})

    def test_on_is_data_in_place_and_the_trim(self):
        for text in ("on\n", "ON \r\n", "on"):
            with tree(text) as d:
                env = {"PATH": "x"}
                self.assertTrue(B.save_disk_setting(d))
                made = B.save_disk_apply(d, env)
                self.assertEqual(made, {"SSBU_DATA_IN_PLACE": "1", "SSBU_TRIM": "c,nsp,map"})
                self.assertEqual(env, {"PATH": "x", "SSBU_DATA_IN_PLACE": "1", "SSBU_TRIM": "c,nsp,map"})

    def test_a_variable_set_by_hand_wins(self):
        with tree("on\n") as d:
            env = {"SSBU_DATA_IN_PLACE": "0", "SSBU_TRIM": ""}
            self.assertEqual(B.save_disk_apply(d, env), {})
            self.assertEqual(env, {"SSBU_DATA_IN_PLACE": "0", "SSBU_TRIM": ""})
            env = {"SSBU_TRIM": "map"}
            self.assertEqual(B.save_disk_apply(d, env), {"SSBU_DATA_IN_PLACE": "1"})
            self.assertEqual(env["SSBU_TRIM"], "map")

    def test_the_trim_names_are_trims_own(self):
        import trim
        self.assertEqual(set(B.SAVE_DISK["SSBU_TRIM"].split(",")), set(trim.WHAT))

    def test_the_installer_and_the_launcher_read_the_same_file(self):
        ps1 = (ROOT / "tools" / "install_windowsmash.ps1").read_text(encoding="utf-8-sig")
        core = (ROOT / "OpenSmash.Core" / "DiskSpace.cs").read_text(encoding="utf-8")
        self.assertEqual(B.SAVE_DISK_FILE.replace("\\", "/"), "config/save_disk.txt")
        self.assertIn("'save_disk.txt'", ps1)
        self.assertIn("$env:SSBU_TRIM = 'c,nsp,map'", ps1)
        self.assertIn("$env:SSBU_DATA_IN_PLACE = '1'", ps1)
        self.assertIn('SettingFile = "save_disk.txt"', core)
        self.assertIn('TrimWhat = "c,nsp,map"', core)


if __name__ == "__main__":
    unittest.main()
