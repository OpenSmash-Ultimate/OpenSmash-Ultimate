#!/usr/bin/env python3
"""Mods: ARCropolis-style file mods (GameBanana's kind) installed, turned on and off, and applied.

Usage:
  mods.py list                          the installed mods, as JSON
  mods.py install <archive> [--name N] [--gamebanana ID] [--title T] [--image URL]
  mods.py gamebanana <mod id> [--file FILE_ID]   download a mod's file from GameBanana and install it
  mods.py enable|disable|remove <name>
  mods.py order <name>...               the mods' order: a later one wins where two change a file
  mods.py apply                         make the game read the turned-on mods (none: the game as it is)
  mods.py vanilla                       the game as it is, without taking the mods off the list
  (every command takes --tree <dir>: the port's folder, the default)

A mod is a folder in <tree>/mods: files at the paths the game's data.arc has them at -
fighter/mario/model/body/c00/def_mario_001_col.nutexb, ui/replace/..., sound/..., and
"stream;/..." for stream: files (music) - as ARCropolis reads them on a Switch. Its state is in
<tree>/mods/mods.json.

How it is applied, with no change to the game or the runtime. The runtime reads the game's RomFS
through a layer map (romfs.cfg, runtime/hle_fs.c): byte ranges of the image from source files.
data.arc is the image's last file; the RomFS's own tables follow it. `apply` writes
<tree>/dump/mods/overlay.bin and a second layer map in which
  - data.arc grows past its old end, and its header points at a new copy of its file table there;
  - the game reads a folder's files as one chunk (folder offsets: offset, sizes), read as a
    stream: each file (a data entry an info-to-data entry of the folder names) where the one
    before it ends, 16-aligned. So each folder a mod changes is laid out again after data.arc's
    old end, its files in their order: the unchanged ones as they were - read from the game's own
    files through the layer map, not copied (the overlay has a hole there) - and a changed one's
    new bytes in its place, the files after it moved along, files a mod adds at the end; the
    folder's data entries and its folder entry point at the new place;
  - a file the game doesn't have (a custom model's own textures, say) is added to the directory
    whose files are in its folder (fighter/mario/model/body/c00/x.nutexb: fighter/mario/c00): new
    entries at the end of the file table's tables (Table: the counts the game reads grow, no entry
    it has moves), the directory's run of files copied to the end with the new ones after it;
  - a new costume slot (c08 and up) is made from the mod's config.json, as ARCropolis does: its
    directories (new-dir-infos, new-dir-infos-base), paths that are the game's files
    (share-to-vanilla, share-to-added), the directories' files (new-dir-files) - the mod's own in
    a folder of the new directory's, the rest copies of the files they share, as the game's own
    extra costumes are; its character select entry comes from the mod's ui_chara_db.prcxml;
  - every new path is added to data.arc's search section too (Search: the game lists a folder's
    files with it - a costume's model, fighter/mario/model/body/c08/ - and finds none otherwise);
  - ARCropolis' patch files are merged into the file they patch, each turned-on mod's in order:
    .xmsbt into an .msbt (text: msbt.py), .prcx and .prcxml into a .prc (params: prc.py) - into a
    mod's whole file if one has it, else the game's (a regional file: each region's own);
  - a stream: file (music) is stored whole, its stream data entries pointed at it;
  - the RomFS's tables come after data.arc's new end, with its new size;
and romfs.cfg is switched to it (the game as it is: romfs_vanilla.cfg, kept).
  - a mod's file at a path other paths share the file of (a costume's model the fighter's other
    costumes read too) is made that path's own, as ARCropolis unshares it: the other paths keep
    the game's file (config.json's unshare-blacklist keeps a path shared);
  - a new costume slot is completed as the game's own are, whatever its config.json leaves out:
    the fighter's other slot directories (camera, result, kirbycopy, movie), a linked fighter's
    (Pyra's slot makes Mythra's and both's, fighter/element), and the like slot's files it
    doesn't list, shared to the game's (a path with no name known made from the like one's hash);
Limits, as against ARCropolis on a Switch: a file in a folder the game doesn't have, with no
config.json telling of its directory, is skipped and listed.
"""
import argparse
import filecmp
import hashlib
import json
import os
import re
import shutil
import ssl
import stat
import struct
import subprocess
import sys
import tempfile
import time
import zlib
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arc as _arc  # noqa: E402
if "extra_at" in open(_arc.__file__, encoding="utf-8").read() and hasattr(_arc, "unzstd"):
    from arc import Arc, LOW40, hash40, romfs_entries, unzstd  # noqa: E402
else:   # (an arc.py from before the mods' fix - the file table's extra entries counted wrong: the
    # mod manager's own copy, which an installed mod manager brings without changing the build's)
    from modmanager.arc_mods import Arc, LOW40, hash40, romfs_entries, unzstd  # noqa: E402
from nca import RomfsImage, read_layers, write_layers  # noqa: E402
import msbt  # noqa: E402
import prc  # noqa: E402
import csk  # noqa: E402

try:
    from compression.zstd import CompressionParameter, compress as zstd_compress   # Python 3.14

    def game_compress(data):
        """zstd as the game's own files are: a 2 MB window, no content size, no checksum. The
        game's decompressor overruns its buffers on other frames (a single-segment one, whose
        window is the whole file: a crash in a copy, loading a big modded texture)."""
        return zstd_compress(data, options={CompressionParameter.window_log: 21, CompressionParameter.content_size_flag: 0,
                                            CompressionParameter.checksum_flag: 0})
except ImportError:                                           # 3.10 to 3.13: the zstandard package
    import zstandard

    def zstd_compress(data):
        return zstandard.ZstdCompressor().compress(data)

    def game_compress(data):
        params = zstandard.ZstdCompressionParameters.from_level(3, window_log=21, write_content_size=False, write_checksum=False)
        return zstandard.ZstdCompressor(compression_params=params).compress(data)

LAYER_ZERO = 0xFFFFFFFF
MODDED_MAP = "romfs_mods.layers"
# A mod's own folder holds these at its top (the game's data.arc's top folders, and ARCropolis'
# names for the stream: and prebuilt: ones)
TOP = {"fighter", "ui", "sound", "stream;", "prebuilt;", "effect", "stage", "camera", "item", "param", "assist",
       "boss", "enemy", "finding", "pokemon", "spirits", "standard", "common", "menu", "render", "shader", "snapshot",
       "fighter_pass", "miihat", "replay", "sp_enemy"}
# ...and these, which aren't data.arc's: a mod made only of them is a mod too (plugins read them from
# mods:/ - the CSK Collection's options, mods:/flags/<option>.flag and reslot.toml)
PLUGIN_DATA = {"flags"}
ROOTS = TOP | PLUGIN_DATA
# ARCropolis' patch files: merged into the game's file of the same name (or a mod's whole one) when
# mods are applied - .xmsbt into an .msbt (text), .prcx and .prcxml into a .prc (params), and the stage
# files' .stdatx / .stdatxml and .stprmx / .stprmxml (param files too, as ARCropolis' prc.rs takes them)
PATCHES = {".xmsbt": ".msbt", ".prcx": ".prc", ".prcxml": ".prc",
           ".stdatx": ".stdat", ".stdatxml": ".stdat", ".stprmx": ".stprm", ".stprmxml": ".stprm"}
# ARCropolis' regions, in the order of a regional file's data entries (the game reads to_data + 1 + its
# region). A file or a patch named <name>+<region>.<ext> (msg_name+us_en.xmsbt) is that region's only
# (ARCropolis' strip_regional): for a regional file its region's data entry, for any other file only
# when the game's region is that one - always us_en here (runtime/hle_env.c: nn::oe::GetDesiredLanguage)
REGIONS = ("jp_ja", "us_en", "us_fr", "us_es", "eu_en", "eu_fr", "eu_es", "eu_de", "eu_nl", "eu_it", "eu_ru", "kr_ko",
           "zh_cn", "zh_tw")
GAME_REGION = "us_en"
REGION_SUFFIX = re.compile(r"\+([a-z]{2}_[a-z]{2})(?=\.[^./]*$)", re.I)


def strip_region(path):
    """(path without its +<region>, the region or None)."""
    m = REGION_SUFFIX.search(path)
    if not m or m.group(1).lower() not in REGIONS:
        return path, None
    return path[:m.start()] + path[m.end():], m.group(1).lower()
SKIP_FILES = {"info.toml", "config.json", "preview.webp", "preview.png", "preview.jpg", "readme.txt", "readme.md"}
GAMEBANANA = "https://gamebanana.com/apiv11"


class Error(Exception):
    """Something the person can act on; the message says what."""


# ---- where things are ----------------------------------------------------------------------
class Tree:
    def __init__(self, root):
        self.root = os.path.abspath(root)
        self.mods = os.path.join(self.root, "mods")
        self.state_path = os.path.join(self.mods, "mods.json")
        self.modules = os.path.join(self.root, "dump", "modules")
        self.cfg = os.path.join(self.modules, "romfs.cfg")
        self.vanilla_cfg = os.path.join(self.modules, "romfs_vanilla.cfg")
        self.work = os.path.join(self.root, "dump", "mods")

    def load(self):
        try:
            with open(self.state_path, encoding="utf-8") as f:
                state = json.load(f)
        except (OSError, ValueError):
            state = {}
        state.setdefault("order", [])
        state.setdefault("mods", {})
        present = [n for n in sorted(os.listdir(self.mods)) if os.path.isdir(os.path.join(self.mods, n)) and not n.startswith(".")
                   ] if os.path.isdir(self.mods) else []   # (.packs: the saved modpacks)
        state["order"] = [n for n in state["order"] if n in present] + [n for n in present if n not in state["order"]]
        for n in present:
            # A folder that was only just dropped in is not on until it's enabled. Vegito was
            # picked up mid-install because a missing entry used to default to on.
            state["mods"].setdefault(n, {"enabled": False})
        state["mods"] = {n: m for n, m in state["mods"].items() if n in present}
        return state

    def save(self, state):
        os.makedirs(self.mods, exist_ok=True)
        tmp = self.state_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(state, f, indent=1)
        os.replace(tmp, self.state_path)

    def applied(self):
        """The mods the game reads now (romfs.cfg's), or None for the game as it is."""
        try:
            with open(os.path.join(self.work, "applied.json"), encoding="utf-8") as f:
                applied = json.load(f)
        except (OSError, ValueError):
            return None
        return applied if self.is_modded() else None

    def is_modded(self):
        """Whether the game runs with mods: romfs.cfg reads them, or (the real ARCropolis, which loads
        them as the game runs from the list in dump/skyline/mods.txt) the last apply turned some on."""
        try:
            with open(self.cfg, encoding="utf-8") as f:
                if MODDED_MAP in f.read():
                    return True
        except OSError:
            return False
        try:
            with open(os.path.join(self.work, "applied.json"), encoding="utf-8") as f:
                applied = json.load(f)
        except (OSError, ValueError):
            return False
        return bool(applied.get("arcropolis") and applied.get("mods"))


