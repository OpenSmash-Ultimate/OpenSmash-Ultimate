#!/usr/bin/env python3
"""The game-code fixes new costume slots (c08 and up) need outside regular matches - Classic Mode,
Tourney Mode, Inkling, Terry, Pyra/Mythra (patch 0011, branch mac-costume-slot-fixes: ARCropolis'
src/fixes.rs and three more checks) - as exact edits to a WindowSmash folder's tools/recomp.py and
tools/lift.py, and the rebuild that puts them in the game. Used by install_mods.py.

An edit is made only when every one finds its text exactly once (a WindowSmash whose recompiler
has moved on is left as it is, and said so); recomp.py with CODE_PATCHES has them already.
"""
import os
import shutil
import subprocess
import sys

FILES = ("tools/recomp.py", "tools/lift.py")
MARK = "CODE_PATCHES"   # (recomp.py with the fixes)
# [(file, its text, the text it becomes)] - patch 0011, its context narrowed to what is unique
CODE_FIXES = [
 [
  "tools/lift.py",
  "\n\nclass Lifter(SimdMixin):\n    def __init__(self, image, entries, base=BASE, forward=None, setjmp=None, pads=None):\n        self.img = image\n        self.base = base         # guest address the module is loaded at\n        self.forward = forward or {}   # entry offset -> native function that replaces it\n        self.setjmp = setjmp or set()  # offsets of setjmp (or its import stub): calls become host setjmp\n        self.pads = pads or {}   # function offset -> [(call range start, end, landing pad)] (eh.py)\n        self.entries = entries   # set of known function entry offsets\n        self.tables = 0          # jump tables resolved\n        self.indirect_tail = 0   # br sites treated as indirect tail calls\n",
  "\n\nclass Lifter(SimdMixin):\n    def __init__(self, image, entries, base=BASE, forward=None, setjmp=None, pads=None, patches=None):\n        self.img = image\n        self.base = base         # guest address the module is loaded at\n        self.forward = forward or {}   # entry offset -> native function that replaces it\n        self.setjmp = setjmp or set()  # offsets of setjmp (or its import stub): calls become host setjmp\n        self.pads = pads or {}   # function offset -> [(call range start, end, landing pad)] (eh.py)\n        self.patches = patches or {}   # offset -> [words] lifted in place of the instruction (recomp.CODE_PATCHES)\n        self.entries = entries   # set of known function entry offsets\n        self.tables = 0          # jump tables resolved\n        self.indirect_tail = 0   # br sites treated as indirect tail calls\n"
 ],
 [
  "tools/lift.py",
  "            wrapper = f\"GuestFn {native[1]};\\nvoid {name}(Cpu *c) {{ {native[1]}(c); }}\\n\"\n            name += \"_orig\"\n        self.words = struct.unpack_from(f\"<{size // 4}I\", self.img, start)\n        self.sites = self.pads.get(start, [])\n        self.leaders.update(lp for _s, _e, lp in self.sites)   # entered from an exception\n        self.analyze_tables()\n",
  "            wrapper = f\"GuestFn {native[1]};\\nvoid {name}(Cpu *c) {{ {native[1]}(c); }}\\n\"\n            name += \"_orig\"\n        self.words = struct.unpack_from(f\"<{size // 4}I\", self.img, start)\n        here = {off: ws for off, ws in self.patches.items() if start <= off < self.fend}\n        if here:   # (the last word in the instruction's place, so the jump table analysis sees it)\n            self.words = tuple(here[start + i * 4][-1] if start + i * 4 in here else w for i, w in enumerate(self.words))\n        self.sites = self.pads.get(start, [])\n        self.leaders.update(lp for _s, _e, lp in self.sites)   # entered from an exception\n        self.analyze_tables()\n"
 ],
 [
  "tools/lift.py",
  "            pc = start + i * 4\n            try:\n                stmt, falls = self.insn(w, pc)\n            except Unsupported as e:\n                reasons[(str(e), w)] += 1\n                stmt, falls = f'rt_trap(c, {const(self.base +pc)}, \"unsupported\");', True\n",
  "            pc = start + i * 4\n            try:\n                stmt, falls = self.insn(w, pc)\n                if pc in here:   # the words before the last run first\n                    stmt = \" \".join([self.insn(x, pc)[0] for x in here[pc][:-1]] + [stmt])\n            except Unsupported as e:\n                reasons[(str(e), w)] += 1\n                stmt, falls = f'rt_trap(c, {const(self.base +pc)}, \"unsupported\");', True\n"
 ],
 [
  "tools/recomp.py",
  "    return True\n\n\ndef _init(image_path, entries, base, forward, setjmp=None, pads=None):\n    f = open(image_path, \"rb\")\n    _state[\"img\"] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)\n    _state[\"args\"] = (entries, base, forward, setjmp, pads)\n\n\ndef unwind_jumpto(mod, found):\n",
  "    return True\n\n\ndef _init(image_path, entries, base, forward, setjmp=None, pads=None, patches=None):\n    f = open(image_path, \"rb\")\n    _state[\"img\"] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)\n    _state[\"args\"] = (entries, base, forward, setjmp, pads, patches)\n\n\n# Code patches, as ARCropolis (github.com/Raytwo/ARCropolis, src/fixes.rs) makes them on the\n# console, for costume slots a mod adds (c08 and up, tools/mods.py): the game checks a color\n# against 8 in places, and aborts (or Inkling reads past its ink table) for a higher one.\n# {module build id: [(offset, original word, [words], what)]}: one word replaces the instruction\n# at that offset; with several, the first ones run before the last, as an inline hook would (a\n# branch to the offset runs them too). A patch whose original word is not there is left out.\nCODE_PATCHES = {\n    \"21450C647B8C5940ED5B68218EA128FD3C5D597D\": [   # main, 13.0.5 (ARCropolis' list is for 13.0.5, though it says 13.0.2)\n        # Pyra/Mythra (Aegis): cmp xN, #8 -> #256. ARCropolis' 0x183527c, a b.eq, is a slip for 0x183526c.\n        (0x183526C, 0xF100227F, [0xF104027F], \"cmp x19, #256 (Aegis)\"),\n        (0x18355BC, 0xF100227F, [0xF104027F], \"cmp x19, #256 (Aegis)\"),\n        (0x18355D8, 0xF100211F, [0xF104011F], \"cmp x8, #256 (Aegis)\"),\n        (0x18359A8, 0xF10022FF, [0xF10402FF], \"cmp x23, #256 (Aegis)\"),\n        (0x1835E3C, 0xF100211F, [0xF104011F], \"cmp x8, #256 (Aegis)\"),\n        (0x1836594, 0xF100227F, [0xF104027F], \"cmp x19, #256 (Aegis)\"),\n        (0x18368B0, 0xF100213F, [0xF104013F], \"cmp x9, #256 (Aegis)\"),\n        (0x1A1CDF0, 0xF100211F, [0xF104011F], \"cmp x8, #256 (Aegis)\"),\n        (0x1A1CE34, 0xF100211F, [0xF104011F], \"cmp x8, #256 (Aegis)\"),\n        # Terry\n        (0x14DE31C, 0x7100213F, [0x7104013F], \"cmp w9, #256 (Terry)\"),\n        (0x14E1624, 0x710022FF, [0x710402FF], \"cmp w23, #256 (Terry)\"),\n        (0x14E17B0, 0x7100211F, [0x7104011F], \"cmp w8, #256 (Terry)\"),\n        # Tourney Mode\n        (0x1A56084, 0x94603F7B, [0x52800020], \"mov w0, #1 (Tourney)\"),\n        (0x1A561B4, 0xF100211F, [0xF104011F], \"cmp x8, #256 (Tourney)\"),\n        (0x1A55A74, 0x7100215F, [0x7104015F], \"cmp w10, #256 (Tourney)\"),\n        (0x1A55B0C, 0x7100201F, [0x7104001F], \"cmp w0, #256 (Tourney)\"),\n        (0x1A55B38, 0xF100201F, [0xF104001F], \"cmp x0, #256 (Tourney)\"),\n        (0x1A55AA0, 0xF100201F, [0xF104001F], \"cmp x0, #256 (Tourney)\"),\n        (0x1A55AD4, 0x7100217F, [0x7104017F], \"cmp w11, #256 (Tourney)\"),\n        # A player whose color is over 7 is taken for a cheat (\"An error has been detected.\n        # Disconnecting.\", pop_online_cheat_error) when Classic Mode starts: not in ARCropolis' list\n        (0x1799ACC, 0x71001D1F, [0x7103FD1F], \"cmp w8, #255 (the player check)\"),\n        # Classic Mode: the color it keeps for the run, modulo 8 (set_global_color_for_classic_mode)\n        (0x23DA5CC, 0xB9001AA9, [0x12000929, 0xB9001AA9], \"and w9, w9, #7 before str w9, [x21, #0x18] (Classic)\"),\n        # Inkling: no color check before the ink table (inkling_patch), the ink cleared with\n        # color modulo 8 (clear_ink_patch), and two calls skipped for c10+ (inkling_c10plus)\n        (0x35BC504, 0x7100229F, [0xD503201F], \"nop: cmp w20, #8 (Inkling)\"),\n        (0x35BC508, 0x54003D62, [0xD503201F], \"nop: b.hs (Inkling)\"),\n        (0x35BCF90, 0x7100231F, [0x12000B18, 0x7100231F], \"and w24, w24, #7 before cmp w24, #8 (Inkling ink)\"),\n        (0xB0B614, 0xF9400008, [0x1400000E], \"b +0x38 (Inkling c10+)\"),\n        # Inkling's ink color list (8 colors), read with the color in three places: the index wraps\n        # instead of aborting past the end (not in ARCropolis' list; with Inkling c08 the match\n        # stopped loading, and refilling ink stopped the game)\n        (0x46D904, 0x93407E68, [0x93407E68, 0x9ACA090B, 0x9B0AA168], \"sxtw x8, w19; udiv x11, x8, x10; msub x8, x11, x10, x8 (ink color)\"),\n        (0x46DA04, 0x93407E88, [0x93407E88, 0x9ACA090B, 0x9B0AA168], \"sxtw x8, w20; udiv x11, x8, x10; msub x8, x11, x10, x8 (ink color)\"),\n        (0x69378C, 0x2A1303E8, [0x2A1303E8, 0x9ACA090B, 0x9B0AA168], \"mov w8, w19; udiv x11, x8, x10; msub x8, x11, x10, x8 (ink color)\"),\n    ],\n}\n\n\ndef code_patches(mod, name):\n    \"\"\"offset -> [words] of CODE_PATCHES for this module, each checked against its code.\"\"\"\n    out = {}\n    for off, was, words, what in CODE_PATCHES.get(mod.build_id, []):\n        if mod.u32(off) == was:\n            out[off] = words\n        else:\n            print(f\"{name}: code patch at {off:#x} ({what}) left out: {mod.u32(off):08x} is there, not {was:08x}\")\n    return out\n\n\ndef unwind_jumpto(mod, found):\n"
 ],
 [
  "tools/recomp.py",
  "    pads = landing_pads(mod)\n    chunks, cur, count = [], [], 0\n",
  "    pads = landing_pads(mod)\n    patches = code_patches(mod, name)\n    chunks, cur, count = [], [], 0\n"
 ],
 [
  "tools/recomp.py",
  "        chunks.append(cur)\n    defined = {a for a, _ in funcs}\n    missing, manifest, bad, unresolved = set(), [], 0, []\n    with Pool(jobs, _init, (image_file(mod), set(found), base, forward, setjmp_targets(mod), pads)) as pool:\n        tasks = [(i, c, outdir) for i, c in enumerate(chunks)]\n        for fname, ext, insns, biggest, b, unres in pool.imap_unordered(_emit, tasks):\n            missing |= ext - defined\n",
  "        chunks.append(cur)\n    defined = {a for a, _ in funcs}\n    missing, manifest, bad, unresolved = set(), [], 0, []\n    with Pool(jobs, _init, (image_file(mod), set(found), base, forward, setjmp_targets(mod), pads, patches)) as pool:\n        tasks = [(i, c, outdir) for i, c in enumerate(chunks)]\n        for fname, ext, insns, biggest, b, unres in pool.imap_unordered(_emit, tasks):\n            missing |= ext - defined\n"
 ],
 [
  "tools/recomp.py",
  "    size = sum(os.path.getsize(os.path.join(outdir, n)) for n, _i, _b in manifest)\n    print(f\"{name}: {len(manifest)} files, {size / 1048576:,.0f} MB of C, {len(defined):,} functions, \"\n          f\"{sum(i for _n, i, _b in manifest):,} instructions; {bad} untranslated, {len(missing)} calls to nowhere, \"\n          f\"{len(unresolved)} unresolved jump tables, {len(forward)} replaced natively; \"\n          f\"{imports:,} symbols referenced by {relocs:,} relocations\")\n",
  "    size = sum(os.path.getsize(os.path.join(outdir, n)) for n, _i, _b in manifest)\n    print(f\"{name}: {len(manifest)} files, {size / 1048576:,.0f} MB of C, {len(defined):,} functions, \"\n          f\"{sum(i for _n, i, _b in manifest):,} instructions; {bad} untranslated, {len(missing)} calls to nowhere, \"\n          f\"{len(unresolved)} unresolved jump tables, {len(forward)} replaced natively, {len(patches)} code patches; \"\n          f\"{imports:,} symbols referenced by {relocs:,} relocations\")\n"
 ]
]



