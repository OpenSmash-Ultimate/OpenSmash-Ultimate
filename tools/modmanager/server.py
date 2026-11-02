#!/usr/bin/env python3
"""WindowSmash Mods: the mod manager's app, for Windows and macOS - a small web server on this
computer (127.0.0.1 only, a random port and key) that the window talks to, over tools/mods.py.

Usage:
  server.py [--tree <port folder>] [--no-window] [--port N]

The window: on macOS the WindowSmash Mods app (tools/mac/ModsShell.swift, a web view) starts this
and shows its page; elsewhere this opens it in Edge's or Chrome's app mode (a window of its own),
else the default browser. Started that way, the server ends when the window has been closed for a
while (the page pings it every few seconds).

The page (ui/) asks /api/...: the installed mods, GameBanana's catalog (fetched here: the page
can't ask GameBanana itself), installing (from GameBanana, a link, or a file dropped on the
window), turning mods on and off, their order, applying them, modpacks (packs.py) and the clash
check. Slow work (downloads, applying) runs as a job the page polls for its progress.
"""
import argparse
import contextlib
import hashlib
import io
import html
import json
import os
import re
import secrets
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import mods  # noqa: E402
import skyline  # noqa: E402
import packs  # noqa: E402
import describe  # noqa: E402
import std_plugins  # noqa: E402

UI = os.path.join(HERE, "ui")
GAME = 6498           # GameBanana's Super Smash Bros. Ultimate
SKINS = 3330          # its Skins category: a subcategory for each fighter
SORTS = {"liked": "Generic_MostLiked", "downloaded": "Generic_MostDownloaded", "newest": "Generic_Newest",
         "updated": "Generic_LatestUpdated"}
TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8", ".js": "text/javascript; charset=utf-8",
         ".svg": "image/svg+xml", ".png": "image/png", ".ico": "image/x-icon", ".woff2": "font/woff2"}


# ---- slow work, polled by the page -----------------------------------------------------------
LOG = None   # dump/mods/manager.log: what the jobs did, and any error's traceback (main() opens it)


def note(text):
    if LOG:
        try:
            LOG.write(time.strftime("%Y-%m-%d %H:%M:%S ") + str(text).rstrip() + "\n")
            LOG.flush()
        except (OSError, ValueError):
            pass


class Jobs:
    def __init__(self):
        self.lock = threading.Lock()
        self.jobs = {}
        self.busy = threading.Lock()   # (one change to the mods at a time)

    def start(self, title, fn):
        jid = secrets.token_hex(6)
        job = {"id": jid, "title": title, "log": [], "progress": None, "done": False, "error": None, "result": None,
               "started": time.time()}
        with self.lock:
            self.jobs[jid] = job

        def log(text):
            text = str(text)
            job["log"].append(text)
            del job["log"][:-200]
            step = re.match(r"^(.*) — (\d+)%, (.*)$", text)
            if step:
                job["detail"] = step.group(1)
                job["eta"] = step.group(3)
                job["progress"] = int(step.group(2)) / 100
            elif text and not text.startswith("  "):
                job["detail"] = text
            note(f"[{title}] {text}")

        def progress(done, total=None):
            job["progress"] = (done / total) if total else done

        def run():
            with self.busy:
                try:
                    job["result"] = fn(log, progress)
                except (mods.Error, packs.PackError) as e:
                    job["error"] = str(e)
                except (OSError, urllib.error.URLError, ValueError) as e:
                    job["error"] = f"{type(e).__name__}: {e}"
                except SystemExit as e:
                    job["error"] = str(e)
                except Exception as e:   # (shown, not swallowed: a bug to report)
                    job["error"] = f"{type(e).__name__}: {e}"
                    note(traceback.format_exc())
                finally:
                    job["done"] = True
                    job["seconds"] = round(time.time() - job["started"])
                    note(f"[{title}] " + (f"failed: {job['error']}" if job["error"] else f"done in {job['seconds']} s"))
                    job["progress"] = 1.0 if not job["error"] else job["progress"]
        threading.Thread(target=run, daemon=True).start()
        return job

    def get(self, jid):
        with self.lock:
            return self.jobs.get(jid)

    def active(self):
        with self.lock:
            return [j for j in self.jobs.values() if not j["done"]]


