"""Progress bars with the time left, for the long jobs: the build (tools/recomp.py all, through
Build and Line) and Prepare Shaders (tools/shader_aot.py)."""
import heapq
import os
import shutil
import sys
import threading
import time

# Started without a console - by the Mods app's pythonw.exe on Windows - a program has no
# sys.stdout or sys.stderr (None), and the first progress line stopped the game's build: what it
# prints goes nowhere instead.
if sys.stdout is None:
    sys.stdout = open(os.devnull, "w", encoding="utf-8")
if sys.stderr is None:
    sys.stderr = open(os.devnull, "w", encoding="utf-8")


def time_left(seconds):
    """A time left, as a person says it."""
    minutes = round(seconds / 60)
    if minutes < 1:
        return "under a minute left"
    if minutes < 60:
        return f"about {minutes} min left"
    return f"about {minutes // 60} h {minutes % 60:02d} min left"


def bar(frac, width=30):
    filled = int(min(1.0, max(0.0, frac)) * width)
    return f"[{'#' * filled}{'-' * (width - filled)}]"


def left_text(frac, left):
    """The time left (None: not known yet), or done."""
    if frac >= 1.0:
        return "done"
    return time_left(left) if left is not None else "estimating the time left"


def line_text(frac, left, label=""):
    """The bar, the share done (whole percents, rounded down: 100 % is all of it), the time left
    (None: not known yet) and what is being done."""
    if frac >= 1.0:
        return f"{bar(1.0)} 100%  done"
    return f"{bar(frac)} {int(frac * 100):3d}%  {left_text(frac, left)}" + (f"  ({label})" if label else "")


# ---- what the build costs -------------------------------------------------------------------------
# A generated file's compile, in seconds of one compiler process on a fast PC (a Core Ultra 9 285K
# compiling 6 to 16 files at a time): clang's time grows with the file's size, and with the square
# of its biggest function - a few fighter modules have a function of 100,000 to 210,000
# instructions, and each takes 1.5 to 10 minutes on its own. (Measured on 13.0.5's modules: main's
# files together within a few %; those biggest functions can take up to half as long again.)
PER_INSN = 9e-5
PER_BIGGEST_SQUARED = 1.2e-8
TABLE = 1.0                     # g_module.c, the module's tables
GEN = (0.5, 5e-7)               # writing a module's C: seconds, plus per instruction
LINK = 25.0                     # compiling the runtime, and the link
PRIOR = (40000, 6000)           # a file of a module whose C is still to be written: instructions, biggest function
MEASURED = 60.0                 # (estimated) seconds of compiling to go by before the time left is given
MEASURED_GEN = 10.0             # of writing C, when that is all there is to do (recomp.py all --generate-only)
WINDOW = 600                    # the time left goes by the pace of the last 10 minutes' compiles
BIG = 60                        # a file compiling this many seconds is said to be a big one (the line would look stuck)


def file_cost(insns, biggest):
    return PER_INSN * insns + PER_BIGGEST_SQUARED * biggest * biggest


def level_cost(insns, biggest, level, cxx=False):
    """file_cost at a level (tools/ladder.py's): /Od, whose time does not grow with the square, as -O0's
    below; a biggest function left optimised when the longer ones are not."""
    return PER_INSN * insns if level == "/Od" else file_cost(insns, biggest)


# The same with clang for Linux (tools/linux/build_linux.py; the macOS build's ladder goes by it too),
# measured on 13.0.5's main and lua2cpp_master compiled for Linux: the square counts half as much as
# with clang-cl, a file compiled as C++ takes a third longer, and -O0 (giant functions full of landing
# sites) has no square at all.
CLANG_PER_INSN, CLANG_PER_BIGGEST_SQUARED, CLANG_CXX, CLANG_PER_INSN_O0 = 1.05e-4, 6e-9, 1.35, 9e-5


def clang_cost(insns, biggest, level, cxx=False):
    if level == "-O0":
        return CLANG_PER_INSN_O0 * insns
    return (CLANG_PER_INSN * insns + CLANG_PER_BIGGEST_SQUARED * biggest * biggest) * (CLANG_CXX if cxx else 1.0)


