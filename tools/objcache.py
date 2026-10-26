"""Whether a compiled object is up to date with what it was built from: the Windows build
(tools/recomp.py) and the Linux one (tools/linux/build_linux.py). (The macOS build keeps a record
of its own: tools/mac/build_mac.py.)

An object is current when the key it was compiled with is this build's. The key is a SHA-256 over
  - the content of its sources (the generated C; on Linux also the C++ file that includes it),
  - the content of the runtime headers it is built with (header_files: for the generated C,
    recomp.h or module.h and what they include from runtime/; a file with hook slots
    includes hookable.h too: game_headers),
  - the compiler: its path and what it prints for --version (compiler_id, once per build),
  - the whole compile command: optimisation level, instruction set, defines, include folders.
So another flag, header or compiler compiles the objects built with it again, and a file saved
again unchanged does not: the decision goes by content, never by date. (An object the compile
ladder compiled another way, tools/ladder.py, is keyed on the command it was compiled with, its
optnone header's content with it, and recorded with how: ladder.check.)

Each build folder keeps its record in objcache.json: each object's key, with the parts it is made
of (which say why an object is compiled again), and each source's SHA-256 with its size and time
(to the nanosecond): a source with the same size and time is not read again. An object's key is
recorded once it has compiled (Record.compiled) and forgotten before it is compiled again
(Record.forget), the record written whole each time (a new file, then os.replace): a build that
stops half-way, or a file that fails to compile, leaves no key for an object it did not make.
No record, or one that cannot be read: its objects are compiled again - except in a game folder an
older version built, which has objects but has never had a record (adopt_by_date): there an object
newer than its sources and than every runtime header is taken as current, as the old rule (by date)
took it, and recorded, so updating to this version does not compile the whole game again - only when
the build compiles with the flags the older version did (the date cannot say what an object was
compiled with), which the callers check (tools/isa.py's adopts): no build does today. One there
that cannot be opened (another program holding it) stops the build before anything is
compiled or removed."""
import functools
import hashlib
import json
import os
import re
import subprocess
import threading
import time

