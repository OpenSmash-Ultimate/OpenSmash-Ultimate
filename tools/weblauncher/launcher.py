#!/usr/bin/env python3
"""OpenSmash Launcher for Linux (Steam Deck / SteamOS / Bazzite Game Mode too) and macOS: what OpenSmash.exe
(OpenSmash.Launcher/, Windows only) does, as a page in the browser - a small web server on this computer
(127.0.0.1 only, a random port and key), Python's standard library only, so it runs before the setup has
made WindowSmash's own Python.

Usage:
  launcher.py [--tree <WindowSmash folder>] [--port N] [--no-window] [--fullscreen]

opensmash-launcher.sh (Linux) and OpenSmash Launcher.command (macOS) start it. The page asks /api/...; the
work is done by the same programs the Windows launcher and the command-line guides run:

  first-run setup   tools/linux/install_linux.py, tools/mac/install_mac.py (packages, Python, parts, your
                    folders, the build), given the folders and name chosen on the page
  build             tools/build_game.py (after an update)
  update            the installers' --update-only (GitHub's main: git pull, or the ZIP's newer files)
  play              play-ssbu.sh / launchers/play-ssbu-offline.sh (Linux), tools/mac/launch.sh (macOS), after
                    tools/os_mods.py apply or vanilla (as OpenSmash.exe's Play); a plugin build first when the
                    turned-on mods need one (os_mods.py build)
  settings          display.ini, controllers.ini, online_server.txt, online_name.txt, online_direct.txt,
                    launcher.ini: the files and values OpenSmash.exe writes (runtime/config_folder.h's folder)
  mods              WindowSmash Mods, tools/modmanager/server.py, started with WindowSmash's Python: its
                    page opens from here, with a way back

The page works with a mouse, a keyboard (arrows, Enter, Escape) and a game pad (tools/modmanager/ui/gamepad.js:
the D-pad or left stick moves, A presses, B goes back, LB/RB change tab), so it can be used in Steam's Game
Mode as a non-Steam game. Started with a window (not --no-window), the server ends a while after its page is
closed, unless the game it started is still running.
"""
import argparse
import json
import os
import re
import secrets
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
UI = os.path.join(HERE, "ui")
SHARED_UI = os.path.join(TOOLS, "modmanager", "ui")   # (gamepad.js, icon.svg: shared with WindowSmash Mods)
HOST = "linux" if sys.platform.startswith("linux") else "macos" if sys.platform == "darwin" else "windows"
TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8", ".js": "text/javascript; charset=utf-8",
         ".svg": "image/svg+xml", ".png": "image/png", ".ico": "image/x-icon"}
PUBLIC_LOBBY = "smash.recomps.wtf:39421"   # (OpenSmash.Launcher's Public)
REPO_API = "https://api.github.com/repos/OpenSmash-Ultimate/OpenSmash-Ultimate"


class Error(Exception):
    """Something the player can act on; the message says what."""


# ---- the settings files (OpenSmash.Launcher/LauncherForm.cs's IniValue, SetIni, FirstLine, SetFirstLine) -----------
def config_file(root, name):
    """Where the game reads a settings file (runtime/config_folder.h): config/ once the game has made it
    (config/version.ini), else the folder itself; online_server.txt is config/'s only once there is one there."""
    inside = os.path.join(root, "config", name)
    if os.path.isfile(os.path.join(root, "config", "version.ini")) and (name != "online_server.txt" or os.path.isfile(inside)):
        return inside
    return os.path.join(root, name)


def read_lines(path):
    try:
        with open(path, encoding="utf-8-sig", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def write_lines(path, lines):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + ("\n" if lines else ""))
    os.replace(tmp, path)


def ini_value(path, section, key):
    """A key's value in an .ini file (section "": the keys before any section), or None."""
    in_section = section == ""
    for raw in read_lines(path):
        line = raw.strip()
        if not line or line[0] in ";#":
            continue
        if line[0] == "[":
            end = line.find("]")
            name = line[1:end].strip() if end > 1 else ""
            in_section = section != "" and name.lower() == section.lower()
            continue
        if not in_section or "=" not in line[1:]:
            continue
        k, v = line.split("=", 1)
        if k.strip().lower() != key.lower():
            continue
        v = v.strip()
        m = re.search(r"[ \t][;#]", v)
        return (v[:m.start()] if m else v).strip()
    return None


def set_ini(path, section, key, value):
    lines = read_lines(path)
    in_section = section == ""
    section_at = 0 if section == "" else -1
    key_at = -1
    for i, raw in enumerate(lines):
        line = raw.strip()
        if line.startswith("["):
            end = line.find("]")
            name = line[1:end].strip() if end > 1 else ""
            in_section = section != "" and name.lower() == section.lower()
            if in_section:
                section_at = i
            continue
        if not in_section or not line or line[0] in ";#":
            continue
        eq = line.find("=")
        if eq > 0 and line[:eq].strip().lower() == key.lower():
            key_at = i
            break
    row = f"{key} = {value}"
    if key_at >= 0:
        lines[key_at] = row
    elif section == "":
        at = next((i for i, l in enumerate(lines) if l.lstrip().startswith("[")), len(lines))
        lines.insert(at, row)
    elif 0 <= section_at < len(lines):
        lines.insert(section_at + 1, row)
    else:
        if lines and lines[-1].strip():
            lines.append("")
        lines += [f"[{section}]", row]
    write_lines(path, lines)


