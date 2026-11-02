#!/usr/bin/env python3
"""Compile the game's shaders ahead of time, into the renderer's disk cache.

Usage:
  shader_aot.py [--jobs N] [--dry] [--parts]

The game's GPU programs are stored in data.arc as they are uploaded: in the effect files
(.eff), the shader databases (.nushdb) and nested archives (.arc); a few more are built into the
game's main executable (dump/modules/main.reloc.bin). This finds every program there (by its
header word: 256-byte boundaries + 0x30 in effects and archives, 8-byte ones in databases - half
of a database's programs start 8 bytes past a 16-byte one - and 16-byte ones in the executables),
keeps one copy of each, pairs each pixel program with the vertex programs of its file (a vertex
program's outputs are its pixel program's inputs), and has tools/shaderaot.c translate and
compile them all into dump/shadercache, exactly as the renderer would: a program's first
use in the game then loads its DXBC instead of compiling it.

The files are dated 2001-01-01 until the game first uses them, so the start's prewarm only
makes the shaders the game has used. Programs with indirect branches are left out (their key
depends on a draw's constant buffers), as are compute programs (on their launch's size).
--dry translates without compiling and compares the keys with the cache that is there.

How the work is shared out: one discovery pass (shaderaot --dry) lists every key once; the
keys not in the cache yet go into one queue, longest compile first (by the times recorded in
dump/aot/compile_times.txt on earlier runs; keys never timed go first of all, vertex
programs before pixel programs, larger first), and N worker processes (shaderaot --serve) each
pull the next key from it until it is empty, so no worker sits idle while another finishes a
long one. --parts runs the older way instead: the keys split between N processes by hash.
Measured on a 16-thread Linux desktop, 14 processes, a cold cache: 46 min split by hash (the
last 2 % took 16 min, on two processes), 34 min queued on a first run, 33 min with the times
of an earlier run, which is the floor: 450 CPU-minutes over 14, with the two longest compiles,
29 and 20 min each, started first.
"""
import argparse
import collections
import hashlib
import os
import re
import struct
import queue
import subprocess
import sys
import threading
import time
try:
    from compression.zstd import decompress as zstd_decompress   # Python 3.14
except ImportError:                                               # 3.10 to 3.13: the zstandard package
    import zstandard

    def zstd_decompress(data):
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arc as A    # noqa: E402
from progress import bar, time_left   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINUX = sys.platform.startswith("linux")   # (compiled with vkd3d-shader there, with fxc on Windows)
LOW40 = (1 << 40) - 1
MAX_PROGRAM = 0x40000   # as the renderer reads


def is_header(w):
    """A vertex (type 1, shader type 1) or pixel (type 2, shader type 5) program's first word:
    header version 3, SASS version 3."""
    t, v, st, sass = w & 0x1F, (w >> 5) & 0x1F, (w >> 10) & 0xF, (w >> 17) & 0xF
    return v == 3 and sass == 3 and ((t == 1 and st == 1) or (t == 2 and st == 5))


def containers(arc):
    """(extension, (offset, stored size, size, flags)) of every distinct .eff/.nushdb/.arc."""
    want = {A.hash40(e): e for e in ("eff", "nushdb", "arc")}
    seen, out = set(), []
    off, size, count = arc.tab["paths"]
    for i in range(count):
        full, ext = struct.unpack_from("<QQ", arc.t, off + i * size)
        e = want.get(ext & LOW40)
        if not e:
            continue
        try:
            info = arc._entry("info_indices", full >> 40, "<2I")[1]
            where = arc._data(arc._entry("infos", info, "<4I")[2])
        except struct.error:
            continue
        if where and where[0] not in seen:
            seen.add(where[0])
            out.append((e, where))
    return out


READ_TRIES = 3          # a read the disk fails is tried this many times (a flaky SD card can manage the next)
READ_GIVE_UP = 10       # this many files running that the disk can't read: it has stopped answering


class DiskGone(Exception):
    """The disk fails every read (taken out, unmounted, failing): no point going on."""


def read_retried(arc, off, size, pause=0.5):
    """arc.read, tried again after a disk's read error; the error once every try failed."""
    for attempt in range(READ_TRIES):
        try:
            return arc.read(off, size)
        except OSError:
            if attempt == READ_TRIES - 1:
                raise
            time.sleep(pause)


