"""A real git archive must contain the tagged source and exclude untracked data."""
import hashlib
import importlib.util
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("prepare_desktop_source", ROOT / "tools/prepare_desktop_source.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


@unittest.skipUnless(shutil.which("git"), "git is required")
class SourceReleaseTests(unittest.TestCase):
    def test_committed_source_and_embedded_manifest_agree(self):
        with tempfile.TemporaryDirectory(prefix="opensmash-release-source-") as temporary:
            root = pathlib.Path(temporary)
            def git(*args):
                return subprocess.check_output(["git", "-C", str(root), *args], stderr=subprocess.DEVNULL, text=True).strip()
            git("init")
            files = ["runtime/fixture.txt", "tools/os_mods.py", "tools/recomp.py", "tools/install_windowsmash.ps1", "tools/linux/install.sh", "tools/mac/install.sh"]
            for name in files:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("synthetic source", encoding="utf-8")
            git("add", ".")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "synthetic release fixture")
            (root / "untracked-game-data.txt").write_text("synthetic, not game data", encoding="utf-8")
            manifest = release.prepare(root, root / "output", "example/OpenSmash", "desktop-v0.1.0")
            archive = root / "output/OpenSmash-Source.zip"
            with zipfile.ZipFile(archive) as zip:
                self.assertTrue(set(files).issubset(zip.namelist()))
                self.assertNotIn("untracked-game-data.txt", zip.namelist())
            self.assertEqual(manifest["Revision"], git("rev-parse", "HEAD"))
            self.assertEqual(manifest["Sha256"], hashlib.sha256(archive.read_bytes()).hexdigest())
            self.assertEqual(manifest["ArchiveUrl"], "https://github.com/example/OpenSmash/releases/download/desktop-v0.1.0/OpenSmash-Source.zip")
            self.assertEqual(json.loads((root / "output/bootstrap-source.json").read_text()), manifest)

    def test_invalid_release_identifiers_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix="opensmash-release-invalid-") as temporary:
            root = pathlib.Path(temporary)
            with self.assertRaises(ValueError):
                release.prepare(root, root / "out", "../wrong/repo", "desktop-v1")
            with self.assertRaises(ValueError):
                release.prepare(root, root / "out", "example/repo", "../escape")
