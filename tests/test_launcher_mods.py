"""Launcher Python selection and mod-command results, using the production C# helper.

No game files, network calls, WebView2 or test packages are needed. On Windows with
.NET 8 SDK, run: python -m unittest discover -s tests -p test_launcher_mods.py -v
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parent.parent
DOTNET = shutil.which("dotnet")


@unittest.skipUnless(os.name == "nt" and DOTNET, "requires Windows and the .NET 8 SDK")
class LauncherMods(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.project = tempfile.TemporaryDirectory(prefix="launcher-mod-tests-")
        cls.addClassCleanup(cls.project.cleanup)
        folder = Path(cls.project.name)
        source = escape(str(ROOT / "OpenSmash.Launcher" / "ModManagerProcess.cs"))
        (folder / "Check.csproj").write_text(f"""<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
  </PropertyGroup>
  <ItemGroup><Compile Include="{source}" Link="ModManagerProcess.cs" /></ItemGroup>
</Project>""", encoding="utf-8")
        (folder / "Program.cs").write_text("""using OpenSmash.Launcher;
if (args[0] == "python")
    Console.Write(ModManagerProcess.FindPython(args[1], args[2]));
else
{
    try { ModManagerProcess.ValidateResult(int.Parse(args[1]), Console.In.ReadToEnd()); }
    catch (InvalidOperationException ex)
    {
        Console.Error.Write(ex.Message);
        Environment.ExitCode = 1;
    }
}
""", encoding="utf-8")
        built = subprocess.run([DOTNET, "build", str(folder / "Check.csproj"),
                                "--nologo", "--verbosity", "quiet"],
                               capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
        if built.returncode:
            raise RuntimeError(built.stdout + built.stderr)
        cls.runner = folder / "bin" / "Debug" / "net8.0" / "Check.dll"

    def setUp(self):
        self.files = tempfile.TemporaryDirectory(prefix="launcher-python-")
        self.addCleanup(self.files.cleanup)
        self.root = Path(self.files.name)
        self.build = self.root / "build"
        self.build.mkdir()

    def invoke(self, *args, output=""):
        return subprocess.run([DOTNET, str(self.runner), *map(str, args)], input=output,
                              capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)

    def touch(self, relative):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return path

    def selected(self):
        result = self.invoke("python", self.root, self.build)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def configure(self, value):
        (self.build / "python_path.txt").write_text(value, encoding="utf-8")

    def test_system_fallback(self):
        self.assertEqual(self.selected(), "python")

    def test_project_venv(self):
        venv = self.touch(".venv/Scripts/python.exe")
        self.assertEqual(self.selected(), str(venv))

    def test_explicit_configuration_precedes_venv(self):
        configured = self.touch("custom python 零/python.exe")
        self.touch(".venv/Scripts/python.exe")
        self.configure(" \n" + str(configured) + "\n")
        self.assertEqual(self.selected(), str(configured))

    def test_bundled_runtime_retains_priority(self):
        bundled = self.touch("deps/python/python.exe")
        configured = self.touch("custom/python.exe")
        self.configure(str(configured))
        self.touch(".venv/Scripts/python.exe")
        self.assertEqual(self.selected(), str(bundled))

    def test_stale_configuration_falls_back_to_venv(self):
        self.configure(str(self.root / "missing.exe"))
        venv = self.touch(".venv/Scripts/python.exe")
        self.assertEqual(self.selected(), str(venv))

    def test_empty_configuration_falls_back_to_path(self):
        self.configure("\n")
        self.assertEqual(self.selected(), "python")

    def test_success(self):
        result = self.invoke("validate", 0, output='LOG percent 100\r\nRESULT {"ok": true}\r\n')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_python_import_crash_without_result_is_failure(self):
        crashed = subprocess.run([sys.executable, "-c",
                                  "import opensmash_missing_dependency_for_test"],
                                 capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
        self.assertNotEqual(crashed.returncode, 0)
        result = self.invoke("validate", crashed.returncode, output=crashed.stderr)
        self.assertEqual(result.returncode, 1)
        self.assertIn("opensmash_missing_dependency_for_test", result.stderr)

    def test_result_error_is_preserved(self):
        for code in (0, 1):
            with self.subTest(code=code):
                result = self.invoke("validate", code,
                                     output='RESULT {"ok":false,"error":"checksum mismatch"}')
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stderr, "checksum mismatch")

    def test_nonzero_exit_cannot_succeed(self):
        result = self.invoke("validate", 1, output='RESULT {"ok":true}')
        self.assertEqual(result.returncode, 1)
        self.assertIn("exit 1", result.stderr)

    def test_missing_or_invalid_result_cannot_succeed(self):
        for output in ("", "LOG done", "RESULT invalid", "RESULT null", "RESULT []",
                       'RESULT {}', 'RESULT {"ok":"true"}', 'RESULT {"ok":false}',
                       'RESULT {"ok":false,"error":42}'):
            with self.subTest(output=output):
                result = self.invoke("validate", 0, output=output)
                self.assertEqual(result.returncode, 1)
                self.assertIn("did not complete", result.stderr)

    def test_last_result_is_authoritative(self):
        result = self.invoke("validate", 0,
                             output='RESULT {"ok":true}\nRESULT {"ok":false,"error":"stopped"}')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, "stopped")

    def test_failure_details_are_bounded(self):
        result = self.invoke("validate", 1,
                             output="\n".join(f"trace line {n}" for n in range(40)))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(len(result.stderr.splitlines()), 13)
        self.assertIn("trace line 39", result.stderr)
        self.assertNotIn("trace line 27", result.stderr)


if __name__ == "__main__":
    unittest.main()