RECORD = "objcache.json"
VERSION = 1
# A source written this recently is read again next time: a disk that keeps times to the second or
# two (FAT, exFAT, some shares) would give a file changed in the same second the same time.
RECENT_NS = 3 * 10**9
INCLUDE = re.compile(rb'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.M)
# What a generated file with hook slots holds (tools/recomp.py writes it, runtime/hookable.h): a
# Record's scan finds it, and game_headers then keys the file on hookable.h's headers too.
HOOK_SLOTS = b'#include "hookable.h"'
# The header sets a generated file is built with (header_files' names): game_headers picks one.
GAME_HEADERS = (("recomp.h",), ("recomp.h", "hookable.h"), ("module.h",))

# why an object is compiled; the last four are worth a line in the build's log (summary)
NEW, CHANGED = "no object yet", "its source changed"
UNRECORDED = "no record of what they were built from"
COMPILER, COMMAND, HEADERS = "the compiler changed", "the compile command changed", "the runtime headers changed"
NOTABLE = (UNRECORDED, COMPILER, COMMAND, HEADERS)
PARTS = ("compiler", "command", "headers", "source")


def sha(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(p.encode("utf-8", "surrogateescape") + b"\0")
    return h.hexdigest()


def file_sha(path, scan=()):
    """(SHA-256 of a file's content, which of the byte strings `scan` it holds)."""
    h, found, tail = hashlib.sha256(), set(), b""
    keep = max((len(s) for s in scan), default=1) - 1
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            if scan:
                window = tail + chunk
                found |= {s for s in scan if s in window}
                tail = window[-keep:] if keep else b""
    return h.hexdigest(), found


def header_files(folder, *names):
    """The headers `names` (in folder) and each header they include with quotes, and so on: found
    beside the file including it, else in folder. A name found in neither is left out (a system
    header: the compiler's, which compiler_id stands for)."""
    found, todo = set(), [(folder, n) for n in names]
    while todo:
        here, name = todo.pop()
        path = next((p for p in (os.path.join(here, name), os.path.join(folder, name)) if os.path.isfile(p)), None)
        if path is None or os.path.normpath(path) in found:
            continue
        path = os.path.normpath(path)
        found.add(path)
        with open(path, "rb") as f:
            todo += [(os.path.dirname(path), m.decode("utf-8", "replace")) for m in INCLUDE.findall(f.read())]
    return sorted(found)


def game_headers(name, found):
    """Which of GAME_HEADERS the generated file `name` is built with: g_module.c module.h, a file
    holding HOOK_SLOTS (`found`: Record.source's scan) recomp.h and hookable.h, the rest recomp.h.
    So a change to hookable.h compiles again only the files with hook slots."""
    if name == "g_module.c":
        return ("module.h",)
    return GAME_HEADERS[1] if HOOK_SLOTS in found else GAME_HEADERS[0]


def digest_files(paths):
    """One SHA-256 over these files' names and content (headers: a few small files, read each build)."""
    return sha(*(f"{os.path.basename(p)}\0{file_sha(p)[0]}" for p in sorted(paths)))


@functools.lru_cache(maxsize=None)
def compiler_id(compiler):
    """The compiler's path and what it prints for --version: another compiler, or another version at
    the same path, compiles everything again. Run once per build. `compiler`: its path, or a tuple
    of the arguments that run it."""
    args = [compiler] if isinstance(compiler, str) else list(compiler)
    try:
        r = subprocess.run([*args, "--version"], capture_output=True, text=True, errors="replace", timeout=120)
        said = (r.stdout + r.stderr).strip()
    except (OSError, subprocess.SubprocessError) as e:
        said = f"(could not run it: {e})"
    return " ".join(args) + "\n" + said


def _retried(do):
    """do(), tried again for a moment on PermissionError: on Windows something (an antivirus) can hold
    the record open just after it was written."""
    for attempt in range(5):
        try:
            return do()
        except PermissionError:
            if attempt == 4:
                raise
            time.sleep(0.1 * (attempt + 1))


def summary(reasons):
    """The build log's line for why objects are compiled again (reasons: a Counter of Record.check's
    answers), or None when it is only new and changed sources."""
    notable = [(why, reasons[why]) for why in NOTABLE if reasons.get(why)]
    others = [(why, n) for why, n in reasons.items() if n and why not in NOTABLE + (NEW, CHANGED)]
    notable += others   # (a record that could not be read)
    if not notable:
        return None
    total = sum(n for _w, n in notable)
    said = notable[0][0] if len(notable) == 1 else ", ".join(f"{why} ({n})" for why, n in notable)
    return f"{total} object{'s' if total != 1 else ''} to compile again: {said}"


class Record:
    """A build folder's objcache.json. Its methods may be called from several threads.

    save() keeps the objects and sources this Record looked at (check, compiled): a build looks at
    every object of its folder, and one it did not look at is no longer built there."""

    def __init__(self, folder, scan=(), log=print, adopt_by_date=None, adopt_found=None):
        """adopt_by_date: the runtime headers (paths) of a game folder. If the folder has no record at
        all (an older version built it), an object there newer than its sources and these headers is
        taken as current: what the old rule did. None: an object without a key is compiled again.
        adopt_found: {one of `scan`: more headers}, which an object must be newer than too when its
        source holds that (HOOK_SLOTS: hookable.h, which only those files include)."""
        assert all(s in scan for s in adopt_found or ()), "adopt_found goes by what scan finds"
        self.folder, self.path, self.scan, self.log = folder, os.path.join(folder, RECORD), tuple(scan), log
        self.lock = threading.Lock()
        self.objects, self.sources = {}, {}
        self.seen_objects, self.seen_sources, self.recent = set(), set(), set()
        self.unrecorded = UNRECORDED
        self.adopt_ns, self.adopt_found, self.adopted, self.inputs = None, {}, 0, {}
        try:
            with _retried(lambda: open(self.path, encoding="utf-8")) as f:
                data = json.load(f)
            if not isinstance(data, dict) or data.get("version") != VERSION:
                raise ValueError(f"not version {VERSION}'s")
            self.objects = {k: v for k, v in data["objects"].items() if isinstance(v, dict)}
            self.sources = {k: v for k, v in data["sources"].items() if isinstance(v, list) and len(v) == 4}
        except FileNotFoundError:
            if adopt_by_date is not None:
                self.adopt_ns = max((os.stat(h).st_mtime_ns for h in adopt_by_date), default=0)
                self.adopt_found = {s: max((os.stat(h).st_mtime_ns for h in hs), default=0)
                                    for s, hs in (adopt_found or {}).items()}
        except OSError as e:   # there but not to be opened: not taken for no record, which compiles hours again
            raise SystemExit(f"{self.path} could not be opened ({e}). Build again once no other program holds it "
                             f"open; deleting it compiles the objects in {folder} again.") from e
        except (ValueError, KeyError, TypeError, AttributeError) as e:
            self.unrecorded = f"{RECORD} could not be read"
            log(f"{self.path} could not be read ({e}): the objects there are compiled again")

    def name(self, obj):
        return os.path.relpath(obj, self.folder)

    def source(self, path):
        """(SHA-256 of a source's content, which of `scan` it holds); read again only when its size
        or time changed."""
        path = os.path.abspath(path)
        st = os.stat(path)
        with self.lock:
            self.seen_sources.add(path)
            was = self.sources.get(path)
        if was and was[0] == st.st_size and was[1] == st.st_mtime_ns and isinstance(was[3], dict) \
                and all(s.decode() in was[3] for s in self.scan):
            return was[2], {s for s in self.scan if was[3][s.decode()]}
        digest, found = file_sha(path, self.scan)
        with self.lock:
            self.sources[path] = [st.st_size, st.st_mtime_ns, digest, {s.decode(): s in found for s in self.scan}]
            if time.time_ns() - st.st_mtime_ns < RECENT_NS:
                self.recent.add(path)   # (this build trusts it; the record does not keep it)
            else:
                self.recent.discard(path)
        return digest, found

    def entry(self, sources, command, compiler, headers):
        """The key of an object compiled from `sources` (their content) by `command` (a list), with
        `compiler` (compiler_id) and runtime headers whose digest_files is `headers`; with its parts."""
        parts = {"compiler": sha(compiler), "command": sha(*command), "headers": headers,
                 "source": sha(*(self.source(p)[0] for p in sources))}
        entry = {"key": sha(*(parts[p] for p in PARTS)), **{p: parts[p][:16] for p in PARTS}}
        if self.adopt_ns is not None:
            found = max((self.adopt_found[s] for p in sources for s in self.source(p)[1] if s in self.adopt_found),
                        default=0)
            with self.lock:
                self.inputs[entry["key"]] = ([os.path.abspath(p) for p in sources], found)
        return entry

    def check(self, obj, entry):
        """None if the object is there and was compiled with this key, else why it is to be compiled."""
        name = self.name(obj)
        with self.lock:
            self.seen_objects.add(name)
            was = self.objects.get(name)
        if not os.path.exists(obj):
            return NEW
        if not was:
            if self.adopt_ns is not None and self._older_rule_kept(obj, entry):
                with self.lock:
                    self.objects[name] = entry
                    self.adopted += 1
                return None
            return self.unrecorded
        if was.get("key") == entry["key"]:
            return None
        if was.get("source") != entry["source"]:
            return CHANGED   # (the cause even when its level or language changed with it)
        return next((why for p, why in zip(PARTS, (COMPILER, COMMAND, HEADERS)) if was.get(p) != entry[p]), CHANGED)

    def _older_rule_kept(self, obj, entry):
        """Whether the rule before this record (by date) kept this object: newer than its sources and
        the runtime headers (adopt_found's too, for a source that holds one)."""
        with self.lock:
            sources, found = self.inputs.get(entry["key"], ((), 0))
        if not sources:
            return False
        try:
            return os.stat(obj).st_mtime_ns >= max([self.adopt_ns, found] + [os.stat(p).st_mtime_ns for p in sources])
        except OSError:
            return False

    def recorded(self, obj):
        """The entry an object was recorded with, or None (with its fields beyond entry()'s: the compile
        ladder's "ladder", the way it compiled the object, tools/ladder.py)."""
        with self.lock:
            return self.objects.get(self.name(obj))

    def forget(self, objs):
        """Before these objects are compiled: their keys gone from the record on disk. A record that
        cannot be written stops the build here, before the builds remove an object."""
        with self.lock:
            for obj in objs:
                self.objects.pop(self.name(obj), None)
            self._save()

    def compiled(self, obj, entry):
        """An object compiled with this key: recorded at once (a build stopped later keeps it)."""
        if not os.path.exists(obj):
            return
        with self.lock:
            self.objects[self.name(obj)] = entry
            self.seen_objects.add(self.name(obj))
            try:
                self._save()
            except OSError as e:   # (not worth stopping the build for: the object is compiled again next time)
                self.log(f"could not write {self.path} ({e}): its objects may be compiled again next time")

    def save(self):
        with self.lock:
            if self.adopted:   # (said once: the record now holds them)
                self.log(f"{self.folder}: {self.adopted} object{'s' if self.adopted != 1 else ''} an older version "
                         "compiled taken as current (newer than their sources and the runtime headers) and recorded")
                self.adopted = 0
            try:
                self._save()
            except OSError as e:   # (the sources' digests: they are read again next time)
                self.log(f"could not write {self.path} ({e}): its sources are read again next time")

    def _save(self):
        data = {"version": VERSION,
                "objects": {k: v for k, v in self.objects.items() if k in self.seen_objects},
                "sources": {k: v for k, v in self.sources.items() if k in self.seen_sources and k not in self.recent}}
        os.makedirs(self.folder, exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, separators=(",", ":"), sort_keys=True)
        _retried(lambda: os.replace(tmp, self.path))