# ---- GameBanana ------------------------------------------------------------------------------
def gb(path):
    data, _ctx = mods.gamebanana_get(path)
    return data


def gb_image(media, big=False):
    images = (media or {}).get("_aImages") or []
    if not images:
        return ""
    i = images[0]
    f = i.get("_sFile") if big else (i.get("_sFile530") or i.get("_sFile220") or i.get("_sFile"))
    return f"{i['_sBaseUrl']}/{f}"


def gb_card(r):
    return {"id": r.get("_idRow"), "name": html.unescape(r.get("_sName") or "?"), "image": gb_image(r.get("_aPreviewMedia")),
            "author": (r.get("_aSubmitter") or {}).get("_sName", ""),
            "category": (r.get("_aRootCategory") or {}).get("_sName", ""),
            "likes": r.get("_nLikeCount") or 0, "views": r.get("_nViewCount") or 0,
            "nsfw": bool(r.get("_bHasContentRatings")), "updated": r.get("_tsDateUpdated") or r.get("_tsDateAdded") or 0,
            "online": packs.listing_safety(root_category=(r.get("_aRootCategory") or {}).get("_sName", ""),
                                           text=html.unescape(r.get("_sName") or ""))}


def gb_browse(q):
    page = max(1, int(q.get("page", 1)))
    search = q.get("q", "").strip()
    cat = q.get("category")
    sort = SORTS.get(q.get("sort", "liked"), SORTS["liked"])
    if search:
        path = (f"/Util/Search/Results?_sModelName=Mod&_sOrder=best_match&_idGameRow={GAME}"
                f"&_sSearchString={urllib.parse.quote(search)}&_nPage={page}")
    elif cat:
        path = f"/Mod/Index?_nPage={page}&_nPerpage=24&_aFilters%5BGeneric_Category%5D={int(cat)}&_sSort={sort}"
    else:
        path = f"/Mod/Index?_nPage={page}&_nPerpage=24&_aFilters%5BGeneric_Game%5D={GAME}&_sSort={sort}"
    data = gb(path)
    recs = [r for r in data.get("_aRecords") or [] if r.get("_sModelName", "Mod") == "Mod"]
    meta = data.get("_aMetadata") or {}
    return {"mods": [gb_card(r) for r in recs], "more": not meta.get("_bIsComplete", True) and bool(recs)}


def gb_categories(q):
    parent = q.get("parent")
    path = (f"/Mod/Categories?_idCategoryRow={int(parent)}&_sSort=a_to_z&_bShowEmpty=false" if parent
            else f"/Mod/Categories?_idGameRow={GAME}&_sSort=a_to_z&_bShowEmpty=false")
    return [{"id": c["_idRow"], "name": c["_sName"], "count": c.get("_nItemCount", 0),
             "icon": c.get("_sIconUrl", "")} for c in gb(path) if c.get("_idRow")]


def plain(text):
    s = re.sub(r"<br\s*/?>|</p>|</li>|</h\d>", "\n", text or "")
    s = re.sub(r"<[^>]+>", "", s)
    return re.sub(r"\n{3,}", "\n\n", html.unescape(s)).strip()


