"""How a local page (WindowSmash Mods, the OpenSmash Launcher for Linux and macOS) opens as a window of its own on
Linux and macOS: a Chromium-family browser's app mode (--app=: no address bar, no tabs), installed from the
distribution or as a Flatpak (Bazzite's, SteamOS's and Fedora Atomic's browsers are Flatpaks, not on PATH); full
screen in Steam's Game Mode, where Firefox's kiosk mode will do too. None: the default browser, in a tab
(webbrowser.open). Python's standard library only: the launcher uses it before the setup has installed anything."""
import os
import shutil
import subprocess

LINUX_APP_BROWSERS = ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "brave-browser",
                      "microsoft-edge", "microsoft-edge-stable", "vivaldi")
LINUX_APP_FLATPAKS = ("com.google.Chrome", "org.chromium.Chromium", "com.brave.Browser", "com.microsoft.Edge",
                      "io.github.ungoogled_software.ungoogled_chromium")
MAC_APP_BROWSERS = ("/Applications/Google Chrome.app", "/Applications/Microsoft Edge.app", "/Applications/Brave Browser.app",
                    "/Applications/Chromium.app")


def installed_flatpaks(which=shutil.which):
    """The Flatpak apps installed (for the user or the system), or none without Flatpak."""
    if not which("flatpak"):
        return set()
    try:
        out = subprocess.run(["flatpak", "list", "--app", "--columns=application"], capture_output=True, text=True,
                             timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return set()
    return {line.strip() for line in out.splitlines() if line.strip()}


def linux_app_browser(which=shutil.which, flatpaks=None):
    """The command that starts a Chromium-family browser here (to add --app= to), or None."""
    for exe in LINUX_APP_BROWSERS:
        if which(exe):
            return [exe]
    flatpaks = installed_flatpaks(which) if flatpaks is None else flatpaks
    for app in LINUX_APP_FLATPAKS:
        if app in flatpaks:
            return ["flatpak", "run", app]
    return None


def command(url, host, fullscreen=False, size="1280,820", which=shutil.which, flatpaks=None, isdir=os.path.isdir):
    """The command that shows `url` as a window of its own, or None (the default browser)."""
    if host == "linux":
        flatpaks = installed_flatpaks(which) if flatpaks is None else flatpaks
        app = linux_app_browser(which, flatpaks)
        if app:
            return [*app, f"--app={url}", f"--window-size={size}"] + (["--start-fullscreen"] if fullscreen else [])
        if fullscreen:
            if which("firefox"):
                return ["firefox", "--kiosk", url]
            if "org.mozilla.firefox" in flatpaks:
                return ["flatpak", "run", "org.mozilla.firefox", "--kiosk", url]
        return None
    if host == "macos":
        for app in MAC_APP_BROWSERS:
            if isdir(app):
                return ["open", "-na", app, "--args", f"--app={url}", f"--window-size={size}"]
    return None
