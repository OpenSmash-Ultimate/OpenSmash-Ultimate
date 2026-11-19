import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SideBySideTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("build_avalonia", ROOT / "tools/build_avalonia.py")
        self.builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.builder)

    def test_builder_preserves_production_launcher(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.builder.ROOT = root
            production = root / "OpenSmash.exe"
            production.write_bytes(b"production")
            package = root / "package"
            package.mkdir()
            (package / "OpenSmash.Avalonia.exe").write_bytes(b"experimental")
            installed = self.builder.install(package, "win-x64")
            self.assertEqual(installed.name, "OpenSmash.Avalonia.exe")
            self.assertEqual(installed.read_bytes(), b"experimental")
            self.assertEqual(production.read_bytes(), b"production")

    def test_linux_builder_preserves_source_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.builder.ROOT = root
            source = root / "OpenSmash.Avalonia" / "OpenSmash.csproj"
            source.parent.mkdir()
            source.write_bytes(b"source project")
            package = root / "package"
            package.mkdir()
            (package / "OpenSmash.Avalonia").write_bytes(b"linux launcher")
            installed = self.builder.install(package, "linux-x64")
            self.assertEqual(installed, root / "OpenSmash.Avalonia.bin")
            self.assertEqual(installed.read_bytes(), b"linux launcher")
            self.assertEqual(source.read_bytes(), b"source project")

    def test_existing_frontends_and_tests_are_retained(self):
        for name in [
            "OpenSmash.Launcher/LauncherForm.cs",
            "OpenSmash.Launcher/InstallerForm.cs",
            "OpenSmash.Launcher/FirstRunForm.cs",
            "OpenSmash.Launcher/RemapPage.cs",
            "OpenSmash.Launcher/ProjectSetup.cs",
            "tools/weblauncher/launcher.py",
            "tools/launcher.ps1",
            ".github/workflows/launcher-release.yml",
            ".github/ISSUE_TEMPLATE/bug_report.yml",
            "tests/launcher_settings/Program.cs",
            "tests/launcher_localization/Program.cs",
            "tests/test_weblauncher.py",
            "tests/test_launcher_ini.py",
        ]:
            with self.subTest(path=name):
                self.assertTrue((ROOT / name).is_file())