def remove_folder(folder):
    """Take a mod out of mods/. A symbolic link or a junction is the link only; the folder
    it points at stays where it is."""
    try:
        st = os.lstat(folder)
    except OSError:
        return
    reparse = getattr(st, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    tag = getattr(st, "st_reparse_tag", 0)
    link = stat.S_ISLNK(st.st_mode) or (
        reparse and tag in (stat.IO_REPARSE_TAG_SYMLINK, stat.IO_REPARSE_TAG_MOUNT_POINT))
    if link:
        os.rmdir(folder)
        return
    shutil.rmtree(folder)


# ---- a mod's files ---------------------------------------------------------------------------
def arc_path(rel):
    """A file's path in data.arc from its path in a mod's folder."""
    rel = rel.replace(os.sep, "/")
    if rel.startswith("stream;/"):
        return "stream:/" + rel[len("stream;/"):]
    if rel.startswith("prebuilt;/"):
        return "prebuilt:/" + rel[len("prebuilt;/"):]
    return rel


def mod_files(folder):
    """(arc path, file) of every file a mod's folder holds (not its code plugins: plugins())."""
    out = []
    for dp, dirs, files in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for n in sorted(files):
            if n.startswith(".") or n.lower() in SKIP_FILES:
                continue
            full = os.path.join(dp, n)
            path = arc_path(os.path.relpath(full, folder))
            if is_plugin(path) or path.split("/", 1)[0].lower() in PLUGIN_DATA or "/" not in path:
                continue   # (and a file at the mod's top - victory.toml, config_param.toml: its plugins' settings, not data.arc's)
            out.append((path, full))
    return out


def is_plugin(path):
    """A Skyline code plugin (plugin.nro, a moveset's or a new character's code): Switch code that
    tools/skyline.py recompiles into the game - not a file of data.arc's (those .nro are under prebuilt:/nro/)."""
    return path.lower().endswith(".nro") and not path.startswith("prebuilt:")


def plugins(folder):
    """The code plugins a mod's folder holds (paths in it)."""
    out = []
    for dp, dirs, files in os.walk(folder):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for n in files:
            rel = os.path.relpath(os.path.join(dp, n), folder)
            if is_plugin(arc_path(rel)):
                out.append(rel.replace(os.sep, "/"))
    return sorted(out)


def find_roots(top):
    """The folders under `top` laid out as data.arc is (each a mod's own folder). A pack can hold
    mods in subfolders of one that is itself a mod ("Pack/ui", "Pack/(Moveset) X/fighter"): those
    are mods of their own (install copies each without the others: nested_roots)."""
    roots = []
    for dp, dirs, _files in os.walk(top):
        if any(d.lower() in ROOTS for d in dirs):
            roots.append(dp)
        dirs[:] = [d for d in dirs if d.lower() not in ROOTS]   # (a mod's own data folders are not other mods)
    return roots


def nested_roots(root, roots):
    """The roots inside `root` (a pack's mods in its subfolders), which its own copy leaves out."""
    return [r for r in roots if r != root and os.path.commonpath([root, r]) == root]


# ---- installing ------------------------------------------------------------------------------
def extract(archive, into):
    """A .zip with Python's own reader; .7z and .rar (and the rest) with macOS's bsdtar."""
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z:
            for m in z.infolist():
                target = os.path.realpath(os.path.join(into, m.filename))
                if not target.startswith(os.path.realpath(into) + os.sep) and target != os.path.realpath(into):
                    raise Error(f"{os.path.basename(archive)} has a file outside its folder ({m.filename}): not installed")
            z.extractall(into)
        return
    r = subprocess.run(["tar", "-xf", archive, "-C", into], capture_output=True, text=True)
    if r.returncode and os.name == "nt":   # (Windows' tar may not read .7z or .rar: 7-Zip, when it's installed)
        for seven in (os.path.join(os.environ.get(v, ""), "7-Zip", "7z.exe") for v in ("ProgramFiles", "ProgramFiles(x86)")):
            if os.path.exists(seven):
                r = subprocess.run([seven, "x", "-y", f"-o{into}", archive], capture_output=True, text=True)
                break
    if r.returncode and archive.lower().endswith(".7z"):   # (no 7-Zip: py7zr, which the installer adds)
        try:
            import py7zr
        except ImportError:
            py7zr = None
        if py7zr:
            with py7zr.SevenZipFile(archive) as z:
                outside = [n for n in z.getnames() if not os.path.realpath(os.path.join(into, n)).startswith(
                    os.path.realpath(into) + os.sep)]
                if outside:
                    raise Error(f"{os.path.basename(archive)} has a file outside its folder ({outside[0]}): not installed")
                z.extractall(into)
            return
    if r.returncode:
        hint = " (install 7-Zip, from 7-zip.org, to open .7z and .rar files)" if os.name == "nt" else ""
        raise Error(f"cannot unpack {os.path.basename(archive)}{hint}: {(r.stderr or r.stdout).strip()[-400:]}")


def safe_name(name):
    name = re.sub(r"[^\w .()+&'-]+", "_", name).strip(" ._") or "mod"
    return name[:80]


def install(tree, archive, name=None, meta=None):
    """Unpack `archive` and add each mod folder in it; returns the names added."""
    os.makedirs(tree.mods, exist_ok=True)
    state = tree.load()
    added = []
    with tempfile.TemporaryDirectory(prefix="windowsmash-mod-") as tmp:
        if os.path.isdir(archive):
            shutil.copytree(archive, os.path.join(tmp, "x"))
        else:
            os.makedirs(os.path.join(tmp, "x"))
            extract(archive, os.path.join(tmp, "x"))
        roots = find_roots(os.path.join(tmp, "x"))
        if not roots and plugins(os.path.join(tmp, "x")):   # (only code: a moveset or gameplay plugin, recompiled into the game)
            roots = [os.path.join(tmp, "x")]
        if not roots:
            raise Error(f"{os.path.basename(archive)} has no files laid out as the game's (fighter/, ui/, sound/, stream;/ ...) "
                        "and no Skyline plugin (.nro)")
        base = safe_name(name or os.path.splitext(os.path.basename(archive))[0])
        for root in roots:
            leaf = os.path.basename(root)
            label = base if len(roots) == 1 else f"{base} - {safe_name(leaf)}"
            final, k = label, 2
            while os.path.exists(os.path.join(tree.mods, final)):
                final, k = f"{label} ({k})", k + 1
            inner = {os.path.normcase(r) for r in nested_roots(root, roots)}
            shutil.copytree(root, os.path.join(tree.mods, final),
                            ignore=lambda d, names: [n for n in names if os.path.normcase(os.path.join(d, n)) in inner])
            entry = {"enabled": True}
            entry.update(meta or {})
            state = tree.load()
            state["mods"][final] = entry
            tree.save(state)
            added.append(final)
    return added


def gamebanana_get(path):
    ctx = ssl.create_default_context()
    try:
        import certifi   # (python.org's Python has no certificates of its own until it is told)
        ctx = ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        pass
    req = urllib.request.Request(GAMEBANANA + path, headers={"User-Agent": "WindowSmash mod manager"})
    with urllib.request.urlopen(req, timeout=30, context=ctx) as r:
        return json.load(r), ctx


def gamebanana_install(tree, mod_id, file_id=None, progress=None):
    data, ctx = gamebanana_get(f"/Mod/{int(mod_id)}?_csvProperties=_sName,_aFiles,_aPreviewMedia,_aSubmitter,_aCategory")
    files = data.get("_aFiles") or []
    if not files:
        raise Error("this mod has no files to download on GameBanana")
    f = next((x for x in files if str(x.get("_idRow")) == str(file_id)), None) if file_id else files[0]
    if not f:
        raise Error(f"the mod has no file {file_id}")
    if f.get("_sAvResult") not in (None, "", "clean"):
        raise Error(f"GameBanana's virus scan of {f.get('_sFile')} says: {f.get('_sAvResult')}. Not installed.")
    images = (data.get("_aPreviewMedia") or {}).get("_aImages") or []
    image = f"{images[0]['_sBaseUrl']}/{images[0].get('_sFile530') or images[0]['_sFile']}" if images else ""
    with tempfile.TemporaryDirectory(prefix="windowsmash-dl-") as tmp:
        target = os.path.join(tmp, os.path.basename(f["_sFile"]))
        req = urllib.request.Request(f["_sDownloadUrl"], headers={"User-Agent": "WindowSmash mod manager"})
        md5 = hashlib.md5()
        with urllib.request.urlopen(req, timeout=60, context=ctx) as r, open(target, "wb") as out:
            total, done = int(r.headers.get("Content-Length") or f.get("_nFilesize") or 0), 0
            while chunk := r.read(1 << 20):
                out.write(chunk)
                md5.update(chunk)
                done += len(chunk)
                if progress:
                    progress(done, total)
        if f.get("_sMd5Checksum") and md5.hexdigest() != f["_sMd5Checksum"].lower():
            raise Error(f"{f['_sFile']} did not download whole (its checksum differs): try again")
        meta = {"gamebanana": int(mod_id), "title": data.get("_sName", ""), "image": image,
                "author": (data.get("_aSubmitter") or {}).get("_sName", ""),
                "category": (data.get("_aCategory") or {}).get("_sName", ""), "file": f["_sFile"], "file_id": f["_idRow"],
                "md5": f.get("_sMd5Checksum", "")}
        return install(tree, target, data.get("_sName") or f["_sFile"], meta)


# ---- applying --------------------------------------------------------------------------------
def disk_size(path):
    """The disk space a file takes (a sparse file's holes don't count)."""
    st = os.stat(path)
    if hasattr(st, "st_blocks"):
        return st.st_blocks * 512
    if os.name == "nt":   # (Windows: no st_blocks - the allocated size, sparse holes left out)
        try:
            import ctypes
            from ctypes import wintypes
            high = wintypes.DWORD()
            f = ctypes.windll.kernel32.GetCompressedFileSizeW
            f.argtypes, f.restype = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)], wintypes.DWORD
            low = f(path, ctypes.byref(high))
            if low != 0xFFFFFFFF or ctypes.GetLastError() == 0:
                return (high.value << 32) | low
        except (OSError, AttributeError):
            pass
    return st.st_size


def make_sparse(f):
    """On Windows, an NTFS file whose holes cost nothing (macOS and Linux files are so already):
    the overlay's unchanged files are holes, read from the game's own files (aliases) - without
    this Windows writes them out as zeros, gigabytes of them, slowly."""
    if os.name != "nt":
        return
    try:
        import ctypes
        import msvcrt
        from ctypes import wintypes
        dio = ctypes.windll.kernel32.DeviceIoControl
        dio.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.LPVOID,
                        wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        dio(msvcrt.get_osfhandle(f.fileno()), 0x900C4, None, 0, None, 0, ctypes.byref(wintypes.DWORD()), None)   # FSCTL_SET_SPARSE
    except (OSError, AttributeError, ImportError):
        pass   # (FAT32 or exFAT: no sparse files - only slower)


def replace_file(tmp, path):
    """os.replace, saying what to do when Windows has the file open elsewhere."""
    for _ in range(10):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            time.sleep(0.5)
    try:
        os.replace(tmp, path)
    except PermissionError:
        raise Error(f"{os.path.basename(path)} is open in another program (the game, or another WindowSmash Mods window): "
                    "close it and apply again")


def close_image(image):
    """A RomfsImage's files closed (an nca.py whose RomfsImage has no close() keeps them open, and
    Windows won't replace a file that is open: overlay.bin, at the next apply)."""
    if hasattr(image, "close"):
        image.close()
        return
    for f in [getattr(image, "f", None)] + list(getattr(image, "sources", None) or []):
        if f is not None:
            try:
                f.close()
            except OSError:
                pass


def play_vanilla(tree, log=print):
    """The game without mods, the list of mods staying as it is (Apply brings them back): romfs.cfg the
    game's own, and for the real ARCropolis no mod in sd:/ultimate/mods (dump/skyline/mods.txt), so it
    loads none and runs none of their plugins. The community plugins still load; with no mods they
    change nothing."""
    vanilla_cfg(tree)
    shutil.copyfile(tree.vanilla_cfg, tree.cfg)
    listed = os.path.join(tree.root, "dump", "skyline", "mods.txt")
    if os.path.exists(listed):
        open(listed, "w").close()
    arcropolis = os.path.exists(os.path.join(tree.root, "skyline", "plugins", "libarcropolis.nro"))
    os.makedirs(tree.work, exist_ok=True)
    with open(os.path.join(tree.work, "applied.json"), "w", encoding="utf-8") as f:
        json.dump({"mods": [], "files": 0, "skipped": {}, "arcropolis": arcropolis}, f)
    log("the game as it is (the mods stay on the list: Apply to game turns them back on)")


def vanilla_cfg(tree):
    """romfs_vanilla.cfg: the game as it is (romfs.cfg, kept the first time mods are applied)."""
    if not tree.is_modded() and os.path.exists(tree.cfg):
        shutil.copyfile(tree.cfg, tree.vanilla_cfg)
    if not os.path.exists(tree.vanilla_cfg):
        raise Error("the game isn't set up here yet (no dump/modules/romfs.cfg): build it first")
    with open(tree.vanilla_cfg, encoding="utf-8") as f:
        lines = [l.rstrip("\r\n") for l in f if l.strip()]
    return lines


TABLES = ("hash_to_path", "paths", "info_indices", "dir_hash_to_info", "dirs", "folder_offsets", "child_hashes",
          "infos", "info_to_datas", "datas")
OWN_FILE = 0x1000000      # an info-to-data entry's last field for a file in its directory's own folder
SHARED_INFO = 0x300000    # a file info's flags for a copy of another directory's file (a costume sharing c00's)
REGIONAL_INFO = 0x18000   # a file info's flags for a file with one data per region or language
SLOT = re.compile(r"/c\d\d(?=/|$)")
# fighters whose costume slots go together: one picked on the character select screen loads the others'
# directories of the same slot (Pyra/Mythra and both together; Pokémon Trainer and the three; Ice Climbers)
LINKED = [("eflame", "elight", "element"), ("ptrainer", "pzenigame", "pfushigisou", "plizardon"), ("popo", "nana")]
# a costume's own effects (effect/fighter/sonic/ef_sonic_c06.eff): only the One Slot Effects
# Skyline plugin reads them; added to the game as a mod's config.json says (to the costume's
# directory), the game stops loading the fighter - so they are left out, the fighter's own effects used
ONE_SLOT_PLUGIN = False   # (apply: the One Slot Effects plugin is among the standard plugins, <tree>/skyline/plugins, so
                          # a costume's own effect file goes in as a new file, as ARCropolis adds it on the console)
ONE_SLOT_EFFECT = re.compile(r"^effect/fighter/[^/]+/ef_[^/]+_c\d+\.eff$", re.I)   # a costume slot in a directory's path (fighter/mario/c08, .../c08/camera)


def config_hash(value):
    """A path in config.json, or a hash40 of one ("0x..." or a number): (hash40, the path or None)."""
    if isinstance(value, int):
        return value & LOW40, None
    value = value.strip()
    if value.lower().startswith("0x"):
        return int(value, 16) & LOW40, None
    return hash40(value), value


def reslot(h, at, old, new):
    """The hash40 of a path `h` is the hash40 of, with the character at `at` `new` for `old`
    (CRC-32 is linear: a costume slot's digit changed, without the path's name)."""
    if old == new:
        return h
    n = h >> 32
    d = bytearray(n)
    d[at] = ord(old) ^ ord(new)
    return (h & 0xFFFFFFFF ^ zlib.crc32(bytes(d)) ^ zlib.crc32(bytes(n))) | n << 32


def reslot2(h, at, old, new):
    """reslot for a two-digit slot whose second digit is at `at` ("00" -> "08")."""
    return reslot(reslot(h, at, old[1], new[1]), at - 1, old[0], new[0])


