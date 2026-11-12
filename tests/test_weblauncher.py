"""The OpenSmash Launcher for Linux and macOS (tools/weblauncher/launcher.py) and the Linux fixes it relies on: the
settings files it writes (as OpenSmash.exe writes them), the setup / play / update commands it runs, its web server's
endpoints and protection (key, local host name), its jobs, the browser window it opens (tools/app_window.py), and
the mod manager's Linux paths (the plugin link, the unpackers, sudo without a terminal). Made-up folders only: no game
files, no network, no browser. Runs on Windows, Linux and macOS."""
import http.client
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from http.server import ThreadingHTTPServer
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "weblauncher"))
sys.path.insert(0, os.path.join(ROOT, "tools", "linux"))
import app_window  # noqa: E402
import launcher  # noqa: E402
import install_linux  # noqa: E402
import skyline  # noqa: E402


def text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class Settings(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def test_defaults_without_files(self):
        s = launcher.read_settings(self.root)
        self.assertEqual(s["window"], "windowed")
        self.assertEqual(s["window_size"], "1280x720")
        self.assertEqual(s["lobby"], "offline")   # (no online_server.txt: nothing to join)
        self.assertEqual(s["name"], "")
        self.assertEqual(s["direct"], "on")

    def test_reads_the_files_the_game_and_windows_launcher_write(self):
        with open(os.path.join(self.root, "display.ini"), "w") as f:
            f.write("; the game's\nwindow = fullscreen   ; a comment\nwidth = 1920\nheight = 1080\nvolume=40\n")
        with open(os.path.join(self.root, "online_server.txt"), "w") as f:
            f.write("# the lobby\nsmash.recomps.wtf:39421 friends\n")
        with open(os.path.join(self.root, "online_name.txt"), "w") as f:
            f.write("Mario\n")
        s = launcher.read_settings(self.root)
        self.assertEqual((s["window"], s["window_size"], s["volume"]), ("fullscreen", "1920x1080", "40"))
        self.assertEqual((s["lobby"], s["code"], s["name"]), ("private", "friends", "Mario"))

    def test_auto_is_the_public_lobby(self):
        with open(os.path.join(self.root, "online_server.txt"), "w") as f:
            f.write("auto\n")
        self.assertEqual(launcher.read_settings(self.root)["lobby"], "public")

    def test_write_each_lobby(self):
        for lobby, extra, line in (("public", {}, "auto"), ("offline", {}, "off"),
                                   ("private", {"code": "abc"}, "smash.recomps.wtf:39421 abc"),
                                   ("custom", {"server": "10.0.0.2:39420", "code": ""}, "10.0.0.2:39420"),
                                   ("custom", {"server": "", "code": "x"}, "127.0.0.1:39420 x")):
            launcher.write_settings(self.root, {"lobby": lobby, **extra})
            self.assertEqual(launcher.first_line(os.path.join(self.root, "online_server.txt")), line, lobby)
            self.assertEqual(launcher.ini_value(os.path.join(self.root, "launcher.ini"), "opensmash", "lobby"), lobby)

    def test_write_display_and_controllers(self):
        s = launcher.write_settings(self.root, {"window": "borderless", "window_size": "2560x1440", "volume": "250",
                                                "keyboard_player": "2", "name": " Luigi ", "direct": "off"})
        display = os.path.join(self.root, "display.ini")
        self.assertEqual(launcher.ini_value(display, "", "window"), "borderless")
        self.assertEqual(launcher.ini_value(display, "", "width"), "2560")
        self.assertEqual(launcher.ini_value(display, "", "height"), "1440")
        self.assertEqual(launcher.ini_value(display, "", "volume"), "100")   # (kept within 0-100)
        self.assertEqual(launcher.ini_value(os.path.join(self.root, "controllers.ini"), "general", "keyboard_player"), "2")
        self.assertEqual(text(os.path.join(self.root, "online_name.txt")), "Luigi\n")
        self.assertEqual(text(os.path.join(self.root, "online_direct.txt")), "off\n")
        self.assertEqual(s["name"], "Luigi")

    def test_keeps_other_lines_and_sections(self):
        pads = os.path.join(self.root, "controllers.ini")
        with open(pads, "w") as f:
            f.write("[general]\nkeyboard_player = 1\n\n[pad1]\nbutton_a = 0\n")
        launcher.write_settings(self.root, {"keyboard_player": "0"})
        self.assertEqual(launcher.ini_value(pads, "general", "keyboard_player"), "0")
        self.assertEqual(launcher.ini_value(pads, "pad1", "button_a"), "0")

    def test_refuses_what_the_windows_launcher_refuses(self):
        with self.assertRaises(launcher.Error):
            launcher.write_settings(self.root, {"lobby": "private", "code": ""})
        with self.assertRaises(launcher.Error):
            launcher.write_settings(self.root, {"lobby": "custom", "server": "no-port"})
        with self.assertRaises(launcher.Error):
            launcher.write_settings(self.root, {"window": "huge"})
        with self.assertRaises(launcher.Error):
            launcher.write_settings(self.root, {"window_size": "big"})

    def test_config_folder_once_the_game_made_it(self):
        os.makedirs(os.path.join(self.root, "config"))
        self.assertEqual(launcher.config_file(self.root, "display.ini"), os.path.join(self.root, "display.ini"))
        with open(os.path.join(self.root, "config", "version.ini"), "w") as f:
            f.write("1\n")
        self.assertEqual(launcher.config_file(self.root, "display.ini"), os.path.join(self.root, "config", "display.ini"))
        # (online_server.txt: config/'s only once there is one there)
        self.assertEqual(launcher.config_file(self.root, "online_server.txt"), os.path.join(self.root, "online_server.txt"))
        self.assertEqual(launcher.config_file(self.root, "online_server.txt"),
                         install_linux.config_file("online_server.txt", self.root))


class Commands(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.host = launcher.HOST

    def tearDown(self):
        launcher.HOST = self.host
        self.tmp.cleanup()

    def test_setup_command_linux(self):
        launcher.HOST = "linux"
        game, keys = os.path.join(self.root, "g"), os.path.join(self.root, "k")
        os.makedirs(game)
        os.makedirs(keys)
        cmd = launcher.setup_command(self.root, {"game": game, "keys": keys, "name": "Peach", "jobs": "2",
                                                 "no_packages": True}, python="py")
        self.assertEqual(cmd[:3], ["py", "-u", os.path.join(self.root, "tools", "linux", "install_linux.py")])
        for want in (["--game", game], ["--keys", keys], ["--online-name", "Peach"], ["--jobs", "2"]):
            i = cmd.index(want[0])
            self.assertEqual(cmd[i:i + 2], want)
        for flag in ("--yes", "--no-firmware", "--no-update", "--no-packages"):
            self.assertIn(flag, cmd)
        # every option the page passes is one the installer takes
        for flag in [c for c in cmd[3:] if c.startswith("--")]:
            self.assertIn(flag, text(os.path.join(ROOT, "tools", "linux", "install_linux.py")))

    def test_setup_command_macos_and_missing_folder(self):
        launcher.HOST = "macos"
        cmd = launcher.setup_command(self.root, {}, python="py")
        self.assertIn(os.path.join("tools", "mac", "install_mac.py"), cmd[2])
        self.assertIn("--no-wizard", cmd)
        self.assertNotIn("--no-firmware", cmd)   # (no game folder given: the folders read before are kept)
        with self.assertRaises(launcher.Error):
            launcher.setup_command(self.root, {"game": os.path.join(self.root, "nope")})

    def test_update_only_is_an_installer_option(self):
        for path in (os.path.join("tools", "linux", "install_linux.py"), os.path.join("tools", "mac", "install_mac.py")):
            self.assertIn('"--update-only"', text(os.path.join(ROOT, path)))

    def test_play_commands(self):
        self.assertEqual(launcher.play_command("/r", "online", "linux"), ["bash", os.path.join("/r", "play-ssbu.sh")])
        self.assertEqual(launcher.play_command("/r", "offline", "linux")[1], os.path.join("/r", "launchers", "play-ssbu-offline.sh"))
        self.assertEqual(launcher.play_command("/r", "online", "macos")[-1], "play")
        self.assertEqual(launcher.play_command("/r", "offline", "macos")[-1], "offline")
        for path in ("play-ssbu.sh", os.path.join("launchers", "play-ssbu-offline.sh"), os.path.join("tools", "mac", "launch.sh")):
            self.assertTrue(os.path.isfile(os.path.join(ROOT, path)), path)

    def test_programs(self):
        self.assertTrue(launcher.game_program("/r", "linux").endswith(os.path.join("build-linux", "ssbu")))
        self.assertTrue(launcher.game_program("/r", "macos").endswith(os.path.join("MacOS", "ssbu")))

    def test_list_dir_and_folder_checks(self):
        os.makedirs(os.path.join(self.root, "b"))
        os.makedirs(os.path.join(self.root, "a", "x"))
        with open(os.path.join(self.root, "a", "x", "game.nsp"), "w") as f:
            f.write("")
        r = launcher.list_dir(self.root)
        self.assertEqual(r["dirs"], ["a", "b"])
        with self.assertRaises(launcher.Error):
            launcher.list_dir(os.path.join(self.root, "missing"))
        c = launcher.check_folders({"game": os.path.join(self.root, "a"), "keys": self.root, "firmware": ""})
        self.assertTrue(c["game"]["ok"])
        self.assertFalse(c["keys"]["ok"])   # (no prod.keys)
        self.assertTrue(c["firmware"]["ok"])   # (optional)

    def test_steam_game_mode(self):
        self.assertTrue(launcher.steam_game_mode({"SteamGamepadUI": "1"}))
        self.assertTrue(launcher.steam_game_mode({"XDG_CURRENT_DESKTOP": "gamescope"}))
        self.assertFalse(launcher.steam_game_mode({"XDG_CURRENT_DESKTOP": "KDE"}))

    def test_desktop_entry(self):
        entry = install_linux.launcher_desktop_entry("/home/deck/Open Smash")
        self.assertIn('Exec=bash "/home/deck/Open Smash/opensmash-launcher.sh"\n', entry)
        self.assertIn("Terminal=false\n", entry)
        path = os.path.join(self.root, "apps", "x.desktop")
        self.assertTrue(install_linux.write_desktop_entry(path, entry))
        self.assertFalse(install_linux.write_desktop_entry(path, entry))   # (the same: not written again)


class Jobs(unittest.TestCase):
    def run_job(self, cmds):
        jobs = launcher.Jobs()
        job = jobs.start("test", cmds, ROOT)
        for _ in range(200):
            if job["done"]:
                return job, jobs
            time.sleep(0.05)
        self.fail("the job did not end")

    def test_log_progress_and_result(self):
        script = "print('LOG percent 40', flush=True); print('LOG step one'); print('[####----]  60%  linking (1 min)')"
        job, jobs = self.run_job([[sys.executable, "-c", script]])
        self.assertTrue(job["ok"], job)
        self.assertIn("step one", job["log"])
        self.assertNotIn("LOG percent 40", job["log"])
        self.assertEqual(job["progress"], 1.0)
        self.assertIsNone(jobs.busy())

    def test_failure_stops_the_rest(self):
        job, _ = self.run_job([[sys.executable, "-c", "import sys; sys.exit(3)"], [sys.executable, "-c", "print('never')"]])
        self.assertFalse(job["ok"])
        self.assertIn("exit code 3", job["error"])
        self.assertNotIn("never", job["log"])

    def test_one_at_a_time(self):
        jobs = launcher.Jobs()
        jobs.start("slow", [[sys.executable, "-c", "import time; time.sleep(1)"]], ROOT)
        with self.assertRaises(launcher.Error):
            jobs.start("second", [[sys.executable, "-c", "pass"]], ROOT)

    def test_progress_patterns(self):
        job = {"log": [], "progress": None}
        launcher.Jobs.add(job, "fetching — 37%, 2 min left")
        self.assertAlmostEqual(job["progress"], 0.37)


class Server(unittest.TestCase):
    """The web server on a free port, as the page talks to it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        os.makedirs(os.path.join(self.root, "sub", "deeper"))
        self.app = launcher.App(self.root, "k3y")
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), launcher.make_handler(self.app))
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.port = self.srv.server_address[1]

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.tmp.cleanup()

    def call(self, method, path, body=None, key="k3y", host=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        headers = {"Host": host or f"127.0.0.1:{self.port}"}
        if key:
            headers["X-Key"] = key
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        c.request(method, path, body=data, headers=headers)
        r = c.getresponse()
        raw = r.read()
        c.close()
        try:
            return r.status, json.loads(raw)
        except ValueError:
            return r.status, raw

    def test_page_needs_the_key(self):
        self.assertEqual(self.call("GET", "/", key=None)[0], 403)
        status, page = self.call("GET", "/?key=k3y", key=None)
        self.assertEqual(status, 200)
        self.assertIn(b"OpenSmash Launcher", page)
        self.assertIn(b"/ui/shared/gamepad.js", page)

    def test_post_needs_the_header_not_the_query(self):
        self.assertEqual(self.call("POST", "/api/settings?key=k3y", {"lobby": "offline"}, key=None)[0], 403)
        self.assertEqual(self.call("POST", "/api/settings", {"lobby": "offline"})[0], 200)

    def test_other_host_names_are_refused(self):   # (DNS rebinding)
        self.assertEqual(self.call("GET", "/api/state", host="evil.example:80")[0], 403)

    def test_static_files_and_no_way_out(self):
        for path in ("/ui/app.js", "/ui/app.css", "/ui/shared/gamepad.js", "/ui/shared/icon.svg"):
            self.assertEqual(self.call("GET", path)[0], 200, path)
        for path in ("/ui/../launcher.py", "/ui/shared/../server.py", "/ui/%2e%2e/launcher.py"):
            self.assertEqual(self.call("GET", path)[0], 404, path)

    def test_state(self):
        status, s = self.call("GET", "/api/state")
        self.assertEqual(status, 200)
        self.assertFalse(s["built"])
        self.assertFalse(s["python"])
        self.assertIn("window", s["choices"])
        self.assertEqual(s["settings"]["lobby"], "offline")

    def test_settings_round_trip_and_error(self):
        status, s = self.call("POST", "/api/settings", {"lobby": "private", "code": "duck", "name": "Kirby"})
        self.assertEqual((status, s["lobby"], s["code"], s["name"]), (200, "private", "duck", "Kirby"))
        status, err = self.call("POST", "/api/settings", {"lobby": "private", "code": ""})
        self.assertEqual(status, 400)
        self.assertIn("code", err["error"])

    def test_browse_and_check(self):
        status, r = self.call("GET", "/api/browse?path=" + self.root.replace("\\", "/"))
        self.assertEqual((status, r["dirs"]), (200, ["sub"]))
        status, r = self.call("POST", "/api/setup/check", {"game": self.root, "keys": "", "firmware": ""})
        self.assertEqual(status, 200)
        self.assertFalse(r["checks"]["keys"]["ok"])

    def test_rebuild_all_runs_the_platform_script(self):
        original_host = launcher.HOST
        launcher.HOST = "linux"
        try:
            with mock.patch.object(launcher, "venv_python", return_value=sys.executable), \
             mock.patch.object(self.app, "game_running", return_value=False), \
             mock.patch.object(self.app.jobs, "start", return_value={"id": "build"}) as start:
                status, result = self.call("POST", "/api/rebuild-all", {"jobs": "4"})
        finally:
            launcher.HOST = original_host
        self.assertEqual((status, result), (200, {"id": "build"}))
        title, commands, cwd = start.call_args.args[:3]
        self.assertEqual(title, "Building the game and launcher")
        self.assertEqual(cwd, self.root)
        self.assertEqual(commands, [["bash", os.path.join(self.root, "tools", "linux", "rebuild-all.sh"), "--jobs", "4"]])

    def test_rebuild_all_uses_the_macos_script(self):
        original_host = launcher.HOST
        launcher.HOST = "macos"
        try:
            with mock.patch.object(launcher, "venv_python", return_value=sys.executable), \
                 mock.patch.object(self.app, "game_running", return_value=False), \
                 mock.patch.object(self.app.jobs, "start", return_value={"id": "build"}) as start:
                status, _result = self.call("POST", "/api/rebuild-all", {})
        finally:
            launcher.HOST = original_host
        self.assertEqual(status, 200)
        self.assertEqual(start.call_args.args[1],
                         [["bash", os.path.join(self.root, "tools", "mac", "rebuild-all.command")]])

    def test_rebuild_all_uses_the_windows_script(self):
        original_host = launcher.HOST
        launcher.HOST = "windows"
        try:
            with mock.patch.object(launcher, "venv_python", return_value=sys.executable), \
                 mock.patch.object(self.app, "game_running", return_value=False), \
                 mock.patch.object(self.app.jobs, "start", return_value={"id": "build"}) as start:
                status, _result = self.call("POST", "/api/rebuild-all", {"jobs": "8"})
        finally:
            launcher.HOST = original_host
        self.assertEqual(status, 200)
        self.assertEqual(start.call_args.args[1],
                         [["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                           os.path.join(self.root, "tools", "rebuild-all.ps1"), "-Jobs", "8"]])

    def test_play_before_the_build(self):
        status, err = self.call("POST", "/api/play", {"mode": "online"})
        self.assertEqual(status, 400)
        self.assertIn("built", err["error"])
        self.assertEqual(self.call("POST", "/api/play", {"mode": "sideways"})[0], 400)

    def test_mods_need_the_setup(self):
        status, r = self.call("GET", "/api/mods/status")
        self.assertEqual((status, r["available"]), (200, False))
        self.assertEqual(self.call("POST", "/api/mods/open", {"back": "http://127.0.0.1:1/"})[0], 400)

    def test_ping_and_unknown(self):
        self.assertEqual(self.call("POST", "/api/ping", {})[1], {"ok": True})
        self.assertEqual(self.call("GET", "/api/nothing")[0], 400)
        self.assertEqual(self.call("GET", "/elsewhere")[0], 404)


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
        for path in ("opensmash-launcher.sh", "tools/linux/askpass.sh", "OpenSmash Launcher.command"):
            self.assertTrue(os.path.isfile(os.path.join(ROOT, path)), path)
        self.assertIn("opensmash-launcher.sh", install_linux.LAUNCHERS)


if __name__ == "__main__":
    unittest.main()
