"""tools/publish_avalonia.py's layout when --layout is not given (#542): folder for macOS, where a compressed
single-file .NET 8 launcher crashed at start on Apple Silicon, single-file elsewhere; OPENSMASH_MAC_SINGLE_FILE=1
keeps the single file on macOS too. No publish is run."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("publish_avalonia", ROOT / "tools" / "publish_avalonia.py")
publish = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publish)


class DefaultLayout(unittest.TestCase):
    def test_macos_gets_the_folder(self):
        for rid in ("osx-arm64", "osx-x64"):
            self.assertEqual(publish.default_layout(rid, {}), "folder", rid)

    def test_windows_and_linux_keep_the_single_file(self):
        for rid in ("win-x64", "linux-x64"):
            self.assertEqual(publish.default_layout(rid, {}), "single-file", rid)
            self.assertEqual(publish.default_layout(rid, {"OPENSMASH_MAC_SINGLE_FILE": "1"}), "single-file", rid)

    def test_switch_keeps_the_old_default_on_macos(self):
        self.assertEqual(publish.default_layout("osx-arm64", {"OPENSMASH_MAC_SINGLE_FILE": "1"}), "single-file")
        self.assertEqual(publish.default_layout("osx-arm64", {"OPENSMASH_MAC_SINGLE_FILE": "0"}), "folder")

    def test_every_rid_has_a_layout(self):
        for rid in publish.RIDS:
            self.assertIn(publish.default_layout(rid, {}), ("single-file", "folder"), rid)


if __name__ == "__main__":
    unittest.main()