class Table:
    """data.arc's file table, to change: each of its tables a list of entries of its own, so files
    can be added (add_file); bytes() puts the table together again.

    How the game reads it (f_710374f910, after it reads the table): the header's counts give each
    table's place - infos, info-to-datas and datas each hold the game's own entries, a patch's
    (the header's 0x28), then extra ones (0x4C, 0x50, 0x54) - and a last section, after datas, maps
    path hashes to the extra entries; the game's per-file state (one per path, per info index, per
    folder) is allocated from the same counts. A new file's entries go at the end of each table:
    a path and an info index (their tables have no extra part), and an info, an info-to-data and a
    data after the extra ones, their counts grown - no entry the game has moves. A directory's files
    are a run of infos (its start and count), so a directory that gets files has its run copied to
    the end, the new files after it.

    A mod's new directories (ARCropolis' config.json: a new costume slot, fighter/mario/c08 and its
    camera/c08, kirbycopy/c08 ...) are made as the game's own extra costumes are (c06 is mostly
    c00's): a directory (dirs, and its hash in dir_hash_to_info, sorted), in its parent's children
    (the parent's run of child hashes copied to the end, the new one after it), a folder of its own
    (an extra folder: the header's 0x40) whose chunk holds the mod's files (and whose range of data
    entries has one for each of the directory's files, empty for one it shares, as the game's
    folders have), and a file info for each file it doesn't have of its own, a copy of the file it shares (share-to-vanilla): flags with
    SHARED_INFO, and an info-to-data whose last field names the shared file's info index."""

    def __init__(self, arc):
        self.arc = arc
        t = arc.t
        self.head = bytearray(t[:arc.tab["hash_to_path"][0]])   # the header, regions, streams, buckets
        self.size = {}
        self.tabs = {}
        for name in TABLES:
            o, size, count = arc.tab[name]
            self.size[name] = size
            self.tabs[name] = bytearray(t[o:o + size * count])
        self.extra = bytes(t[arc.extra_at:])
        self.base_datas = self.count("datas")
        self.streams = None
        self.owned = None
        self.parents = None
        self.replaced = {}   # folder -> {data index: (stored bytes, size, flags)}
        self.added = {}      # folder -> [(data index, stored bytes, size, flags)], in the order they go
        self.new_files = {}  # directory -> [(path, bytes)]
        self.new_paths = []  # (hash40, path index)
        self.dirs_by_hash = None
        self.new_dirs = {}      # a new directory -> the existing one it is made like (its flags, its files' flags)
        self.based = set()      # new directories that are another's (new-dir-infos-base: its folder and files)
        self.new_children = {}  # directory -> [child hash40 | index << 40] it gets
        self.listed = {}        # directory -> [(hash40, path)]: the files config.json gives it (new-dir-files)
        self.listed_in = {}     # file hash40 -> the directory config.json gives it to
        self.shares = {}        # new path hash40 -> (path, the hash40 of the file it is)
        self.notes = []         # what was left out, for the log
        self.path_names = []    # the new paths (for the search section)
        self.ii_paths = None    # info index index -> how many paths name it
        self.path_infos = None  # path index -> (directory, info) of the directory's entry for it
        self.vanilla_ii = {}    # path index -> the info index index it named before it was unshared
        self.keep_shared = set()   # hash40s of paths not to unshare (config.json's unshare-blacklist)
        self.unshared = {}      # directory -> [(path index, info index index, bytes, flags)]: files made its own
        self.folder_readers = None   # folder -> (up to 2 of) the directories that read files from it
        self.kept_shared = []   # paths a mod changes that stayed shared (unshare: they'd stop matches loading)
        self.crowded = set()    # files (info index indices) one mod has different versions of for 2+ costumes
        self.reshare_left = []  # hash40s of paths reshare left on a mod's shared sound bank (they can't be made their own)
        self.dir_names = {}     # the game's directory a new one is made like -> its path
        self.dir_names_new = {}  # a new directory -> its path, from config.json
        self.slots = {}         # a new costume slot's directory (made like the game's c00..c07 one) -> its slot ("08")
        self.like_paths = []    # (new path hash40, the game's path hash40 it is made from, at, old, new): for the search section

    def count(self, table):
        return len(self.tabs[table]) // self.size[table]

    def entry(self, table, index, fmt):
        if table in self.tabs:
            return struct.unpack_from(fmt, self.tabs[table], index * self.size[table])
        o, size, _ = self.arc.tab[table]
        return struct.unpack_from(fmt, self.head, o + index * size)

    def put(self, table, index, fmt, *values):
        if table in self.tabs:
            struct.pack_into(fmt, self.tabs[table], index * self.size[table], *values)
            return
        o, size, _ = self.arc.tab[table]
        struct.pack_into(fmt, self.head, o + index * size, *values)

    def append(self, table, fmt, *values):
        index = self.count(table)
        self.tabs[table] += struct.pack(fmt, *values).ljust(self.size[table], b"\0")
        return index

    def path_index(self, path):
        return self.path_index_of(hash40(path))

    def path_index_of(self, h):
        start, count = struct.unpack_from("<2I", self.head, self.arc.bucket_table + (h % self.arc.buckets) * 8)
        lo, hi = start, start + count
        while lo < hi:
            mid = (lo + hi) // 2
            v = self.entry("hash_to_path", mid, "<Q")[0]
            if v & LOW40 == h:
                return v >> 40
            lo, hi = (mid + 1, hi) if v & LOW40 < h else (lo, mid)
        return None

    def file_targets(self, path):
        """The (folder, data) entries of a file (one per region for a regional file), or None."""
        p = self.path_index(path)
        if p is None:
            return None
        full = self.entry("paths", p, "<Q")[0]
        info = self.entry("info_indices", full >> 40, "<2I")[1]
        _, _, to_data, flags = self.entry("infos", info, "<4I")
        out = []
        # (a regional file: an empty entry, then one per region - the game reads to_data + 1 + its region)
        for r in range(1, self.arc.regions + 1) if flags & 0x8000 else (0,):
            folder, data, _ = self.entry("info_to_datas", to_data + r, "<3I")
            out.append((folder, data))
        return sorted(set(out))

    def region_targets(self, path):
        """[(folder, data, regions)] of a file: for a regional file each data entry with the regions that
        read it (two regions can share one, the same text), for any other file its one entry with None;
        [] for a path the game doesn't have."""
        p = self.path_index(path)
        if p is None:
            return []
        full = self.entry("paths", p, "<Q")[0]
        info = self.entry("info_indices", full >> 40, "<2I")[1]
        _, _, to_data, flags = self.entry("infos", info, "<4I")
        if not flags & 0x8000:
            folder, data, _ = self.entry("info_to_datas", to_data, "<3I")
            return [(folder, data, None)]
        by = {}
        for r in range(1, self.arc.regions + 1):
            folder, data, _ = self.entry("info_to_datas", to_data + r, "<3I")
            by.setdefault((folder, data), set()).add(REGIONS[r - 1] if r - 1 < len(REGIONS) else f"region {r - 1}")
        return [(f, d, rs) for (f, d), rs in sorted(by.items())]

    def original(self, folder, data):
        """A file's bytes as the game has them (its folder and data entries)."""
        offset, stored, size, flags = self.arc._entry("datas", data, "<4I")
        raw = self.arc.read(self.arc.file_section + self.arc._entry("folder_offsets", folder, "<Q")[0] + offset * 4, stored)
        return unzstd(raw, f"data entry {data}") if flags & 3 else raw

    def replace(self, folder, data, raw):
        """A file's new bytes (stored as the original is: zstd-compressed or not)."""
        flags = self.entry("datas", data, "<4I")[3]
        stored = game_compress(raw) if flags & 3 else raw
        self.replaced.setdefault(folder, {})[data] = (stored, len(raw), flags)

    def sharers(self, path):
        """The hash40s of the paths that are the same file as `path` (its own too: one path names
        each info index index of a file no other shares), or None for a path the game doesn't have."""
        p = self.path_index(path)
        if p is None:
            return None
        if self.ii_paths is None:
            self.ii_paths = {}
            for v, _ext, _parent, _name in struct.iter_unpack("<4Q", self.tabs["paths"]):
                self.ii_paths.setdefault(v >> 40, []).append(v & LOW40)
        return self.ii_paths.get(self.entry("paths", p, "<Q")[0] >> 40, [])

    def unshare(self, path, raw):
        """A mod's file at a path other paths share the file of (a costume's model other costumes
        read from the fighter's common folder, a copy of c00's texture): the path made a file of its
        own, as ARCropolis unshares it - a new info index index the path names, a file in its
        directory's folder (commit_files: the directory's run of infos copied to the end, the new
        file after it, its data after the folder's files, as an added file's) - so the other paths
        read the game's file still. The directory's entry for the shared file stays as it was (the
        game reads the files of a folder its directory has in their order). A regional file (flag
        0x8000: a fighter's voice bank, which the costumes of a fighter share as they share a model)
        gets one data entry for every region (commit_files). Returns False where it can't be (the
        other regional kind, 0x10000; a path in no directory): the shared file is changed then."""
        if self.path_infos is None:
            self.path_infos = {}
            info_path = [e[0] for e in struct.iter_unpack("<4I", self.tabs["infos"])]
            for d, e in enumerate(struct.iter_unpack("<QQQ7I", self.tabs["dirs"])):
                for i in range(e[5], e[5] + e[6]):
                    self.path_infos.setdefault(info_path[i], (d, i))
        if path is None:
            return False
        p = self.path_index(path)
        if p is None or hash40(path) in self.keep_shared or p not in self.path_infos:
            return False
        d, i = self.path_infos[p]
        _p, ii, _to_data, fflags = self.entry("infos", i, "<4I")
        full = self.entry("paths", p, "<Q")[0]
        if fflags & 0x10000 or ii != full >> 40:
            return False
        # Where unsharing makes matches never finish loading, the file stays shared - the mod's file
        # for every costume that shares it, as before unsharing - and is listed (self.kept_shared):
        # - a file can't be made a directory's own when other directories read files from that
        #   directory's folder too (a c00 costume's folder, which c01-c07 read most of their files
        #   from): seen, Uncensored Pyra/Mythra's c00 files hung c07, Mr. L's hung Luigi's c01;
        # - nor a file it reads from another folder (the fighter's common one, c00's) when one mod has
        #   different versions of that file for two or more costumes (self.crowded, from apply's first
        #   look): seen, Blaziken's c01-c06 copies of one texture hung Captain Falcon's c00, Jotaro
        #   Kujo's c04 texture (its other costumes' differ) hung Terry's c00. One costume of a mod
        #   making it its own is fine (Swimsuit Pyra/Mythra's c07, Ultimate Shadow's c07), also beside
        #   another mod's c00 version of the same file (Uncensored Pyra/Mythra's).
        if self.unshare_risk(path) is not None:
            self.kept_shared.append(path)
            return False
        self._unshare_at(p, d, fflags, raw)
        return True

    def _unshare_at(self, p, d, fflags, raw):
        """Path index p, in directory d, made a file of its own (unshare's, checked there): raw is its
        bytes, or for a regional file a list of them, one for each entry of its run."""
        full = self.entry("paths", p, "<Q")[0]
        ii = full >> 40
        new_ii = self.append("info_indices", "<2I", d, 0)
        self.unshared.setdefault(d, []).append((p, new_ii, raw, fflags & ~SHARED_INFO))
        self.put("paths", p, "<Q", (full & LOW40) | new_ii << 40)
        self.vanilla_ii[p] = ii
        self.ii_paths[ii].remove(full & LOW40)
        self.ii_paths[new_ii] = [full & LOW40]

    def reshare(self, path, wanted):
        """The other way round, for a sound bank unshare keeps shared (its costume's folder is one the
        other costumes read: a fighter's c00): each other path that shares it and that no mod gives a
        file of its own is made its own with the game's bytes (each region's, for a regional voice
        bank), so the shared file can take the mod's for `path` alone - as ARCropolis'
        reshare_dependent_files leaves the other costumes the game's. Kept shared, the c00 mod's voice
        and sounds were every costume's that shares them (seen: Bowser's c04-c06, with no mod, spoke
        with c00's). Only sound banks: they are read one by one, not with their directory's folder,
        and the copies go into the other costumes' own folders. Models made so hung the match (in a
        folder no directory read yet). A path that can't be (unshare's rules and risks for it: the
        bank's own path without a costume, vc_koopa.nus3audio, is in the shared sound folder) keeps the
        shared one, and is in self.reshare_left. Returns how many paths were made their own, or None."""
        own = hash40(path)
        others = [h for h in self.sharers(path) or [] if h != own and h not in wanted]
        if not others:
            return None
        plan = []
        for h in others:
            p = self.path_index_of(h)
            ok = p is not None and h not in self.keep_shared and p in self.path_infos
            if ok:
                d, i = self.path_infos[p]
                _p, ii, _td, fflags = self.entry("infos", i, "<4I")
                ok = not (fflags & 0x10000 or ii != self.entry("paths", p, "<Q")[0] >> 40 or self.unshare_risk_index(p) is not None)
            if ok:
                plan.append((p, d, fflags))
            else:
                self.reshare_left.append(h)
        if not plan:
            return None
        sp = self.path_index(path)
        info = self.entry("info_indices", self.entry("paths", sp, "<Q")[0] >> 40, "<2I")[1]
        _p, _ii, td, sflags = self.entry("infos", info, "<4I")
        run = [self.entry("info_to_datas", td + r, "<3I")[:2] for r in range(self.arc.regions + 1 if sflags & 0x8000 else 1)]
        got = {}
        for fd in run:   # (a regional run's first entry is an empty one: no bytes to read)
            if fd not in got:
                got[fd] = self.original(*fd) if self.entry("datas", fd[1], "<4I")[2] else b""
        raws = [got[fd] for fd in run]
        raw = raws if sflags & 0x8000 else raws[0]
        for p, d, fflags in plan:
            self._unshare_at(p, d, fflags | (sflags & 0x8000), raw)
        return len(plan)

    def unshare_risk(self, path):
        """Why unsharing `path` would stop matches loading ("folder", "crowded"), or None (see unshare)."""
        return self.unshare_risk_index(self.path_index(path))

    def unshare_risk_index(self, p):
        if p is None or p not in (self.path_infos or {}):
            return None
        d, i = self.path_infos[p]
        _p, ii, to_data, _fl = self.entry("infos", i, "<4I")
        folder = self.entry("dirs", d, "<Q")[0] >> 40
        if self.folder_read_by_others(folder, d):
            return "folder"
        if self.entry("info_to_datas", to_data, "<3I")[0] != folder and ii in self.crowded:
            return "crowded"
        return None

    def note_unshare_candidates(self, paths):
        """apply's first look at the paths it will try to unshare, [(path, mod)]: the files one mod has
        two or more of them share (self.crowded)."""
        self.unshare(None, None)   # (builds path_infos)
        count = {}
        for path, mod in paths:
            p = self.path_index(path)
            if p is None or p not in self.path_infos:
                continue
            ii = self.entry("infos", self.path_infos[p][1], "<4I")[1]
            count[(ii, mod)] = count.get((ii, mod), 0) + 1
        self.crowded = {ii for (ii, _mod), n in count.items() if n > 1}

    def folder_read_by_others(self, folder, d):
        """Whether a directory other than `d` reads files from `folder`."""
        if self.folder_readers is None:
            self.folder_readers = {}
            infos = self.tabs["infos"]
            for dd, e in enumerate(struct.iter_unpack("<QQQ7I", self.tabs["dirs"])):
                for i in range(e[5], e[5] + e[6]):
                    f = self.entry("info_to_datas", struct.unpack_from("<I", infos, i * 16 + 8)[0], "<3I")[0]
                    r = self.folder_readers.setdefault(f, set())
                    if len(r) < 2:
                        r.add(dd)
        return bool(self.folder_readers.get(folder, set()) - {d})

    # ---- new files and directories ----
    def dir_index(self, h):
        """A directory's index from its path's hash40 (a new one's too), or None."""
        if self.dirs_by_hash is None:
            self.dirs_by_hash = {v & LOW40: v >> 40 for (v,) in struct.iter_unpack("<Q", self.tabs["dir_hash_to_info"])}
        return self.dirs_by_hash.get(h)

    def like_dir(self, path):
        """The game's directory a new one is made like: the same path with another costume slot
        (c00 first: fighter/mario/c08 -> fighter/mario/c00), else Luigi's c00 (as ARCropolis)."""
        if SLOT.search(path):
            for k in range(8):
                like = SLOT.sub(f"/c0{k}", path, count=1)
                d = self.dir_index(hash40(like))
                if d is not None and d not in self.new_dirs:
                    self.dir_names[d] = like
                    return d
        return self.dir_index(hash40("fighter/luigi/c00"))

    def add_dir(self, path, base=None):
        """A directory the game doesn't have (config.json's new-dir-infos), and its parents; with a
        `base` (new-dir-infos-base), one that is the base's: its folder, files and children.
        Returns its index, or None (the base or a parent can't be made)."""
        path = path.strip("/")
        h = hash40(path)
        d = self.dir_index(h)
        if d is not None:
            return d
        parent = path.rsplit("/", 1)[0] if "/" in path else ""
        p = None
        if parent:
            p = self.dir_index(hash40(parent))
            if p is None:
                p = self.add_dir(parent)
            if p is None:
                return None
        like = self.dir_index(hash40(base.strip("/"))) if base else self.like_dir(path)
        if like is None:
            self.notes.append(f"{path}: its base {base} isn't a directory of the game's")
            return None
        d = self.new_dir(h, hash40(path.rsplit("/", 1)[-1]), hash40(parent) if parent else 0, p, like, bool(base))
        self.dir_names_new[d] = path
        m = SLOT.search(path)
        if not base and m and SLOT.search(self.dir_path(like) or ""):
            self.slots[d] = path[m.start() + 2:m.start() + 4]
        return d

    def dir_path(self, d):
        """A directory's path from config.json, or None (the game's names are hashes)."""
        return self.dir_names.get(d)

    def new_dir(self, h, name, parent, p, like, based):
        """A new directory's entries: hash40 `h`, its name's and its parent's hash40s, its parent's
        index `p` (or None), made like directory `like` - with `based`, one that is like's (its
        folder, files and children), else one with a folder of its own, its files to come."""
        lpath, _name, _parent, re1, re2, start, n, cstart, ccount, flags = self.entry("dirs", like, "<QQQ7I")
        if based:
            folder = lpath >> 40
        else:   # (a folder of its own, its chunk empty until the mod's files are laid in it)
            lfo = self.entry("folder_offsets", lpath >> 40, "<QIIIII")
            folder = self.append("folder_offsets", "<QIIIII", 0, 0, 0, 0, 0, lfo[5] if n else 0xFFFFFF)
            start, n, cstart, ccount = 0, 0, 0, 0
        d = self.append("dirs", "<QQQ7I", h | folder << 40, name, parent, re1, re2, start, n, cstart, ccount, flags)
        self.append("dir_hash_to_info", "<Q", h | d << 40)
        self.dirs_by_hash[h] = d
        self.new_dirs[d] = like
        if based:
            self.based.add(d)
        if p is not None:
            self.new_children.setdefault(p, []).append(h | d << 40)
        return d

    def use_config(self, config):
        """ARCropolis' config.json (the turned-on mods', merged): new directories made now; files
        shared and given to directories when the files are committed."""
        for path in config.get("new-dir-infos", []):
            if self.add_dir(path) is None:
                self.notes.append(f"new directory {path} not made")
        for path, base in sorted(config.get("new-dir-infos-base", {}).items()):
            if self.add_dir(path, base) is None:
                self.notes.append(f"new directory {path} not made")
        self.keep_shared = {config_hash(p)[0] for p in config.get("unshare-blacklist", [])}
        for key in ("share-to-vanilla", "share-to-added"):
            for target, news in config.get(key, {}).items():
                th, _ = config_hash(target)
                for new in [news] if isinstance(news, str) else news:
                    nh, np = config_hash(new)
                    if np:   # (a path entry needs the path's own parts: a bare hash can't be one)
                        self.shares.setdefault(nh, (np, th))
        for dpath, files in config.get("new-dir-files", {}).items():
            d = self.dir_index(config_hash(dpath.strip("/") if isinstance(dpath, str) else dpath)[0])
            if d is None:
                if files:
                    self.notes.append(f"new-dir-files: {dpath} isn't a directory")
                continue
            for f in files:
                fh, fp = config_hash(f)
                if fp and ONE_SLOT_EFFECT.match(fp) and not ONE_SLOT_PLUGIN:
                    continue
                self.listed.setdefault(d, []).append((fh, fp))
                self.listed_in.setdefault(fh, d)

    def directories_of(self, parent):
        """The directories whose files are in folder `parent` (a hash40 of "a/b/c/"): (directory,
        whether it has a file there of its own, not another directory's)."""
        if self.parents is None:
            self.parents = {}
            info_path = [e[0] for e in struct.iter_unpack("<4I", self.tabs["infos"])]
            info_index = [e[1] for e in struct.iter_unpack("<4I", self.tabs["infos"])]
            ii_dir = [e[0] for e in struct.iter_unpack("<2I", self.tabs["info_indices"])]
            path_parent = [e[2] & LOW40 for e in struct.iter_unpack("<4Q", self.tabs["paths"])]
            for d, e in enumerate(struct.iter_unpack("<QQQ7I", self.tabs["dirs"])):
                start, n = e[5], e[6]
                for i in range(start, start + n):
                    found = self.parents.setdefault(path_parent[info_path[i]], {})
                    found[d] = found.get(d, False) or ii_dir[info_index[i]] == d
            for d, files in self.listed.items():   # (a new directory's files, as config.json gives them)
                for _h, path in files:
                    if path and "/" in path:
                        self.parents.setdefault(hash40(path.rsplit("/", 1)[0] + "/"), {}).setdefault(d, True)
        return self.parents.get(parent, {})

    def add_file(self, path, raw):
        """Add a file the game doesn't have to its directory (the one config.json gives it to, else
        the one whose files are in its folder); returns None, or why it can't be added."""
        if "/" not in path or ":" in path:
            return "not in a folder of data.arc's"
        d = self.listed_in.get(hash40(path))
        if d is None:
            folder_path = path.rsplit("/", 1)[0] + "/"
            found = self.directories_of(hash40(folder_path))
            own = sorted(d for d, mine in found.items() if mine) or sorted(found)
            if not own:
                return f"a folder the game doesn't have ({folder_path}: a new costume slot needs the mod's config.json)"
            d = own[0]
        self.new_files.setdefault(d, []).append((path, raw))
        return None

    def new_path(self, path, ii):
        """A path entry for a new file, naming file info index `ii`."""
        leaf = path.rsplit("/", 1)[1]
        ext = leaf.rsplit(".", 1)[1] if "." in leaf else ""
        h = hash40(path)
        p = self.append("paths", "<4Q", h | ii << 40, hash40(ext) | 0xFFFFFF << 40, hash40(path.rsplit("/", 1)[0] + "/"),
                        hash40(leaf))
        self.new_paths.append((h, p))
        self.path_names.append(path)
        return p

    def new_path_hashed(self, h, parts, ii):
        """new_path for a path known by its hash40s only: (extension, folder, name)."""
        ext, parent, name = parts
        p = self.append("paths", "<4Q", h | ii << 40, ext | 0xFFFFFF << 40, parent, name)
        self.new_paths.append((h, p))
        return p

    def file_flags(self, d):
        """extension hash40 -> a file info's flags of one of the directory's own files (or of the
        directory a new one is made like), None -> any of them."""
        out = {}
        for dd in (d, self.new_dirs.get(d)):
            if dd is None:
                continue
            _dp, _n, _p, _r1, _r2, start, n, _cs, _cc, _fl = self.entry("dirs", dd, "<QQQ7I")
            for i in range(start, start + n):
                p, ii, _td, fflags = self.entry("infos", i, "<4I")
                if self.entry("info_indices", ii, "<2I")[0] == dd and not fflags & SHARED_INFO:
                    out.setdefault(self.entry("paths", p, "<4Q")[1] & LOW40, fflags)
                    out.setdefault(None, fflags)
            if out:
                break
        return out

    def complete_slots(self):
        """A new costume slot as the game's own are, whatever config.json leaves out (a reslotter's
        list of names misses some of the game's files): for a new fighter/<f>/cNN, each of the
        fighter's directories with a costume directory of the slot it is made like (camera/c00,
        result/c00, kirbycopy/c00, movie/c00) gets its cNN too, and each new slot directory's
        files - and children (cmn, camera: the like one's) - that config.json doesn't give it are
        the files the like one has, shared. A path the game has no name for is made from the like
        one's hash40 (reslot), the digit's place found where another of the game's slots has it."""
        if not self.slots:
            return
        known = {v & LOW40 for (v,) in struct.iter_unpack("<Q", self.tabs["hash_to_path"])}
        for d, slot in list(self.slots.items()):   # (a linked fighter's slot: Mythra's and both's for Pyra's)
            m = re.fullmatch(r"fighter/([^/]+)/c(\d\d)", self.dir_names_new.get(d, ""))
            for group in LINKED if m else ():
                for other in group if m.group(1) in group else ():
                    path = f"fighter/{other}/c{slot}"
                    if self.dir_index(hash40(path)) is None and self.dir_index(hash40(f"fighter/{other}/c00")) is not None:
                        if self.add_dir(path) is not None:
                            self.notes.append(f"made {path}, as config.json didn't (the fighter goes with {m.group(1)})")
        for d, slot in list(self.slots.items()):
            path = self.dir_names_new.get(d, "")
            if not re.fullmatch(r"fighter/[^/]+/c\d\d", path):
                continue
            like = self.new_dirs[d]
            old = self.dir_path(like)[-2:]
            fighter = self.dir_index(hash40(path.rsplit("/", 1)[0]))
            name = hash40(f"c{old}")
            for c in self.children(fighter):
                for g in self.children(c):
                    gh, gname = self.entry("dirs", g, "<QQ")
                    if gname & LOW40 != name or g in self.new_dirs:
                        continue
                    gh &= LOW40
                    nh = reslot2(gh, (gh >> 32) - 1, old, slot)
                    if self.dir_index(nh) is None:
                        self.slots[self.new_dir(nh, hash40(f"c{slot}"), self.entry("dirs", c, "<Q")[0] & LOW40, c, g, False)] = slot
                        self.notes.append(f"made {path.rsplit('/', 1)[0]}/<{hex(gh)}>: c{slot}, as config.json didn't")
        for d, slot in self.slots.items():
            like = self.new_dirs[d]
            dh = self.entry("dirs", d, "<Q")[0] & LOW40
            lh = self.entry("dirs", like, "<Q")[0] & LOW40
            at = (lh >> 32) - 1
            old = None
            for k in range(8):   # (which of the game's slots `like` is)
                if reslot2(dh, at, slot, f"0{k}") == lh:
                    old = f"0{k}"
            if old is None:
                continue
            kids = {self.entry("dirs", k, "<Q")[0] & LOW40 for k in self.children(d)}
            for k in self.children(like):
                kh, kname = self.entry("dirs", k, "<QQ")
                nh = reslot2(kh & LOW40, at, old, slot)
                if nh not in kids and self.dir_index(nh) is None:
                    self.new_dir(nh, kname, dh, d, k, True)
            self.complete_files(d, like, old, slot, known)

    def children(self, d):
        """A directory's child directories (the new ones too)."""
        _p, _n, _pa, _r1, _r2, _s, _c, cstart, ccount, _f = self.entry("dirs", d, "<QQQ7I")
        out = [self.entry("child_hashes", i, "<Q")[0] >> 40 for i in range(cstart, cstart + ccount)]
        return out + [v >> 40 for v in self.new_children.get(d, ())]

    def complete_files(self, d, like, old, slot, known):
        """The files of `like` new slot directory `d` has no path for (config.json's new-dir-files
        doesn't list them), shared to the game's (as share-to-vanilla would)."""
        have = {h for h, _p in self.listed.get(d, ())} | {hash40(p) for p, _r in self.new_files.get(d, ())}
        _lp, _n, _p, _r1, _r2, start, n, _cs, _cc, _fl = self.entry("dirs", like, "<QQQ7I")
        missing = added = 0
        for i in range(start, start + n):
            v, ext, parent, name = self.entry("paths", self.entry("infos", i, "<4I")[0], "<4Q")
            h, ext, parent, name = v & LOW40, ext & LOW40, parent & LOW40, name & LOW40
            # (the place other slots of the game's have the file at - the most of them, as a name
            # can have digits of its own: def_light_001_col)
            votes = {a: sum(reslot2(h, a, old, f"0{k}") in known for k in "01234567" if f"0{k}" != old) for a in range(1, h >> 32)}
            at = max(votes, key=lambda a: (votes[a], a)) if votes and max(votes.values()) else None
            if at is None:   # (no other slot has it: a file of this slot's only)
                missing += 1
                continue
            nh = reslot2(h, at, old, slot)
            if nh in have:
                continue
            plen = parent >> 32
            if at < plen:
                parent = reslot2(parent, at, old, slot)
            elif (h >> 32) == plen + (name >> 32):
                name = reslot2(name, at - plen, old, slot)
            else:
                missing += 1
                continue
            have.add(nh)
            added += 1
            self.listed.setdefault(d, []).append((nh, None))
            self.listed_in.setdefault(nh, d)
            if nh not in self.shares:   # (a path two directories list - result/c00's motion_list.bin and c00's - is shared once)
                self.shares[nh] = ((ext, parent, name), h)
                self.like_paths.append((nh, h, at, old, slot))
        name = self.dir_names_new.get(d) or hex(self.entry("dirs", d, "<Q")[0] & LOW40)
        if added:
            self.notes.append(f"{name}: {added} of the game's files shared to it, as config.json didn't list them")
        if missing:
            self.notes.append(f"{name}: {missing} of {self.dir_path(like) or 'its like'}'s files left out (no other costume has them)")

    def commit_files(self):
        """The added files' and directories' entries: each directory that gets files has its run of
        infos copied to the end, its new files after (the mod's own, in its folder's chunk, and
        copies of the files it shares); then the new directories in their parents' children."""
        regions = self.arc.regions
        self.complete_slots()
        own = {}   # hash40 -> (directory, path, bytes, path index, info index index)
        for d in sorted(self.new_files):
            for path, raw in sorted(self.new_files[d]):
                ii = self.append("info_indices", "<2I", d, 0)
                own[hash40(path)] = (d, path, raw, self.new_path(path, ii), ii)
        shared = {}   # hash40 -> (path index, info index index) of a new path that is another file
        for nh, (np, th) in sorted(self.shares.items()):
            if nh in own or self.path_index_of(nh) is not None:
                continue   # (the mod's own file, or the game's, wins)
            if th in own:
                tii = own[th][4]
            elif th in shared:
                tii = shared[th][1]
            else:
                tp = self.path_index_of(th)
                if tp is None:
                    self.notes.append(f"{np}: the file it shares isn't the game's or the mod's")
                    continue
                # (the game's file, not a mod's file unshared from it: a new costume's model is the
                # game's c00 one, whatever a mod makes of c00's)
                tii = self.vanilla_ii.get(tp, self.entry("paths", tp, "<Q")[0] >> 40)
            shared[nh] = (self.new_path(np, tii) if isinstance(np, str) else self.new_path_hashed(nh, np, tii), tii)

        # each directory's new files, in order (the listed ones by path, then the rest)
        plans = {}
        for d in sorted(set(self.new_files) | set(self.listed) | set(self.unshared)):
            _dp, _n, _p, _r1, _r2, start, n, _cs, _cc, _fl = self.entry("dirs", d, "<QQQ7I")
            have = {self.entry("paths", self.entry("infos", i, "<4I")[0], "<Q")[0] & LOW40 for i in range(start, start + n)}
            plan = []
            for h, path in sorted(set(self.listed.get(d, ())), key=lambda x: (x[1] or "", x[0])):
                if h in have:
                    continue
                if h in own and own[h][0] == d:
                    plan.append(("own", h))
                elif h in own or h in shared:
                    plan.append(("share", h))
                elif path and self.path_index_of(h) is not None:
                    plan.append(("share", h))   # (a file of the game's, listed in the directory as it is)
                else:
                    self.notes.append(f"{path or hex(h)}: listed in a new directory, but the mod doesn't have it")
                    continue
                have.add(h)
            plan += [("own", hash40(path)) for path, _raw in sorted(self.new_files.get(d, ())) if hash40(path) not in have]
            plan += [("unshared", u) for u in self.unshared.get(d, ())]
            if plan:
                plans[d] = plan

        # the mod's own files' data, in each directory's order: a directory that is another's (its
        # folder that one's) gets a folder of its own for them. A new directory's folder has a data
        # entry for each of its files, in their order (its range: start, count), as the game's own
        # folders have - empty for a file it shares
        place = {}   # hash40 -> (folder, data, info-to-data, flags)
        for d, plan in plans.items():
            dir_path, name, parent, re1, re2, start, n, cstart, ccount, flags = self.entry("dirs", d, "<QQQ7I")
            fresh = d in self.new_dirs and d not in self.based and n == 0
            if not fresh and not any(kind in ("own", "unshared") for kind, _h in plan):
                continue
            folder = dir_path >> 40
            if d in self.based:
                res = self.entry("folder_offsets", folder, "<QIIIII")[5]
                folder = self.append("folder_offsets", "<QIIIII", 0, 0, 0, 0, 0, res)
                self.put("dirs", d, "<QQQ7I", (dir_path & LOW40) | folder << 40, name, parent, re1, re2, start, n, cstart, ccount, flags)
                self.based.discard(d)
                self.new_dirs.setdefault(d, None)
            sibling = self.file_flags(d)
            first = None
            for kind, h in plan:
                if kind == "unshared":
                    # (a regional file: its run as the game's are, an entry before the regions' - the game
                    # reads to_data + 1 + its region - and one for each region: the mod's one file, or
                    # with reshare the game's own bytes of each, one data entry for each distinct one)
                    p, _ii, raw, fflags = h
                    raws = raw if isinstance(raw, list) else [raw] * (regions + 1 if fflags & 0x8000 else 1)
                    datas = {}
                    to_data = None
                    for r in raws:
                        if id(r) not in datas:
                            stored = game_compress(r)
                            datas[id(r)] = self.append("datas", "<4I", 0, len(stored), len(r), 3)
                            first = datas[id(r)] if first is None else first
                            self.added.setdefault(folder, []).append((datas[id(r)], stored, len(r), 3))
                        e = self.append("info_to_datas", "<3I", folder, datas[id(r)], OWN_FILE)
                        to_data = e if to_data is None else to_data
                    place[("unshared", p)] = (folder, datas[id(raws[0])], to_data, fflags)
                    continue
                if kind != "own":
                    if fresh:
                        data = self.append("datas", "<4I", 0, 0, 0, 0)
                        first = data if first is None else first
                    continue
                path, raw = own[h][1], own[h][2]
                ext = path.rsplit("/", 1)[1].rsplit(".", 1)[1] if "." in path.rsplit("/", 1)[1] else ""
                fflags = sibling.get(hash40(ext), sibling.get(None, 0x1000 if ext in ("nutexb", "eff") else 0x10))
                stored = game_compress(raw)
                data = self.append("datas", "<4I", 0, len(stored), len(raw), 3)
                first = data if first is None else first
                to_data = self.append("info_to_datas", "<3I", folder, data, OWN_FILE)
                self.added.setdefault(folder, []).append((data, stored, len(raw), 3))
                place[h] = (folder, data, to_data, fflags & ~(SHARED_INFO | REGIONAL_INFO))
            fo = list(self.entry("folder_offsets", folder, "<QIIIII"))
            if d in self.new_dirs and fo[2] == 0 and fo[3] == 0:
                fo[3] = first   # (its first data entry)
                self.put("folder_offsets", folder, "<QIIIII", *fo)

        own_ii = {o[4]: h for h, o in own.items()}
        like_runs = {}   # directory -> {info index index: (flags, info-to-data)} of its run

        def copy_of(tii, d):
            """A copy in directory `d` of the file at info index index `tii`: (flags, info-to-data
            entries), as the game's costumes copy c00's - from the entry the directory `d` is made
            like has for it (else the file's own): its own file shared (SHARED_INFO, the last field
            the info index index), a file it reads from its folder's dependency (the folder entry's
            last field) as it is, one from another folder shared; None for a mod's file in no
            directory."""
            if tii in own_ii:
                if own_ii[tii] not in place:
                    return None
                folder, data, _td, fflags = place[own_ii[tii]]
                return fflags | SHARED_INFO, [(folder, data, tii)]
            like = self.new_dirs.get(d)
            if like is not None and like not in like_runs:
                _lp, _n, _p, _r1, _r2, lstart, ln, _cs, _cc, _fl = self.entry("dirs", like, "<QQQ7I")
                like_runs[like] = {}
                for i in range(lstart, lstart + ln):
                    _p, ii, td, fflags = self.entry("infos", i, "<4I")
                    like_runs[like].setdefault(ii, (fflags, td))
            fflags, td = like_runs.get(like, {}).get(tii) or self.entry("infos", self.entry("info_indices", tii, "<2I")[1], "<4I")[3:1:-1]
            run = [self.entry("info_to_datas", td + r, "<3I") for r in range(regions + 1 if fflags & 0x8000 else 1)]
            dep = self.entry("folder_offsets", self.entry("dirs", d, "<Q")[0] >> 40, "<QIIIII")[5]
            if run[0][2] >> 24 == 1 or (run[0][2] >> 24 == 3 and run[0][0] != dep):
                return fflags | SHARED_INFO, [(run[0][0], run[0][1], tii)] * len(run)
            return fflags, run   # (already another's, or its folder's dependency's: the same entries)

        for d, plan in plans.items():
            dir_path, name, parent, re1, re2, start, n, cstart, ccount, flags = self.entry("dirs", d, "<QQQ7I")
            new_start = self.count("infos")
            for i in range(start, start + n):
                p, ii, to_data, fflags = self.entry("infos", i, "<4I")
                moved = self.append("infos", "<4I", p, ii, to_data, fflags)
                in_dir, info = self.entry("info_indices", ii, "<2I")
                if info == i:
                    self.put("info_indices", ii, "<2I", in_dir, moved)
            for kind, h in plan:
                if kind == "unshared":
                    p, ii, _raw, _fl = h
                    _folder, _data, to_data, fflags = place[("unshared", p)]
                    self.put("info_indices", ii, "<2I", d, self.append("infos", "<4I", p, ii, to_data, fflags))
                    continue
                if kind == "own":
                    _d, _path, _raw, p, ii = own[h]
                    _folder, _data, to_data, fflags = place[h]
                    info = self.append("infos", "<4I", p, ii, to_data, fflags)
                    self.put("info_indices", ii, "<2I", d, info)
                    continue
                p, tii = shared[h] if h in shared else (own[h][3], own[h][4]) if h in own else (None, None)
                if p is None:
                    p = self.path_index_of(h)
                    tii = self.entry("paths", p, "<Q")[0] >> 40
                copy = copy_of(tii, d)
                if copy is None:
                    continue
                fflags, run = copy
                to_data = self.count("info_to_datas")
                for e in run:
                    self.append("info_to_datas", "<3I", *e)
                self.append("infos", "<4I", p, tii, to_data, fflags)
            count = self.count("infos") - new_start
            self.put("dirs", d, "<QQQ7I", dir_path, name, parent, re1, re2, new_start, count, cstart, ccount, flags)
            folder = dir_path >> 40
            if d in self.new_dirs and d not in self.based:   # (a new folder counts its directory's files)
                fo = list(self.entry("folder_offsets", folder, "<QIIIII"))
                fo[4] = count - len(self.added.get(folder, ()))
                self.put("folder_offsets", folder, "<QIIIII", *fo)
        for h, o in own.items():
            if h not in place:
                self.notes.append(f"{o[1]}: in no directory")
        self.new_files = {}
        self.unshared = {}

        # new directories in their parents' children: the parent's run copied to the end, the new after
        for p in sorted(self.new_children):
            dir_path, name, parent, re1, re2, start, n, cstart, ccount, flags = self.entry("dirs", p, "<QQQ7I")
            kids = [self.entry("child_hashes", i, "<Q")[0] for i in range(cstart, cstart + ccount)] + self.new_children[p]
            new_start = self.count("child_hashes")
            for k in kids:
                self.append("child_hashes", "<Q", k)
            self.put("dirs", p, "<QQQ7I", dir_path, name, parent, re1, re2, start, n, new_start, len(kids), flags)
        self.new_children = {}

        if self.new_paths:   # each bucket's paths sorted by hash, the new ones among them
            old = self.tabs["hash_to_path"]
            new = bytearray()
            by_bucket = {}
            for h, p in self.new_paths:
                by_bucket.setdefault(h % self.arc.buckets, []).append(h | p << 40)
            for b in range(self.arc.buckets):
                at = self.arc.bucket_table + b * 8
                start, n = struct.unpack_from("<2I", self.head, at)
                struct.pack_into("<2I", self.head, at, len(new) // 8, n + len(by_bucket.get(b, ())))
                if b in by_bucket:
                    values = list(struct.unpack_from(f"<{n}Q", old, start * 8)) + by_bucket[b]
                    new += struct.pack(f"<{len(values)}Q", *sorted(values, key=lambda v: v & LOW40))
                else:
                    new += old[start * 8:(start + n) * 8]
            self.tabs["hash_to_path"] = new
            self.new_paths = []

    def to_bytes(self):
        """The table, its counts as its tables have grown."""
        fs = list(struct.unpack_from("<22I", self.head, 0))
        assert self.count("hash_to_path") == self.count("paths")
        assert self.count("dirs") == self.count("dir_hash_to_info")
        fs[1] = self.count("paths")
        fs[2] = self.count("info_indices")
        fs[3] = self.count("dirs")
        fs[5] = self.count("child_hashes")
        fs[16] = self.count("folder_offsets") - fs[4] - fs[9]   # 0x40: extra folders
        fs[19] = self.count("infos") - fs[6] - fs[10]          # 0x4C: extra infos
        fs[20] = self.count("info_to_datas") - fs[7] - fs[10]  # 0x50: extra info-to-datas
        fs[21] = self.count("datas") - fs[8] - fs[10]          # 0x54: extra datas
        # (the game finds a directory by a binary search of its hash)
        self.tabs["dir_hash_to_info"] = bytearray(b"".join(sorted(
            (self.tabs["dir_hash_to_info"][i:i + 8] for i in range(0, len(self.tabs["dir_hash_to_info"]), 8)),
            key=lambda e: int.from_bytes(e, "little") & LOW40)))
        struct.pack_into("<I", self.head, self.arc.bucket_table - 8, fs[1])   # (the buckets' entry count)
        fs[0] = len(self.head) + sum(len(v) for v in self.tabs.values()) + len(self.extra)
        struct.pack_into("<22I", self.head, 0, *fs)
        return bytes(self.head) + b"".join(self.tabs[name] for name in TABLES) + self.extra

    def folder_files(self):
        """folder -> the file data entries in its chunk: the ones its info-to-data entries name
        (the folder's own range of data entries counts other folders' too)."""
        if self.owned is None:
            self.owned = {}
            for folder, data, _ in struct.iter_unpack("<3I", self.tabs["info_to_datas"]):
                if data < self.base_datas:   # (unused entries name no data; added files come after)
                    self.owned.setdefault(folder, set()).add(data)
        return self.owned

    def rebuild_folders(self, alias, place):
        """Each folder a mod changes, laid out again: its files in their order, a changed file's
        new bytes in its place, the unchanged ones' original bytes (`alias`: read from the game's
        files, not copied), added files at the end; the folder and its files' entries pointed there.
        The game reads a folder's chunk as one stream, each file where the one before ends
        (16-aligned): its loader (f_7103544a70) steps through the chunk by each file's stored
        size, not by the file's offset. So a changed file has to stay in its place, the files
        after it moved along (a file put after the chunk: the files read after it are read from
        the wrong place - a hang, or a crash in the loader's copy)."""
        owned = self.folder_files()
        for folder in sorted(set(self.replaced) | set(self.added)):
            off, decomp, comp, start, count, res = self.entry("folder_offsets", folder, "<QIIIII")
            files = [(o, d) for o, _, d in sorted((self.entry("datas", d, "<4I")[0] * 4, self.entry("datas", d, "<4I")[1] > 0, d)
                                                  for d in owned.get(folder, ()))]   # (an empty file before a file at its offset)
            changes = self.replaced.get(folder, {})
            added = self.added.get(folder, [])
            # the chunk in pieces: runs of unchanged files (old offsets [a, b), the files) and changed files
            # (an empty file, stored size 0, is in the run it is at the end of)
            pieces, pos = [], 0
            for in_folder, d in files:
                if in_folder != pos:
                    raise Error(f"folder {folder}'s files aren't laid out one after another (file data {d})")
                pos = align(pos + self.entry("datas", d, "<4I")[1], 0x10)
                if d in changes:
                    pieces.append(("new", d) + changes[d])
                elif pieces and pieces[-1][0] == "old":
                    pieces[-1][2] = pos
                    pieces[-1][3].append((in_folder, d))
                else:
                    pieces.append(["old", in_folder, pos, [(in_folder, d)]])
            if pos != comp:
                raise Error(f"folder {folder}'s files don't make up its data")
            pieces += [("new",) + a for a in added]
            base, end, grow = None, None, 0
            for piece in pieces:
                if piece[0] == "old":
                    _, a, b, members = piece
                    where = alias(self.arc.file_section + off + a, b - a, 0x10)
                    base = where if base is None else base
                    for in_folder, d in members:
                        o, stored, size, flags = self.entry("datas", d, "<4I")
                        self.put("datas", d, "<4I", (where + in_folder - a - base) // 4, stored, size, flags)
                    where_end = where + b - a
                else:
                    _, d, stored, size, flags = piece
                    where = place(stored, 0x10)
                    base = where if base is None else base
                    grow += size - self.entry("datas", d, "<4I")[2] if d < self.base_datas else align(size, 0x10)
                    self.put("datas", d, "<4I", (where - base) // 4, len(stored), size, flags)
                    where_end = align(where + len(stored), 0x10)
                if end is not None and where != end:   # (alias and place pad to 0x10, as the chunk is)
                    raise Error(f"folder {folder}: a file isn't where the one before it ends")
                end = where_end
            self.put("folder_offsets", folder, "<QIIIII", base - self.arc.file_section, max(0, decomp + grow),
                     end - base, start, count + len(added), res)

    def stream_targets(self, path):
        """The stream-data entries of a stream: file (each of its variants), or None."""
        if self.streams is None:
            self.streams = {}
            for i in range(self.arc.tab["stream_entries"][2]):
                full, flags = self.entry("stream_entries", i, "<QI")
                self.streams[full & LOW40] = (full >> 40, flags)
        found = self.streams.get(hash40(path))
        if not found:
            return None
        index, flags = found
        nidx = self.arc.tab["stream_indices"][2]
        out = []
        for r in range((14 if flags & 1 else 5) if flags & 3 else 1):
            if index + r < nidx:
                out.append(self.entry("stream_indices", index + r, "<I")[0])
        return sorted(set(out))


def mod_config(tree, names):
    """The turned-on mods' config.json (ARCropolis'), merged in their order: new-dir-infos,
    new-dir-infos-base, share-to-vanilla, share-to-added, new-dir-files (what a mod adding a costume
    slot has, as the reslotter writes it), unshare-blacklist (paths whose file stays shared)."""
    out = {"new-dir-infos": [], "new-dir-infos-base": {}, "share-to-vanilla": {}, "share-to-added": {}, "new-dir-files": {},
           "unshare-blacklist": []}
    for name in names:
        path = os.path.join(tree.mods, name, "config.json")
        if not os.path.exists(path):
            continue
        try:
            with open(path, encoding="utf-8-sig") as f:
                cfg = json.load(f)
        except (OSError, ValueError) as e:
            raise Error(f"{name}: its config.json can't be read ({e})")
        out["new-dir-infos"] += [d for d in cfg.get("new-dir-infos", []) if d not in out["new-dir-infos"]]
        out["unshare-blacklist"] += [p for p in cfg.get("unshare-blacklist", []) if p not in out["unshare-blacklist"]]
        out["new-dir-infos-base"].update(cfg.get("new-dir-infos-base", {}))
        for key in ("share-to-vanilla", "share-to-added", "new-dir-files"):
            for k, v in (cfg.get(key) or {}).items():
                have = out[key].setdefault(k, [])
                have += [x for x in ([v] if isinstance(v, str) else v) if x not in have]
    return out


NO_CHILD = 0xFFFFFF
SEARCH_FOLDER = 0x400000   # a search path entry's parent index for a folder (a folder's child folder)


class Search:
    """data.arc's search section (the header's sixth field: zstd-compressed as the file table is):
    every path by folder - folders (a lookup by hash, sorted), paths (a lookup by hash, sorted, to a
    place in a list of path indices) - each folder's files and folders a linked list of paths. The
    game lists a folder's files with it (a costume's model: fighter/mario/model/body/c08/), so a
    mod's new paths are added to it as ARCropolis adds them (a new folder made, its parents too).

    A folder: its path (hash40, its index the number of its folders), its parent's (index: the
    number of its files), its name, and its first child (the low 32 bits of the last field, the
    number of children above them). A path: its own (index: the next in its folder's list), its
    folder's (index SEARCH_FOLDER for a folder), its name, its extension."""

    def __init__(self, read, at):
        offset, _decomp, comp, _ = struct.unpack("<4I", read(at, 16))
        s = unzstd(read(at + offset, comp), "data.arc's search section")
        _size, nf, ni, npaths = struct.unpack_from("<QIII", s, 0)
        o = 0x14
        parts = []
        for count, size in ((nf, 8), (nf, 32), (ni, 8), (ni, 4), (npaths, 32)):
            parts.append(bytearray(s[o:o + count * size]))
            o += count * size
        self.folder_lookup, self.folders, self.path_lookup, self.indices, self.paths = parts
        self.folder_at = {v & LOW40: v >> 40 for (v,) in struct.iter_unpack("<Q", self.folder_lookup)}
        self.path_at = {v & LOW40: v >> 40 for (v,) in struct.iter_unpack("<Q", self.path_lookup)}
        self.added = 0

    def add_path(self, entry, h):
        """A path entry at the end of the paths, and its place in the path indices; returns that place."""
        p = len(self.paths) // 32
        self.paths += struct.pack("<4Q", *entry)
        at = len(self.indices) // 4
        self.indices += struct.pack("<I", p)
        self.path_lookup += struct.pack("<Q", h | at << 40)
        self.path_at[h] = at
        return at

    def link(self, folder, entry, h, is_folder):
        """A path first in a folder's list of children; the folder's counts grown."""
        path, parent, name, ext = struct.unpack_from("<4Q", self.folders, folder * 32)
        at = self.add_path((entry[0] | (ext & NO_CHILD) << 40,) + tuple(entry[1:]), h)
        if is_folder:
            path += 1 << 40
        else:
            parent += 1 << 40
        ext = ((ext >> 32) + 1) << 32 | at
        struct.pack_into("<4Q", self.folders, folder * 32, path, parent, name, ext)

    def folder(self, path):
        """The folder of a path ("a/b/c", no slash at the end), made (and its parents) if new."""
        h = hash40(path)
        if h in self.folder_at:
            return self.folder_at[h]
        parent = path.rsplit("/", 1)[0] if "/" in path else ""
        f = len(self.folders) // 32
        self.folders += struct.pack("<4Q", h, hash40(parent), hash40(path.rsplit("/", 1)[-1]), NO_CHILD)
        self.folder_lookup += struct.pack("<Q", h | f << 40)
        self.folder_at[h] = f
        if parent:
            self.link(self.folder(parent), (h, hash40(parent) | SEARCH_FOLDER << 40, hash40(path.rsplit("/", 1)[-1]), 0), h, True)
        return f

    def add_file(self, path):
        """A file's path, in its folder (a new path: one the section doesn't have)."""
        h = hash40(path)
        if h in self.path_at or "/" not in path:
            return
        folder, leaf = path.rsplit("/", 1)
        ext = leaf.rsplit(".", 1)[1] if "." in leaf else ""
        self.link(self.folder(folder), (h, hash40(folder), hash40(leaf), hash40(ext)), h, False)
        self.added += 1

    def folder_like(self, h, like, at, old, new):
        """The folder of hash40 `h`, made (and its parents) if new from folder `like` - the game's
        one whose path is h's with `old` for the slot `new` at `at` (Table.complete_slots)."""
        if h in self.folder_at:
            return self.folder_at[h]
        f = self.folder_at[like]
        _path, parent, name, _ext = struct.unpack_from("<4Q", self.folders, f * 32)
        parent &= LOW40
        plen = parent >> 32 if parent else -1
        if at < plen:
            parent = reslot2(parent, at, old, new)
            self.folder_like(parent, self.folders_parent(f), at, old, new)
        elif at > plen:
            name = reslot2(name & LOW40, at - plen - 1, old, new)
        nf = len(self.folders) // 32
        self.folders += struct.pack("<4Q", h, parent, name & LOW40, NO_CHILD)
        self.folder_lookup += struct.pack("<Q", h | nf << 40)
        self.folder_at[h] = nf
        if parent:
            self.link(self.folder_at[parent], (h, parent | SEARCH_FOLDER << 40, name & LOW40, 0), h, True)
        return nf

    def folders_parent(self, f):
        return struct.unpack_from("<4Q", self.folders, f * 32)[1] & LOW40

    def add_like(self, h, like, at, old, new):
        """A path known by its hash40 only, made from the game's path `like` (Table.complete_slots)."""
        if h in self.path_at or like not in self.path_at:
            return
        p = struct.unpack_from("<I", self.indices, self.path_at[like] * 4)[0]
        _path, folder, name, ext = struct.unpack_from("<4Q", self.paths, p * 32)
        folder &= LOW40
        flen = folder >> 32
        if at < flen:
            new_folder = reslot2(folder, at, old, new)
            self.folder_like(new_folder, folder, at, old, new)
            folder = new_folder
        else:
            name = reslot2(name & LOW40, at - flen - 1, old, new)
        self.link(self.folder_at[folder], (h, folder, name & LOW40, ext & LOW40), h, False)
        self.added += 1

    def to_bytes(self):
        """The section (uncompressed), its lookups sorted by hash."""
        def by_hash(table):
            return b"".join(sorted((table[i:i + 8] for i in range(0, len(table), 8)),
                                   key=lambda e: int.from_bytes(e, "little") & LOW40))
        body = by_hash(self.folder_lookup) + self.folders + by_hash(self.path_lookup) + self.indices + self.paths
        return struct.pack("<QIII", 0x14 + len(body), len(self.folders) // 32, len(self.indices) // 4, len(self.paths) // 32) + body


def align(n, a):
    return (n + a - 1) // a * a


def split_layers(entries, size, overrides):
    """Layer entries (at, src, off) over [0, size) with `overrides` (at, end, src, off) - apart from
    each other - laid on top: one sweep over both, in order."""
    spans = []
    for k, (at, src, off) in enumerate(entries):
        end = entries[k + 1][0] if k + 1 < len(entries) else size
        if at < end:
            spans.append((at, end, src, off))
    overrides = sorted(o for o in overrides if o[0] < o[1])
    out, i = [], 0
    for at, end, src, off in spans:
        pos = at
        while i < len(overrides) and overrides[i][1] <= pos:
            i += 1
        j = i
        while pos < end:
            if j < len(overrides) and overrides[j][0] < end:
                oat, oend = overrides[j][0], overrides[j][1]
                if oat > pos:
                    out.append((pos, src, off if src == LAYER_ZERO else off + (pos - at)))
                pos = max(pos, oend)
                j += 1
            else:
                out.append((pos, src, off if src == LAYER_ZERO else off + (pos - at)))
                pos = end
    out += [(oat, osrc, ooff) for oat, _oend, osrc, ooff in overrides]
    out.sort()
    return out


def pieces(entries, size, a, b, to):
    """Layer overrides that put image bytes [a, b) (as `entries` lay them) at `to` on."""
    out = []
    for k, (at, src, off) in enumerate(entries):
        end = entries[k + 1][0] if k + 1 < len(entries) else size
        lo, hi = max(at, a), min(end, b)
        if lo < hi:
            out.append((to + lo - a, to + hi - a, src, off if src == LAYER_ZERO else off + (lo - at)))
    return out


def apply(tree, log=print):
    """Make romfs.cfg read the turned-on mods (or the game as it is, when none is on)."""
    global ONE_SLOT_PLUGIN
    ONE_SLOT_PLUGIN = os.path.exists(os.path.join(tree.root, "skyline", "plugins", "libone_slot_eff.nro"))
    lines = vanilla_cfg(tree)
    state = tree.load()
    on = [n for n in state["order"] if state["mods"].get(n, {}).get("enabled")]
    if os.path.exists(os.path.join(tree.root, "skyline", "plugins", "libarcropolis.nro")):
        # the real ARCropolis is among the standard plugins: it reads the turned-on mods from
        # sd:/ultimate/mods (runtime/hle_fs.c) as the game loads, so the game keeps its own data
        select_screen_room(tree, on, log)
        characters_mod(tree, log)
        missing_shares(tree, on, log)
        sync_plugins(tree, log)
        shutil.copyfile(tree.vanilla_cfg, tree.cfg)
        os.makedirs(tree.work, exist_ok=True)
        with open(os.path.join(tree.work, "applied.json"), "w", encoding="utf-8") as f:
            json.dump({"mods": on, "files": 0, "skipped": {}, "arcropolis": True}, f)
        log(f"{len(on)} mod(s) on: ARCropolis loads them as the game runs (the game's own data stays as it is)")
        return {"mods": on, "files": 0, "skipped": {}}
    if not on:
        sync_plugins(tree, log)
        shutil.copyfile(tree.vanilla_cfg, tree.cfg)
        os.makedirs(tree.work, exist_ok=True)
        with open(os.path.join(tree.work, "applied.json"), "w", encoding="utf-8") as f:
            json.dump({"mods": [], "files": 0, "skipped": {}}, f)
        log("no mod is turned on: the game as it is")
        return {"mods": [], "files": 0, "skipped": {}}
    if lines[0] != "layered":
        raise Error("the game's data isn't read through a layer map here (romfs.cfg): mods need it (run the installer again)")
    image = RomfsImage(tree.vanilla_cfg)
    try:
        result = _apply(tree, image, lines, state, on, log)
    finally:
        close_image(image)
    sync_plugins(tree, log)
    return result


def plugin_file_callbacks(tree, table, log):
    """Files Skyline plugins change as the game loads them (ARCropolis' file callbacks, which the
    runtime runs when it starts: runtime/hle_skyline.c). For each file listed in
    dump/skyline/callbacks.txt, its bytes as the mods make it go to dump/skyline/arc/<hash>.in; what the
    plugins made of those bytes (<hash>.out, from the last start) goes in the game's data. A .out made
    from other bytes is dropped (tools/skyline.py probe starts the game again and applies)."""
    folder = os.path.join(tree.root, "dump", "skyline", "arc")
    listed = os.path.join(tree.root, "dump", "skyline", "callbacks.txt")
    if not os.path.exists(listed):
        return
    os.makedirs(folder, exist_ok=True)
    for line in open(listed, encoding="utf-8"):
        parts = line.split()
        if not parts:
            continue
        h = int(parts[0], 16)
        p = table.path_index_of(h)
        if p is None:
            log(f"Skyline plugins: a file callback for {h:#012x}, which the game doesn't have: left out")
            continue
        full = table.entry("paths", p, "<Q")[0]
        info = table.entry("info_indices", full >> 40, "<2I")[1]
        _, _, to_data, flags = table.entry("infos", info, "<4I")
        targets = sorted({tuple(table.entry("info_to_datas", to_data + r, "<3I")[:2])
                          for r in (range(1, table.arc.regions + 1) if flags & 0x8000 else (0,))})
        f0, d0 = targets[0]
        if d0 in table.replaced.get(f0, {}):
            stored, _size, dflags = table.replaced[f0][d0]
            data = unzstd(stored, "a modded file") if dflags & 3 else stored
        else:
            data = table.original(f0, d0)
        src, out = os.path.join(folder, f"{h:010x}.in"), os.path.join(folder, f"{h:010x}.out")
        if not os.path.exists(src) or open(src, "rb").read() != data:
            with open(src, "wb") as f:
                f.write(data)
            if os.path.exists(out):
                os.remove(out)   # (made from other bytes)
        if os.path.exists(out):
            new = open(out, "rb").read()
            for folder_i, d in targets:
                table.replace(folder_i, d, new)
            log(f"Skyline plugins: {h:#012x} changed by a plugin's file callback ({len(data)} -> {len(new)} bytes)")


CHARACTERS = ".characters"   # (mods/.characters: the plugins' new characters as a mod ARCropolis loads last)


def characters_mod(tree, log):
    """With the real ARCropolis: the select-screen entries Skyline plugins add (dump/skyline/csk.json,
    the CSK Collection's API, which runs natively) merged into the game's ui_chara_db / ui_layout_db,
    as files of a mod of their own (mods/.characters, hidden from the menu; the runtime lists it to
    ARCropolis last, runtime/hle_fs.c). The mods' own .prcxml patches go on top, as ARCropolis does."""
    folder = os.path.join(tree.mods, CHARACTERS)
    entries = csk.load(tree.root)
    shutil.rmtree(folder, ignore_errors=True)
    hidden_path = os.path.join(tree.root, "dump", "skyline", "hidden.txt")
    hidden = []   # (mods' own copies of the files made here: the runtime hides them from ARCropolis)
    if not entries:
        open(hidden_path, "w").close()
        return
    state = tree.load()
    on = [n for n in state["order"] if state["mods"].get(n, {}).get("enabled")]
    entries = csk.limit_new(entries, log)
    image = RomfsImage(tree.vanilla_cfg)
    try:
        arc_at = next(o for p, o, s in romfs_entries(image) if p == "data.arc")
        table = Table(Arc(lambda off, n: image.read(arc_at + off, n)))
        for target in csk.targets(entries):
            found = table.file_targets(target)
            if not found:
                continue
            base = table.original(*found[0])
            for name in on:   # (a mod's whole copy of the file: the last one, with the characters added to it)
                for root in find_roots(os.path.join(tree.mods, name)) or [os.path.join(tree.mods, name)]:
                    own = os.path.join(root, *target.split("/"))
                    if os.path.isfile(own):
                        with open(own, "rb") as f:
                            base = f.read()
                        hidden.append(os.path.relpath(own, tree.mods).replace(os.sep, "/"))
            data, notes = csk.apply(base, entries, "ui_layout_db" if target == csk.LAYOUT_DB else "ui_chara_db")
            out = os.path.join(folder, *target.split("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as f:
                f.write(data)
            for note in notes:
                log(f"Skyline plugins: {note}")
    finally:
        close_image(image)
    with open(hidden_path, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(h + "\n" for h in hidden)
    if hidden:
        log("the new characters' select-screen files include these mods' own copies: " + ", ".join(hidden))


ARRANGEMENT = ("ui", "param", "menu", "chara_icon_arrangement.prc")
LAYOUT_FIX_ID, LAYOUT_FIX_FILE = 468582, 1055469   # (the GameBanana mod and its file without Squad Strike)
LAYOUT_FIX = f"gamebanana.com/mods/{LAYOUT_FIX_ID} (CSS 91+ Layout Fix)"


def new_characters(tree, names):
    """([mods among `names` whose plugin adds a select-screen entry (the CSK Collection's API)],
    whether one of them gives the select screen a bigger arrangement)."""
    new, arranged = [], False
    for name in names:
        for root in find_roots(os.path.join(tree.mods, name)) or [os.path.join(tree.mods, name)]:
            arranged |= os.path.isfile(os.path.join(root, *ARRANGEMENT))
            nro = os.path.join(root, "plugin.nro")
            if os.path.isfile(nro) and name not in new:
                with open(nro, "rb") as f:
                    if b"add_chara_db_entry_info" in f.read():
                        new.append(name)
    return new, arranged


def select_screen_room(tree, on, log):
    """The select screen holds 91 tiles (13 x 7: the game's ui/param/menu/chara_icon_arrangement.prc):
    the game's own fill all but 4. More new characters (plugins adding a select-screen entry, the CSK
    Collection's API) need a mod with a bigger arrangement and layout, or the screen breaks (one
    column of tiles, or the game stops in the menus). When there's none, the CSS 91+ Layout Fix is
    turned on (installed from GameBanana first if it isn't); `on` gains it. Without the internet,
    says what to do."""
    new, arranged = new_characters(tree, on)
    if len(new) <= 4 or arranged:
        return
    state = tree.load()
    have = [n for n, m in state["mods"].items() if str(m.get("gamebanana")) == str(LAYOUT_FIX_ID)]
    try:
        if not have:
            log(f"{len(new)} new characters are on, and the select screen has room for 4: installing {LAYOUT_FIX}")
            have = gamebanana_install(tree, LAYOUT_FIX_ID, LAYOUT_FIX_FILE)
            state = tree.load()
    except Exception as e:   # (offline, GameBanana down...: the rest of the mods still apply)
        log(f"{len(new)} new characters are on, and the select screen has room for 4; {LAYOUT_FIX} couldn't be "
            f"installed ({e}): install it from My mods, or turn some off (" + ", ".join(new) + ")")
        return
    for n in have:
        if not state["mods"][n].get("enabled"):
            log(f"{n}: turned on (the select screen needs it for {len(new)} new characters)")
        state["mods"][n]["enabled"] = True
        if n not in on:
            on.append(n)
    tree.save(state)


SLOT = re.compile(r"/c(\d{2,3})/")


def missing_shares(tree, on, log):
    """With the real ARCropolis: files a mod's config.json lists as a new costume's (new-dir-files) that
    the mod doesn't ship and nothing shares to it (Sans' c120 lists Palutena's wings and portal and
    ships neither: their weapons' set-up then reads a model that never loaded, a crash). Each is shared
    from the game's own file of the base costume (c00), in mods/.characters/config.json."""
    shares, found = {}, 0
    image = RomfsImage(tree.vanilla_cfg)
    try:
        arc_at = next(o for p, o, s in romfs_entries(image) if p == "data.arc")
        game = Arc(lambda off, n: image.read(arc_at + off, n))
        for name in on:
            for root in find_roots(os.path.join(tree.mods, name)) or [os.path.join(tree.mods, name)]:
                try:
                    with open(os.path.join(root, "config.json"), encoding="utf-8-sig") as f:
                        cfg = json.load(f)
                except (OSError, ValueError):
                    continue
                if not isinstance(cfg, dict) or not isinstance(cfg.get("new-dir-files"), dict):
                    continue
                shared = {t for key in ("share-to-vanilla", "share-to-added")
                          for v in (cfg.get(key) or {}).values() for t in (v if isinstance(v, list) else [v])}
                for files in cfg["new-dir-files"].values():
                    for path in files if isinstance(files, list) else []:
                        if path in shared or os.path.isfile(os.path.join(root, *path.split("/"))):
                            continue
                        m = SLOT.search(path)
                        source = path[:m.start()] + "/c00/" + path[m.end():] if m and int(m.group(1)) >= 8 else None
                        if source and game.locate(source) is not None and path not in shares.setdefault(source, []):
                            shares[source].append(path)
                            found += 1
    finally:
        close_image(image)
    if not found:
        return
    folder = os.path.join(tree.mods, CHARACTERS)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "config.json"), "w", encoding="utf-8") as f:
        json.dump({"share-to-vanilla": {s: t for s, t in shares.items() if t}}, f, indent=1)
    log(f"{found} file(s) mods list for new costumes without shipping them: the game's own (c00) used")


def sync_plugins(tree, log):
    """The turned-on mods' Skyline plugins, recompiled and listed for the game to load (tools/skyline.py).
    Each plugin.nro is also compiled into build/plugins/<name>.dll here, so adding a mod or
    downloading one makes the DLL. The game is not linked."""
    try:
        import skyline
        skyline.sync(tree.root, log)
        skyline.cache_mod_plugins(tree.root, skyline.build_jobs(), log, quiet=True)
    except (Exception, SystemExit) as e:   # (the mods' files are applied either way)
        log(f"Skyline plugins: not updated: {e}")


def _apply(tree, image, lines, state, on, log):
    t0 = time.time()
    size, entries = read_layers(lines[1])
    sources = lines[2:]
    if len(sources) >= 8:
        raise Error("romfs.cfg already has 8 sources")
    rhdr = list(struct.unpack("<10Q", image.read(0, 0x50)))
    arc_at, arc_size = next((o, s) for p, o, s in romfs_entries(image) if p == "data.arc")
    meta_at = rhdr[1]
    if arc_at + arc_size > meta_at or rhdr[3] < meta_at or rhdr[5] < meta_at or rhdr[7] < meta_at or rhdr[7] + rhdr[8] > size:
        raise Error("the game's RomFS isn't laid out as expected (data.arc last, its tables after it)")
    arc = Arc(lambda off, n: image.read(arc_at + off, n))
    log("reading the game's file table")
    table = Table(arc)
    table.use_config(mod_config(tree, on))

    # every file the turned-on mods hold (a later mod wins), sorted into kinds; patches, every mod's
    # in order, by the file they patch
    chosen = {}
    patches = {}
    regional = {}   # path -> {region: (path, file, mod)}: whole files for one region (a later mod wins)
    for name in on:
        for plugin in plugins(os.path.join(tree.mods, name)):
            log(f"{name}: {plugin} is a Skyline code plugin (a moveset, a new character...): recompiled into the game "
                "(tools/skyline.py)")
        for path, src in mod_files(os.path.join(tree.mods, name)):
            base, region = strip_region(path)
            stem, ext = os.path.splitext(base)
            if ext.lower() in PATCHES:
                patches.setdefault((stem + PATCHES[ext.lower()]).lower(), []).append((path, src, name, region))
            elif region:   # (a whole file for one region: msg_name+us_en.msbt)
                regional.setdefault(base.lower(), {})[region] = (path, src, name)
            else:
                chosen[path.lower()] = (path, src, name)
    # the characters plugins add to the select screen (the CSK Collection's API: dump/skyline/csk.json)
    csk_entries = csk.load(tree.root)
    for target in csk.targets(csk_entries):
        patches.setdefault(target, []).append(("csk.json", None, "Skyline plugins", None))
    os.makedirs(tree.work, exist_ok=True)
    overlay_path = os.path.join(tree.work, "overlay.bin")
    tmp_path = overlay_path + ".tmp"
    skipped = {}
    added = {}
    merged = {}
    changed = 0
    unshared = 0
    reshared = 0   # sound banks kept the mod's for one costume by giving the others the game's (Table.reshare)
    kept_shared = {}   # mod -> paths it changes for every costume sharing them (Table.unshare)
    wanted = {hash40(path): src for path, src, _name in chosen.values()}

    def same_file(a, b):
        return a == b or filecmp.cmp(a, b, shallow=False)
    HEAD = 0x100   # overlay: [0, 0x50) the RomFS header, [0x50, 0x88) data.arc's, from HEAD data.arc's new bytes
    grow_at = align(arc_size, 0x10)   # data.arc's new bytes start here (data.arc offsets)
    with open(tmp_path, "wb") as out:
        make_sparse(out)
        out.write(b"\0" * HEAD)
        out.write(b"\0" * (grow_at - arc_size))
        at = grow_at

        def place(data, a):
            nonlocal at
            pad = align(at, a) - at
            out.write(b"\0" * pad)
            at += pad
            where = at
            out.write(data)
            at += len(data)
            return where

        aliases = []   # (data.arc offset, the original's data.arc offset, size): read from the game's own files

        def alias(original, size, a):
            nonlocal at
            pad = align(at, a) - at
            out.write(b"\0" * pad)
            at += pad
            where = at
            out.seek(size, os.SEEK_CUR)   # (a hole: no disk space, the layer map reads the game's bytes there)
            at += size
            aliases.append((where, original, size))
            return where

        # (first, the paths that would be unshared: see Table.unshare)
        candidates = []
        for key in sorted(chosen):
            path, src, name = chosen[key]
            if path.startswith("stream:/") or table.file_targets(path) is None:
                continue
            sharers = table.sharers(path) or []
            if any(h not in wanted or not same_file(wanted[h], src) for h in sharers):
                candidates.append((path, name))
        table.note_unshare_candidates(candidates)
        for key in sorted(chosen):
            path, src, name = chosen[key]
            if path.startswith("stream:/"):
                targets = table.stream_targets(path)
                if not targets:
                    skipped.setdefault(name, []).append(path)
                    continue
                with open(src, "rb") as f:
                    data = f.read()
                where = place(data, 0x10)
                for d in targets:
                    table.put("stream_datas", d, "<QQ", len(data), where)
                changed += 1
                continue
            targets = table.file_targets(path)
            with open(src, "rb") as f:
                data = f.read()
            if targets is None and ONE_SLOT_EFFECT.match(path) and not ONE_SLOT_PLUGIN:
                skipped.setdefault(name, []).append(path)
                log(f"{name}: {path.rsplit('/', 1)[1]} left out: one-slot effects need the One Slot Effects plugin "
                    "(not supported yet) - this costume uses the fighter's own effects")
                continue
            if targets is None:   # a file the game doesn't have: added to its folder's directory
                why = table.add_file(path, data)
                if why:
                    skipped.setdefault(name, []).append(path)
                    log(f"{name}: {path} not added: {why}")
                else:
                    added.setdefault(name, []).append(path)
                continue
            # (a data entry of size 0 is a costume slot reusing another's file: the game sizes the
            # file's buffer from that other one, so new bytes here overrun it - left as it is)
            targets = [(f, d) for f, d in targets if table.entry("datas", d, "<4I")[2]]
            if not targets:
                skipped.setdefault(name, []).append(path)
                continue
            # a file other paths share: the mod's for this path only (unless the mods give every one
            # of them the same file)
            kept = len(table.kept_shared)
            if any(h not in wanted or not same_file(wanted[h], src) for h in table.sharers(path)) and table.unshare(path, data):
                unshared += 1
                changed += 1
                continue
            if len(table.kept_shared) > kept and path.lower().startswith("sound/"):
                n = table.reshare(path, wanted)
                if n:   # (the other costumes keep the game's sounds; the shared bank takes the mod's, below)
                    table.kept_shared.pop()
                    reshared += 1
                    log(f"{name}: {path}: {n} other costume{'s' if n > 1 else ''} sharing it made {'their' if n > 1 else 'its'} own "
                        "with the game's file, so the mod's is this costume's only")
            if len(table.kept_shared) > kept:
                kept_shared.setdefault(name, []).append(path)
            for folder, d in targets:
                table.replace(folder, d, data)
            changed += 1
        # patches, and whole files for one region, by the file they are for: each data entry (a regional
        # file: each region's own text) gets the patches for every region or for its own, over the mod's
        # whole file for its region, else the mod's whole file, else the game's own
        for key in sorted(set(patches) | set(regional)):
            plist = patches.get(key, [])
            whole = chosen.get(key)
            mine = regional.get(key, {})
            targets = [(f, d, rs) for f, d, rs in table.region_targets(whole[0] if whole else key)
                       if table.entry("datas", d, "<4I")[2]]
            if not targets:
                for path, _src, name, _region in plist:
                    skipped.setdefault(name, []).append(path)
                    log(f"{name}: {path} not merged: the game has no {key}")
                for path, _src, name in mine.values():
                    skipped.setdefault(name, []).append(path)
                    log(f"{name}: {path} left out: the game has no {key}")
                continue
            if whole:
                with open(whole[1], "rb") as f:
                    whole_data = f.read()
            used = set()   # (a region's file or a patch that went into some data entry)
            failed = set()
            for folder, d, regions in targets:
                rs = regions or {GAME_REGION}   # (a file with one text for all: the game's region's)
                own = next((mine[r] for r in sorted(rs) if r in mine), None)
                todo = [e for e in plist if e[3] is None or e[3] in rs]
                if not own and not todo:
                    continue
                if own:
                    with open(own[1], "rb") as f:
                        data = f.read()
                    used.add(own[0])
                else:
                    data = whole_data if whole else table.original(folder, d)
                for path, src, name, _region in todo:
                    if src is None:   # (csk.json: the plugins' characters)
                        try:
                            data, notes = csk.apply(data, csk_entries, "ui_layout_db" if key == csk.LAYOUT_DB else "ui_chara_db")
                        except (prc.Error, ValueError, KeyError, struct.error) as e:
                            raise Error(f"the Skyline plugins' characters can't be merged into {key}: {e}")
                        for note in notes:
                            log(f"Skyline plugins: {note}")
                        continue
                    if path in failed:
                        continue
                    with open(src, "rb") as f:
                        patch = f.read()
                    try:
                        if path.lower().endswith(".xmsbt"):
                            data = msbt.patch_bytes(data, patch)
                        else:
                            data = prc.patch_bytes(data, patch, xml=path.lower().endswith("xml"))
                        used.add(path)
                    except (msbt.Error, prc.Error, ValueError, struct.error, ET.ParseError) as e:
                        # one patch that can't be merged is left out, the others still go in (as
                        # ARCropolis leaves out a patch it can't read): before, it stopped the whole apply
                        # (seen: a mod's ui_series_db.prcx against another mod's whole ui_series_db.prc)
                        failed.add(path)
                        skipped.setdefault(name, []).append(path)
                        log(f"{name}: {path} left out: it can't be merged into {key}: {e}")
                table.replace(folder, d, data)
            for path, _src, name, _region in plist:
                if path in used:
                    merged.setdefault(name, []).append(path)
                elif path not in failed and _src is not None:
                    skipped.setdefault(name, []).append(path)
                    log(f"{name}: {path} left out: the game reads the {GAME_REGION} text of {key}")
            for path, _src, name in mine.values():
                if path in used:
                    changed += 1
                else:
                    skipped.setdefault(name, []).append(path)
                    log(f"{name}: {path} left out: the game reads the {GAME_REGION} text of {key}")
        plugin_file_callbacks(tree, table, log)
        if table.kept_shared:
            log(f"{len(table.kept_shared)} file{'s' if len(table.kept_shared) > 1 else ''} kept shared with the fighter's other "
                "costumes (made a costume's own, they'd stop those costumes' matches loading): the mod's version shows on "
                "every costume that shares it - " + ", ".join(table.kept_shared[:3]) + (" ..." if len(table.kept_shared) > 3 else ""))
        log(f"building the file table ({sum(len(v) for v in table.new_files.values())} new files)")
        table.commit_files()
        unlisted = [n for n in table.notes if n.endswith("listed in a new directory, but the mod doesn't have it")]
        for note in table.notes:
            if note not in unlisted:
                log(f"config.json: {note}")
        if unlisted:   # (a reslotter's config often lists files the mod leaves out: harmless)
            log(f"config.json lists {len(unlisted)} file{'s' if len(unlisted) > 1 else ''} its mod doesn't have (left out)")
        log(f"laying out the changed folders ({len(set(table.replaced) | set(table.added))})")
        table.rebuild_folders(alias, place)

        # the table (and the search section, with the new paths), then data.arc's new end
        hdr = list(arc.header)
        if table.path_names or table.like_paths:
            search = Search(arc.read, arc.header[5])
            for path in table.path_names:
                search.add_file(path)
            for like in table.like_paths:
                search.add_like(*like)
            t = search.to_bytes()
            z = zstd_compress(t)
            hdr[5] = place(struct.pack("<4I", 0x10, len(t), len(z), align(len(z), 4)) + z, 0x10)
        log("writing the new file table")
        t = table.to_bytes()
        z = zstd_compress(t)
        table_at = place(struct.pack("<4I", 0x10, len(t), len(z), align(len(z), 4)) + z, 0x10)
        new_arc_size = at
        hdr[4] = table_at

        # the RomFS's tables after data.arc's new end, data.arc's size in them
        new_meta_at = align(arc_at + new_arc_size, 0x10)
        out.write(b"\0" * (new_meta_at - (arc_at + new_arc_size)))
        meta = bytearray(image.read(meta_at, size - meta_at))
        fmo, fms = rhdr[7] - meta_at, rhdr[8]
        o, found = fmo, False
        while o < fmo + fms:
            _p, _s, foff, fsize, _h, nlen = struct.unpack_from("<IIQQII", meta, o)
            if meta[o + 0x20:o + 0x20 + nlen] == b"data.arc":
                struct.pack_into("<Q", meta, o + 0x10, new_arc_size)
                found = True
                break
            o += 0x20 + align(nlen, 4)
        if not found:
            raise Error("data.arc's entry isn't in the RomFS's file table")
        meta_in_overlay = HEAD + (new_meta_at - arc_at - arc_size)
        out.write(meta)
        delta = new_meta_at - meta_at
        for k in (1, 3, 5, 7):
            rhdr[k] += delta
        out.seek(0)
        out.write(struct.pack("<10Q", *rhdr))
        out.write(struct.pack("<7Q", *hdr))
    replace_file(tmp_path, overlay_path)

    # the layer map: the game's, with the two headers, the new bytes and the tables from the overlay
    src = len(sources)
    new_size = new_meta_at + len(meta)
    base = [(a, s, o) for a, s, o in entries if a < meta_at]
    overrides = [(0, 0x50, src, 0), (arc_at, arc_at + 0x38, src, 0x50),
                 (arc_at + arc_size, new_meta_at, src, HEAD), (new_meta_at, new_size, src, meta_in_overlay)]
    if aliases:   # the new bytes, but where the game's own original bytes are read again (aliases)
        ext = overrides[2]
        overrides[2:3] = []
        pos = ext[0]
        for where, original, n in sorted(aliases):
            if arc_at + where > pos:
                overrides.append((pos, arc_at + where, src, HEAD + (pos - (arc_at + arc_size))))
            overrides += pieces(base, meta_at, arc_at + original, arc_at + original + n, arc_at + where)
            pos = arc_at + where + n
        if pos < ext[1]:
            overrides.append((pos, ext[1], src, HEAD + (pos - (arc_at + arc_size))))
    layers = split_layers(base, meta_at, overrides)
    map_path = os.path.join(tree.modules, MODDED_MAP)
    write_layers(map_path, new_size, layers)
    cfg_text = "layered\n" + map_path + "\n" + "".join(s + "\n" for s in sources) + overlay_path + "\n"
    log("checking the modded files read back right")
    check(cfg_text, {k: v for k, v in chosen.items() if k not in patches}, skipped, added, tree)
    with open(tree.cfg + ".tmp", "w", encoding="utf-8") as f:
        f.write(cfg_text)
    replace_file(tree.cfg + ".tmp", tree.cfg)
    written = disk_size(overlay_path)
    summary = {"mods": on, "files": changed, "added": added, "merged": merged, "skipped": skipped, "kept_shared": kept_shared,
               "bytes": written}
    with open(os.path.join(tree.work, "applied.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    log(f"{len(on)} mod(s), {changed} files changed ({unshared} unshared, {reshared} reshared), {sum(len(v) for v in added.values())} added, "
        f"{sum(len(v) for v in merged.values())} merged, "
        f"{sum(len(v) for v in skipped.values())} skipped, {written / 1048576:.1f} MB written, in {time.time() - t0:.0f} s")
    return summary


def check(cfg_text, chosen, skipped, added, tree):
    """Read the modded RomFS back as the game will: a few changed files, and every added one, must be
    the mods' bytes."""
    probe = os.path.join(tree.work, "check.cfg")
    with open(probe, "w", encoding="utf-8") as f:
        f.write(cfg_text)
    image = RomfsImage(probe)
    try:
        arc_at = next(o for p, o, _s in romfs_entries(image) if p == "data.arc")
        arc = Arc(lambda off, n: image.read(arc_at + off, n))
        bad = set(p for v in skipped.values() for p in v)
        new = set(p for v in added.values() for p in v)
        sample = [v for v in chosen.values() if v[0] not in bad | new and not v[0].startswith("stream:/")][:8]
        sample += [v for v in chosen.values() if v[0] in new]
        for path, src, _name in sample:
            with open(src, "rb") as f:
                if arc.get(path) != f.read():
                    raise Error(f"check failed: {path} doesn't read back as the mod has it (nothing was changed)")
    finally:
        close_image(image)
        os.remove(probe)


# ---- the command line ------------------------------------------------------------------------
def listing(tree):
    state = tree.load()
    applied = tree.applied()
    out = []
    for n in state["order"]:
        m = state["mods"][n]
        files = mod_files(os.path.join(tree.mods, n))
        out.append({"name": n, "enabled": bool(m.get("enabled")), "files": len(files),
                    "added": len((applied or {}).get("added", {}).get(n, [])),
                    "merged": len((applied or {}).get("merged", {}).get(n, [])),
                    "skipped": len((applied or {}).get("skipped", {}).get(n, [])),
                    "kept_shared": len((applied or {}).get("kept_shared", {}).get(n, [])),
                    **{k: v for k, v in m.items() if k != "enabled"}})
    return {"mods": out, "applied": (applied or {}).get("mods"), "modded": tree.is_modded()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("command")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--tree", default=os.path.dirname(HERE))
    ap.add_argument("--name")
    ap.add_argument("--file")
    ap.add_argument("--gamebanana")
    ap.add_argument("--title")
    ap.add_argument("--image")
    ap.add_argument("--json", action="store_true", help="results as one line of JSON (the Mods app reads it)")
    a = ap.parse_args()
    tree = Tree(a.tree)

    def say(text):
        print(json.dumps({"log": text}) if a.json else text, flush=True)

    try:
        if a.command == "list":
            print(json.dumps(listing(tree), indent=None if a.json else 1))
            return 0
        if a.command == "install":
            meta = {k: v for k, v in (("gamebanana", a.gamebanana), ("title", a.title), ("image", a.image)) if v}
            for path in a.args:
                say(f"installed: {', '.join(install(tree, path, a.name, meta))}")
        elif a.command == "gamebanana":
            last = [0.0]

            def progress(done, total):
                if a.json and time.time() - last[0] > 0.25:
                    last[0] = time.time()
                    print(json.dumps({"progress": done / total if total else 0}), flush=True)
            say(f"installed: {', '.join(gamebanana_install(tree, a.args[0], a.file, progress))}")
        elif a.command in ("enable", "disable"):
            state = tree.load()
            for n in a.args:
                if n not in state["mods"]:
                    raise Error(f"no mod called {n}")
                state["mods"][n]["enabled"] = a.command == "enable"
            tree.save(state)
        elif a.command == "remove":
            state = tree.load()
            for n in a.args:
                if n in state["mods"]:
                    remove_folder(os.path.join(tree.mods, n))
            tree.save(tree.load())
        elif a.command == "order":
            state = tree.load()
            state["order"] = [n for n in a.args if n in state["mods"]] + [n for n in state["order"] if n not in a.args]
            tree.save(state)
        elif a.command == "apply":
            result = apply(tree, say)
            if a.json:
                print(json.dumps({"result": result}), flush=True)
        elif a.command == "vanilla":
            vanilla_cfg(tree)
            shutil.copyfile(tree.vanilla_cfg, tree.cfg)
            say("the game as it is (the mods stay on the list)")
        else:
            raise Error(f"unknown command {a.command}")
    except Error as e:
        print(json.dumps({"error": str(e)}) if a.json else f"error: {e}", flush=True)
        return 1
    except (OSError, urllib.error.URLError) as e:
        print(json.dumps({"error": str(e)}) if a.json else f"error: {e}", flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
