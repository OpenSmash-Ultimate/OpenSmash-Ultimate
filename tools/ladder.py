"""The compile ladder: no single file can hang or stop a player's build (the Windows build,
tools/recomp.py with tools/cc.py; the Linux one, tools/linux/build_linux.py; the macOS one,
tools/mac/build_mac.py).

Every file is compiled as it always was: the same command, at the same level. Only when that
compile goes wrong is the file compiled again another way, and the build says so:

  - The compiler crashed (crashed): a PC short of memory, or one not quite stable, does that, and
    it rarely happens twice to the same file. Once the others are done, the file is compiled again
    on its own at its level, then one level lower (RETRY: /O2 to /O1, -O2 to -O1; never to the
    lowest for a crash: that is for running too long, below).
  - It ran longer than its time limit (Ladder.limit): a few of LLVM's passes are worse than linear
    in a function of tens of thousands of instructions with thousands of labels (a few fighter modules
    have one of 100,000 to 210,000: tools/progress.py). The compiler is stopped with every process it started (kill_tree), what
    it was writing removed (remove_partial), and the file compiled again at its level with its
    longest functions unoptimised: `optnone` (and noinline) on each, declared in a header given to
    the compiler before the file (-include, clang-cl's /FI; the generated C is not touched), first
    on the functions longer than 20,000 instructions (THRESHOLDS), then 5,000, then 1,250
    (function_sizes counts them in the C); then the whole file at the lowest level (-O0, /Od).
    Each try has a time limit of its own. musttail calls stay tail calls at every level, optnone
    and -O0 included (tests/test_compile_ladder.py runs a chain of ten million of them).

A generated file's time limit is the larger of FLOOR and TIMES times its estimate (the build's cost
model, tools/progress.py, or what it took on this PC last time if that is more: Times), at this
PC's pace: the seconds it has taken for an estimated second in this build so far (Pace; the last
builds' pace until enough is compiled, PACE_UNKNOWN before any; never less than 1). The pace is
asked again every second, so a big file started first gets a limit that follows what the build
learns. Time the compiler does not run, a PC asleep (clock) or a build stopped with Ctrl+Z
(stopped), does not count; a PC too busy to look on time counts in full (run) where stopped can
tell the two apart (Linux; macOS from Python 3.13), and is taken for a pause where it cannot
(Windows, where Modern Standby pauses the build in a clock that runs on; macOS before 3.13).
SSBU_COMPILE_LIMIT=<seconds> gives every generated file that limit instead (0: none; then the files
the ladder compiled another way are compiled again the normal way). The runtime's files have no
time limit: only the crash retry.

A file the ladder compiled another way is recorded with the command it was compiled with: its level
and the optnone header's content are in its key (check, key_command; on macOS its line in
game_sources.tsv), so the next build neither compiles it again (the header made again from the C as
it is then is the same) nor keeps it once its C, the flags, the headers, the compiler or
SSBU_OPTNONE_ABOVE changed (ladder_entry). A crash retry keeps its own level's key
(tools/objcache.py).

Each generated file's compile seconds and level go into the build folder's compile_times.json
(Times): the next build's progress bar estimates by them, and so do its time limits. Each build
starts its files in the order it always has (the Windows one the manifest's, the Linux and macOS
ones the biggest source first). Each time the ladder steps in it is said in the build's log, listed
again at the end and added to the build folder's ladder_report.txt (Report).

For benchmarking, off unless set: SSBU_OPTNONE_ABOVE=<instructions> compiles every generated file
with a function longer than that with those functions unoptimised from the start, and the rest of
the file at the level it would have without them (eager): the way ssbu-recomp's build_opt.py
compiles. The default build compiles a file with a function over 20,000 instructions at /O1, whole.
SSBU_LONGEST_FIRST=1 starts the files the longest first by what they took last time
(longest_first)."""
import glob
import hashlib
import json
import os
import re
import signal
import subprocess
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from typing import NamedTuple

LIMIT_ENV, OPTNONE_ENV, ORDER_ENV = "SSBU_COMPILE_LIMIT", "SSBU_OPTNONE_ABOVE", "SSBU_LONGEST_FIRST"
FLOOR = 600.0           # seconds: no generated file is stopped sooner (most take a few)
TIMES = 4.0             # its estimate times this, at this PC's pace (the biggest files take up to 1.5 times theirs)
PACE_UNKNOWN = 4.0      # this PC's pace taken to be this until it is measured
MEASURED = 60.0         # estimated seconds compiled before a pace counts (as tools/progress.py's)
THRESHOLDS = (20000, 5000, 1250)   # optnone on the functions longer than these (instructions), in turn
POLL = 1.0              # how often a running compile's limit is asked again
PAUSED = 5.0            # a look this much later than asked: its compiler asked again, or taken for a pause (run)
REPORT, TIMES_FILE = "ladder_report.txt", "compile_times.json"

# A crash is told from an error in the code by what clang prints (it exits with 1 either way): its crash
# handler's report, or LLVM's own fatal errors such as running out of memory. A clang-cl that crashes
# outright exits with the exception code (0xC0000005 and up); on Linux and macOS a compiler a signal
# stopped (the system's out-of-memory killer, say) has a negative exit status, and clang's driver says
# its compiler process crashed ("unable to execute command", "frontend command failed due to signal").
RETRY = {"/O2": ("/O2", "/O1"), "/O1": ("/O1", "/O1"), "/Od": ("/Od", "/Od"),
         "-O2": ("-O2", "-O1"), "-O1": ("-O1", "-O1"), "-O0": ("-O0", "-O0")}