def gb_mod(mid):
    d = gb(f"/Mod/{int(mid)}?_csvProperties=_sName,_sText,_aFiles,_aPreviewMedia,_nDownloadCount,_aSubmitter,"
           f"_aCategory,_aSuperCategory,_nLikeCount,_nViewCount,_tsDateUpdated,_aGame")
    images = [f"{i['_sBaseUrl']}/{i.get('_sFile530') or i.get('_sFile')}" for i in ((d.get("_aPreviewMedia") or {}).get("_aImages") or [])]
    files = [{"id": f["_idRow"], "name": f.get("_sFile", "?"), "size": f.get("_nFilesize", 0),
              "downloads": f.get("_nDownloadCount", 0), "scan": f.get("_sAvResult") or "",
              "description": f.get("_sDescription") or "", "date": f.get("_tsDateAdded", 0),
              "online": packs.listing_safety(text=(f.get("_sFile") or "") + " " + (f.get("_sDescription") or ""))}
             for f in d.get("_aFiles") or []]
    game = (d.get("_aGame") or {}).get("_idRow")
    text = plain(d.get("_sText"))
    online = packs.listing_safety((d.get("_aCategory") or {}).get("_sName", ""), (d.get("_sName") or "") + "\n" + text,
                                  (d.get("_aSuperCategory") or {}).get("_sName", ""))
    return {"id": int(mid), "name": html.unescape(d.get("_sName") or "?"), "text": plain(d.get("_sText"))[:6000],
            "images": images[:12], "files": files, "downloads": d.get("_nDownloadCount", 0),
            "likes": d.get("_nLikeCount", 0), "views": d.get("_nViewCount", 0),
            "author": (d.get("_aSubmitter") or {}).get("_sName", ""),
            "category": (d.get("_aCategory") or {}).get("_sName", ""),
            "smash": game in (None, GAME), "url": f"https://gamebanana.com/mods/{int(mid)}", "online": online,
            "works": packs.listing_works((d.get("_aCategory") or {}).get("_sName", ""), (d.get("_sName") or "") + "\n" + text,
                                         (d.get("_aSuperCategory") or {}).get("_sName", ""))}


def pack_file(entry):
    """A modpack's GameBanana mod's file id, by its file name (packs made before ids were kept)."""
    if not entry.get("file_name"):
        return None
    for f in gb_mod(entry["gamebanana"])["files"]:
        if f["name"] == entry["file_name"]:
            return f["id"]
    return None


GB_LINK = re.compile(r"gamebanana\.com/(?:mods|dl|mmdl)/(\d+)")


# ---- installing ------------------------------------------------------------------------------
def download(url, progress, log):
    """A link's file into a temporary folder: (path, its folder to remove after)."""
    m = re.search(r"drive\.google\.com/(?:file/d/|open\?id=|uc\?id=)([\w-]+)", url)
    if m:   # (Google Drive's page, as its direct download)
        url = f"https://drive.usercontent.google.com/download?id={m.group(1)}&export=download&confirm=t"
    m = re.search(r"dropbox\.com/.+", url)
    if m:
        url = re.sub(r"[?&]dl=0", "", url) + ("&" if "?" in url else "?") + "dl=1"
    req = urllib.request.Request(url, headers={"User-Agent": "WindowSmash mod manager"})
    tmp = tempfile.mkdtemp(prefix="windowsmash-dl-")
    try:
        ctx = ssl.create_default_context()
        try:
            import certifi   # (python.org's Python has no certificates of its own until it is told)
            ctx = ssl.create_default_context(cafile=certifi.where())
        except ImportError:
            pass
        with urllib.request.urlopen(req, timeout=60, context=ctx) as r:
            name = None
            cd = r.headers.get("Content-Disposition") or ""
            m = re.search(r"filename\*?=(?:UTF-8'')?\"?([^\";]+)", cd)
            if m:
                name = urllib.parse.unquote(m.group(1))
            name = os.path.basename(name or urllib.parse.urlparse(r.url).path) or "mod.zip"
            if r.headers.get("Content-Type", "").startswith("text/html"):
                raise mods.Error("that link opens a web page, not a file: use the link to the download itself")
            total = int(r.headers.get("Content-Length") or 0)
            target = os.path.join(tmp, mods.safe_name(name))
            done = 0
            with open(target, "wb") as out:
                while chunk := r.read(1 << 20):
                    out.write(chunk)
                    done += len(chunk)
                    progress(done, total) if total else None
        log(f"downloaded {name} ({done / 1048576:.1f} MB)")
        return target, tmp
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise


def install_url(tree, url, log, progress, meta=None):
    m = GB_LINK.search(url)
    if m:
        return mods.gamebanana_install(tree, int(m.group(1)), None, progress)
    path, tmp = download(url, progress, log)
    try:
        return mods.install(tree, path, None, dict(meta or {}, url=url))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def game_running():
    try:
        if sys.platform == "darwin":
            return subprocess.run(["pgrep", "-f", "MacOS/ssbu"], capture_output=True).returncode == 0
        if os.name == "nt":
            r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ssbu.exe", "/NH"], capture_output=True, text=True)
            return "ssbu.exe" in r.stdout.lower()
    except OSError:
        pass
    return False


def fetch_standard(root, log, every=False):
    """The community plugins that run here (tools/std_plugins.py: their official releases, checked;
    the CSK Collection ported to 13.0.5), the missing ones - or, `every`, all of them again."""
    for name, p in std_plugins.PLUGINS.items():
        if p[5] == "runs" and (every or not os.path.exists(os.path.join(std_plugins.folder(root), name))):
            try:
                std_plugins.fetch(root, name, log)
            except SystemExit as e:   # (std_plugins.py is a command too: its errors end it)
                raise mods.Error(str(e))


# ---- the server ------------------------------------------------------------------------------

def has_new_slots(folder):
    """Whether a mod adds costume slots (its config.json's new directories; a config.json can hold only an
    unshare-blacklist)."""
    try:
        with open(os.path.join(folder, "config.json"), encoding="utf-8-sig") as f:
            c = json.load(f)
    except (OSError, ValueError):
        return False
    return bool(c.get("new-dir-infos") or c.get("new-dir-infos-base"))

