"""recomplauncher.json (RecompLauncher, #543): what it names exists, the release zip carries the release tag and the
whole project, and the release workflow makes it."""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

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



class RecompLauncherZip(unittest.TestCase):
    """OpenSmash-<tag>-Windows-x64.zip: what RecompLauncher installs, and what its update compares the next release
    with. A release with OpenSmash.exe alone took every file of the old zip out of the players' folders (v0.5.1)."""

    def test_layout(self):
        if shutil.which("git") is None or subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True).returncode != 0:
            self.skipTest("no git checkout")
        with tempfile.TemporaryDirectory() as tmp:
            exe = pathlib.Path(tmp) / "OpenSmash.exe"
            exe.write_bytes(b"MZ the release's launcher")
            out = make_recomplauncher_zip.build("v9.9.9", exe, pathlib.Path(tmp) / "dist", rev="HEAD")
            self.assertEqual(out.name, "OpenSmash-v9.9.9-Windows-x64.zip")
            with zipfile.ZipFile(out) as z:
                names = z.namelist()
                # (at the top, no folder around them: RecompLauncher's game folder is the project)
                for top in ("OpenSmash.exe", "recomplauncher.json", "tools/install_opensmash.ps1", "recomplauncher-prepare.cmd",
                            "tools/recomplauncher_prepare.ps1", "runtime/"):
                    self.assertTrue(any(n == top or n.startswith(top) for n in names), top)
                self.assertEqual(names.count("OpenSmash.exe"), 1)
                self.assertEqual(names.count("recomplauncher.json"), 1)
                self.assertEqual(z.read("OpenSmash.exe"), b"MZ the release's launcher")
                data = json.loads(z.read("recomplauncher.json"))
                self.assertEqual(data["prepare"]["env"]["OPENSMASH_RELEASE_TAG"], "v9.9.9")
                # (the player's own: never in it, so an update never replaces or takes them)
                for own in ("save/", "miis/", "build/"):
                    self.assertFalse(any(n.startswith(own) for n in names), own)

    def test_release_workflow_makes_it(self):
        flow = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        draft = flow[flow.index("\n  draft:"):]
        step = draft.index("tools/make_recomplauncher_zip.py")
        self.assertIn("--exe dist/OpenSmash.exe", draft[step:step + 300])
        self.assertIn("--out dist", draft[step:step + 300])
        self.assertLess(step, draft.index("make_release.py manifest"))   # (audited and in SHA256SUMS with the rest)
        self.assertLess(draft.index("download-artifact"), step)


if __name__ == "__main__":
    unittest.main()
