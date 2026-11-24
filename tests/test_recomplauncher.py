"""recomplauncher.json (RecompLauncher, #543): what it names exists, and the release zip carries the release tag."""
import json
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import make_recomplauncher_zip  # noqa: E402


class RecompLauncherManifest(unittest.TestCase):
    def setUp(self):
        self.data = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf-8"))

    def test_format_and_inputs(self):
        self.assertEqual(self.data["format"], 1)
        ids = {i["id"] for i in self.data["inputs"]}
        self.assertEqual(ids, {"game", "prodkeys", "titlekeys"})
        for i in self.data["inputs"]:
            if "shared" in i:
                self.assertEqual(i["role"], "key")
                self.assertNotIn("formats", i)

    def test_programs_are_in_the_release(self):
        self.assertTrue((ROOT / self.data["prepare"]["program"][0]).is_file())
        self.assertTrue((ROOT / "tools" / "recomplauncher_prepare.ps1").is_file())
        self.assertEqual(self.data["start"]["program"], ["OpenSmash.exe"])
        self.assertEqual(self.data["start"]["args"], ["--play"])
        for arg in self.data["prepare"]["args"]:
            if arg.startswith("{input:"):
                self.assertIn(arg[7:-1], {i["id"] for i in self.data["inputs"]})

    def test_licence_is_never_ticked_for_the_player(self):
        option = self.data["prepare"]["options"][0]
        self.assertIn("license", option)
        self.assertTrue(option["license"]["url"].startswith("https://"))
        self.assertEqual(option["args"], ["-AcceptLicense"])

    def test_zip_manifest_has_the_tag(self):
        data = json.loads(make_recomplauncher_zip.manifest("v9.9.9"))
        self.assertEqual(data["prepare"]["env"]["OPENSMASH_RELEASE_TAG"], "v9.9.9")


if __name__ == "__main__":
    unittest.main()