def first_line(path):
    for raw in read_lines(path):
        line = raw.strip()
        if line and line[0] != "#":
            return line
    return ""


def set_first_line(path, value):
    lines = read_lines(path)
    at = next((i for i, l in enumerate(lines) if l.strip() and l.strip()[0] != "#"), -1)
    if not value:
        if at >= 0:
            del lines[at]
    elif at >= 0:
        lines[at] = value
    else:
        lines.append(value)
    write_lines(path, lines)


# What the page offers, per setting: (key, label, [(value, label)]) - OpenSmash.Launcher's _settings. The renderer
# and graphics card are Windows' only (Linux draws through DXVK, macOS through Metal: runtime/renderer.h).
CHOICES = {
    "window": [("windowed", "A window"), ("borderless", "Borderless (whole monitor)"), ("fullscreen", "Full screen")],
    "window_size": [(v, v) for v in ("1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160")],
    "pacing": [("clock", "Steady 60 (any monitor)"), ("vrr", "G-Sync / FreeSync monitor"), ("display", "Follow the monitor's refresh")],
    "render_scale": [("1", "1: 1920x1080 (the game's own)"), ("auto", "Auto: as big as the window"), ("1.5", "1.5: 2880x1620"),
                     ("2", "2: 3840x2160 (4K)"), ("3", "3: 5760x3240")],
    "overlay": [("1", "Show it in the corner"), ("0", "Hidden")],
    "hide_cursor": [("1", "When it stops moving"), ("0", "Keep it shown")],
    "lobby": [("public", "Public lobby"), ("private", "Private lobby (a code)"), ("custom", "Your own server"), ("offline", "Offline")],
    "direct": [("on", "On"), ("off", "Off")],
    "keyboard_player": [(str(n), f"Player {n}") for n in range(1, 9)] + [("0", "Nobody (no keyboard)")],
    "background": [("1", "Work while another window is in front"), ("0", "Only while the game is in front")],
}
if HOST == "windows":
    CHOICES["renderer"] = [("auto", "Auto"), ("direct3d11", "Direct3D 11"), ("vulkan", "Vulkan")]


def read_settings(root):
    """The settings as OpenSmash.Launcher's ReadSettings reads them."""
    display, pads = config_file(root, "display.ini"), config_file(root, "controllers.ini")

    def d(key, fallback):
        v = ini_value(display, "", key)
        return fallback if v is None else v
    s = {"window": d("window", "windowed"), "width": d("width", "1280"), "height": d("height", "720"),
         "pacing": d("pacing", "clock"), "render_scale": d("render_scale", "1"), "overlay": d("overlay", "1"),
         "hide_cursor": d("hide_cursor", "1"), "volume": d("volume", "100"),
         "keyboard_player": ini_value(pads, "general", "keyboard_player") or "1",
         "background": ini_value(pads, "general", "background") or "1"}
    s["window_size"] = f"{s['width']}x{s['height']}"
    if HOST == "windows":
        r = d("renderer", "auto").strip().lower()
        s["renderer"] = "vulkan" if r in ("vulkan", "vk") else "direct3d11" if r in ("direct3d11", "direct3d", "dx11") else "auto"
    words = first_line(config_file(root, "online_server.txt")).split()
    host = words[0] if words else ""
    s["code"] = words[1] if len(words) > 1 else ""
    s["server"] = ""
    if not host or host.lower() == "off":
        s["lobby"] = "offline"
    elif host.lower() in ("auto", "na", "eu"):
        s["lobby"] = "public"
    elif host.lower() == PUBLIC_LOBBY:
        s["lobby"] = "private" if s["code"] else "public"
    else:
        s["lobby"], s["server"] = "custom", host
    launcher_ini = config_file(root, "launcher.ini")
    saved = {k: ini_value(launcher_ini, "opensmash", k) for k in ("lobby", "code", "server")}
    server_file = config_file(root, "online_server.txt")
    prefer_saved = os.path.isfile(launcher_ini) and os.path.getmtime(launcher_ini) >= (
        os.path.getmtime(server_file) if os.path.isfile(server_file) else 0)
    if prefer_saved and saved["lobby"] in ("public", "private", "custom", "offline"):
        s["lobby"] = saved["lobby"]
    if not s["code"] and saved["code"] is not None:
        s["code"] = saved["code"]
    if (prefer_saved or not s["server"]) and saved["server"] is not None:
        s["server"] = saved["server"]
    s["direct"] = "off" if first_line(config_file(root, "online_direct.txt")).lower() == "off" else "on"
    s["name"] = first_line(config_file(root, "online_name.txt"))
    return s


