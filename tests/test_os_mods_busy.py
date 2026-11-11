"""tools/os_mods.py's recomp_busy: only a game build of THIS install counts as busy (a player's report,
2026-11-11: the launcher kept saying "plugins are already being built into the game")."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import os_mods  # noqa: E402


class Busy(unittest.TestCase):
    def setUp(self):
        self.root = os.path.normcase(os.path.abspath(os.path.join(os.sep, "games", "OpenSmash")))

    def line(self, root):
        j = os.path.join
        return (f'"python.exe" {j(root, "tools", "recomp.py")} all {j(root, "dump", "exefs")} '
                f'--out {j(root, "build")} --jobs 8 --tree {root}')

    def test_this_install_building(self):
        self.assertTrue(os_mods.recomp_line_busy(self.line(self.root), self.root))

    def test_another_install_building(self):
        other = os.path.normcase(os.path.abspath(os.path.join(os.sep, "games", "WindowSmash")))
        self.assertFalse(os_mods.recomp_line_busy(self.line(other), self.root))

    def test_not_a_build(self):
        self.assertFalse(os_mods.recomp_line_busy(f'"python.exe" {os.path.join(self.root, "tools", "os_mods.py")} status', self.root))


if __name__ == "__main__":
    unittest.main()
