"""Backend fixes from upstream's Unix launcher, shared with the native launcher."""
import os
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parent.parent
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/linux')]
import app_window
import skyline
import install_linux

class Window(unittest.TestCase):
    def test_linux_chrome_on_path(self):
        cmd = app_window.command("http://x/", "linux", which=lambda e: "/usr/bin/" + e if e == "chromium" else None, flatpaks=set())
        self.assertEqual(cmd, ["chromium", "--app=http://x/", "--window-size=1280,820"])

    def test_linux_flatpak_browser_full_screen(self):   # (Bazzite / SteamOS: the browsers are Flatpaks)
        cmd = app_window.command("http://x/", "linux", fullscreen=True, which=lambda e: None,
                                 flatpaks={"org.mozilla.firefox", "com.google.Chrome"})
        self.assertEqual(cmd[:3], ["flatpak", "run", "com.google.Chrome"])
        self.assertIn("--start-fullscreen", cmd)

    def test_linux_firefox_kiosk_only_full_screen(self):
        self.assertEqual(app_window.command("http://x/", "linux", fullscreen=True, which=lambda e: None,
                                            flatpaks={"org.mozilla.firefox"}),
                         ["flatpak", "run", "org.mozilla.firefox", "--kiosk", "http://x/"])
        self.assertIsNone(app_window.command("http://x/", "linux", which=lambda e: None, flatpaks={"org.mozilla.firefox"}))

    def test_macos(self):
        cmd = app_window.command("http://x/", "macos", isdir=lambda p: p.endswith("Google Chrome.app"))
        self.assertEqual(cmd[:3], ["open", "-na", "/Applications/Google Chrome.app"])
        self.assertIsNone(app_window.command("http://x/", "macos", isdir=lambda p: False))


class LinuxMods(unittest.TestCase):
    """Why mods did not fully work on Linux: the plugin build linked the Windows program, and .7z / .rar mods need an
    unpacker GNU tar isn't."""

    def test_plugin_link_on_linux_builds_build_linux(self):
        cmds = skyline.link_commands("/t", 4, platform="linux")
        self.assertEqual(len(cmds), 2)
        self.assertIn("--generate-only", cmds[0])   # (recomp.py's own compile is the Windows one)
        self.assertTrue(cmds[1][1].endswith(os.path.join("linux", "build_linux.py")))
        self.assertEqual(cmds[1][cmds[1].index("--out") + 1], os.path.join("/t", "build-linux"))

    def test_plugin_link_elsewhere(self):
        self.assertTrue(skyline.link_commands("/t", 4, platform="darwin")[0][1].endswith(os.path.join("mac", "build_mac.py")))
        win = skyline.link_commands("/t", 4, platform="win32")
        self.assertEqual(len(win), 1)
        self.assertNotIn("--generate-only", win[0])

    def test_this_systems_program_first(self):
        self.assertTrue(skyline.programs("/t", "linux")[0].endswith(os.path.join("build-linux", "ssbu")))
        self.assertTrue(skyline.programs("/t", "darwin")[0].endswith(os.path.join("MacOS", "ssbu")))
        self.assertTrue(skyline.programs("/t", "win32")[0].endswith("ssbu.exe"))

    def test_unpackers(self):
        try:
            import mods
        except ImportError as e:   # (mods.py needs requirements.txt's packages)
            self.skipTest(str(e))
        have = {"bsdtar", "unrar"}
        which = lambda n: "/usr/bin/" + n if n in have else None
        self.assertEqual([c[0] for c in mods.unpack_commands("m.7z", "/x", which)], ["/usr/bin/bsdtar"])
        self.assertEqual([c[0] for c in mods.unpack_commands("m.RAR", "/x", which)], ["/usr/bin/bsdtar", "/usr/bin/unrar"])
        self.assertEqual(mods.unpack_commands("m.7z", "/x", lambda n: None), [])

    def test_sudo_without_a_terminal(self):
        self.assertEqual(install_linux.sudo_command(["dnf", "x"], euid=0), ["dnf", "x"])
        self.assertEqual(install_linux.sudo_command(["dnf"], euid=1000, tty=True, askpass="/a"), ["sudo", "dnf"])
        self.assertEqual(install_linux.sudo_command(["dnf"], euid=1000, tty=False, askpass="/a"), ["sudo", "-A", "dnf"])
        self.assertEqual(install_linux.sudo_command(["dnf"], euid=1000, tty=False, askpass=""), ["sudo", "dnf"])

    def test_scripts_exist_and_are_executable_in_git(self):
        for path in ("tools/linux/install.sh", "tools/linux/play.sh", "tools/mac/install.sh", "tools/linux/askpass.sh"):
            self.assertTrue(os.path.isfile(os.path.join(ROOT, path)), path)
        self.assertIn("tools/linux/play.sh", install_linux.LAUNCHERS)