def write_settings(root, values):
    """Check and write the settings (OpenSmash.Launcher's SaveGameSettings and WriteSettingsFiles); returns them as read
    back. Raises Error for a private lobby with no code, or a server that isn't host:port."""
    s = read_settings(root)
    for k, v in values.items():
        if k in s or k in CHOICES:
            s[k] = str(v).strip()
    for k, options in CHOICES.items():
        if k in values and s[k] not in [v for v, _ in options] and k != "window_size":
            raise Error(f"{k}: {s[k]} is not one of the choices")
    if not re.fullmatch(r"\d{3,5}x\d{3,5}", s["window_size"]):
        raise Error("the window size is width x height, for example 1280x720")
    s["width"], s["height"] = s["window_size"].split("x")
    try:
        s["volume"] = str(max(0, min(100, int(float(s["volume"])))))
    except ValueError:
        raise Error("the volume is a number from 0 to 100") from None
    lobby, code, host = s["lobby"], s["code"].replace(" ", ""), s["server"].strip()
    if lobby == "private" and not code:
        raise Error("Type a lobby code: a word your friends type too.")
    if lobby == "custom" and host and not re.fullmatch(r"\S+:\d+", host):
        raise Error("Your own server is host:port, for example 192.168.1.20:39420.")
    if len(s["name"]) > 64:
        raise Error("The name is 64 letters at most.")
    server = {"offline": "off", "private": f"{PUBLIC_LOBBY} {code}",
              "custom": (host or "127.0.0.1:39420") + (f" {code}" if code else "")}.get(lobby, "auto")
    display = config_file(root, "display.ini")
    for key in ("window", "width", "height", "pacing", "render_scale", "hide_cursor", "overlay", "volume") + (
            ("renderer",) if HOST == "windows" else ()):
        set_ini(display, "", key, s[key])
    pads = config_file(root, "controllers.ini")
    set_ini(pads, "general", "keyboard_player", s["keyboard_player"])
    set_ini(pads, "general", "background", s["background"])
    set_first_line(config_file(root, "online_server.txt"), server)
    set_first_line(config_file(root, "online_name.txt"), s["name"].strip())
    set_first_line(config_file(root, "online_direct.txt"), s["direct"])
    launcher_ini = config_file(root, "launcher.ini")
    set_ini(launcher_ini, "opensmash", "lobby", lobby)
    set_ini(launcher_ini, "opensmash", "code", code)
    set_ini(launcher_ini, "opensmash", "server", host)
    return read_settings(root)


# ---- where things are -------------------------------------------------------------------------------------------
def venv_python(root):
    """WindowSmash's own Python (the setup makes it: deps/venv; macOS's installer can also unpack one into
    deps/python), or None before the setup."""
    for p in (os.path.join(root, "deps", "venv", "bin", "python"), os.path.join(root, "deps", "venv", "Scripts", "python.exe"),
              os.path.join(root, "deps", "python", "bin", "python3")):
        if os.path.isfile(p):
            return p
    return None


def game_program(root, host=HOST):
    return {"linux": os.path.join(root, "build-linux", "ssbu"),
            "macos": os.path.join(root, "build-mac", "WindowSmash.app", "Contents", "MacOS", "ssbu")}.get(
        host, os.path.join(root, "build", "ssbu.exe"))


def game_files_ready(root):
    """Whether the setup has read the game files (dump/: the program and the game data's map)."""
    return os.path.isfile(os.path.join(root, "dump", "exefs", "main")) and os.path.isfile(
        os.path.join(root, "dump", "modules", "romfs.cfg"))


def local_commit(root):
    """The commit this folder is: git's, else tools/source_version.txt's (GitHub's ZIP), else ""."""
    if os.path.exists(os.path.join(root, ".git")) and shutil.which("git"):
        try:
            out = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, capture_output=True, text=True, timeout=10)
            if out.returncode == 0 and re.fullmatch(r"[0-9a-f]{40}", out.stdout.strip()):
                return out.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    for raw in read_lines(os.path.join(root, "tools", "source_version.txt")):
        m = re.search(r"(?<![0-9a-fA-F])([0-9a-fA-F]{40})(?![0-9a-fA-F])", raw)
        if m:
            return m.group(1).lower()
    return ""


def installer_command(root, python=None):
    """The first-run setup's program on this system."""
    py = python or sys.executable
    if HOST == "linux":
        return [py, "-u", os.path.join(root, "tools", "linux", "install_linux.py")]
    if HOST == "macos":
        return [py, "-u", os.path.join(root, "tools", "mac", "install_mac.py"), "--no-wizard"]
    raise Error("On Windows, use OpenSmash.exe")