# the first two bytes of every word is_header takes (the second's top bits are flags: MRT, kills pixels)
HEADER_START = re.compile(rb"\x61[\x04\x44\x84\xc4]|\x62[\x14\x54\x94\xd4]")


def add_file(programs, ids, data, step, start):
    """The programs in one file's bytes (at start + a multiple of step) into programs/ids, each
    pixel program paired with every vertex program of the file: the one before it is right for
    only about 60 % of the cache's pairs, the file's for about 95 % (the game has only a few dozen
    distinct vertex output masks, so this stays small)."""
    mv = memoryview(data)
    at = [k for k in (m.start() for m in HEADER_START.finditer(data))
          if k >= start and (k - start) % step == 0 and k < len(data) - 0x60 and is_header(int.from_bytes(mv[k:k + 4], "little"))]
    here = ([], [])
    for j, k in enumerate(at):
        end = min(len(data), k + MAX_PROGRAM, at[j + 1] if j + 1 < len(at) else len(data))
        blob = bytes(mv[k:end])
        stage = 0 if blob[0] & 0x1F == 1 else 1
        key = hashlib.blake2b(blob, digest_size=16).digest()
        pid = ids.get(key)
        if pid is None:
            pid = ids[key] = len(programs)
            programs.append([stage, blob, set()])
        here[stage].append(pid)
    for pid in here[1]:
        programs[pid][2].update(here[0])


MODULES = ("main", "sdk", "subsdk0")


def module_images(tree):
    """The game's executables as loaded (<tree>/dump/modules/<name>.reloc.bin) that are there: some
    programs are built into main (the menus', the title's, the screen's own passes)."""
    paths = [os.path.join(tree, "dump", "modules", name + ".reloc.bin") for name in MODULES]
    return [p for p in paths if os.path.isfile(p)]


def find_programs(arc, read_errors=None, pause=0.5, modules=()):
    """Distinct programs: list of [stage, bytes, set of paired vertex program ids], and how many
    files could not be read. A file the disk can't read is left out (its shaders compile when the
    game first uses them, as without this tool); read_errors, a list, gets each such error.
    modules: executables' images, scanned too (their programs on 16-byte boundaries; one that
    isn't one - no end within MAX_PROGRAM - shaderaot leaves out)."""
    programs, ids = [], {}
    unread, failing = 0, 0
    for path in modules:
        with open(path, "rb") as f:
            add_file(programs, ids, f.read(), 16, 0)
    for ext, (off, stored, size, flags) in containers(arc):
        if not stored or not size:   # (an empty file - data.arc has a few, marked compressed: nothing in it)
            continue
        try:
            data = read_retried(arc, off, stored, pause)
            failing = 0
        except OSError as e:
            unread += 1
            failing += 1
            if read_errors is not None:
                read_errors.append(e)
            if failing >= READ_GIVE_UP:
                raise DiskGone(e) from e
            continue
        if flags & 3:
            try:
                data = zstd_decompress(data)
            except Exception:
                unread += 1
                continue
        step, start = (8, 0) if ext == "nushdb" else (256, 0x30)
        add_file(programs, ids, data, step, start)
    return programs, unread


def read_error_text(errors, tree=None):
    """What to say about the files the disk could not read."""
    files = list(dict.fromkeys(e.filename for e in errors if getattr(e, "filename", None)))
    why = errors[0].strerror or str(errors[0])
    lines = [f"The disk could not read {len(errors)} of the game's files ({why}"
             + (f", in {', '.join(files)}" if files else "") + "). Their shaders are left out: they compile when the game "
             "first uses them, as they would without this. The rest go ahead.",
             "A read error comes from the disk itself: most often an SD card or drive that is failing or not seated "
             "well, or a damaged copy of that file. The game can stop with the same error when it needs those files."]
    dump = os.path.join(os.path.abspath(tree or ROOT), "dump") + os.sep
    if any(os.path.abspath(f).startswith(dump) for f in files):
        lines.append("That file is a copy WindowSmash made: delete it and run the installer again to make it afresh.")
    if LINUX:
        lines.append("The system's own record of the error: sudo dmesg | grep -i error")
    return "\n".join(lines)