CRASH_SIGNS = ("PLEASE submit a bug report", "Stack dump:", "frontend command failed", "LLVM ERROR:")
OUT_OF_MEMORY = ("LLVM ERROR: out of memory", "std::bad_alloc", "Cannot allocate memory")
# a function the lifter wrote (tools/lift.py): its definition, a line for each instruction (and the
# registers it keeps in locals) indented, its labels not, then the brace that ends it
DEFINITION = re.compile(r"void (f_[0-9a-f]+(?:_orig)?)\(Cpu \*c\) \{$")


def lowest(level):
    return "/Od" if level.startswith("/") else "-O0"


def _setting(env, name, what, integer=False):
    text = (env.get(name) or "").strip()
    if not text:
        return None
    try:
        value = int(text) if integer else float(text)
    except ValueError:
        value = -1
    if not value >= 0:
        raise SystemExit(f"{name}={text}: {what}")
    return value


def limit_setting(env=None):
    """SSBU_COMPILE_LIMIT: None when not set (each file's limit from its estimate), else the seconds every
    generated file has (0: no limit)."""
    return _setting(os.environ if env is None else env, LIMIT_ENV, "the seconds a file may take to compile (0: no limit)")


def optnone_setting(env=None):
    """SSBU_OPTNONE_ABOVE: the instructions above which a function is compiled unoptimised from the start,
    or None (not set, or 0)."""
    return _setting(os.environ if env is None else env, OPTNONE_ENV,
                    "the instructions above which a function is compiled unoptimised (0: none)", integer=True) or None


def longest_first_setting(env=None):
    """SSBU_LONGEST_FIRST: whether the files start the longest first by what they took last time
    (longest_first); off unless set to 1 (for benchmarking: the order is the build's speed). 1 or 0."""
    env = os.environ if env is None else env
    what = "1 to start the files the longest first by what they took last time (0: off)"
    value = _setting(env, ORDER_ENV, what, integer=True)
    if value not in (None, 0, 1):
        raise SystemExit(f"{ORDER_ENV}={env.get(ORDER_ENV).strip()}: {what}")
    return value == 1


def settings(env=None):
    """(limit_setting, optnone_setting, longest_first_setting), which a build reads before it does anything
    else: a value that is not a number stops it then, not once the C is written."""
    return limit_setting(env), optnone_setting(env), longest_first_setting(env)


def duration(seconds):
    """A time, as a person says it."""
    s = int(round(seconds))
    if s < 60:
        return f"{s} s"
    if s < 3600:
        return f"{s // 60} min {s % 60:02d} s"
    return f"{s // 3600} h {s // 60 % 60:02d} min"


# ---- crashes ----------------------------------------------------------------------------------------------
def crashed(code, out):
    """Whether a failed compile (exit code, output) was the compiler crashing."""
    return code < 0 or code >= 0xC0000000 or any(sign in out for sign in CRASH_SIGNS)


def crash_cause(code, out):
    """A crash's cause in a few words, for the build's log."""
    if any(sign in out for sign in OUT_OF_MEMORY):
        return "out of memory"
    found = re.search(r"Exception Code: (0x[0-9A-Fa-f]+)", out)
    if found:
        return f"exception {found.group(1)}"
    if code >= 0xC0000000:
        return f"exception 0x{code:08X}"
    if code < 0:
        try:
            name = signal.Signals(-code).name
        except ValueError:
            name = f"signal {-code}"
        return name + (": the system most likely ran out of memory" if name == "SIGKILL" else "")
    found = re.search(r"unable to execute command: (.+)", out)
    return found.group(1).strip() if found else "a crash"


def crash_report(out):
    """A crash's output without the compiler's command line and its raw stack frames."""
    return "\n".join(line for line in out.splitlines() if line.strip() and not re.match(r"\s*#\d+ 0x", line)
                     and "Program arguments:" not in line and not line.startswith("PLEASE submit"))


def crash_files(out):
    """Removes what clang wrote for a bug report after a crash: a preprocessed copy of the file and a
    script, in the temporary folder (clang-cl's commands turn that off, the Linux and macOS ones do not)."""
    temp = os.path.realpath(tempfile.gettempdir())
    for path in re.findall(r"diagnostic msg: (\S+\.(?:c|cpp|i|ii|sh))\s*$", out, re.M):
        if os.path.dirname(os.path.realpath(path)) == temp:
            try:
                os.remove(path)
            except OSError:
                pass


# ---- running a compiler -----------------------------------------------------------------------------------
class Run(NamedTuple):
    code: int
    out: str
    seconds: float
    timed_out: bool = False


class Stopped(Exception):
    """The build was interrupted while a compile ran."""


_running, _running_lock = set(), threading.Lock()


def _clock():
    """The clock compiles are timed by (run), in seconds: one that does not count the time the PC is asleep.
    Linux's and macOS's time.monotonic do not (CLOCK_MONOTONIC, mach_absolute_time); Windows' does, so there
    it is QueryUnbiasedInterruptTime, which leaves out sleep and hibernation."""
    if os.name == "nt":
        try:
            import ctypes
            query = ctypes.WinDLL("kernel32").QueryUnbiasedInterruptTime
            query.argtypes, query.restype = [ctypes.POINTER(ctypes.c_ulonglong)], ctypes.c_int
            if query(ctypes.byref(ctypes.c_ulonglong())):
                def unbiased():
                    t = ctypes.c_ulonglong()
                    query(ctypes.byref(t))
                    return t.value / 1e7   # (100 ns units)
                return unbiased
        except (ImportError, AttributeError, OSError):
            pass
    return time.monotonic