def setup_command(root, body, python=None):
    """install_linux.py / install_mac.py with the page's answers: the folders, the online name, the jobs, and
    --yes (no questions: the page asked them)."""
    cmd = installer_command(root, python) + ["--yes"]
    for key in ("game", "keys"):
        v = (body.get(key) or "").strip()
        if v:
            if not os.path.isdir(os.path.expanduser(v)):
                raise Error(f"The {key} folder isn't there: {v}")
            cmd += [f"--{key}", os.path.expanduser(v)]
    firmware = (body.get("firmware") or "").strip()
    if firmware:
        if not os.path.isdir(os.path.expanduser(firmware)):
            raise Error(f"The firmware folder isn't there: {firmware}")
        cmd += ["--firmware", os.path.expanduser(firmware)]
    elif body.get("game"):
        cmd.append("--no-firmware")   # (none chosen: the open fonts, and not asked again)
    name = (body.get("name") or "").strip()
    if name:
        cmd += ["--online-name", name[:64]]
    if body.get("jobs"):
        cmd += ["--jobs", str(int(body["jobs"]))]
    if body.get("skip_build"):
        cmd.append("--skip-build")
    if body.get("no_update", True):
        cmd.append("--no-update")   # (the page's Update does that)
    if HOST == "linux" and body.get("no_packages"):
        cmd.append("--no-packages")
    return cmd


def play_command(root, mode, host=HOST):
    """The launcher script that starts the game: online (the lobby online_server.txt names) or offline."""
    if host == "linux":
        return ["bash", os.path.join(root, "launchers", "play-ssbu-offline.sh") if mode == "offline" else os.path.join(root, "play-ssbu.sh")]
    if host == "macos":
        return ["/bin/bash", os.path.join(root, "tools", "mac", "launch.sh"), "offline" if mode == "offline" else "play"]
    raise Error("On Windows, use OpenSmash.exe")


def detect_folders(root):
    """The folders the installer would suggest (last time's, else the emulators'), each with its check."""
    sys.path.insert(0, os.path.join(TOOLS, "linux"))
    import install_linux as il
    if HOST == "macos":
        sys.path.insert(0, os.path.join(TOOLS, "mac"))
        import install_mac
        found = install_mac.detected_folders()
    else:
        found = il.detected_folders()
    return {"folders": {k: found.get(k) or "" for k in ("game", "keys", "firmware")}, "checks": check_folders(found)}


def check_folders(f):
    sys.path.insert(0, os.path.join(TOOLS, "linux"))
    import install_linux as il
    out = {}
    keys_ok, keys_say = il.check_keys(f.get("keys") or "")
    out["keys"] = {"ok": keys_ok, "say": keys_say}
    fw_ok, fw_say = il.check_firmware(f.get("firmware") or "")
    out["firmware"] = {"ok": fw_ok, "say": fw_say}
    game = f.get("game") or ""
    if not game or not os.path.isdir(game):
        out["game"] = {"ok": False, "say": "choose the folder with your game's files (.nsp, .xci or .nca, with the 13.0.5 update)"}
    else:
        n = 0
        for _dp, _dirs, files in os.walk(game):
            n += sum(1 for x in files if x.lower().endswith((".nsp", ".xci", ".nca", ".nsz", ".xcz")))
            if n > 2000:
                break
        out["game"] = {"ok": n > 0, "say": f"{n} game file(s) here (.nsp, .xci, .nca); the setup checks they are 13.0.5's"
                       if n else "there are no .nsp, .xci or .nca files here"}
    return out


def list_dir(path):
    """A folder's subfolders, for the page's folder picker (no file contents)."""
    path = os.path.abspath(os.path.expanduser(path or "~"))
    if not os.path.isdir(path):
        raise Error(f"{path} isn't a folder")
    try:
        names = sorted((n for n in os.listdir(path) if os.path.isdir(os.path.join(path, n))), key=str.lower)
    except OSError as e:
        raise Error(f"can't open {path}: {e.strerror}") from None
    files = 0
    try:
        files = sum(1 for n in os.listdir(path) if n.lower().endswith((".nsp", ".xci", ".nca", ".keys")))
    except OSError:
        pass
    shown = [n for n in names if not n.startswith(".")]
    hidden = [n for n in names if n.startswith(".") and n in (".var", ".local", ".config", ".switch")]
    return {"path": path, "parent": os.path.dirname(path) if os.path.dirname(path) != path else "", "dirs": hidden + shown,
            "files": files, "home": os.path.expanduser("~")}


# ---- slow work, polled by the page ------------------------------------------------------------------------------
PERCENT = re.compile(r"(?:^LOG percent (\d{1,3})$)|(?:\]\s+(\d{1,3})%)|(?: — (\d{1,3})%,)")