def write_pack(programs, path):
    with open(path, "wb") as f:
        f.write(b"SAOT" + struct.pack("<I", len(programs)))
        for stage, blob, pairs in programs:
            pairs = sorted(pairs)
            f.write(struct.pack(f"<3I{len(pairs)}I", stage, len(blob), len(pairs), *pairs))
            f.write(blob)


def build_tool(exe):
    sources = [os.path.join(ROOT, "tools", "shaderaot.c"), os.path.join(ROOT, "runtime", "gpu_shader.c")]
    headers = [os.path.join(ROOT, "runtime", n) for n in ("host.h", "gpu_shader.h", "gpu_shader_cache.h")]
    if LINUX:
        sources.append(os.path.join(ROOT, "runtime", "linux", "d3dcompile_vkd3d.c"))
    if os.path.exists(exe) and os.path.getmtime(exe) > max(os.path.getmtime(p) for p in sources + headers):
        return
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    if LINUX:
        build_tool_linux(exe, sources)
        return
    import cc
    r = subprocess.run([cc.CLANG, "/nologo", "/O2", *cc.LINK_FLAGS, *cc.CFLAGS, *sources, "/Fe" + exe, "/link", *cc.LINK_OPTIONS, "d3dcompiler.lib"],
                       capture_output=True, text=True, cwd=os.path.dirname(exe))
    if r.returncode:
        raise SystemExit("cannot build shaderaot:\n" + (r.stdout + r.stderr)[-3000:])