clock = _clock()


# (None where it cannot tell a compiler stopped: Windows, and macOS's Python before 3.13, which has no waitid)
waitid = None if os.name == "nt" else getattr(os, "waitid", None)


def stopped(proc):
    """Whether a running compiler has been stopped, or let go on, since this was last asked: what a build
    stopped with Ctrl+Z does to every process in it, its compilers too (they stay in its process group).
    waitid reports that of a child without taking one that has ended from subprocess (macOS reports the
    stop as well as the going on). None where nothing tells (waitid): on Windows, where Modern Standby
    pauses every desktop program, the build and its compilers, while the clock runs on (it is the
    working state, S0), and on macOS with a Python before 3.13."""
    if waitid is None:
        return None
    if proc.returncode is not None:
        return False
    found = False
    for _ in range(4):
        try:
            r = waitid(os.P_PID, proc.pid, os.WCONTINUED | os.WNOHANG)
        except (OSError, ValueError):   # (ChildProcessError: it has ended, and subprocess has it)
            return found
        if r is None:
            return found
        found = found or r.si_code in (os.CLD_STOPPED, os.CLD_CONTINUED)
    return found


def _killed(proc):
    """Whether kill_tree stopped a running compiler, not one that had just ended by itself with its own exit
    code: on Linux and macOS one it stopped ends with SIGKILL; on Windows taskkill ends it with 1, which is
    an error's exit code too, so any but 0 is taken for it there."""
    return proc.returncode != 0 if os.name == "nt" else proc.returncode == -signal.SIGKILL


def run(argv, limit=None, stop=None, **popen):
    """Runs a compiler: Run(exit code, what it printed, seconds, whether its time limit stopped it). limit:
    None, seconds, or a function giving them (or None), asked again every POLL seconds; stop: an Event
    that stops it (Stopped). A compile stopped either way, or by an exception here (Ctrl+C), is stopped
    with every process it started. Its seconds leave out the time the compiler did not run, so that the
    time limit moves on by that much: a PC asleep (clock does not count it), and the time between two
    looks in which the compiler was stopped and let go on (a build stopped with Ctrl+Z: stopped). A look
    late only because the PC is too busy (paging, say, as a compile running away with the memory makes
    it) counts in full. Where stopped cannot tell (None: Windows, macOS's Python before 3.13), a look
    more than PAUSED seconds later than asked is taken for a pause (Modern Standby, Ctrl+Z), as a PC too
    busy to look is not told from it there."""
    t0 = last = clock()
    paused, wait = 0.0, 0.0

    def look():   # (the time now, past every stop so far)
        nonlocal last, paused
        # (asked at every look, a stop long before not taken for this look's; before the time is read, so that a
        # stop that comes between the two is inside this look's time, and found by the asking again below)
        held = stopped(proc)
        now = clock()
        late = now - last - wait
        if held is False and late > PAUSED:   # (the build can be let go on a moment before its compiler is)
            time.sleep(0.05)
            held = stopped(proc)
            now = clock()
            late = now - last - wait
        if held is None:   # (nothing tells: a look this late is taken for a pause)
            held = late > PAUSED
        if held and late > 0:
            paused += late
        last = now
        return now - t0 - paused
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace", **popen)
    with _running_lock:
        _running.add(proc)
    try:
        while True:
            allowed = limit() if callable(limit) else limit
            spent = look()
            if allowed is not None and spent >= allowed:
                finished = proc.poll() is not None
                if not finished:
                    kill_tree(proc)
                out, err = _drain(proc)
                # (one that finished by itself meanwhile, an error or a crash too: its own result, and its object kept)
                return Run(proc.returncode, out + err, spent, not finished and _killed(proc))
            if stop is not None and stop.is_set():
                raise Stopped
            if limit is None and stop is None:
                out, err = proc.communicate()
                return Run(proc.returncode, out + err, clock() - t0)
            wait = POLL if allowed is None else min(POLL, allowed - spent)
            try:
                out, err = proc.communicate(timeout=wait)
            except subprocess.TimeoutExpired:
                continue
            return Run(proc.returncode, out + err, look())
    except BaseException:
        kill_tree(proc)
        _drain(proc)
        raise
    finally:
        with _running_lock:
            _running.discard(proc)


def _drain(proc):
    """What a stopped compiler printed (nothing if something it started holds its output open)."""
    try:
        out, err = proc.communicate(timeout=30)
        return out or "", err or ""
    except (subprocess.TimeoutExpired, ValueError, OSError):
        for pipe in (proc.stdout, proc.stderr):
            try:
                pipe.close()
            except (OSError, AttributeError):
                pass
        proc.wait()
        return "", ""


def kill_running():
    """Stops every compiler run() is running (the build was interrupted)."""
    with _running_lock:
        procs = list(_running)
    for proc in procs:
        kill_tree(proc)