class Jobs:
    """Programs run for the page, one at a time (a build, the setup, an update): their output kept for the page."""

    def __init__(self):
        self.lock = threading.Lock()
        self.jobs = {}
        self.current = None

    def start(self, title, cmds, cwd, env=None, then=None):
        """Run each command of cmds in turn (stopping at the first that fails); then(job) after, if they worked."""
        with self.lock:
            if self.current and not self.current["done"]:
                raise Error(f"{self.current['title']} is still running")
            jid = secrets.token_hex(6)
            job = {"id": jid, "title": title, "log": [], "progress": None, "done": False, "ok": None, "error": None,
                   "started": time.time(), "result": None}
            self.jobs[jid] = job
            self.current = job

        def run():
            try:
                for cmd in cmds:
                    job["log"].append("$ " + " ".join(cmd))
                    proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", bufsize=1,
                                            start_new_session=True)
                    job["pid"] = proc.pid
                    with proc.stdout:
                        for line in proc.stdout:
                            line = line.rstrip("\r\n")
                            for part in line.split("\r"):
                                if part.strip():
                                    self.add(job, part)
                    rc = proc.wait()
                    if rc:
                        job["error"] = f"stopped (exit code {rc}): the log says why"
                        return
                if then:
                    job["result"] = then(job)
                job["ok"] = True
            except Error as e:
                job["error"] = str(e)
            except OSError as e:
                job["error"] = f"couldn't start it: {e}"
            finally:
                job["ok"] = job["error"] is None
                job["done"] = True
                if job["ok"]:
                    job["progress"] = 1.0
        threading.Thread(target=run, daemon=True).start()
        return job

    @staticmethod
    def add(job, line):
        m = PERCENT.search(line.strip())
        if m:
            job["progress"] = min(100, int(next(g for g in m.groups() if g))) / 100
        if line.startswith("LOG percent"):
            return
        if line.startswith("LOG "):
            line = line[4:]
        job["log"].append(line[:500])
        del job["log"][:-400]

    def get(self, jid):
        with self.lock:
            return self.jobs.get(jid)

    def busy(self):
        with self.lock:
            return self.current if self.current and not self.current["done"] else None

    def cancel(self, jid):
        job = self.get(jid)
        if job and not job["done"] and job.get("pid"):
            try:
                os.killpg(job["pid"], signal.SIGTERM)
            except (OSError, AttributeError):
                pass