class App:
    def __init__(self, tree, key, watch):
        self.tree = mods.Tree(tree)
        self.key = key
        self.jobs = Jobs()
        self.last_ping = time.time()
        self.bye = None
        self.watch = watch

    def settings(self):
        """The app's settings (mods/.settings.json): theme dark (the default), light or system."""
        try:
            with open(os.path.join(self.tree.mods, ".settings.json"), encoding="utf-8") as f:
                s = json.load(f)
        except (OSError, ValueError):
            s = {}
        s.setdefault("theme", "dark")
        return s

    def library(self):
        out = mods.listing(self.tree)
        for m in out["mods"]:
            folder = os.path.join(self.tree.mods, m["name"])
            for img in ("preview.webp", "preview.png", "preview.jpg"):
                if os.path.exists(os.path.join(folder, img)):
                    m["local_image"] = f"/local/{urllib.parse.quote(m['name'])}/{img}"
                    break
            m["new_slots"] = has_new_slots(folder)
            m["online"] = packs.online_safety(folder)
            m["plugins"] = mods.plugins(folder)
            m["plugin_state"] = skyline.plugin_state(self.tree.root, m["name"], m["plugins"]) if m["plugins"] else []
            m["about"] = describe.describe(folder)
        # parts of one download (a moveset and its Assist Trophy, a pack's characters): shown together
        out["groups"] = {}
        for name, key in describe.groups(out["mods"]).items():
            m = next(x for x in out["mods"] if x["name"] == name)
            m["group"], m["part"] = key, describe.part_label(name, m.get("title"))
            g = out["groups"].setdefault(key, {"title": (m.get("title") if key.startswith("gb:") else None)
                                               or name.split(" - ", 1)[0], "image": None})
            g["image"] = g["image"] or m.get("image") or m.get("local_image")
        out["unsafe_on"] = [m.get("title") or m["name"] for m in out["mods"] if m["enabled"] and m["online"]["level"] == "unsafe"]
        out["same_on"] = [m.get("title") or m["name"] for m in out["mods"] if m["enabled"] and m["online"]["level"] == "same"]
        out["unbuilt"] = [m.get("title") or m["name"] for m in out["mods"] if m["enabled"] and any(not p["built"] for p in m["plugin_state"])]
        out["game_running"] = game_running()
        out["root"] = self.tree.root
        out["platform"] = sys.platform
        out["log"] = os.path.join(self.tree.root, "dump", "mods", "manager.log")
        return out

    def api(self, method, path, q, body):
        t = self.tree
        if path == "/api/ping":
            self.last_ping = time.time()
            self.bye = None
            return {"ok": True}
        if path == "/api/bye":   # (the window is closing: the server ends soon after, unless it pings again - a reload)
            self.bye = time.time()
            return {"ok": True}
        if path == "/api/library":
            return self.library()
        if path == "/api/jobs":
            return {"jobs": self.jobs.active()}
        if path.startswith("/api/job/"):
            j = self.jobs.get(path.rsplit("/", 1)[1])
            if not j:
                raise mods.Error("no such job")
            return j
        # GameBanana
        if path == "/api/gb/browse":
            return gb_browse(q)
        if path == "/api/gb/categories":
            return gb_categories(q)
        if path == "/api/gb/mod":
            return gb_mod(q["id"])
        if path == "/api/settings" and method == "GET":
            return self.settings()
        if path == "/api/packs":
            out = packs.saved_packs(t)
            for p in out:
                p["unsafe"] = [m.get("title") or m["name"] for m in p["mods"] if (n := packs.local_name(t, m))
                               and packs.online_safety(os.path.join(t.mods, n))["level"] == "unsafe"]
            return {"packs": out}
        if path == "/api/plugins/standard":   # (the community plugins: version, fetched, whether they run here)
            return {"plugins": [{"name": n, "version": v, "have": os.path.exists(os.path.join(std_plugins.folder(t.root), n)),
                                 "status": st, "source": src, "ported": n in std_plugins.PORTS}
                                for n, (v, _u, _m, _s, src, st) in std_plugins.PLUGINS.items()]}
        if method != "POST":
            raise mods.Error(f"unknown {path}")
        # changes
        if path == "/api/enable":
            state = t.load()
            for n in body["names"]:
                if n in state["mods"]:
                    state["mods"][n]["enabled"] = bool(body["enabled"])
            t.save(state)
            return self.library()
        if path == "/api/order":
            state = t.load()
            names = [n for n in body["names"] if n in state["mods"]]
            state["order"] = names + [n for n in state["order"] if n not in names]
            t.save(state)
            return self.library()
        if path == "/api/remove":
            for n in body["names"]:
                folder = os.path.join(t.mods, mods.safe_name(n))
                if n == mods.safe_name(n) and (os.path.isdir(folder) or os.path.islink(folder)):
                    mods.remove_folder(folder)
            t.save(t.load())
            return self.library()
        if path == "/api/rename":
            state = t.load()
            n = body["name"]
            if n in state["mods"]:
                state["mods"][n]["title"] = str(body.get("title", ""))[:120]
                t.save(state)
            return self.library()
        if path == "/api/check":
            names = body.get("names")
            if names is None:
                state = t.load()
                names = [n for n in state["order"] if state["mods"][n].get("enabled")]
            return {"issues": packs.check(t, names)}
        if path == "/api/apply":
            if game_running():
                raise mods.Error("the game is open: close it first (it reads the mods while it runs)")
            return self.jobs.start("Applying mods", lambda log, progress: mods.apply(t, log))
        if path == "/api/plugins/build":   # (the turned-on mods' Skyline plugins compiled into the game)
            if game_running():
                raise mods.Error("the game is open: close it first (its program is rebuilt)")

            def build(log, progress):
                fetch_standard(t.root, log)   # (the community plugins movesets need, if one is missing)
                mods.apply(t, log)   # (lists the plugins to build, and their select-screen entries)
                if skyline.build(t.root, 3, log):
                    raise mods.Error("the game's build failed: see the log")
            return self.jobs.start("Building the plugins into the game", build)
        if path == "/api/plugins/fetch":
            return self.jobs.start("Getting the community plugins", lambda log, progress: fetch_standard(t.root, log, every=True))
        if path == "/api/vanilla":
            if game_running():
                raise mods.Error("the game is open: close it first")

            return self.jobs.start("Turning mods off in the game", lambda log, progress: mods.play_vanilla(t, log))
        if path == "/api/install/gamebanana":
            mid, fid = int(body["id"]), body.get("file")
            return self.jobs.start(body.get("title") or f"GameBanana mod {mid}",
                                   lambda log, progress: {"installed": mods.gamebanana_install(t, mid, fid, progress)})
        if path == "/api/install/url":
            url = body["url"].strip()
            if not re.match(r"https?://", url):
                raise mods.Error("that isn't a web link (it starts with http:// or https://)")
            return self.jobs.start("Installing from a link", lambda log, progress: {"installed": install_url(t, url, log, progress)})
        if path == "/api/install/path":
            p = body["path"]
            if not os.path.exists(p):
                raise mods.Error(f"{p} isn't there")
            return self.jobs.start(f"Installing {os.path.basename(p)}", lambda log, progress: {"installed": mods.install(t, p)})
        # modpacks
        if path == "/api/pack/save":
            pack = packs.pack_from_mods(t, body["names"], body.get("name") or "My modpack", body.get("author", ""),
                                        body.get("description", ""))
            pid = packs.save_pack(t, pack)
            return {"id": pid, "packs": packs.saved_packs(t)}
        if path == "/api/settings":
            s = self.settings()
            if body.get("theme") in ("dark", "light", "system"):
                s["theme"] = body["theme"]
            os.makedirs(t.mods, exist_ok=True)
            with open(os.path.join(t.mods, ".settings.json"), "w", encoding="utf-8") as f:
                json.dump(s, f, indent=1)
            return s
        if path == "/api/installer":
            import make_installer
            pid, bundle = body.get("id"), bool(body.get("bundle_all"))

            def make(log, progress):
                out = make_installer.build(t, pid, bundle, os.path.join(os.path.expanduser("~"), "Downloads"), log)
                reveal(out)
                return {"path": out, "size": os.path.getsize(out)}
            return self.jobs.start("Making the installer", make)
        if path == "/api/uninstall":   # (WindowSmash as it was: install_mods.py --uninstall)
            if game_running():
                raise mods.Error("the game is open: close it first")
            import install_mods
            delete = bool(body.get("delete_mods"))

            def gone(log, progress):
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    code = install_mods.uninstall(t.root, delete)
                for line in out.getvalue().splitlines():
                    log(line.strip())
                if code:
                    raise mods.Error(out.getvalue().strip().splitlines()[-1])
                threading.Timer(8, lambda: os._exit(0)).start()   # (nothing left to serve)
                return {"done": True}
            return self.jobs.start("Removing WindowSmash Mods", gone)
        if path == "/api/pack/delete":
            packs.delete_pack(t, body["id"])
            return {"packs": packs.saved_packs(t)}
        if path == "/api/pack/read":
            pack = packs.from_code(body["code"]) if body.get("code") else packs.validate(body["pack"])
            return {"pack": pack, "missing": packs.missing(t, pack), "code": packs.to_code(pack)}
        if path == "/api/pack/use":
            pack = packs.from_code(body["code"]) if body.get("code") else packs.validate(body["pack"])
            only = bool(body.get("only", True))

            def use(log, progress):
                need = packs.missing(t, pack)
                for k, m in enumerate(need):
                    log(f"downloading {m.get('title') or m['name']} ({k + 1} of {len(need)})")
                    if m.get("gamebanana"):
                        mods.gamebanana_install(t, int(m["gamebanana"]), m.get("file_id") or pack_file(m),
                                                lambda d, tot: progress((k + (d / tot if tot else 0)) / max(1, len(need))))
                    elif m.get("url"):
                        install_url(t, m["url"], log, lambda d, tot: progress((k + (d / tot if tot else 0)) / max(1, len(need))))
                    else:
                        log(f"{m['name']} travels with the pack's installer: it isn't here")
                left = packs.use_pack(t, pack, only)
                packs.save_pack(t, pack)
                if left:
                    log("not installed: " + ", ".join(m["name"] for m in left))
                if body.get("apply", True) and not game_running():
                    mods.apply(t, log)
                return {"left": [m["name"] for m in left]}
            return self.jobs.start(f"Modpack {pack.get('name', '')}", use)
        raise mods.Error(f"unknown {path}")


