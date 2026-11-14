"""Relocated setup wrappers keep project cwd, arguments and backend exit codes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
BASH = str(Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Git/bin/bash.exe") if os.name == "nt" else shutil.which("bash")


@unittest.skipUnless(BASH and Path(BASH).is_file(), "needs bash")
class InternalEntryPoints(unittest.TestCase):
    def test_setup_from_an_unrelated_working_directory(self):
        for platform in ("linux", "mac"):
            # macOS's system readlink may lack -f; the Linux wrapper is exercised
            # on Linux and Git Bash, while the macOS wrapper stays tested here.
            if platform == "linux" and subprocess.run([BASH, "-c", "readlink -f ."], capture_output=True).returncode:
                continue
            with self.subTest(platform=platform), tempfile.TemporaryDirectory(prefix="OpenSmash setup ") as temporary:
                root = Path(temporary) / "Game folder"
                script = root / "tools" / platform / "install.sh"
                script.parent.mkdir(parents=True)
                shutil.copy2(ROOT / "tools" / platform / "install.sh", script)
                binary = root / "fake-bin"
                binary.mkdir()
                stubs = {
                    "python3": '#!/bin/bash\nif [ "$1" = -c ]; then exit 0; fi\nprintf "ROOT=%s\\n" "$PWD"\nprintf "ARG=%s\\n" "$@"\nexit 7\n',
                    "uname": '#!/bin/bash\necho arm64\n',
                    "id": '#!/bin/bash\necho 1000\n',
                    "xattr": '#!/bin/bash\nexit 0\n',
                }
                for name, text in stubs.items():
                    path = binary / name
                    path.write_text(text, newline="\n")
                    path.chmod(0o755)
                command = 'export PATH="$(cygpath -u "$1" 2>/dev/null || echo "$1"):$PATH"; cd /; bash "$2" --game "files with spaces" --no-update'
                result = subprocess.run([BASH, "-c", command, "fixture", str(binary), str(script)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
                self.assertIn("Game folder", result.stdout)
                self.assertIn("ARG=tools/" + platform + "/install_" + platform + ".py", result.stdout)
                self.assertIn("ARG=files with spaces", result.stdout)
                self.assertTrue((root / "install.log").is_file())
                self.assertFalse((root / "tools" / platform / "install.log").exists())


if __name__ == "__main__":
    unittest.main()