# ---- the app ----------------------------------------------------------------------------------------------------
class App:
    def __init__(self, root, key):
        self.root = os.path.abspath(root)
        self.key = key
        self.jobs = Jobs()
        self.game = None   # (the game's launcher script, started from here)
        self.mods_server = None
        self.last_ping = time.time()
        self.bye = None
        self.update_info = None

    # the game
    def game_running(self):
        if self.game and self.game.poll() is None:
            return True
        try:
            return subprocess.run(["pgrep", "-x", "ssbu"], capture_output=True, timeout=5).returncode == 0
        except (OSError, subprocess.SubprocessError):
            return False

    def python(self):
        return venv_python(self.root) or sys.executable

    def os_mods(self, *args, timeout=600):
        """tools/os_mods.py with WindowSmash's Python: (exit code, its output)."""
        py = venv_python(self.root)
        if not py:
            raise Error("The mods need WindowSmash's Python: run the setup first")
        r = subprocess.run([py, os.path.join(self.root, "tools", "os_mods.py"), *args], cwd=self.root, capture_output=True,
                           text=True, encoding="utf-8", errors="replace", timeout=timeout, stdin=subprocess.DEVNULL)
        return r.returncode, r.stdout + r.stderr

    def mods_status(self):
        """os_mods.py status, shortened for the home page: how many mods are on, and what a Play needs first."""
        if not venv_python(self.root):
            return {"available": False}
        try:
            code, out = self.os_mods("status", timeout=120)
        except (subprocess.SubprocessError, OSError) as e:
            return {"available": False, "error": str(e)}
        line = next((l for l in reversed(out.splitlines()) if l.lstrip().startswith("{")), "")
        try:
            data = json.loads(line)
        except ValueError:
            return {"available": False, "error": out.strip()[-400:]}
        if "error" in data:
            return {"available": False, "error": data["error"]}
        mods = [m for m in data.get("mods", []) if not m["name"].startswith(":")]
        on = [m for m in mods if m.get("enabled")]
        return {"available": True, "installed": len(mods), "on": len(on), "modded": bool(data.get("modded")),
                "needs_build": data.get("needs_build") or "", "needs_build_vanilla": data.get("needs_build_vanilla") or "",
                "waiting": sorted({m.get("title") or m["name"] for m in on if m.get("code") == "waiting"}),
                "names": [m.get("title") or m["name"] for m in on][:12]}

    def state(self):
        exe = game_program(self.root)
        return {"host": HOST, "root": self.root, "built": os.path.isfile(exe), "program": os.path.relpath(exe, self.root),
                "files_ready": game_files_ready(self.root), "python": bool(venv_python(self.root)),
                "commit": local_commit(self.root)[:7], "game_running": self.game_running(),
                "job": self.jobs.busy(), "settings": read_settings(self.root), "choices": CHOICES,
                "name_set": bool(read_settings(self.root)["name"]), "update": self.update_info,
                "steam_game_mode": steam_game_mode()}

    def check_update(self):
        sys.path.insert(0, os.path.join(TOOLS, "linux"))
        import install_linux as il
        local = local_commit(self.root)
        if not local:
            info = {"state": "unknown", "say": "This folder doesn't say which version it is (no git, no tools/source_version.txt)."}
        else:
            head, err = il.api(f"{REPO_API}/commits/main")
            if not head:
                info = {"state": "error", "say": f"GitHub could not be reached: {err[1]}"}
            elif head["sha"] == local:
                info = {"state": "current", "say": f"Up to date ({local[:7]})."}
            else:
                cmp, err = il.api(f"{REPO_API}/compare/{local}...{head['sha']}")
                if not cmp:
                    info = {"state": "error", "say": "This copy's version isn't on GitHub's main (a development copy?)."}
                elif cmp.get("status") == "ahead":
                    n = int(cmp.get("ahead_by", 0))
                    titles = [c["commit"]["message"].split("\n")[0] for c in cmp.get("commits", [])]
                    titles = [t for t in reversed(titles) if not re.match(r"Merge (pull request|branch) ", t)][:6]
                    info = {"state": "available", "say": f"A newer version is on GitHub: {n} new commit{'s' if n != 1 else ''}.",
                            "titles": titles}
                elif cmp.get("status") in ("identical", "behind"):
                    info = {"state": "current", "say": f"Up to date ({local[:7]})."}
                else:
                    info = {"state": "error", "say": "This copy comes from another branch: update it with git."}
        info["checked"] = time.time()
        self.update_info = info
        return info

    def start_game(self, mode):
        if self.game_running():
            raise Error("The game is already running")
        if not os.path.isfile(game_program(self.root)):
            raise Error("The game isn't built yet: run the setup first")
        settings = read_settings(self.root)
        if mode != "offline" and settings["lobby"] != "offline" and not settings["name"]:
            # (play-ssbu.sh would ask in its terminal, which there is none of here)
            set_first_line(config_file(self.root, "online_name.txt"), f"Player{secrets.randbelow(9000) + 1000}")
        os.makedirs(os.path.join(self.root, "build"), exist_ok=True)
        out = open(os.path.join(self.root, "build", "launcher-play.log"), "w", encoding="utf-8")
        env = {**os.environ}
        self.game = subprocess.Popen(play_command(self.root, mode), cwd=self.root, stdin=subprocess.DEVNULL, stdout=out,
                                     stderr=subprocess.STDOUT, env=env)
        out.close()
        return {"started": True, "pid": self.game.pid}

    def play(self, body):
        """Play as OpenSmash.exe's: the mods applied (or the game without them), a plugin build first when one is
        needed and asked for ("build": true), then the game."""
        mode = body.get("mode", "online")
        if mode not in ("online", "offline", "vanilla"):
            raise Error("mode: online, offline or vanilla")
        if self.game_running():
            raise Error("The game is already running")
        status = self.mods_status()
        if status.get("available"):
            vanilla = mode == "vanilla"
            reason = status["needs_build_vanilla"] if vanilla else status["needs_build"]
            if reason and not body.get("build") and not body.get("anyway"):
                return {"ask_build": True, "reason": reason, "waiting": status["waiting"]}
            cmds = []
            py = venv_python(self.root)
            os_mods = os.path.join(self.root, "tools", "os_mods.py")
            if vanilla:
                cmds.append([py, "-u", os_mods, "vanilla"])
            elif status["on"] and not status["modded"]:
                cmds.append([py, "-u", os_mods, "apply"])
            if reason and body.get("build"):
                cmds.append([py, "-u", os_mods, "build"])
            if cmds:
                game_mode = "online" if mode == "vanilla" else mode
                job = self.jobs.start("Getting the mods ready" if not body.get("build") else "Building the mods' plugins into the game",
                                      cmds, self.root, then=lambda job: self.start_game(game_mode))
                return {"job": job}
        return self.start_game("online" if mode == "vanilla" else mode)

    def open_mods(self, back):
        """WindowSmash Mods' server (one per folder: a running one is reused), its page's address with a way back here."""
        py = venv_python(self.root)
        if not py:
            raise Error("The mod manager needs WindowSmash's Python: run the setup first")
        info = None
        try:
            with open(os.path.join(self.root, "dump", "mods", "server.json"), encoding="utf-8") as f:
                info = json.load(f)
            u = urllib.parse.urlparse(info["url"])
            import urllib.request
            req = urllib.request.Request(f"{u.scheme}://{u.netloc}/api/ping", headers={
                "X-Key": urllib.parse.parse_qs(u.query)["key"][0]})
            with urllib.request.urlopen(req, timeout=3) as r:
                if r.status != 200:
                    info = None
        except (OSError, ValueError, KeyError, IndexError):
            info = None
        if not info:
            self.mods_server = subprocess.Popen([py, os.path.join(self.root, "tools", "modmanager", "server.py"), "--tree",
                                                 self.root, "--no-window"], cwd=self.root, stdin=subprocess.DEVNULL,
                                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            line = self.mods_server.stdout.readline()
            try:
                info = json.loads(line)
            except ValueError:
                raise Error("WindowSmash Mods did not start: dump/mods/manager.log says why") from None
        url = info["url"]
        if back:
            url += "&back=" + urllib.parse.quote(back, safe="")
        return {"url": url}

    def api(self, method, path, q, body):
        if path == "/api/ping":
            self.last_ping, self.bye = time.time(), None
            return {"ok": True}
        if path == "/api/bye":
            self.bye = time.time()
            return {"ok": True}
        if path == "/api/state":
            return self.state()
        if path == "/api/mods/status":
            return self.mods_status()
        if path.startswith("/api/job/"):
            job = self.jobs.get(path.rsplit("/", 1)[1])
            if not job:
                raise Error("no such job")
            return job
        if path == "/api/game":
            return {"running": self.game_running(), "log": read_lines(os.path.join(self.root, "build", "play.log"))[-30:]
                    or read_lines(os.path.join(self.root, "build", "launcher-play.log"))[-30:]}
        if path == "/api/setup/folders":
            return detect_folders(self.root)
        if path == "/api/browse":
            return list_dir(q.get("path") or "~")
        if path == "/api/update" and method == "GET":
            return self.check_update()
        if method != "POST":
            raise Error(f"unknown {path}")
        if path == "/api/settings":
            return write_settings(self.root, body)
        if path == "/api/setup/check":
            return {"checks": check_folders({k: os.path.expanduser(body.get(k) or "") for k in ("game", "keys", "firmware")})}
        if path == "/api/setup/run":
            if self.game_running():
                raise Error("Close the game first")
            env = {**os.environ, "PYTHONUNBUFFERED": "1"}
            askpass = os.path.join(TOOLS, "linux", "askpass.sh")
            if HOST == "linux" and os.path.isfile(askpass):
                env.setdefault("SUDO_ASKPASS", askpass)   # (sudo's password, in a dialog: no terminal here)
            return self.jobs.start("Setting up and building the game", [setup_command(self.root, body)], self.root, env)
        if path == "/api/build":
            if self.game_running():
                raise Error("Close the game first")
            py = venv_python(self.root)
            if not py:
                raise Error("Run the setup first")
            cmd = [py, "-u", os.path.join(self.root, "tools", "build_game.py")]
            if body.get("jobs"):
                cmd += ["--jobs", str(int(body["jobs"]))]
            return self.jobs.start("Building the game", [cmd], self.root, {**os.environ, "PYTHONUNBUFFERED": "1"})
        if path == "/api/rebuild-all":
            if self.game_running():
                raise Error("Close the game first")
            if not venv_python(self.root):
                raise Error("Run the setup first")
            if HOST == "windows":
                script = os.path.join(self.root, "tools", "rebuild-all.ps1")
                cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script]
                if body.get("jobs"):
                    cmd += ["-Jobs", str(int(body["jobs"]))]
            else:
                if HOST == "linux":
                    script = os.path.join(self.root, "tools", "linux", "rebuild-all.sh")
                else:
                    script = os.path.join(self.root, "tools", "mac", "rebuild-all.command")
                cmd = ["bash", script]
                if body.get("jobs"):
                    cmd.append("--jobs")
                    cmd.append(str(int(body["jobs"])))
            return self.jobs.start("Building the game and launcher", [cmd], self.root,
                                   {**os.environ, "PYTHONUNBUFFERED": "1"})
        if path == "/api/update":
            if self.game_running():
                raise Error("Close the game first")
            cmd = installer_command(self.root) + ["--update-only"]

            def then(job):
                self.update_info = None
                return {"updated": True}
            return self.jobs.start("Updating OpenSmash", [cmd], self.root, {**os.environ, "PYTHONUNBUFFERED": "1"}, then)
        if path == "/api/job/cancel":
            self.jobs.cancel(body.get("id", ""))
            return {"ok": True}
        if path == "/api/play":
            return self.play(body)
        if path == "/api/mods/build":
            if self.game_running():
                raise Error("Close the game first")
            py = venv_python(self.root)
            if not py:
                raise Error("Run the setup first")
            return self.jobs.start("Building the mods' plugins into the game",
                                   [[py, "-u", os.path.join(self.root, "tools", "os_mods.py"), "build"]], self.root)
        if path == "/api/mods/open":
            return self.open_mods(body.get("back") or "")
        if path == "/api/quit":
            threading.Timer(0.5, lambda: os._exit(0)).start()
            return {"ok": True}
        raise Error(f"unknown {path}")