def make_handler(app):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):   # (quiet)
            pass

        def send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
            data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(data)

        def authorized(self, q):
            cookie = self.headers.get("Cookie") or ""
            return (self.headers.get("X-Key") == app.key or q.get("key") == app.key
                    or f"wsk={app.key}" in cookie)

        def handle_any(self, method):
            u = urllib.parse.urlparse(self.path)
            q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
            path = u.path
            if not self.authorized(q):
                return self.send(403, {"error": "not this app's page"})
            if path in ("/", "/index.html"):
                with open(os.path.join(UI, "index.html"), "rb") as f:   # (the theme set before the page shows)
                    page = f.read().replace(b'<html lang="en">', f'<html lang="en" data-theme="{app.settings()["theme"]}">'.encode(), 1)
                    return self.send(200, page, TYPES[".html"],
                                     {"Set-Cookie": f"wsk={app.key}; Path=/; SameSite=Strict; HttpOnly"})
            if path.startswith("/ui/"):
                rel = os.path.normpath(path[4:]).lstrip(os.sep)
                full = os.path.join(UI, rel)
                if not full.startswith(UI + os.sep) or not os.path.isfile(full):
                    return self.send(404, {"error": "not found"})
                with open(full, "rb") as f:
                    return self.send(200, f.read(), TYPES.get(os.path.splitext(full)[1], "application/octet-stream"))
            if path.startswith("/local/"):   # (a mod's own preview image)
                parts = [urllib.parse.unquote(x) for x in path.split("/")[2:]]
                if len(parts) == 2 and parts[1] in ("preview.webp", "preview.png", "preview.jpg") and parts[0] == mods.safe_name(parts[0]):
                    full = os.path.join(app.tree.mods, parts[0], parts[1])
                    if os.path.isfile(full):
                        with open(full, "rb") as f:
                            return self.send(200, f.read(), "image/" + parts[1].rsplit(".", 1)[1])
                return self.send(404, {"error": "not found"})
            if path == "/api/upload" and method == "POST":   # (a file dropped on the window)
                name = mods.safe_name(q.get("name", "mod.zip"))
                n = int(self.headers.get("Content-Length") or 0)
                tmp = tempfile.mkdtemp(prefix="windowsmash-up-")
                target = os.path.join(tmp, name)
                with open(target, "wb") as f:
                    left = n
                    while left > 0:
                        chunk = self.rfile.read(min(left, 1 << 20))
                        if not chunk:
                            break
                        f.write(chunk)
                        left -= len(chunk)

                def inst(log, progress):
                    try:
                        if name.lower().endswith(".wsmodpack"):
                            with open(target, encoding="utf-8") as fh:
                                pack = packs.validate(json.load(fh))
                            packs.save_pack(app.tree, pack)
                            return {"pack": pack}
                        return {"installed": mods.install(app.tree, target)}
                    finally:
                        shutil.rmtree(tmp, ignore_errors=True)
                return self.send(200, app.jobs.start(f"Installing {name}", inst))
            if path.startswith("/api/"):
                body = {}
                if method == "POST":
                    n = int(self.headers.get("Content-Length") or 0)
                    body = json.loads(self.rfile.read(n) or b"{}") if n else {}
                try:
                    return self.send(200, app.api(method, path, q, body))
                except (mods.Error, packs.PackError) as e:
                    return self.send(400, {"error": str(e)})
                except (urllib.error.URLError, OSError) as e:
                    return self.send(502, {"error": f"couldn't reach it: {getattr(e, 'reason', e)}"})
                except (KeyError, ValueError, TypeError) as e:
                    return self.send(400, {"error": f"bad request ({e})"})
            return self.send(404, {"error": "not found"})

        def do_GET(self):
            self.handle_any("GET")

        def do_POST(self):
            self.handle_any("POST")
    return Handler