def state(target):
    """"there" (the fixes are in recomp.py), "ready" (they can be made) or why they can't."""
    texts = {}
    for f in FILES:
        try:
            with open(os.path.join(target, *f.split("/")), encoding="utf-8") as fh:
                texts[f] = fh.read()
        except OSError:
            return f"{f} isn't there"
    if MARK in texts["tools/recomp.py"]:
        return "there"
    for f, old, _new in CODE_FIXES:
        if texts[f].count(old) != 1:
            return f"this WindowSmash's {f} isn't the one the fixes were made for"
    return "ready"


def apply(target, record):
    """Make the edits (each file kept as .before-modmanager, the install's record told); True if made."""
    if state(target) != "ready":
        return False
    for f in FILES:
        path = os.path.join(target, *f.split("/"))
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        for ff, old, new in CODE_FIXES:
            if ff == f:
                text = text.replace(old, new, 1)
        keep = path + ".before-modmanager"
        if not os.path.exists(keep):
            shutil.copy2(path, keep)
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
        record.setdefault(f, "replaced")
    return True


def game_binary(target):
    if sys.platform == "darwin":
        return os.path.join(target, "build-mac", "WindowSmash.app", "Contents", "MacOS", "ssbu")
    if os.name == "nt":
        return os.path.join(target, "build", "ssbu.exe")
    return os.path.join(target, "build-linux", "ssbu")


