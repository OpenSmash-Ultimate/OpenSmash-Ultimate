"""The shared launcher's Linux adapters must see game failures through tee."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


@unittest.skipUnless(sys.platform == "linux", "Linux bundle scripts")
class BundleExitTests(unittest.TestCase):
    def test_game_exit_status_survives_log_pipeline(self):
        with tempfile.TemporaryDirectory(prefix="opensmash launcher ") as folder:
            tree = Path(folder)
            (tree / "scripts").mkdir()
            (tree / "scripts" / "common.sh").write_text(
                "find_game_data() { return 0; }\n"
                "check_libraries() { return 0; }\n"
                "go_online() { return 0; }\n")
            game = tree / "synthetic game"
            for status in (0, 7):
                game.write_text(f"#!/bin/bash\necho synthetic-game-output\nexit {status}\n")
                game.chmod(0o700)
                for name in ("run.sh", "run-offline.sh"):
                    with self.subTest(status=status, launcher=name):
                        script = tree / name
                        shutil.copyfile(ROOT / "tools" / "linux" / "bundle" / name, script)
                        result = subprocess.run(["bash", str(script)], cwd=tree,
                            env={**os.environ, "SSBU_GAME_DIR": str(tree), "SSBU_EXE": str(game)},
                            capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, status, result.stderr)
                        self.assertIn("synthetic-game-output", (tree / "build" / "play.log").read_text())


if __name__ == "__main__":
    unittest.main()