def steam_game_mode(env=None):
    """Whether this runs in Steam's Game Mode (gamescope's session): the page then fills the screen."""
    env = os.environ if env is None else env
    return bool(env.get("SteamGamepadUI") or env.get("GAMESCOPE_WAYLAND_DISPLAY")
                or (env.get("XDG_CURRENT_DESKTOP") or "").lower() == "gamescope")


def make_handler(app):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
            data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False, default=str).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(data)

        def local_host(self):
            """Only this computer's own name for the server (a page elsewhere renaming itself 127.0.0.1 - DNS
            rebinding - gets nothing)."""
            host = (self.headers.get("Host") or "").rsplit(":", 1)[0].strip("[]")
            return host in ("127.0.0.1", "localhost", "::1")

        def authorized(self, q, method):
            if method == "POST":   # (a page elsewhere can't set this header: no cross-site POST does anything)
                return self.headers.get("X-Key") == app.key
            cookie = self.headers.get("Cookie") or ""
            return self.headers.get("X-Key") == app.key or q.get("key") == app.key or f"oslk={app.key}" in cookie

        def static(self, folder, rel):
            full = os.path.normpath(os.path.join(folder, urllib.parse.unquote(rel).lstrip("/\\")))
            if not full.startswith(folder + os.sep) or not os.path.isfile(full):   # (normalised: no ../ out of it)
                return self.send(404, {"error": "not found"})
            with open(full, "rb") as f:
                return self.send(200, f.read(), TYPES.get(os.path.splitext(full)[1], "application/octet-stream"))

        def handle_any(self, method):
            u = urllib.parse.urlparse(self.path)
            q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
            path = u.path
            raw = b""
            if method == "POST":   # (read first, even to refuse it: what's left would be read as the next request)
                n = int(self.headers.get("Content-Length") or 0)
                if n > 1 << 20:
                    self.close_connection = True
                    return self.send(413, {"error": "too big"})
                raw = self.rfile.read(n) if n > 0 else b""
            if not self.local_host():
                return self.send(403, {"error": "not this computer's page"})
            if not self.authorized(q, method):
                return self.send(403, {"error": "not this launcher's page"})
            if path in ("/", "/index.html"):
                with open(os.path.join(UI, "index.html"), "rb") as f:
                    return self.send(200, f.read(), TYPES[".html"],
                                     {"Set-Cookie": f"oslk={app.key}; Path=/; SameSite=Strict; HttpOnly"})
            if path.startswith("/ui/shared/"):
                return self.static(SHARED_UI, path[len("/ui/shared/"):])
            if path.startswith("/ui/"):
                return self.static(UI, path[4:])
            if path.startswith("/api/"):
                body = {}
                if method == "POST":
                    try:
                        body = json.loads(raw or b"{}")
                    except ValueError:
                        return self.send(400, {"error": "bad request (not JSON)"})
                try:
                    return self.send(200, app.api(method, path, q, body))
                except Error as e:
                    return self.send(400, {"error": str(e)})
                except (KeyError, ValueError, TypeError) as e:
                    return self.send(400, {"error": f"bad request ({e})"})
                except (OSError, subprocess.SubprocessError) as e:
                    return self.send(500, {"error": f"{type(e).__name__}: {e}"})
            return self.send(404, {"error": "not found"})

        def do_GET(self):
            self.handle_any("GET")

        def do_POST(self):
            self.handle_any("POST")
    return Handler