def kill_tree(proc):
    """Stops a compiler and every process it started (clang's driver runs its compiler as a process of
    its own on Linux and macOS). Compilers stay in the build's own process group, so Ctrl+C and a closed
    terminal stop them as before; this finds them by their parents instead: on Windows taskkill /T,
    elsewhere ps (or Linux's /proc), each stopped first so that none starts another meanwhile."""
    if proc.poll() is not None:
        return
    if os.name == "nt":
        try:
            subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"], capture_output=True, timeout=60)
        except (OSError, subprocess.SubprocessError):
            pass
        try:
            proc.kill()
        except OSError:
            pass
        return
    seen = []
    for _ in range(5):
        new = [pid for pid in [proc.pid] + _descendants(proc.pid) if pid not in seen]
        if not new:
            break
        for pid in new:
            _signal(pid, signal.SIGSTOP)
        seen += new
    for pid in seen:
        _signal(pid, signal.SIGKILL)


def _signal(pid, sig):
    try:
        os.kill(pid, sig)
    except OSError:
        pass


def _descendants(root):
    """The processes under `root`, from ps, else (on Linux) /proc."""
    pairs = []
    try:
        out = subprocess.run(["ps", "-A", "-o", "pid=", "-o", "ppid="], capture_output=True, text=True, timeout=30).stdout
        pairs = [tuple(int(n) for n in line.split()) for line in out.splitlines() if len(line.split()) == 2]
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    if not pairs and os.path.isdir("/proc"):
        for name in os.listdir("/proc"):
            try:
                with open(f"/proc/{name}/stat") as f:
                    pairs.append((int(name), int(f.read().rsplit(")", 1)[1].split()[1])))
            except (OSError, ValueError, IndexError):
                pass
    children = {}
    for pid, ppid in pairs:
        children.setdefault(ppid, []).append(pid)
    found, todo = [], [root]
    while todo:
        for child in children.get(todo.pop(), []):
            if child not in found:
                found.append(child)
                todo.append(child)
    return found


def remove_partial(obj):
    """An object a stopped compiler was writing: gone, with clang's temporary file for it
    (<name>-<random><extension>.tmp, which it renames to the object when it has finished). Tried again
    for a moment: on Windows the file can be held just after its writer has gone."""
    stem, ext = os.path.splitext(obj)
    for path in [obj] + glob.glob(glob.escape(stem) + "-*" + ext + ".tmp"):
        for attempt in range(5):
            try:
                os.remove(path)
                break
            except FileNotFoundError:
                break
            except OSError:
                time.sleep(0.1 * (attempt + 1))


# ---- the optnone header -----------------------------------------------------------------------------------
def function_sizes(path):
    """[(instructions, name)] of every function the lifter wrote into a generated file: its indented
    lines, one an instruction (tools/lift.py)."""
    out, name, n = [], None, 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if name is None:
                if line.startswith("void f_"):
                    m = DEFINITION.match(line.rstrip("\r\n"))
                    if m:
                        name, n = m.group(1), 0
            elif line.startswith("}"):
                out.append((n, name))
                name = None
            elif line.startswith("    "):
                n += 1
    return out


def optnone_header(sizes, above):
    """(the header that has a generated file's functions longer than `above` instructions compiled
    unoptimised, those functions [(instructions, name)]), or (None, []) when it has none. The header is
    given before the file, so it declares them after what recomp.h declares, with C linkage for a file
    compiled as C++ (one with landing sites, on Linux and macOS); the definitions take the attributes."""
    big = [(n, name) for n, name in sizes if n > above]
    if not big:
        return None, []
    return ("/* tools/ladder.py: functions too long to optimise in a bounded time, compiled unoptimised */\n"
            '#include "recomp.h"\n#ifdef __cplusplus\nextern "C" {\n#endif\n'
            + "".join(f"void {name}(Cpu *c) __attribute__((optnone, noinline));\n" for _n, name in big)
            + "#ifdef __cplusplus\n}\n#endif\n"), big


def header_name(obj):
    """The optnone header's file, beside the object (written for a compile, removed after it)."""
    return os.path.splitext(obj)[0] + ".optnone.h"


class NoHeader(Exception):
    """The optnone header could not be written (_write_header): a try that needs it is not made."""


def _write_header(path, text):
    """Writes the optnone header: None, or why it could not be (a full disk; on Windows the file still held
    a moment after the compiler stopped before it let go, so tried again for a moment, as remove_partial)."""
    for attempt in range(5):
        if attempt:
            time.sleep(0.1 * attempt)
        try:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            return None
        except OSError as e:
            failed = e
    try:   # (what was written of it)
        os.remove(path)
    except OSError:
        pass
    return failed


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def key_command(command, header):
    """A compile command as tools/objcache.py keys an object on it: with the optnone header's content."""
    return list(command) + ([f"(optnone header {digest(header)})"] if header else [])


class Step(NamedTuple):
    """One way of compiling a file: at a level, with its functions longer than `above` unoptimised
    (header: their header; names: [(instructions, name)]; left: the longest function left optimised)."""
    level: str
    above: int = 0
    header: str = None
    names: tuple = ()
    left: int = 0

    def record(self):
        """What a record keeps of a file the ladder compiled this way (Step.recorded reads it)."""
        return {"level": self.level, **({"above": self.above} if self.above else {})}

    def tag(self):
        """The same in a word or three, with the header's digest (macOS's game_sources.tsv)."""
        return self.level + (f" optnone>{self.above}:{digest(self.header)[:16]}" if self.above else "")

    def describe(self):
        if self.above:
            k = len(self.names)
            return (f"with {'its' if k > 1 else 'its one'} function{'s' if k > 1 else ''} over {self.above:,} "
                    f"instructions unoptimised (optnone), the rest at {self.level}")
        return f"whole at {self.level}"