def needs_build(target):
    """Whether the game was built before the fixes were put in the recompiler."""
    exe = game_binary(target)
    try:
        return os.path.getmtime(exe) < os.path.getmtime(os.path.join(target, "tools", "recomp.py"))
    except OSError:
        return False   # (not built here at all: the installer of WindowSmash builds it)


def memory_gb():
    try:
        if sys.platform == "darwin":
            return int(subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True).stdout) / 2 ** 30
        if os.name == "nt":
            import ctypes

            class Status(ctypes.Structure):
                _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                            ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                            ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                            ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
            st = Status()
            st.dwLength = ctypes.sizeof(Status)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st))
            return st.ullTotalPhys / 2 ** 30
        with open("/proc/meminfo") as f:
            return int(f.readline().split()[1]) / 2 ** 20
    except (OSError, ValueError, AttributeError):
        return 8


def build_command(target, python):
    """build_game.py with the folders the game was set up from (dump/build_folders.txt), and few
    files compiled at a time - the computer stays usable (8 at a time ran a 24 GB Mac out of memory)."""
    folders = {}
    try:
        with open(os.path.join(target, "dump", "build_folders.txt"), encoding="utf-8") as f:
            for line in f:
                k, _, v = line.strip().partition("=")
                if k and v:
                    folders[k] = v
    except OSError:
        return None
    if not all(k in folders for k in ("game", "keys", "firmware")):
        return None
    gb = memory_gb()
    jobs = 2 if gb < 12 else 3 if gb < 32 else 6
    cmd = [python, os.path.join(target, "tools", "build_game.py")]
    for k in ("game", "keys", "firmware"):
        cmd += [f"--{k}", folders[k]]
    cmd += ["--jobs", str(jobs)]
    if sys.platform == "darwin" and shutil.which("caffeinate"):
        cmd = ["caffeinate", "-i"] + cmd
    return cmd