def build_tool_linux(exe, sources):
    """On Linux the renderer's D3DCompile is vkd3d-shader (runtime/linux/d3dcompile_vkd3d.c): the tool
    is linked with it, so that it makes the same DXBC the game would."""
    sys.path.insert(0, os.path.join(ROOT, "tools", "linux"))
    import toolchain as T
    vkd3d = os.path.join(T.VKD3D_BUILD, "linux", "libvkd3d-shader.a")
    if not os.path.isfile(vkd3d):
        print("the shader compiler (vkd3d-shader), once:", flush=True)
        if subprocess.run([sys.executable, os.path.join(ROOT, "tools", "linux", "build_vkd3d.py"), "linux"]).returncode:
            raise SystemExit("vkd3d-shader could not be built (see above)")
    includes = ["-I", os.path.join(ROOT, "runtime", "linux", "include"), "-I", os.path.join(T.DXVK_INCLUDE, "windows"),
                "-I", os.path.join(T.DXVK_INCLUDE, "directx"), "-I", os.path.join(T.SYSROOT, "usr", "include", "SDL2"),
                "-I", os.path.join(T.VKD3D_SRC, "include")]
    r = subprocess.run([T.CLANG, "-O2", *T.CFLAGS, "-D_GNU_SOURCE", "-D_REENTRANT", *includes, *sources, vkd3d, "-fuse-ld=lld",
                        "-no-pie", "-lm", "-lpthread", "-ldl", "-o", exe], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("cannot build shaderaot:\n" + (r.stdout + r.stderr)[-3000:])


def default_jobs():
    """One process per core but two (the PC stays usable), and as many as the RAM holds: each
    reads the whole pack of programs (about 0.4 GB) and translates them."""
    jobs = max(1, (os.cpu_count() or 2) - 2)
    if LINUX:
        try:
            gb = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / (1 << 30)
            jobs = min(jobs, max(1, int(gb // 2)))
        except (ValueError, OSError):
            pass
    return jobs


def low_priority():
    """Popen's arguments that run the compile below normal priority."""
    if LINUX:
        return {"preexec_fn": lambda: os.nice(10)}
    return {"creationflags": getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--jobs", type=int, default=default_jobs())
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--parts", action="store_true", help="split the keys between the processes by hash (the older way)")
    a = ap.parse_args()
    tree = ROOT
    t0 = time.time()
    arc = A.Arc(os.path.join(tree, "dump", "modules", "romfs.cfg"))
    read_errors = []
    try:
        programs, unread = find_programs(arc, read_errors, modules=module_images(tree))
    except DiskGone as e:
        raise SystemExit(f"\nThe disk stopped answering: {READ_GIVE_UP} of the game's files running could not be read "
                         f"({e}). Check that it is still there (an SD card that came loose, a drive that went to sleep), "
                         "then run this again: what is done stays done.")
    nvs = sum(1 for p in programs if p[0] == 0)
    print(f"{len(programs)} distinct programs ({nvs} vertex, {len(programs) - nvs} pixel) in {time.time() - t0:.0f} s"
          + (f"; {unread} files could not be read" if unread else ""), flush=True)
    if read_errors:
        print(read_error_text(read_errors, tree), flush=True)
    work = os.path.join(tree, "dump", "aot")
    os.makedirs(work, exist_ok=True)
    pack = os.path.join(work, "programs.pack")
    write_pack(programs, pack)
    exe = os.path.join(ROOT, "build-linux", "shaderaot") if LINUX else os.path.join(ROOT, "build", "shaderaot.exe")
    build_tool(exe)
    cache = os.path.join(tree, "dump", "shadercache")
    os.makedirs(cache, exist_ok=True)
    try:
        if a.parts and not a.dry:
            run_parts(exe, pack, cache, a.jobs)
        else:
            run_queue(exe, pack, cache, a.jobs, a.dry)
    except KeyboardInterrupt:   # (said by run_parts or run_queue, under the bar)
        return 1
    print(f"done in {time.time() - t0:.0f} s")
    return 0


STOPPED = "stopped: what is done stays done; run this again to carry on"


def run_parts(exe, pack, cache, jobs):
    """The older way: N processes, each translating every vertex program and compiling the keys
    that are its by hash (shaderaot <part> <parts>)."""
    procs = [subprocess.Popen([exe, pack, cache, str(i), str(jobs)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                              text=True, **low_priority()) for i in range(jobs)]
    print(f"compiling with {jobs} processes at low priority (the PC stays usable; stop it with " + ("Ctrl+C" if LINUX else "closing this window") + ": "
          "what is done stays done)", flush=True)
    try:
        parts = follow(procs, Progress(sys.stdout))
    except KeyboardInterrupt:
        print("\n" + STOPPED)
        raise
    totals = totals_from_parts(parts)
    print(summary(totals) if totals else "\n".join(parts))


def run_queue(exe, pack, cache, jobs, dry):
    """Discovery once, then the keys not cached yet through N workers pulling from one queue."""
    print("getting ready: listing the shaders (translating the vertex programs)...", flush=True)
    keys, untranslated = discover(exe, pack, cache)
    have = cached_keys(cache)
    todo = [k for k in keys if k.key not in have]
    if dry:
        print(f"the cache has {len(have)} vertex/pixel keys; {len(have & {k.key for k in keys})} of them are among the {len(keys)} found here"
              + (f"; {untranslated} programs not translated (no key)" if untranslated else ""))
        return
    times_dir = os.path.dirname(pack)   # (dump/aot: the times outlive a cleared cache)
    times = load_times(times_dir)
    todo = order(todo, times)
    print(f"{len(keys):,} shaders, {len(keys) - len(todo):,} in the cache already, {len(todo):,} to compile"
          + (f"; {sum(1 for k in todo if k.key in times):,} of them timed on an earlier run (longest first)" if times else ""), flush=True)
    totals = collections.Counter()
    totals["keys"] = len(keys)
    totals["existing"] = len(keys) - len(todo)
    totals["untranslated"] = untranslated   # (the discovery pass's: the workers add any of their own)
    if todo:
        jobs = max(1, min(jobs, len(todo)))
        print(f"compiling with {jobs} processes at low priority (the PC stays usable; stop it with " + ("Ctrl+C" if LINUX else "closing this window") + ": "
              "what is done stays done)", flush=True)

        kept = []   # (what the last save wrote: said once the bar's line is complete, or the stop said)

        def save(snapshot, last):
            saved = save_times(times_dir, snapshot)
            if last:
                kept.append(len(saved))

        def kept_said():
            if kept:
                print(f"compile times kept for {kept[-1]:,} shaders ({os.path.join(os.path.relpath(times_dir, ROOT), TIMES_FILE)})", flush=True)
        try:
            totals.update(serve(exe, pack, cache, todo, jobs, times, Progress(sys.stdout, window=None), save=save))
        except KeyboardInterrupt:
            print("\n" + STOPPED)
            kept_said()
            raise
        kept_said()
    print(summary(totals))


# ---- the queue: discovery, ordering, the workers ---------------------------------------------------------
Key = collections.namedtuple("Key", "kind hash mask size")   # kind "vs" or "ps"; mask None for a vertex program
TIMES_FILE = "compile_times.txt"   # (in dump/aot, with the pack: "<kind> <hash> [<mask>] <ms>" per key compiled)


def key_of(kind, hash_, mask):
    """What a key is known by in sets and the times file: ("vs", hash) or ("ps", hash, mask)."""
    return (kind, hash_) if kind == "vs" else (kind, hash_, mask)


Key.key = property(lambda k: key_of(k.kind, k.hash, k.mask))
Key.line = property(lambda k: f"{k.kind} {k.hash}" if k.kind == "vs" else f"{k.kind} {k.hash} {k.mask}")


def parse_key_line(line):
    """A key of shaderaot --dry's output ("vs <hash> <size>", "ps <hash> <mask> <size>"; the size
    is optional, as older tools printed none), else None."""
    p = line.split()
    if len(p) >= 2 and p[0] == "vs" and len(p[1]) == 16:
        return Key("vs", p[1].lower(), None, int(p[2]) if len(p) > 2 and p[2].isdigit() else 0)
    if len(p) >= 3 and p[0] == "ps" and len(p[1]) == 16 and len(p[2]) == 8:
        return Key("ps", p[1].lower(), p[2].lower(), int(p[3]) if len(p) > 3 and p[3].isdigit() else 0)
    return None


def parse_done_line(line):
    """A worker's answer ("done vs <hash> <outcome> <ms>", "done ps <hash> <mask> <outcome> <ms>"):
    (key, outcome, ms), else None."""
    p = line.split()
    if len(p) == 5 and p[0] == "done" and p[1] == "vs":
        return key_of("vs", p[2].lower(), None), p[3], int(p[4]) if p[4].isdigit() else 0
    if len(p) == 6 and p[0] == "done" and p[1] == "ps":
        return key_of("ps", p[2].lower(), p[3].lower()), p[4], int(p[5]) if p[5].isdigit() else 0
    return None


def discover(exe, pack, cache):
    """Every key once, in pack order, and how many programs got no key because the translator gave
    up on them: shaderaot --dry (one process: it translates the vertex programs to know the pixel
    programs' inputs; a vertex program it cannot translate, or a pixel program with no vertex
    program whose inputs it cannot read, is listed in no key, only counted on its last line)."""
    r = subprocess.run([exe, pack, cache, "0", "1", "--dry"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode:
        raise SystemExit(f"shaderaot --dry failed (code {r.returncode}): {r.stderr.strip().splitlines()[-1] if r.stderr.strip() else ''}")
    keys = [k for k in (parse_key_line(l) for l in r.stdout.splitlines()) if k]
    counts = totals_from_parts([l for l in r.stderr.splitlines() if l.startswith("part ")])
    return keys, counts["untranslated"] if counts else 0


def cache_file_name(key, version):
    """The cache file a key is kept in (runtime/gpu_shader_cache.h: jump-table hash 0)."""
    return f"v{version}_{key.kind}_{key.hash}_{key.mask or '00000000'}_{'0' * 16}.dxbc"


def cached_keys(cache, version=None):
    """The keys of this version's files in the cache folder."""
    version = cache_version() if version is None else version
    have = set()
    for name in os.listdir(cache):
        k = parse_cache_file_name(name, version)
        if k:
            have.add(k)
    return have


def parse_cache_file_name(name, version):
    parts = name[:-5].split("_") if name.endswith(".dxbc") else []
    if len(parts) == 5 and parts[0] == "v" + str(version) and parts[1] in ("vs", "ps") and parts[4] == "0" * 16:
        return key_of(parts[1], parts[2], parts[3])
    return None


def load_times(folder):
    """The compile times recorded by earlier runs: {key: ms}. A missing or damaged file is empty."""
    times = {}
    try:
        with open(os.path.join(folder, TIMES_FILE), encoding="utf-8") as f:
            for line in f:
                p = line.split()
                if len(p) == 3 and p[0] == "vs" and p[2].isdigit():
                    times[key_of("vs", p[1].lower(), None)] = int(p[2])
                elif len(p) == 4 and p[0] == "ps" and p[3].isdigit():
                    times[key_of("ps", p[1].lower(), p[2].lower())] = int(p[3])
    except OSError:
        pass
    return times


def save_times(folder, times):
    """The times merged with what the file has (a run that stopped keeps the earlier records),
    written whole, through a temporary file."""
    merged = load_times(folder)
    merged.update(times)
    path = os.path.join(folder, TIMES_FILE)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        for key, ms in merged.items():
            f.write(" ".join(key) + f" {ms}\n")
    os.replace(tmp, path)
    return merged


MS_PER_BYTE = {"vs": 0.16, "ps": 0.10}   # (a compile's cost by its input's size, where no time is recorded: measured on one
                                         # PC, only their ratio matters; recalibrated from the timed keys when there are any)


def weights(keys, times):
    """How much of the work each key is, by key: its recorded compile time where there is one,
    else its input's size at the going rate for its kind, from the keys of that kind that were
    timed (the sizes track the times closely: a rank correlation of 0.96 over the game's)."""
    rate = dict(MS_PER_BYTE)
    for kind in rate:
        timed = [(times[k.key], k.size) for k in keys if k.kind == kind and k.key in times and k.size > 0]
        if timed:
            rate[kind] = sum(ms for ms, _size in timed) / sum(size for _ms, size in timed)
    return {k.key: float(times[k.key]) if k.key in times else k.size * rate[k.kind] + 1.0 for k in keys}


def order(keys, times):
    """The queue's order: keys never timed first (an unknown is taken for expensive until measured),
    vertex programs before pixel programs and larger first among them; then the timed ones,
    longest first. Ties keep the order given (the pack's)."""
    def rank(ik):
        i, k = ik
        ms = times.get(k.key)
        return (0, k.kind != "vs", -k.size, i) if ms is None else (1, -ms, i)
    return [k for _i, k in sorted(enumerate(keys), key=rank)]


class Worker:
    """One shaderaot --serve process: keys written to it, answers read back."""
    IN_FLIGHT = 1   # keys a worker holds at once: one. A second sent ahead would wait behind the first,
                    # and the queue's first keys are its longest compiles (a key behind the 29-minute one
                    # waits 29 minutes); the pipe's round trip is well under a millisecond against an
                    # average compile of about 400 ms, so nothing is gained by it either.

    def __init__(self, proc):
        self.proc = proc
        self.pending = collections.deque()   # keys sent, not answered yet
        self.errors = []                     # compiler failures it reported (its stderr)
        self.drain = threading.Thread(target=self._drain_stderr, daemon=True)
        self.drain.start()

    def _drain_stderr(self):   # (the compiler's own chatter would fill the pipe and stop the worker)
        for line in self.proc.stderr:
            if ("shader compiler failed" in line or "could not be written" in line) and len(self.errors) < 50:
                self.errors.append(line.rstrip())

    def send(self, key):
        self.proc.stdin.write(key.line + "\n")
        self.proc.stdin.flush()
        self.pending.append(key)

    def receive(self):
        """(key, outcome, ms) of the next answer; None once the worker has stopped."""
        while True:
            line = self.proc.stdout.readline()
            if not line:
                return None
            got = parse_done_line(line)
            if got:
                return got

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass


def offer(worker, todo):
    """The queue's next key to the worker; False when the queue is empty or the worker is gone."""
    try:
        key = todo.get_nowait()
    except queue.Empty:
        return False
    try:
        worker.send(key)
    except (OSError, ValueError):   # (gone: feed() reports its pending keys, this one among them)
        worker.pending.append(key)
        return False
    return True


def feed(worker, todo, report):
    """(a thread) The worker's share of the queue: whatever is next, once it has answered its last,
    until the queue is empty. report(key, outcome, ms) for each answer; a worker that stops with a
    key pending reports it as "died"."""
    while True:
        while len(worker.pending) < Worker.IN_FLIGHT and offer(worker, todo):
            pass
        if not worker.pending:
            break
        got = worker.receive()
        if got is None:
            for key in worker.pending:
                report(key.key, "died", 0)
            worker.pending.clear()
            break
        worker.pending.popleft()   # (a worker answers in the order asked)
        reported, outcome, ms = got
        report(reported, outcome, ms)
    worker.close()


def serve(exe, pack, cache, todo, jobs, times, show, every=1.0, popen=None, out=None, save=None, save_every=30.0):
    """The keys compiled by `jobs` workers pulling from one queue, the progress shown through
    `show` ([(done, of)], work=(done, of) in compile time, by weights()) and the compiler's
    failures printed to `out`; compile times recorded in `times`, and given to save(times, last)
    every save_every seconds and once at the end - from a thread of its own, which a Ctrl+C (a
    KeyboardInterrupt lands in the main thread alone, and twice when pressed twice) cannot cut
    short: the last save is waited for before the interrupt goes on. Returns the outcomes counted."""
    out = out or sys.stdout
    popen = popen or (lambda: subprocess.Popen([exe, pack, cache, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                               stderr=subprocess.PIPE, text=True, bufsize=1, **low_priority()))
    q = queue.Queue()
    for k in todo:
        q.put(k)
    weight = weights(todo, times)
    work_of = sum(weight.values())
    totals = collections.Counter()
    lock = threading.Lock()
    done = [0]
    work = [0.0]

    def report(key, outcome, ms):
        with lock:
            totals[outcome] += 1
            done[0] += 1
            work[0] += weight.get(key, 0.0)
            if outcome == "compiled":
                times[key] = ms

    procs = [popen() for _ in range(jobs)]   # (all started before any thread: low_priority's preexec_fn)
    stop = threading.Event()

    def saving():   # (joined in the finally below, so that its last save is waited for, Ctrl+C or not)
        main = threading.main_thread()
        last = time.monotonic()
        while not stop.is_set() and main.is_alive():
            stop.wait(min(1.0, save_every))
            if not stop.is_set() and time.monotonic() - last >= save_every:
                with lock:
                    snapshot = dict(times)
                save(snapshot, False)
                last = time.monotonic()
        with lock:
            snapshot = dict(times)
        save(snapshot, True)
    saver = threading.Thread(target=saving) if save else None
    workers = [Worker(p) for p in procs]   # (each is given one key, and its next once that is answered: longest
                                           # first, so the longest compiles go to as many workers as there are)
    feeders = [threading.Thread(target=feed, args=(w, q, report), daemon=True) for w in workers]
    try:
        if saver:
            saver.start()
        for t in feeders:
            t.start()
        while any(t.is_alive() for t in feeders):
            show([(done[0], len(todo))], work=(work[0], work_of))
            time.sleep(every)
        for w in workers:
            w.proc.wait()
            w.drain.join()
            w.proc.stdout.close()
            w.proc.stderr.close()
        show([(done[0], len(todo))], work=(work[0], work_of), final=True)
    finally:
        stop.set()   # (the saver's last save follows the bar's last line, or an interrupted bar's;
        while saver and saver.is_alive():   # a second Ctrl+C waits for it too, the file is written whole)
            try:
                saver.join()
            except KeyboardInterrupt:
                pass
    errors = [e for w in workers for e in w.errors]
    for e in errors[:20]:
        print(e, file=out)
    if len(errors) > 20:
        print(f"... and {len(errors) - 20} more compiler failures", file=out)
    return totals


# ---- progress: shaderaot's "progress <done> <of>" lines, from every part, as one bar ------------
def parse_progress(line):
    """(done, of) from a "progress <done> <of>" line, else None."""
    p = line.split()
    if len(p) == 3 and p[0] == "progress" and p[1].isdigit() and p[2].isdigit():
        return int(p[1]), int(p[2])
    return None


def progress_text(done, of, rate=None, width=30, work=None):
    """The bar, the share done, and the time left at `rate` shaders a second (once there is one).
    With `work` (done, of) in units of compile time, the share and the time left go by it, at
    `rate` units a second: the queue starts with its longest compiles, so the count says little."""
    wdone, wof = work if work and work[1] else (done, of)
    frac = share(done, of, work)
    text = f"{bar(frac, width)} {frac:4.0%}  {done:,} of {of:,} shaders"
    if rate and 0 < done < of and wdone < wof:
        text += ", " + time_left((wof - wdone) / rate)
    return text


def share(done, of, work=None):
    """The share done, by work when there is any, else by count."""
    wdone, wof = work if work and work[1] else (done, of)
    return min(1.0, wdone / wof) if wof else 1.0


class Progress:
    """Shows the progress (the parts' combined, or the queue's): one line drawn over itself in a
    console, else a line every 5 % (for a log)."""

    WINDOW = 120   # seconds of recent pace the time left goes by (the parts' pace picks up after the start);
                   # None for the pace since the start (the queue's, by work: steadier than a window a long compile lands in)

    def __init__(self, out, console=None, window=WINDOW):
        self.out = out
        self.console = out.isatty() if console is None else console
        self.window = window
        self.samples = []   # (time, shaders done), once every part has said how much it has
        self.ready_said = False
        self.shown = -1     # (the last 5 % step a log line was written for)
        self.last = None
        self.width = 0

    def rate(self, now, done):
        """Shaders (or work) a second over the last `window` seconds, once there are 10 seconds of them."""
        self.samples.append((now, done))
        while self.window is not None and len(self.samples) > 2 and self.samples[1][0] <= now - self.window:
            self.samples.pop(0)
        t0, d0 = self.samples[0]
        return (done - d0) / (now - t0) if now - t0 >= 10 and done > d0 else None

    def __call__(self, state, now=None, final=False, work=None):
        now = time.time() if now is None else now
        if any(of is None for _done, of in state):
            if not self.ready_said:
                self.out.write("getting ready: translating the vertex programs...\n")
                self.out.flush()
                self.ready_said = True
            return
        done, of = sum(d for d, _o in state), sum(o for _d, o in state)
        text = progress_text(done, of, self.rate(now, work[0] if work else done), work=work)
        if self.console:
            self.out.write("\r" + text.ljust(self.width) + ("\n" if final else ""))
            self.width = len(text)
            self.out.flush()
        else:
            step = int(100 * share(done, of, work)) // 5
            if step > self.shown or (final and text != self.last):
                self.out.write(text + "\n")
                self.out.flush()
                self.shown, self.last = step, text


def follow(procs, show, every=1.0):
    """Waits for the parts, showing their progress as it comes; returns their last lines ("part ...")."""
    state = [(0, None)] * len(procs)
    lasts = []

    def read(i, p):
        for line in p.stderr:
            got = parse_progress(line)
            if got:
                state[i] = got
            elif line.startswith("part "):
                lasts.append(line.rstrip())
    readers = [threading.Thread(target=read, args=(i, p), daemon=True) for i, p in enumerate(procs)]
    for r in readers:
        r.start()
    while any(r.is_alive() for r in readers):
        show(state)
        time.sleep(every)
    for p in procs:
        p.wait()
    show([(d, o if o is not None else d) for d, o in state], final=True)
    return lasts


def totals_from_parts(lines):
    """The parts' last lines (--parts) added up: the outcomes counted as serve() counts them, or
    None if a line doesn't read as expected."""
    fields = ("keys", "compiled", "existing", "tables", "failed", "untranslated")
    total = collections.Counter()
    for line in lines:
        m = re.search(r": (\d+) keys; (\d+) compiled, (\d+) already cached, (\d+) with jump tables \(left out\), "
                      r"(\d+) fxc failures, (\d+) not translated", line)
        if not m:
            return None
        for k, v in zip(fields, m.groups()):
            total[k] += int(v)
    return total


def summary(totals):
    """One line of what was done, from the outcomes counted (serve(), totals_from_parts())."""
    t = collections.Counter(totals)
    text = (f"{t['keys']:,} shaders: {t['compiled']:,} compiled, {t['existing']:,} already there, "
            f"{t['tables']:,} left out (jump tables), {t['failed']:,} that fxc could not compile, "
            f"{t['untranslated']:,} not translated")
    extra = {k: v for k, v in t.items() if v and k in ("missing", "died")}
    if extra:
        text += " (" + ", ".join(f"{v:,} {'not in the pack' if k == 'missing' else 'lost when a worker stopped'}" for k, v in extra.items()) + ")"
    return text


def cache_version():
    text = open(os.path.join(ROOT, "runtime", "gpu_shader_cache.h"), encoding="utf-8").read()
    return int(re.search(r"#define SHADER_CACHE_VERSION (\d+)", text).group(1))


if __name__ == "__main__":
    sys.exit(main())