def makespan(costs, loads):
    """When `costs`, handed out in order to whichever worker is free first, are all done; the
    workers (one entry each) are busy for `loads` first."""
    heap = list(loads) or [0.0]
    heapq.heapify(heap)
    for c in costs:
        heapq.heappush(heap, heapq.heappop(heap) + c)
    return max(heap)


class Build:
    """How far tools/recomp.py all has got, and how long the rest takes. Every module's C is
    written first, then each module is compiled, then everything is linked. Each of those steps
    has an estimate in seconds as a fast PC takes it (above; a module's compile as `jobs` compilers
    working through its files in order, as cc.compile_each does), which says the share done; the
    time left is the rest of the estimate at this PC's pace: how long its compiles have taken
    against their estimates (when there is no compiling to do, compile=False, how long writing
    the C has taken). Until there are some of those, the time left is not known. The Linux build
    (tools/linux/build_linux.py) uses it for its compile alone: its "modules" are the runtime and
    the game, and its link's estimate (link=None: no link).

    Its methods are called as the build goes (started and finished from the compiler threads);
    status() says where it is, for a Line."""

    def __init__(self, jobs, compile=True, link=LINK, clock=time.time):
        self.jobs = jobs
        self.compiling = compile
        self.clock = clock
        self.lock = threading.Lock()
        self.names = []            # the modules, in build order
        self.files = {}            # module -> [(source, estimate)]: its compile's files, once known
        self.cost = {("link", None): link} if link else {}   # step -> estimate; steps ("gen" | "compile", module), ("link", None)
        self.done = set()
        self.step = None           # the step under way, and since when
        self.since = 0.0
        self.queue = {}            # (compile) source -> estimate, of the files not started, in order
        self.running = {}          # (compile) source -> (estimate, started at)
        self.compiled = 0          # (compile) files finished
        self.samples = []          # (when, seconds, estimate) of every file compiled
        self.unpaced = set()       # (compile) files the ladder compiles again another way: no sample of the pace
        self.gen_samples = []      # the same of every module whose C was written
        self.shown = 0.0
        self.label = ""

    def module(self, name, insns, files=None):
        """A module to build, of `insns` instructions; `files` [(source, estimate)] if its C is
        there already (then only those are compiled), else None."""
        with self.lock:
            self.names.append(name)
            if files is None:   # (how much compiling it is, is a guess until its C is written)
                self.cost[("gen", name)] = GEN[0] + GEN[1] * insns
                self.cost[("compile", name)] = makespan([file_cost(*PRIOR)] * max(1, -(-insns // PRIOR[0])),
                                                        [0.0] * self.jobs) if self.compiling else 0.0
            else:
                self.cost[("gen", name)] = 0.0
                self._set_files(name, files)

    def _set_files(self, name, files):
        self.files[name] = list(files)
        self.cost[("compile", name)] = makespan([c for _s, c in files], [0.0] * self.jobs) if self.compiling else 0.0

    def written(self, name, files):
        """The module's C is there: compiling it is these files."""
        with self.lock:
            self._set_files(name, files)

    def begin(self, kind, name=None, files=None):
        with self.lock:
            if files is not None:
                self._set_files(name, files)
            self.step, self.since = (kind, name), self.clock()
            if kind == "compile":
                self.queue, self.running, self.compiled, self.unpaced = dict(self.files[name]), {}, 0, set()
            self.label = self._label()

    def started(self, job):
        with self.lock:
            self.running[job[0]] = (self.queue.pop(job[0], 0.0), self.clock())

    def finished(self, job, seconds):
        with self.lock:
            cost, _t = self.running.pop(job[0], (0.0, 0.0))
            self.compiled += 1
            if job[0] in self.unpaced:
                self.unpaced.discard(job[0])
            else:
                self.samples.append((self.clock(), seconds, cost))
            self.label = self._label()

    def stepped(self, job, estimate):
        """The ladder stopped a file over its time limit and compiles it again another way (tools/ladder.py):
        what is left of it is that try's estimate, from now. Its time says nothing of this PC's pace (its
        estimate was not what it took), so it is not a sample of it."""
        with self.lock:
            if job[0] in self.running:
                self.running[job[0]] = (estimate, self.clock())
            self.unpaced.add(job[0])

    def requeued(self, job, estimate):
        """The compiler crashed on a file: compiled again once the others are done (started again then)."""
        with self.lock:
            self.running.pop(job[0], None)
            self.queue[job[0]] = estimate
            self.unpaced.add(job[0])

    def end(self, kind, name=None):
        with self.lock:
            if kind == "gen" and self.cost[("gen", name)]:
                self.gen_samples.append((self.clock(), self.clock() - self.since, self.cost[("gen", name)]))
            self.done.add((kind, name))
            self.step = None

    def _label(self):
        kind, name = self.step
        if kind == "gen":
            return f"writing the C code: {name}, {self.names.index(name) + 1} of {len(self.names)} modules"
        if kind == "compile":
            return f"compiling {name}: {self.compiled:,} of {len(self.files[name]):,} files"
        return "linking"

    def pace(self, now):
        """Seconds this PC takes for an estimated second of compiling (or of writing C, when there
        is no compiling to do), or None (not enough yet)."""
        samples, enough = (self.samples, MEASURED) if self.compiling else (self.gen_samples, MEASURED_GEN)
        recent = [(s, c) for t, s, c in samples if t >= now - WINDOW]
        if sum(c for _s, c in recent) < enough:
            recent = [(s, c) for _t, s, c in samples]
        est = sum(c for _s, c in recent)
        return sum(s for s, _c in recent) / est if est >= enough else None

    def status(self, now=None):
        """(share done, seconds left or None, what is being done)."""
        with self.lock:
            now = self.clock() if now is None else now
            pace = self.pace(now)
            rest = sum(c for step, c in self.cost.items() if step not in self.done and step != self.step)
            label = self.label
            if self.step:
                # a step under way: what is left of it, counting the time it has had at this pace
                # (one past 80 % of its estimate is taken to need a quarter of its time so far again)
                left_of = lambda cost, t: max(cost - (now - t) / (pace or 1.0), (now - t) / (pace or 1.0) / 4)
                if self.step[0] == "compile":
                    loads = [left_of(c, t) for c, t in self.running.values()]
                    rest += makespan(list(self.queue.values()), loads + [0.0] * (self.jobs - len(loads)))
                    if any(now - t >= BIG for _c, t in self.running.values()):
                        label += " - a big one takes minutes"
                else:
                    rest += left_of(self.cost[self.step], self.since)
            total = sum(self.cost.values())
            self.shown = max(self.shown, 1.0 - rest / total if total else 1.0)   # (never backwards)
            return self.shown, rest * pace if pace else None, label


# ---- showing it -----------------------------------------------------------------------------------
# The window title: on Windows the console's own; elsewhere the terminal's, set by the escape
# sequence most terminals know, written to the terminal itself (so it works while the output goes
# through a pipe, as the Linux installer's does through tee). A terminal's title can't be read
# back: it is kept on the terminal's title stack instead (pushed, then popped at the end).
def save_title():
    """What restore_title needs to put the title back, or None if there is no window title to set."""
    if sys.platform == "win32":
        import ctypes
        kernel32 = ctypes.windll.kernel32
        if not kernel32.GetConsoleWindow():
            return None
        buf = ctypes.create_unicode_buffer(1024)
        kernel32.GetConsoleTitleW(buf, len(buf))
        return buf.value
    if os.environ.get("TERM", "dumb") == "dumb":
        return None
    try:
        tty = open("/dev/tty", "w")
        tty.write("\033[22;0t")   # (push the title)
        tty.flush()
    except OSError:
        return None
    return tty


def set_title(saved, text):
    try:
        if sys.platform == "win32":
            import ctypes
            ctypes.windll.kernel32.SetConsoleTitleW(text)
        else:
            saved.write(f"\033]0;{text}\007")
            saved.flush()
    except OSError:
        pass


def restore_title(saved):
    try:
        if sys.platform == "win32":
            set_title(saved, saved)
        else:
            saved.write("\033[23;0t")   # (pop it)
            saved.close()
    except OSError:
        pass


class Line:
    """A progress bar under what a program prints, from status() -> (share done, seconds left or
    None, what is being done), shown every second while the `with` block runs.

    In a console it is the last line, drawn over itself, and what is printed meanwhile (to
    sys.stdout or sys.stderr) goes above it. Otherwise - a log, or a window that reads the output
    through a pipe, as the installer's does - it is written as a line of its own when the share
    done has gone up a whole percent and `every` seconds have passed, and at the end. With a
    `title`, the console window's title (the taskbar's button) shows the share done and the time
    left too, either way; the old title comes back at the end."""

    def __init__(self, status, out=None, console=None, title=None, every=15.0, clock=time.time):
        self.status = status
        self.out = out or sys.stdout
        self.console = self.out.isatty() if console is None else console
        self.title = title
        self.every = every
        self.clock = clock
        self.lock = threading.RLock()
        self.text = None
        self.drawn = 0             # (console) how much of the bar is on the screen
        self.line_start = True     # (console) the program's own output has ended its line
        self.logged = None         # (log) (when, percent, label) of the last line written
        self.old_title = None
        self.shown_title = None
        self.saved = None
        self.stop = threading.Event()
        self.thread = None

    def show(self, frac, left, label, final=False):
        text = line_text(frac, left, label)
        percent = int(frac * 100)
        with self.lock:
            now = self.clock()
            if self.console:
                if text != self.text or not self.drawn:
                    self.text = text
                    self._draw()
            elif self.logged is None or (final and text != self.text) or (
                    (percent != self.logged[1] or label != self.logged[2]) and now - self.logged[0] >= 0.4):
                # A pipe (the Mods window, the launcher) has no redrawing line, so each change of
                # the step or the percent is its own line, often enough that a bar can follow it.
                self.out.write(text + "\n")
                self.out.flush()
                self.text, self.logged = text, (now, percent, label)
            if self.title and self.old_title is not None:
                title = f"{percent}% - {left_text(frac, left)} - {self.title}"
                if title != self.shown_title:
                    set_title(self.old_title, title)
                    self.shown_title = title

    def _draw(self):
        """The bar over the line it is on (or on a new line, after the program's output)."""
        if self.text and self.line_start:
            cols = shutil.get_terminal_size((120, 25)).columns - 1   # (a longer line would wrap)
            text = self.text[:cols].ljust(min(self.drawn, cols))    # (covering a longer one before it)
            self.out.write("\r" + text)
            self.out.flush()
            self.drawn = len(text)

    def _erase(self):
        if self.drawn:
            self.out.write("\r" + " " * self.drawn + "\r")
            self.out.flush()
            self.drawn = 0

    def write_through(self, real, s):
        """What the program prints while the bar is up: above it."""
        with self.lock:
            if self.console and s:
                self._erase()
            real.write(s)
            if self.console and s:
                self.line_start = s.endswith("\n")
                real.flush()
                self._draw()
            return len(s)

    def _tick(self):
        while not self.stop.wait(1.0):
            self._update()

    def _update(self, final=False):
        try:
            self.show(*self.status(), final=final)
        except Exception as e:   # (the bar is not worth stopping the job for)
            why = f"no progress shown: {e!r}"
            self.status = lambda: (0.0, None, why)

    def __enter__(self):
        self.saved = sys.stdout, sys.stderr
        sys.stdout, sys.stderr = _Through(self, sys.stdout), _Through(self, sys.stderr)
        if self.title:
            self.old_title = save_title()
        self._update()
        self.thread = threading.Thread(target=self._tick, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join()
        self._update(final=True)
        with self.lock:
            if self.drawn:   # (the bar stays, as it ended)
                self.out.write("\n")
                self.out.flush()
                self.drawn = 0
            sys.stdout, sys.stderr = self.saved
        if self.old_title is not None:
            restore_title(self.old_title)
        return False


class _Through:
    """sys.stdout or sys.stderr while a Line is up."""

    def __init__(self, line, real):
        self._line, self._real = line, real

    def write(self, s):
        return self._line.write_through(self._real, s)

    def flush(self):
        self._real.flush()

    def __getattr__(self, name):
        return getattr(self._real, name)