def recorded_step(entry):
    """The Step a record's entry (objcache) says the ladder compiled its object with, or None."""
    found = entry.get("ladder") if isinstance(entry, dict) else None
    if not isinstance(found, dict) or not isinstance(found.get("level"), str):
        return None
    above = found.get("above")
    return Step(found["level"], above if isinstance(above, int) and above > 0 else 0)


def tagged_step(tag):
    """The Step a tag (Step.tag) names, without its header, or None."""
    m = re.fullmatch(r"(\S+)(?: optnone>(\d+):[0-9a-f]{16})?", tag or "")
    return Step(m.group(1), int(m.group(2) or 0)) if m else None


def with_header(step, c):
    """The step with its header made from the generated C as it is now; None when the C has no function
    that long now."""
    if not step.above:
        return step
    sizes = function_sizes(c) if os.path.isfile(c) else []
    header, big = optnone_header(sizes, step.above)
    if not header:
        return None
    return step._replace(header=header, names=tuple(big), left=max((n for n, _ in sizes if n <= step.above), default=0))


def timeout_steps(step, sizes):
    """How a file whose compile (the step's way) ran over its time limit is compiled next, in turn: with
    its functions over each threshold unoptimised (one that names no more functions than the one before
    is left out), then whole at the lowest level; nothing from the lowest level."""
    low = lowest(step.level)
    if step.level == low:
        return []
    out, named = [], len(step.names)
    for above in ((step.above // 4, step.above // 16) if step.above else THRESHOLDS):
        header, big = optnone_header(sizes, above)
        if len(big) > named:
            out.append(Step(step.level, above, header, tuple(big), max((n for n, _ in sizes if n <= above), default=0)))
            named = len(big)
    return out + [Step(low)]


def eager(c, biggest, above):
    """SSBU_OPTNONE_ABOVE's way of compiling a generated file whose biggest function (its manifest's) is
    `biggest`: Step(level '', above, header, functions, the longest function left), or None when it has
    none that long (the caller gives it its level, by the longest left)."""
    if not above or biggest <= above or not os.path.isfile(c):
        return None
    return with_header(Step("", above), c)


def ladder_entry(entry, step, start=0):
    """The key (objcache's entry) of an object the ladder compiled the step's way, recorded with that way
    and the SSBU_OPTNONE_ABOVE its file's own compile had (start: the threshold of its own header, 0 for
    none), which check compares."""
    return {**entry, "ladder": {**step.record(), **({"from": start} if start else {})}}


def check(record, obj, entry, key_for, c, start=0, env=None):
    """tools/objcache.py's Record.check for a generated file's object, which the ladder may have compiled
    another way: then it is current when it was compiled the way the record says (its level, with the
    optnone header that way makes from the C as it is now), from the same start as this build's (start:
    ladder_entry's; the default build does not keep what the ladder made of SSBU_OPTNONE_ABOVE's way,
    nor that the other way round) and with everything else as this build has it. key_for(step): the key
    of the object compiled the step's way. Not with SSBU_COMPILE_LIMIT=0 (no limit: the file is compiled
    again the normal way)."""
    why = record.check(obj, entry)
    found = record.recorded(obj) if why else None
    step = recorded_step(found)
    if step is None or limit_setting(env) == 0 or found["ladder"].get("from", 0) != start:
        return why
    step = with_header(step, c)
    return why if step is None else record.check(obj, key_for(step))


# ---- what the build has measured --------------------------------------------------------------------------
class Pace:
    """This PC's seconds for an estimated second (tools/progress.py's cost model), from this build's
    files compiled so far; until enough are, `known` (the last builds': Times.pace) or None."""
    def __init__(self, known=None):
        self.known, self.seconds, self.estimated = known, 0.0, 0.0
        self.lock = threading.Lock()

    def add(self, seconds, estimate):
        with self.lock:
            self.seconds += seconds
            self.estimated += estimate

    def value(self):
        with self.lock:
            return self.seconds / self.estimated if self.estimated >= MEASURED else self.known


class Times:
    """A build folder's compile_times.json: for each generated file compiled (by its name in the folder),
    the seconds its compile took the last time, its level, the cost model's estimate of it, its source's
    digest and, when the ladder stepped in, how. A file compiled again replaces its entry. Never needed:
    one that cannot be read or written is done without."""
    def __init__(self, folder):
        self.folder, self.path = folder, os.path.join(folder, TIMES_FILE)
        self.lock, self.files, self.changed, self._pace = threading.Lock(), {}, False, None
        try:
            with open(self.path, encoding="utf-8") as f:
                data = json.load(f)
            if data.get("version") == 1:
                self.files = {k: v for k, v in data["files"].items() if isinstance(v, dict)}
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            pass

    def put(self, name, seconds, level, model, source, how=""):
        with self.lock:
            self.files[name] = {"seconds": round(seconds, 1), "level": level, "model": round(model, 3),
                                "source": source, **({"how": how} if how else {})}
            self.changed, self._pace = True, None

    def pace(self):
        """This PC's pace in the builds recorded (as Pace's), or None with too few of them."""
        with self.lock:
            if self._pace is None:
                normal = [(e.get("seconds"), e.get("model")) for e in self.files.values() if not e.get("how")]
                normal = [(s, m) for s, m in normal if isinstance(s, (int, float)) and isinstance(m, (int, float))]
                estimated = sum(m for _s, m in normal)
                self._pace = sum(s for s, _m in normal) / estimated if estimated >= MEASURED else 0.0
            return self._pace or None

    def measured(self, name, source, level):
        """What a file took last time at this PC's pace, in the cost model's seconds, when it was compiled
        from the same source at the same level, the normal way; else None."""
        pace = self.pace()
        e = self.files.get(name)
        if pace and e and not e.get("how") and e.get("source") == source and e.get("level") == level \
                and isinstance(e.get("seconds"), (int, float)):
            return e["seconds"] / pace
        return None

    def estimate(self, name, source, level, model):
        """A file's estimate in the cost model's seconds: measured's, else the cost model's (model)."""
        found = self.measured(name, source, level)
        return model if found is None else found

    def save(self):
        with self.lock:
            if not self.changed:
                return
            try:
                os.makedirs(self.folder, exist_ok=True)
                tmp = self.path + ".tmp"
                with open(tmp, "w", encoding="utf-8", newline="\n") as f:
                    json.dump({"version": 1, "files": self.files}, f, indent=0, sort_keys=True)
                os.replace(tmp, self.path)
                self.changed = False
            except OSError:
                pass


class Report:
    """The files the ladder stepped in for in this build: listed at its end and added to the build
    folder's ladder_report.txt (end)."""
    def __init__(self):
        self.lines, self.lock = [], threading.Lock()

    def add(self, name, what, level, seconds):
        with self.lock:
            self.lines.append(f"{name}  {duration(seconds)}  {level or 'not compiled'}  {what}")

    def end(self, folder, log=print, settings=""):
        with self.lock:
            lines, self.lines = self.lines, []
        if not lines:
            return
        path = os.path.join(folder, REPORT)
        log(f"{len(lines)} file{'s' if len(lines) != 1 else ''} compiled another way (also in {path}):")
        for line in lines:
            log("  " + line)
        try:
            os.makedirs(folder, exist_ok=True)
            with open(path, "a", encoding="utf-8", newline="\n") as f:
                f.write(f"# {time.strftime('%Y-%m-%d %H:%M')}  {settings}\n" + "".join(line + "\n" for line in lines))
        except OSError as e:
            log(f"could not write {path} ({e})")


# ---- the ladder -------------------------------------------------------------------------------------------
class Info(NamedTuple):
    """What the ladder knows of a generated file."""
    c: str              # the generated C (which the C++ file the compiler is given may include)
    name: str           # its name in the log, the report and Times
    insns: int          # the manifest's
    biggest: int
    cxx: bool           # compiled as C++
    model: float        # the cost model's estimate of its compile, in seconds (tools/progress.py)
    estimate: float     # the same from what it took last time, if it was compiled from this source (Times)
    source: str         # its source's digest
    above: int = 0      # SSBU_OPTNONE_ABOVE (eager): its functions longer than this compiled unoptimised
    header: str = None  # their header
    names: tuple = ()   # [(instructions, name)]
    left: int = 0       # the longest function left optimised
    measured: bool = False   # whether estimate is what it took last time (Times.measured)


def longest_first(jobs, infos, env=None):
    """With SSBU_LONGEST_FIRST=1 (for benchmarking), the jobs (src, obj, ...) the longest first by their
    estimates (Info.estimate: what each took last time, else the cost model's) once this build folder
    has measured one of them. Else None: each build keeps the order it has always had (the Windows one
    the manifest's, the Linux and macOS ones the biggest source first), which is its speed when nothing
    goes wrong until a benchmark says otherwise."""
    if not longest_first_setting(env) or not any(infos[job[1]].measured for job in jobs if job[1] in infos):
        return None
    return sorted(jobs, key=lambda job: infos[job[1]].estimate if job[1] in infos else 0.0, reverse=True)


class _Hooks:
    """started(job) and finished(job, seconds), and a progress.Build's stepped and requeued when there is
    one; without it, a file the compiler crashed on is started and finished once."""
    def __init__(self, progress, started, finished):
        self.progress = progress
        self.started = started or getattr(progress, "started", None) or (lambda job: None)
        self.finished = finished or getattr(progress, "finished", None) or (lambda job, seconds: None)
        self.requeues = hasattr(progress, "requeued")

    def stepped(self, job, estimate):
        if hasattr(self.progress, "stepped"):
            self.progress.stepped(job, estimate)

    def requeued(self, job, estimate):
        if self.requeues:
            self.progress.requeued(job, estimate)

    def restarted(self, job):
        if self.requeues:
            self.started(job)


class Ladder:
    """A build's ladder: its pace, its report. cost(insns, biggest, level, cxx): the cost model's seconds
    for a file at a level; compiler: its name for the log; popen: how its compilers are started (their
    priority)."""

    def __init__(self, cost, compiler, log=print, popen=None, env=None):
        env = os.environ if env is None else env
        self.cost, self.compiler, self.log, self.popen = cost, compiler, log, dict(popen or {})
        self.setting, self.above, self.longest = limit_setting(env), optnone_setting(env), longest_first_setting(env)
        self.pace, self.report = Pace(), Report()
        self.stopping = threading.Event()

    def describe(self):
        """The time limits, SSBU_OPTNONE_ABOVE and SSBU_LONGEST_FIRST, for the report."""
        if self.setting == 0:
            limit = f"no time limit ({LIMIT_ENV}=0)"
        elif self.setting:
            limit = f"time limit {self.setting:g} s ({LIMIT_ENV})"
        else:
            pace = self.pace.value()
            limit = (f"time limit: the larger of {duration(FLOOR)} and {TIMES:g} times a file's estimate at this PC's pace "
                     + (f"({pace:.2f})" if pace else "(not measured)"))
        return (limit + (f"; {OPTNONE_ENV}={self.above}" if self.above else "") + (f"; {ORDER_ENV}=1" if self.longest else "")
                + f"; {self.compiler}")

    def end(self, folder):
        """At the end of a build: the report, listed and written into `folder`."""
        self.report.end(folder, self.log, self.describe())

    def limit(self, info, step, first):
        """A try's time limit: None, or a function giving it (the pace changes as the build goes)."""
        if info is None or self.setting == 0:
            return None
        if self.setting:
            return self.setting
        if first:   # (the file's own compile: what it took last time, if that is more)
            estimate = max(info.model, info.estimate)
        else:
            estimate = self.cost(info.insns, step.left if step.above else info.biggest, step.level, info.cxx)
        return lambda: max(FLOOR, TIMES * estimate * max(1.0, self.pace.value() or PACE_UNKNOWN))

    def compile(self, jobs, workers, argv, infos=None, progress=None, started=None, finished=None, compiled=None,
                times=None):
        """Compiles the jobs ((src, obj, level, ...), handed out in this order) `workers` at a time; returns
        [(src, error text)] for those that did not compile, in order. argv(job, level, header): the command
        that compiles a job at a level, with the optnone header file (or None). infos {obj: Info}: the
        generated files (a time limit and the ladder; the others have only the crash retry). progress (a
        progress.Build), or started(job) and finished(job, seconds), follow each file. compiled(job, level)
        is called as one compiles, compiled(job, level, step) when the ladder compiled it another way
        (its key: check). times (Times) gets each generated file's seconds."""
        infos = infos or {}
        hooks = _Hooks(progress, started, finished)
        if times is not None and self.pace.known is None:
            self.pace.known = times.pace()
        self.stopping.clear()
        pool = ThreadPoolExecutor(max(1, workers))
        try:
            futures = [pool.submit(self._first, job, argv, infos.get(job[1]), hooks, compiled, times) for job in jobs]
            results = [f.result() for f in futures]
            pool.shutdown()
            failures = []
            for job, result in zip(jobs, results):   # (the crashes: each on its own, once the others are done)
                if result is not None and result[0] == "crash":
                    result = self._after_crash(job, argv, infos.get(job[1]), *result[1:], hooks, compiled, times)
                if result is not None:
                    failures.append(result[1])
            return failures
        except BaseException:
            self.stopping.set()
            kill_running()
            pool.shutdown(wait=True, cancel_futures=True)
            raise
        finally:
            if times is not None:
                times.save()

    def _attempt(self, job, argv, step, info, first=False):
        """One try at a job, the step's way: Run. Stopped when the build is interrupted; NoHeader when its
        optnone header cannot be written (the try is not made)."""
        obj = job[1]
        header = header_name(obj) if step.header else None
        if header:
            failed = _write_header(header, step.header)
            if failed:
                raise NoHeader(f"could not write {header} ({failed})")
        try:
            r = run(argv(job, step.level, header), self.limit(info, step, first), self.stopping, **self.popen)
        except BaseException:
            remove_partial(obj)
            raise
        finally:
            if header:
                try:
                    os.remove(header)
                except OSError:
                    pass
        if r.code and self.stopping.is_set():   # (stopped with the build, by kill_running: Ctrl+C)
            remove_partial(obj)
            raise Stopped
        if r.timed_out:
            remove_partial(obj)
        elif r.code and crashed(r.code, r.out):   # (a signal, the out-of-memory killer's say, leaves what it wrote)
            remove_partial(obj)
            crash_files(r.out)
        return r

    def _label(self, job, info):
        return info.name if info else os.path.basename(job[0])

    def _cost(self, info, step):
        return self.cost(info.insns, step.left if step.above else info.biggest, step.level, info.cxx) if info else 0.0

    def _own(self, job, info):
        """A job's own way: its level, with SSBU_OPTNONE_ABOVE's header if it has one."""
        return Step(job[2], info.above, info.header, info.names, info.left) if info else Step(job[2])

    def _own_attempt(self, job, argv, step, info, first=False):
        """_attempt the job's own way (or a crash retry's level of it): (Run, the step it was compiled with).
        SSBU_OPTNONE_ABOVE's header that cannot be written: the file compiled whole at that level instead,
        which the ladder still follows (the default build's way at that level)."""
        try:
            return self._attempt(job, argv, step, info, first), step
        except NoHeader as e:
            whole = Step(step.level)
            self.log(f"{e}: compiling {self._label(job, info)} {whole.describe()} instead")
            return self._attempt(job, argv, whole, info, first), whole

    def _first(self, job, argv, info, hooks, compiled, times):
        """A job's own compile, then the ladder if it ran over its time limit. None, or ("crash", Run, its
        step) to compile it again once the others are done, or ("fail", (src, error text))."""
        if self.stopping.is_set():
            return None
        own = self._own(job, info)
        hooks.started(job)
        try:
            r, step = self._own_attempt(job, argv, own, info, first=True)
        except Stopped:
            return None
        if not r.code and not r.timed_out:
            if step is own:
                if info:
                    self.pace.add(r.seconds, info.model)
                self._settled(job, info, None, r.seconds, "", hooks, compiled, times)
            else:   # (recorded as the ladder's way, under its own key: ladder.check)
                self._settled(job, info, step, r.seconds, f"its optnone header could not be written: compiled {step.describe()}",
                              hooks, compiled, times, level=step.level)
            return None
        if r.timed_out:
            return self._down(job, argv, info, step, r.seconds, r.seconds, [], hooks, compiled, times)
        if crashed(r.code, r.out):
            hooks.requeued(job, self._cost(info, step))
            return "crash", r, step
        hooks.finished(job, r.seconds)
        return "fail", (job[0], r.out)

    def _after_crash(self, job, argv, info, r, step0, hooks, compiled, times):
        """A job the compiler crashed on (its own way: step0), compiled again on its own (RETRY)."""
        src, level, own = job[0], job[2], self._own(job, info)
        label, cause = self._label(job, info), crash_cause(r.code, r.out)
        retries, spent, code, out = RETRY.get(level, (level, level)), r.seconds, r.code, r.out
        hooks.restarted(job)
        for again in retries:
            self.log(f"the compiler crashed on {label} ({crash_cause(code, out)}): compiling it again on its own"
                     + (f", at {again}" if again != level else ""))
            try:
                r, step = self._own_attempt(job, argv, step0._replace(level=again), info)
            except Stopped:
                return None
            step0 = step._replace(level=step0.level)
            spent += r.seconds
            what = f"the compiler crashed ({cause}): compiled again on its own" + (f", at {again}" if again != level else "")
            if not r.code and not r.timed_out:
                self.log(f"{label} compiled" + (f" (at {again}: a little less optimised, works the same)" if again != level else "")
                         + f" in {duration(r.seconds)}")
                if step.above != own.above:
                    what += f"; its optnone header could not be written: compiled {step.describe()}"
                self._settled(job, info, None if step.above == own.above else step, spent, what, hooks, compiled, times,
                              level=again)
                return None
            if r.timed_out:
                return self._down(job, argv, info, step, r.seconds, spent, [f"the compiler crashed ({cause})"], hooks,
                                  compiled, times)
            if not crashed(r.code, r.out):
                hooks.finished(job, spent)
                return "fail", (src, r.out)
            code, out = r.code, r.out
        hooks.finished(job, spent)
        n = 1 + len(retries)
        self.report.add(label, f"the compiler crashed {n} times ({cause})", None, spent)
        return "fail", (src, f"the compiler ({self.compiler}) crashed on this file {n} times. The same file compiles on other "
                             "PCs with the same compiler, so this PC most likely ran out of memory or is not quite stable "
                             "(overclocked, or overheating). Close other programs and run the build again: it carries on where "
                             "it stopped.\n" + crash_report(out))

    def _down(self, job, argv, info, step, over, spent, what, hooks, compiled, times):
        """A file whose compile (the step's way) ran over its time limit (after `over` seconds), down the
        ladder (timeout_steps)."""
        src, label = job[0], self._label(job, info)
        sizes = function_sizes(info.c) if os.path.isfile(info.c) else []
        what = what + [f"over its time limit ({duration(over)}) at {step.level}"]
        error = None
        for nxt in timeout_steps(step, sizes):
            self.log(f"{label} is still compiling after {duration(over)}, its time limit: stopped, compiling it again "
                     + nxt.describe())
            hooks.stepped(job, self._cost(info, nxt))
            try:
                r = self._attempt(job, argv, nxt, info)
            except Stopped:
                return None
            except NoHeader as e:   # (the ladder goes on to the next, /Od needing none)
                self.log(f"{label}: {e}")
                what.append(f"{nxt.describe()}: {e}")
                continue
            spent += r.seconds
            if not r.code and not r.timed_out:
                self.log(f"{label} compiled {nxt.describe()} in {duration(r.seconds)} ({duration(spent)} in all)")
                self._settled(job, info, nxt, spent, "; ".join(what + ["compiled " + nxt.describe()]), hooks, compiled,
                              times, level=nxt.level)
                return None
            if r.timed_out:
                over = r.seconds
                what.append(f"{nxt.describe()}: over its time limit again ({duration(over)})")
            elif crashed(r.code, r.out):
                what.append(f"{nxt.describe()}: the compiler crashed ({crash_cause(r.code, r.out)})")
            else:
                what.append(f"{nxt.describe()}: an error")
                error = r.out
        hooks.finished(job, spent)
        self.report.add(label, "; ".join(what), None, spent)
        self.log(f"{label} did not compile: " + "; ".join(what))
        return "fail", (src, error or (
            f"{label} did not finish compiling within its time limit, unoptimised either: " + "; ".join(what) + ". Run the "
            f"build again (it carries on where it stopped), with {LIMIT_ENV}=<seconds> to give each file that long "
            "(0: no limit)."))

    def _settled(self, job, info, step, seconds, how, hooks, compiled, times, level=None):
        hooks.finished(job, seconds)
        if compiled:
            if step is None:
                compiled(job, level or job[2])
            else:
                compiled(job, step.level, step)
        if info is not None and times is not None:
            times.put(info.name, seconds, job[2], info.model, info.source, how)
        if how:
            self.report.add(self._label(job, info), how, level or job[2], seconds)