def reveal(path):
    """Show a file in the Finder / Explorer."""
    try:
        if sys.platform == "darwin":
            subprocess.Popen(["open", "-R", path])
        elif os.name == "nt":
            subprocess.Popen(["explorer", "/select,", path])
    except OSError:
        pass


def open_window(url):
    """Edge's or Chrome's app mode (a window with no browser around it), else the default browser."""
    if os.name == "nt":
        for base in (os.environ.get("ProgramFiles(x86)", ""), os.environ.get("ProgramFiles", ""), os.environ.get("LOCALAPPDATA", "")):
            for exe in (r"Microsoft\Edge\Application\msedge.exe", r"Google\Chrome\Application\chrome.exe"):
                p = os.path.join(base, exe)
                if base and os.path.exists(p):
                    profile = os.path.join(os.environ.get("LOCALAPPDATA", tempfile.gettempdir()), "WindowSmash", "ModsWindow")
                    subprocess.Popen([p, f"--app={url}", f"--user-data-dir={profile}", "--window-size=1280,820"])
                    return
    elif sys.platform.startswith("linux"):
        for exe in ("google-chrome", "chromium", "chromium-browser", "microsoft-edge"):
            if shutil.which(exe):
                subprocess.Popen([exe, f"--app={url}", "--window-size=1280,820"])
                return
    webbrowser.open(url)


