"""Launcher packaging changes must not ask players to recompile the game."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import skyline
import std_plugins


class LauncherReadiness(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.exe = self.root / "build" / "ssbu.exe"
        self.exe.parent.mkdir()
        self.exe.write_bytes(b"synthetic fixture")
        os.utime(self.exe, (100, 100))
        (self.root / "tools").mkdir()
        (self.root / "runtime").mkdir()
        for target, answer in [("program", str(self.exe)), ("unbuilt_standard", None),
                               ("unbuilt_mod_plugin", None), ("stale_hooks", False),
                               ("plugin_refresh_reason", None)]:
            self.enterContext(patch.object(skyline, target, return_value=answer))
        self.enterContext(patch.object(std_plugins, "PLUGINS", {}))

    def test_launcher_only_changes_do_not_require_a_game_build(self):
        for name in ("build_launcher.py", "build_launcher.ps1", "build_desktop_launcher.py",
                     "prepare_desktop_source.py", "desktop_update.py", "linux/install.sh", "linux/play.sh",
                     "mac/install.sh", "mac/play.sh", "mac/launch.sh", "linux/install_linux.py", "mac/install_mac.py"):
            with self.subTest(file=name):
                (self.root / "tools" / name).parent.mkdir(parents=True, exist_ok=True)
                (self.root / "tools" / name).write_text("launcher only")
                self.assertIsNone(skyline.game_link_reason(str(self.root)))
                self.assertIsNone(skyline.needs_build(str(self.root)))

    def test_vanilla_readiness_ignores_plugin_work(self):
        with patch.object(skyline.os, "name", "posix"), patch.object(skyline, "unbuilt_mod_plugin", return_value="new plugin missing"):
            self.assertEqual(skyline.game_link_reason(str(self.root)), "new plugin missing")
            self.assertIsNone(skyline.game_link_reason(str(self.root), mods=False))

    def test_vanilla_still_requires_community_plugins(self):
        with patch.object(skyline, "unbuilt_standard", return_value="community plugin missing"):
            self.assertEqual(skyline.game_link_reason(str(self.root), mods=False), "community plugin missing")

    def test_runtime_change_still_requires_a_game_build(self):
        (self.root / "runtime" / "game.c").write_text("game code")
        self.assertIn("runtime/game.c", skyline.game_link_reason(str(self.root)))
        self.assertIn("runtime/game.c", skyline.game_link_reason(str(self.root), mods=False))
        self.assertIn("runtime/game.c", skyline.needs_build(str(self.root)))

    def test_recompiler_change_still_requires_a_game_build(self):
        (self.root / "tools" / "recomp.py").write_text("recompiler code")
        self.assertIn("tools/recomp.py", skyline.game_link_reason(str(self.root)))


if __name__ == "__main__":
    unittest.main()