# ---- the window -------------------------------------------------------------------------------------------------
def open_window(url, fullscreen=False):
    """The page as a window of its own (tools/app_window.py), else in the default browser."""
    sys.path.insert(0, TOOLS)
    import app_window
    cmd = app_window.command(url, HOST, fullscreen, size="1280,800")
    if cmd:
        try:
            subprocess.Popen(cmd, start_new_session=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
            return
        except OSError:
            pass
    webbrowser.open(url)


def install_desktop_entry(root):
    """~/.local/share/applications/opensmash-launcher.desktop (install_linux.py's), written when it says something
    else (the folder moved): from the applications menu, Steam's Add a Non-Steam Game finds it."""
    sys.path.insert(0, os.path.join(TOOLS, "linux"))
    import install_linux as il
    apps = os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "applications")
    path = os.path.join(apps, "opensmash-launcher.desktop")
    try:
        il.write_desktop_entry(path, il.launcher_desktop_entry(root))
    except OSError:
        return None
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tree", default=os.path.dirname(TOOLS))
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--no-window", action="store_true", help="print the page's address only")
    ap.add_argument("--fullscreen", action="store_true", help="the page full screen (on by itself in Steam's Game Mode)")
    a = ap.parse_args(argv)
    root = os.path.abspath(a.tree)
    if HOST == "windows":
        print("On Windows, OpenSmash.exe is the launcher.")
    if HOST == "linux":
        install_desktop_entry(root)
    app = App(root, secrets.token_urlsafe(18))
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(app))
    url = f"http://127.0.0.1:{srv.server_address[1]}/?key={app.key}"
    print(json.dumps({"url": url}), flush=True)
    if not a.no_window:
        open_window(url, a.fullscreen or steam_game_mode())

        def watchdog():   # (the page closed: it said bye, or nothing pings; not while a job or the game runs)
            while True:
                time.sleep(5)
                if app.jobs.busy() or app.game_running():
                    app.last_ping = max(app.last_ping, time.time() - 60)
                    continue
                if app.bye and time.time() - app.bye > 20 or time.time() - app.last_ping > 180:
                    os._exit(0)
        threading.Thread(target=watchdog, daemon=True).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