def running_server(tree):
    """The tree's mod manager server that is already running and answering ({"pid", "url"}), or None."""
    try:
        with open(os.path.join(tree, "dump", "mods", "server.json"), encoding="utf-8") as f:
            info = json.load(f)
        u = urllib.parse.urlparse(info["url"])
        key = urllib.parse.parse_qs(u.query)["key"][0]
        req = urllib.request.Request(f"{u.scheme}://{u.netloc}/api/library", headers={"X-Key": key})
        with urllib.request.urlopen(req, timeout=3) as r:
            if r.status == 200 and json.load(r).get("root"):
                return info
    except (OSError, ValueError, KeyError, IndexError):
        pass
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tree", default=os.path.dirname(os.path.dirname(HERE)))
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--no-window", action="store_true", help="print the page's address only (the macOS app shows it)")
    a = ap.parse_args()
    global LOG
    try:
        os.makedirs(os.path.join(a.tree, "dump", "mods"), exist_ok=True)
        LOG = open(os.path.join(a.tree, "dump", "mods", "manager.log"), "a", encoding="utf-8")
    except OSError:
        LOG = None
    if sys.stdout is None or sys.stderr is None:   # (pythonw on Windows: no console - print to the log)
        sys.stdout = sys.stderr = LOG or open(os.devnull, "w")
    # one server for a tree: a second start (the app opened again) shows the running one's page
    running = running_server(a.tree)
    if running:
        note(f"WindowSmash Mods already running (process {running['pid']}): its window opened again")
        print(json.dumps({"url": running["url"]}), flush=True)
        if not a.no_window:
            open_window(running["url"])
        return
    note(f"WindowSmash Mods started ({sys.platform}, Python {sys.version.split()[0]}, {a.tree})")
    key = secrets.token_urlsafe(18)
    app = App(a.tree, key, watch=True)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(app))
    url = f"http://127.0.0.1:{srv.server_address[1]}/?key={key}"
    marker = os.path.join(a.tree, "dump", "mods", "server.json")
    try:
        with open(marker, "w", encoding="utf-8") as f:
            json.dump({"pid": os.getpid(), "url": url}, f)
    except OSError:
        pass
    print(json.dumps({"url": url}), flush=True)
    if not a.no_window:
        open_window(url)

    def end():
        try:
            with open(marker, encoding="utf-8") as f:
                if json.load(f).get("pid") == os.getpid():
                    os.remove(marker)
        except (OSError, ValueError):
            pass
        note("WindowSmash Mods closed")
        os._exit(0)   # (nothing is left to wait for: no thread keeps the process, and so the log, alive)

    def watchdog():   # (the window closed: it said bye, or nothing pings; jobs still running are let finish)
        while True:
            time.sleep(5)
            if app.jobs.active():
                continue
            if app.bye and time.time() - app.bye > 15 or time.time() - app.last_ping > 180:   # (a minimised window pings about once a minute)
                end()
    threading.Thread(target=watchdog, daemon=True).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    end()


if __name__ == "__main__":
    main()
