"""Objects compiled again when what they are built from changes (tools/objcache.py): the C, the
runtime headers it includes, the compile command, the compiler; not when a file is only saved again.
Then the Windows build (tools/recomp.py with tools/cc.py: a module's objects, the runtime's, a crash
compiled again at /O1) and the Linux one (tools/linux/build_linux.py) with it. Made-up sources, and
a made-up compiler: a Python script that writes an object of a line for each file."""
from collections import Counter
from pathlib import Path
import contextlib
import importlib.util
import io
import json
import os
import runpy
import subprocess
import sys
import tempfile
import time
import unittest
import warnings
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402
import isa  # noqa: E402
import objcache  # noqa: E402
import recomp  # noqa: E402

REAL_RUN, REAL_POPEN = subprocess.run, subprocess.Popen
# The made-up compiler. --version prints version.txt beside it. Otherwise it compiles the one source
# it is given (.c or .cpp; a C++ wrapper's text is that of the C it includes too) into the object
# named by -o or /Fo, as a line naming the source and the rest of the command; a source with "#error"
# is an error in the code, one with "crash at /O2" makes it crash at /O2 as clang-cl does (and at any
# other level so named), "crash like clang at -O2" as clang's driver says its compiler crashed,
# "crash by signal at -O2" by a signal (not on Windows) once it has written half an object and
# clang's temporary file, one with "half-written" leaves half an object and fails. "slow unless
# optnone on f_<address>" runs (a minute) unless the optnone header given before the file (-include,
# /FI: tools/ladder.py) names that function, "slow unless O0" unless it is compiled at -O0 or /Od,
# "slow" always: first it leaves half an object and clang's temporary file, and starts a process of
# its own, which sleeps too (its id in grandchildren.txt). "takes 3 s" compiles in that many seconds.
# Each run leaves a file of its own in runs/ beside it: "<source> <level>", and one with the whole
# command and when (commands()). (One shared file would lose lines when two compiles of a parallel
# build append at once: on Windows, appends aren't atomic.)
FAKE_CC = r'''
import json, os, re, signal, subprocess, sys, tempfile, time
here = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
if args == ["--version"]:
    with open(os.path.join(here, "version.txt")) as f:
        print(f.read().strip())
    sys.exit(0)
src = next(a for a in args if a.endswith((".c", ".cpp")))
obj = args[args.index("-o") + 1] if "-o" in args else next(a[3:] for a in args if a.startswith("/Fo"))
level = next(a for a in args if a in ("/O1", "/O2", "/Od", "-O0", "-O1", "-O2"))
header = args[args.index("-include") + 1] if "-include" in args else next((a[3:] for a in args if a.startswith("/FI")), None)
os.makedirs(os.path.join(here, "runs"), exist_ok=True)
fd, run = tempfile.mkstemp(dir=os.path.join(here, "runs"), suffix=".txt")
with os.fdopen(fd, "w") as f:
    f.write(os.path.basename(src) + " " + level + "\n")
with open(run[:-4] + ".command.json", "w") as f:
    json.dump({"when": time.time(), "args": args}, f)
with open(src) as f:
    text = f.read()
if src.endswith("_cxx.cpp"):
    for included in re.findall(r'#include "([^"]+[.]c)"', text):
        with open(included) as f:
            text += f.read()
optnone = open(header).read() if header else ""
if "#error" in text:
    sys.stderr.write(src + "(1,2): error: a made-up error\n1 error generated.\n")
    sys.exit(1)
if "crash at " + level in text:
    sys.stderr.write("PLEASE submit a bug report\nStack dump:\nException Code: 0xC0000005\n")
    sys.exit(1)
if "crash like clang at " + level in text:
    sys.stderr.write("clang: error: unable to execute command: Segmentation fault: 11\n"
                     "clang: error: clang frontend command failed due to signal (use -v to see invocation)\n")
    sys.exit(254)
def partial():
    stem, ext = os.path.splitext(obj)
    for path in (obj, stem + "-1a2b3c4d" + ext + ".tmp"):
        with open(path, "w") as f:
            f.write("half")
if "crash by signal at " + level in text:
    partial()
    os.kill(os.getpid(), signal.SIGSEGV)
takes = re.search(r"takes ([0-9.]+) s", text)
if takes:
    time.sleep(float(takes.group(1)))
wanted = re.search(r"slow unless optnone on (f_[0-9a-f]+)", text)
if (wanted and wanted.group(1) + "(Cpu *c) __attribute__((optnone, noinline))" not in optnone) or \
        ("slow unless O0" in text and level not in ("-O0", "/Od")) or re.search(r"\bslow\b(?! unless)", text):
    partial()
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    with open(os.path.join(here, "grandchildren.txt"), "a") as f:
        f.write(f"{child.pid}\n")
    time.sleep(60)
with open(obj, "w") as f:
    f.write("half" if "half-written" in text else "object of " + os.path.basename(src) + ": "
            + " ".join(a for a in args if a not in (src, obj, "/Fo" + obj)) + "\n")
sys.exit(1 if "half-written" in text else 0)
'''


def write(path, text, age=3600):
    """A file with this text, dated `age` seconds ago (older than objcache.RECENT_NS: its digest is kept)."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, newline='\n')
    t = time.time() - age
    os.utime(path, (t, t))
    return path


def touch(path):
    """Saved again unchanged: the same content with a time of now."""
    os.utime(path, None)


class FakeCompiler:
    """The made-up compiler in a folder of its own. subprocess.run runs it for a command that names
    `name` as its program (patch()): cc.py's, objcache.compiler_id's and the Linux toolchain's."""
    def __init__(self, folder, name='fakeclang'):
        self.folder, self.name = Path(folder), name
        self.script = write(self.folder / 'fakecc.py', FAKE_CC)
        self.version('fake clang version 19.1.0')

    def version(self, text):
        write(self.folder / 'version.txt', text + '\n')
        objcache.compiler_id.cache_clear()   # (a new build: the compiler is asked again)

    def runs(self):
        """The compiles since the last call, sorted: ["<source> <level>", ...]."""
        lines = []
        for path in sorted((self.folder / 'runs').glob('*.txt')):
            lines += path.read_text().splitlines()
            path.unlink()
        return sorted(lines)

    def commands(self):
        """The whole commands it was given since the last call, in the order they ran (each one after
        its program's name)."""
        found = []
        for path in (self.folder / 'runs').glob('*.command.json'):
            found.append(json.loads(path.read_text()))
            path.unlink()
        return [c['args'] for c in sorted(found, key=lambda c: c['when'])]

    def grandchildren(self):
        """The processes the slow compiles started, by their ids."""
        path = self.folder / 'grandchildren.txt'
        return [int(n) for n in path.read_text().split()] if path.exists() else []

    def command(self, args):
        """What runs for a command: the made-up compiler for one that names it (on Linux, toolchain.run puts
        "nice -n 10" before it)."""
        args = list(args)
        if self.name in args:
            args = [sys.executable, str(self.script), *args[args.index(self.name) + 1:]]
        return args

    def patch(self):
        """subprocess.run and subprocess.Popen (the compile ladder's run: tools/ladder.py) running it."""
        return mock.patch.multiple(subprocess, run=lambda args, **kw: REAL_RUN(self.command(args), **kw),
                                   Popen=lambda args, *a, **kw: REAL_POPEN(self.command(args), *a, **kw))


class Folder(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='objcache-test-')
        self.dir = Path(self.tmp.name)
        self.cc = FakeCompiler(self.dir / 'cc')
        self.runtime = self.dir / 'runtime'
        write(self.runtime / 'recomp.h', '#pragma once\n#include <stdint.h>\n#include "lanes.h"\ntypedef int u32;\n')
        write(self.runtime / 'lanes.h', '#pragma once\ntypedef struct { u32 s[4]; } V128;\n')
        write(self.runtime / 'module.h', '#pragma once\n#include "recomp.h"\ntypedef struct RtModule RtModule;\n')

    def tearDown(self):
        objcache.compiler_id.cache_clear()
        self.tmp.cleanup()


# ---- the record ------------------------------------------------------------------------------
class RecordTests(Folder):
    """One object, a.o from a.c, by objcache alone: the build's look at it, and its compile when stale."""
    def setUp(self):
        super().setUp()
        self.src = write(self.dir / 'src' / 'a.c', '#include "recomp.h"\nvoid f_7100000000(void) {}\n')
        self.out = self.dir / 'out'
        self.obj = self.out / 'a.o'
        self.log = []

    def build(self, flags=('-mavx2', '-mfma'), level='-O2'):
        """Why a.o was compiled (None: it was current)."""
        self.out.mkdir(exist_ok=True)
        record = objcache.Record(str(self.out), log=self.log.append)
        command = [sys.executable, str(self.cc.script), '-c', level, *flags, str(self.src), '-o', str(self.obj)]
        headers = objcache.digest_files(objcache.header_files(str(self.runtime), 'recomp.h'))
        key = record.entry([str(self.src)], command, objcache.compiler_id((sys.executable, str(self.cc.script))), headers)
        why = record.check(str(self.obj), key)
        record.save()
        if why:   # (as the builds do: the old object and its key gone first)
            record.forget([str(self.obj)])
            if self.obj.exists():
                self.obj.unlink()
            if REAL_RUN(command, capture_output=True).returncode == 0:
                record.compiled(str(self.obj), key)
        return why

    def recorded(self):
        return json.loads((self.out / objcache.RECORD).read_text())

    def test_first_build_then_current(self):
        self.assertEqual(self.build(), objcache.NEW)
        self.assertIsNone(self.build())
        self.assertIn('a.o', self.recorded()['objects'])

    def test_source_change(self):
        self.build()
        write(self.src, '#include "recomp.h"\nvoid f_7100000000(void) { return; }\n')
        self.assertEqual(self.build(), objcache.CHANGED)
        self.assertIsNone(self.build())
        write(self.src, '#include "recomp.h"\nvoid f_7100000004(void) { return; }\n', age=1800)   # (the same size)
        self.assertEqual(self.build(), objcache.CHANGED)
        self.assertIsNone(self.build())
        write(self.src, '#include "recomp.h"\nvoid f_7100000008(void) { return; }\n')
        self.assertEqual(self.build(level='-O1'), objcache.CHANGED)   # (a level that comes with the change: not the command)

    def test_object_gone(self):
        self.build()
        self.obj.unlink()
        self.assertEqual(self.build(), objcache.NEW)
        record = objcache.Record(str(self.out))
        record.compiled(str(self.out / 'b.o'), {'key': '0' * 64})   # (no such object: not recorded)
        self.assertNotIn('b.o', self.recorded()['objects'])

    def test_saved_again_unchanged_is_kept(self):
        self.build()
        touch(self.src)
        self.assertIsNone(self.build())
        touch(self.runtime / 'lanes.h')
        self.assertIsNone(self.build())

    def test_same_size_and_time_not_read_again(self):
        self.build()
        read = []
        real = objcache.file_sha
        with mock.patch.object(objcache, 'file_sha', side_effect=lambda p, scan=(): read.append(p) or real(p, scan)):
            self.assertIsNone(self.build())
        self.assertNotIn(os.path.abspath(self.src), read)   # (the headers are read each time: a few small files)

    def test_a_file_written_just_now_is_read_again(self):
        """A disk that keeps times to the second would give a file changed again in the same second the
        same time: the digest of a file written in the last few seconds is not kept."""
        self.build()
        write(self.src, '#include "recomp.h"\nint x;\n', age=0)
        self.assertEqual(self.build(), objcache.CHANGED)
        self.assertNotIn(os.path.abspath(self.src), self.recorded()['sources'])
        read = []
        real = objcache.file_sha
        with mock.patch.object(objcache, 'file_sha', side_effect=lambda p, scan=(): read.append(p) or real(p, scan)):
            self.assertIsNone(self.build())
        self.assertIn(os.path.abspath(self.src), read)

    def test_instruction_set_or_level_change(self):
        self.build(flags=('-mavx2', '-mfma'))
        self.assertEqual(self.build(flags=('-march=x86-64-v2',)), objcache.COMMAND)
        self.assertIsNone(self.build(flags=('-march=x86-64-v2',)))
        self.assertEqual(self.build(flags=('-march=x86-64-v2',), level='-O1'), objcache.COMMAND)

    def test_header_change(self):
        self.build()
        write(self.runtime / 'lanes.h', '#pragma once\ntypedef struct { u32 s[4]; } __attribute__((aligned(16))) V128;\n')
        self.assertEqual(self.build(), objcache.HEADERS)   # (lanes.h is included by recomp.h)
        self.assertIsNone(self.build())
        write(self.runtime / 'module.h', '#pragma once\n#include "recomp.h"\n')
        self.assertIsNone(self.build())   # (not included by a.c)

    def test_compiler_change(self):
        self.build()
        self.cc.version('fake clang version 19.1.0')   # (asked again, the same answer)
        self.assertIsNone(self.build())
        self.cc.version('fake clang version 20.1.8')
        self.assertEqual(self.build(), objcache.COMPILER)
        self.assertIsNone(self.build())

    def test_missing_record(self):
        self.build()
        (self.out / objcache.RECORD).unlink()
        self.assertEqual(self.build(), objcache.UNRECORDED)
        self.assertEqual(self.log, [])   # (said in the build's line: summary)
        self.assertIsNone(self.build())

    def test_unreadable_record_said_once(self):
        self.build()
        for garbage in ('{"version": 1, "objects": {"a.o": ', '[1, 2]', '{"version": 0, "objects": {}, "sources": {}}'):
            with self.subTest(garbage):
                self.log.clear()
                (self.out / objcache.RECORD).write_text(garbage)
                self.assertEqual(self.build(), 'objcache.json could not be read')
                self.assertEqual(len(self.log), 1)
                self.assertIn('could not be read', self.log[0])
                self.assertIsNone(self.build())
                self.assertEqual(len(self.log), 1)

    def test_a_record_held_a_moment_is_read(self):
        """On Windows another program (an antivirus) can hold the record a moment after it was written."""
        self.build()
        held, real = [], open

        def opened(path, *args, **kw):
            if path == str(self.out / objcache.RECORD) and len(held) < 2:
                held.append(path)
                raise PermissionError(13, 'held by another program', path)
            return real(path, *args, **kw)
        with mock.patch.object(objcache, 'open', opened, create=True), mock.patch.object(objcache.time, 'sleep'):
            self.assertIsNone(self.build())
        self.assertEqual(len(held), 2)
        self.assertEqual(self.log, [])

    def test_a_record_that_cannot_be_opened_stops_the_build(self):
        """Not taken for no record (every object compiled again, the record replaced): nothing is compiled or
        removed, and the record is left as it was."""
        self.build()
        before = (self.out / objcache.RECORD).read_text()

        def opened(path, *args, **kw):
            raise PermissionError(13, 'held by another program', path)
        with mock.patch.object(objcache, 'open', opened, create=True), mock.patch.object(objcache.time, 'sleep'), \
                self.assertRaises(SystemExit) as stop:
            self.build()
        self.assertIn('could not be opened', str(stop.exception))
        self.assertEqual((self.out / objcache.RECORD).read_text(), before)
        self.assertTrue(self.obj.exists())
        self.assertIsNone(self.build())

    def test_a_record_that_cannot_be_written(self):
        """After a build's look (save) or a compile (compiled): logged, and the build goes on (its sources are
        read and the object compiled again next time). Before a compile (forget): the build stops there, before
        an old object is removed, as its key could not be dropped."""
        self.build()
        before = (self.out / objcache.RECORD).read_text()
        record = objcache.Record(str(self.out), log=self.log.append)
        key = {'key': '1' * 64}
        with mock.patch.object(objcache.os, 'replace', side_effect=PermissionError(13, 'held')), \
                mock.patch.object(objcache.time, 'sleep'):
            record.save()
            record.compiled(str(self.obj), key)
            with self.assertRaises(PermissionError):
                record.forget([str(self.obj)])
        self.assertEqual(len(self.log), 2)
        self.assertTrue(all('could not write' in line for line in self.log), self.log)
        self.assertEqual((self.out / objcache.RECORD).read_text(), before)

    def test_a_failed_compile_records_nothing(self):
        self.build()
        write(self.src, '#error not this one\n')
        self.assertEqual(self.build(), objcache.CHANGED)
        self.assertFalse(self.obj.exists())
        self.assertNotIn('a.o', self.recorded()['objects'])
        self.assertEqual(self.build(), objcache.NEW)   # (tried again)
        write(self.src, 'half-written\n')              # a compile that fails leaving half an object
        self.assertEqual(self.build(), objcache.NEW)
        self.assertTrue(self.obj.exists())
        self.assertNotIn('a.o', self.recorded()['objects'])
        self.assertEqual(self.build(), objcache.UNRECORDED)   # (compiled again, not taken as current)

    def test_header_files(self):
        write(self.runtime / 'sub' / 'inner.h', '#pragma once\n')
        write(self.runtime / 'sub' / 'outer.h', '#include "inner.h"\n#include "missing.h"\n')
        write(self.runtime / 'lanes.h', '#include "sub/outer.h"\n  #  include "recomp.h"\n')
        names = lambda *h: sorted(os.path.relpath(p, self.runtime) for p in objcache.header_files(str(self.runtime), *h))
        self.assertEqual(names('recomp.h'), ['lanes.h', 'recomp.h', os.path.join('sub', 'inner.h'), os.path.join('sub', 'outer.h')])
        self.assertEqual(names('module.h'), sorted(names('recomp.h') + ['module.h']))

    def test_summary(self):
        self.assertIsNone(objcache.summary(Counter({objcache.NEW: 3, objcache.CHANGED: 2})))
        self.assertEqual(objcache.summary(Counter({objcache.COMMAND: 431, objcache.NEW: 2})),
                         '431 objects to compile again: the compile command changed')
        self.assertEqual(objcache.summary(Counter({objcache.HEADERS: 1})), '1 object to compile again: the runtime headers changed')
        self.assertEqual(objcache.summary(Counter({objcache.COMPILER: 3, objcache.UNRECORDED: 2, objcache.CHANGED: 9})),
                         '5 objects to compile again: no record of what they were built from (2), the compiler changed (3)')


# ---- the Windows build -----------------------------------------------------------------------
class WindowsBuildTests(Folder):
    """recomp.py compile_dir and link's runtime, with the made-up compiler as clang-cl."""
    def setUp(self):
        super().setUp()
        self.module = self.dir / 'build' / 'main'
        write(self.module / 'manifest.tsv', 'g0000.c\t40000\t5000\ng0001.c\t30000\t25000\n')
        write(self.module / 'g0000.c', '#include "recomp.h"\nvoid f_7100000000(void) {}\n')
        write(self.module / 'g0001.c', '#include "recomp.h"\nvoid f_7100001000(void) {}\n')
        write(self.module / 'g_module.c', '#include "module.h"\n')
        self.flags = ['/nologo', '/arch:AVX2', '-mfma', '/I', str(self.runtime)]   # cc.GAME_CFLAGS
        self.rt_flags = list(self.flags)   # cc.CFLAGS: the runtime's, with its libraries' defines and folders
        for patch in (mock.patch.object(cc, 'CLANG', self.cc.name), mock.patch.object(cc, 'GAME_CFLAGS', self.flags),
                      mock.patch.object(cc, 'ISA', 'avx2'),   # (what an older version's folder, without build/isa.txt, is for)
                      mock.patch.object(cc, 'CFLAGS', self.rt_flags), mock.patch.object(cc, 'RUNTIME', str(self.runtime)),
                      self.cc.patch()):
            patch.start()
            self.addCleanup(patch.stop)

    def compile(self, folder=None):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            ok = recomp.compile_dir(str(folder or self.module), 1)
        self.said = out.getvalue()
        return ok, self.cc.runs()

    def test_module(self):
        every = ['g0000.c /O2', 'g0001.c /O1', 'g_module.c /O1']
        self.assertEqual(self.compile(), (True, every))
        self.assertNotIn('to compile again', self.said)   # (a first build: nothing to explain)
        self.assertEqual(self.compile(), (True, []))
        touch(self.module / 'g0000.c')
        self.assertEqual(self.compile(), (True, []))
        write(self.module / 'g0000.c', '#include "recomp.h"\nvoid f_7100000000(void) { return; }\n')
        self.assertEqual(self.compile(), (True, ['g0000.c /O2']))
        self.assertNotIn('to compile again', self.said)
        write(self.runtime / 'lanes.h', '#pragma once\ntypedef struct { u32 s[4]; } V128; /* two */\n')
        self.assertEqual(self.compile(), (True, every))
        self.assertIn('main: 3 objects to compile again: the runtime headers changed', self.said)
        write(self.runtime / 'module.h', '#pragma once\n#include "recomp.h"\n')
        self.assertEqual(self.compile(), (True, ['g_module.c /O1']))
        self.flags[1] = '/arch:SSE4.2'   # (another instruction set: every object, never a mix of the two)
        self.assertEqual(self.compile(), (True, every))
        self.assertIn('main: 3 objects to compile again: the compile command changed', self.said)
        self.assertIn('/arch:SSE4.2', (self.module / 'g0000.obj').read_text())   # (compiled with the flags it is keyed on)
        self.rt_flags += ['/DSSBU_HAVE_SDL3', '/I', str(self.dir / 'SDL3' / 'include')]   # (the runtime's alone)
        self.assertEqual(self.compile(), (True, []))
        self.cc.version('fake clang version 20.1.8')
        self.assertEqual(self.compile(), (True, every))
        self.assertIn('main: 3 objects to compile again: the compiler changed', self.said)
        self.assertEqual(recomp.stale_files(str(self.module)), [])
        # a change to the C that puts the file at another level: said as a change to its C, not to the command
        write(self.module / 'manifest.tsv', 'g0000.c\t40000\t25000\ng0001.c\t30000\t25000\n')
        write(self.module / 'g0000.c', '#include "recomp.h"\nvoid f_7100000000(void) { /* bigger */ }\n')
        self.assertEqual(self.compile(), (True, ['g0000.c /O1']))
        self.assertNotIn('to compile again', self.said)

    def test_an_older_version_s_objects_are_compiled_again(self):
        """Every older version compiled the game with /arch:AVX2 -mfma -mf16c (isa.LEGACY_CLANG_CL, its cc.py's), which
        allows MOVBE and F16C: today's avx2 (-mno-movbe -mno-f16c) neither needs them nor has the program check for
        them. So a folder an older version built (no objcache.json, no build/isa.txt) is compiled again whole for
        avx2, its objects not taken as current by date, nor for x86-64-v2 (a fighter module's folder alike)."""
        every = ['g0000.c /O2', 'g0001.c /O1', 'g_module.c /O1']
        nro = self.module.parent / 'nro' / 'fighter'
        write(nro / 'manifest.tsv', 'g0000.c\t40000\t5000\n')
        write(nro / 'g0000.c', '#include "recomp.h"\nvoid f_7100002000(void) {}\n')
        write(nro / 'g_module.c', '#include "module.h"\n')
        for target in isa.TARGETS:
            with self.subTest(target=target), mock.patch.object(cc, 'ISA', target):
                self.flags[:] = ['/nologo', *isa.LEGACY_CLANG_CL, '/I', str(self.runtime)]   # (the older version's build)
                self.compile()
                self.compile(nro)
                for folder in (self.module, nro):
                    (folder / objcache.RECORD).unlink()
                self.assertIsNone(isa.recorded(str(self.module.parent)))
                self.flags[:] = ['/nologo', *isa.CLANG_CL[target], '/I', str(self.runtime)]   # (today's)
                self.assertEqual(self.compile(), (True, every))
                self.assertIn('main: 3 objects to compile again: no record of what they were built from', self.said)
                self.assertNotIn('taken as current', self.said)
                self.assertNotIn('-mf16c', (self.module / 'g0000.obj').read_text())
                self.assertIn(isa.CLANG_CL[target][-1], (self.module / 'g0000.obj').read_text())
                self.assertEqual(self.compile(nro), (True, ['g0000.c /O2', 'g_module.c /O1']))

    def test_a_folder_an_older_version_built_is_not_compiled_again(self):
        """No record at all (built before objcache.py): an object newer than its C and the runtime headers is taken
        as current, as the old rule (by date) took it, and recorded; an older one, or any after a header changed
        since, is compiled. A record that is there is never second-guessed by date. (Only for a build whose flags
        are the ones the older version compiled with, which no target's are today: made up here.)"""
        every = ['g0000.c /O2', 'g0001.c /O1', 'g_module.c /O1']
        patch = mock.patch.object(isa, 'LEGACY_CLANG_CL', isa.CLANG_CL['avx2'])
        patch.start()
        self.addCleanup(patch.stop)
        self.compile()
        record = self.module / objcache.RECORD
        record.unlink()
        self.assertEqual(self.compile(), (True, []))
        self.assertIn('3 objects an older version compiled taken as current', self.said)
        self.assertEqual(sorted(json.loads(record.read_text())['objects']), ['g0000.obj', 'g0001.obj', 'g_module.obj'])
        self.assertEqual(self.compile(), (True, []))
        self.assertNotIn('taken as current', self.said)
        record.unlink()
        old = time.time() - 7200   # (older than its C, written an hour ago)
        os.utime(self.module / 'g0000.obj', (old, old))
        self.assertEqual(self.compile(), (True, ['g0000.c /O2']))
        self.assertIn('2 objects an older version compiled taken as current', self.said)
        record.unlink()
        touch(self.runtime / 'lanes.h')   # (a header newer than the objects: the old rule can't vouch for them)
        self.assertEqual(self.compile(), (True, every))
        self.assertIn('main: 3 objects to compile again: no record of what they were built from', self.said)
        self.flags[1] = '/arch:SSE4.2'   # (with a record: by key alone)
        self.assertEqual(self.compile(), (True, every))

    def test_a_folder_an_older_version_built_for_another_instruction_set(self):
        """Taken as current by date takes both: the folder built for today's instruction set (build/isa.txt, else
        avx2: every older version's) and today's flags for it the older version's (here made up to be x86-64-v2's).
        Built today for x86-64-v2, an avx2 folder's objects are all compiled again; one build/isa.txt says is
        x86-64-v2's is adopted, and is not when the flags differ (a fighter module's folder, build/nro/<name>,
        goes by build/isa.txt too)."""
        every = ['g0000.c /O2', 'g0001.c /O1', 'g_module.c /O1']
        self.compile()
        record = self.module / objcache.RECORD
        record.unlink()
        self.flags[1:3] = ['/clang:-march=x86-64-v2']
        with mock.patch.object(cc, 'ISA', 'x86-64-v2'), mock.patch.object(isa, 'LEGACY_CLANG_CL', isa.CLANG_CL['x86-64-v2']):
            self.assertEqual(self.compile(), (True, every))
            self.assertIn('main: 3 objects to compile again: no record of what they were built from', self.said)
            self.assertIn('-march=x86-64-v2', (self.module / 'g0000.obj').read_text())
            record.unlink()
            isa.record(str(self.module.parent), 'x86-64-v2', True)   # (what link writes)
            with mock.patch.object(isa, 'LEGACY_CLANG_CL', ('/arch:SSE4.2',)):   # (not the flags they were compiled with)
                self.assertEqual(self.compile(), (True, every))
            record.unlink()
            self.assertEqual(self.compile(), (True, []))
            self.assertIn('3 objects an older version compiled taken as current', self.said)
            nro = self.module.parent / 'nro' / 'fighter'
            write(nro / 'manifest.tsv', 'g0000.c\t40000\t5000\n')
            write(nro / 'g0000.c', '#include "recomp.h"\nvoid f_7100002000(void) {}\n')
            write(nro / 'g_module.c', '#include "module.h"\n')
            self.assertEqual(self.compile(nro), (True, ['g0000.c /O2', 'g_module.c /O1']))
            (nro / objcache.RECORD).unlink()
            self.assertEqual(self.compile(nro), (True, []))
            isa.record(str(self.module.parent), 'avx2', False)
            (nro / objcache.RECORD).unlink()
            self.assertEqual(self.compile(nro), (True, ['g0000.c /O2', 'g_module.c /O1']))

    def test_hook_slots_are_keyed_on_hookable_h(self):
        """A file with hook slots includes hookable.h (tools/recomp.py): a change to it compiles that file
        again, and only that one; the files without slots keep their keys."""
        write(self.runtime / 'hookable.h', '#pragma once\n#include "recomp.h"\n')
        write(self.module / 'g0001.c', '#include "recomp.h"\n#include "hookable.h"\nvoid f_7100001000(void) {}\n')
        self.compile()
        write(self.runtime / 'hookable.h', '#pragma once\n#include "recomp.h"\n/* a slot more */\n')
        self.assertEqual(self.compile(), (True, ['g0001.c /O1']))
        self.assertIn('main: 1 object to compile again: the runtime headers changed', self.said)
        self.assertEqual(self.compile(), (True, []))

    def test_no_object_with_hook_slots_older_than_hookable_h_taken_as_current(self):
        """A folder with no record: the object of a file with hook slots is taken as current by date only when it
        is newer than hookable.h too. Such a file is keyed on it, so the key recorded for an object compiled before
        it changed would say it was compiled with the new one. The others go by their own headers alone. (Only for
        a build whose flags are the ones the older version compiled with, which no target's are today: made up
        here.)"""
        patch = mock.patch.object(isa, 'LEGACY_CLANG_CL', isa.CLANG_CL['avx2'])
        patch.start()
        self.addCleanup(patch.stop)
        write(self.runtime / 'hookable.h', '#pragma once\n#include "recomp.h"\n')
        write(self.module / 'g0001.c', '#include "recomp.h"\n#include "hookable.h"\nvoid f_7100001000(void) {}\n')
        self.compile()
        record = self.module / objcache.RECORD
        record.unlink()
        self.assertEqual(self.compile(), (True, []))   # (newer than hookable.h: taken as current)
        record.unlink()
        write(self.runtime / 'hookable.h', '#pragma once\n#include "recomp.h"\n/* a slot more */\n', age=-7200)
        self.assertEqual(self.compile(), (True, ['g0001.c /O1']))
        self.assertIn('main: 1 object to compile again: no record of what they were built from', self.said)
        self.assertIn('2 objects an older version compiled taken as current', self.said)
        self.assertEqual(self.compile(), (True, []))

    def test_the_folder_spelled_another_way_or_moved(self):
        """The key names a file and its object by their names in the folder: build/main, ./build/main, the
        full path (build\\main on Windows), or the build folder moved, compile nothing again."""
        with contextlib.chdir(self.dir):
            self.assertTrue(self.compile('build/main')[0])
            for spelling in ('./build/main', os.path.join('build', 'main'), str(self.module)):
                with self.subTest(spelling):
                    self.assertEqual(self.compile(spelling), (True, []))
        os.rename(self.dir / 'build', self.dir / 'moved')
        self.assertEqual(self.compile(self.dir / 'moved' / 'main'), (True, []))

    def test_the_game_is_compiled_without_the_runtime_s_libraries(self):
        """SDL3, libopus and libvpx are the runtime's. Their defines and include folders come and go with the
        libraries; they are not in the generated C's command (cc.GAME_CFLAGS), so they compile only the runtime again."""
        lib = write(self.dir / 'opus.lib', '')
        with mock.patch.dict(os.environ, {'SSBU_OPUS_LIBS': str(lib)}):
            spec = importlib.util.spec_from_file_location('cc_under_test', ROOT / 'tools' / 'cc.py')
            fresh = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(fresh)
        self.assertIn('/DSSBU_HAVE_OPUS', fresh.CFLAGS)
        self.assertEqual(fresh.GAME_CFLAGS, [*fresh.BASE_CFLAGS, '/I', fresh.RUNTIME])
        self.assertNotIn('/DSSBU_HAVE_OPUS', fresh.compile_command('g0000.c', 'g0000.obj', '/O2', fresh.GAME_CFLAGS))

    def test_a_crash_compiled_at_O1_is_kept(self):
        """A file the compiler crashed on at /O2 and compiled at /O1 (cc.RETRY) is recorded under its own
        level's key: not compiled again at every build, but compiled again when its flags change."""
        write(self.module / 'g0000.c', '/* crash at /O2 */\n#include "recomp.h"\n')
        crashed = ['g0000.c /O1', 'g0000.c /O2', 'g0000.c /O2']
        self.assertEqual(self.compile(), (True, sorted(crashed + ['g0001.c /O1', 'g_module.c /O1'])))
        self.assertIn('g0000.c compiled (at /O1', self.said)
        self.assertEqual(self.compile(), (True, []))
        self.flags[1] = '/arch:SSE4.2'
        self.assertEqual(self.compile(), (True, sorted(crashed + ['g0001.c /O1', 'g_module.c /O1'])))
        self.assertEqual(self.compile(), (True, []))

    def test_a_failed_or_interrupted_compile_records_nothing(self):
        self.compile()
        write(self.module / 'g0001.c', '#error made up\n')
        ok, runs = self.compile()
        self.assertFalse(ok)
        self.assertIn('FAILED g0001.c', self.said)
        self.assertFalse((self.module / 'g0001.obj').exists())
        self.assertEqual(self.compile()[1], ['g0001.c /O1'])   # (tried again)
        # stopped during g0001.c's compile (Ctrl+C): the objects compiled are kept, g0001's is not
        write(self.module / 'g0000.c', '#include "recomp.h"\nint changed;\n')
        write(self.module / 'g0001.c', '#include "recomp.h"\nint changed;\n')
        popen = subprocess.Popen

        def interrupted(args, *a, **kw):
            if any(str(a).endswith('g0001.c') for a in args):
                raise KeyboardInterrupt
            return popen(args, *a, **kw)
        with mock.patch.object(subprocess, 'Popen', interrupted), self.assertRaises(KeyboardInterrupt), \
                contextlib.redirect_stdout(io.StringIO()):
            recomp.compile_dir(str(self.module), 1)
        self.assertEqual(self.cc.runs(), ['g0000.c /O2'])
        recorded = json.loads((self.module / objcache.RECORD).read_text())['objects']
        self.assertIn('g0000.obj', recorded)
        self.assertNotIn('g0001.obj', recorded)
        self.assertEqual(self.compile(), (True, ['g0001.c /O1']))

    def test_runtime(self):
        """link's runtime objects: compiled again for a header, the flags (the stamp cflags.txt was) or the compiler."""
        write(self.runtime / 'host.h', '#pragma once\n#include "module.h"\n')
        write(self.runtime / 'host.c', '#include "host.h"\nint host;\n')
        write(self.runtime / 'kernel.c', '#include "host.h"\nint kernel;\n')
        write(self.runtime / 'cpu_check.c', '#include "cpu_check.h"\n')
        rt, exe = self.dir / 'build' / 'rt', self.dir / 'build' / 'ssbu.exe'
        check_flags = ['/nologo', '/I', str(self.runtime)]   # cc.CHECK_CFLAGS: for any x64 processor
        linked = []

        def link():
            out = io.StringIO()
            with mock.patch.object(cc, 'link', lambda objs, exe, options=(): linked.append(objs) or Path(exe).write_bytes(b'MZ')), \
                    mock.patch.object(cc, 'SDL3_DLL', None), mock.patch.object(cc, 'CHECK_CFLAGS', check_flags), \
                    contextlib.redirect_stdout(out):
                ok = recomp.link([], str(exe), str(rt))
            self.said = out.getvalue()
            return ok, self.cc.runs()
        every = ['cpu_check.c /O2', 'hle_table.c /O1', 'host.c /O2', 'kernel.c /O2', 'nro_table.c /O1']
        self.assertEqual(link(), (True, every))
        self.assertIn(str(rt / 'cpu_check.obj'), linked[-1])
        self.assertNotIn('/arch:AVX2', (rt / 'cpu_check.obj').read_text())   # (its own flags, not the runtime's)
        self.assertIn('/arch:AVX2', (rt / 'host.obj').read_text())
        self.assertEqual(isa.recorded(str(exe.parent)), (cc.ISA, cc.ISA_TOLD))   # (build_game.py's build_is_current reads it)
        self.assertEqual(link(), (True, []))
        touch(self.runtime / 'host.h')
        self.assertEqual(link(), (True, []))
        write(self.runtime / 'host.c', '#include "host.h"\nint host = 1;\n')
        self.assertEqual(link(), (True, ['host.c /O2']))
        write(self.runtime / 'host.h', '#pragma once\n#include "module.h"\nint more;\n')
        self.assertEqual(link(), (True, every))
        self.assertIn('runtime: 5 objects to compile again: the runtime headers changed', self.said)
        self.rt_flags.append('/DSSBU_HAVE_OPUS')
        self.assertEqual(link(), (True, [j for j in every if not j.startswith('cpu_check')]))
        self.assertIn('runtime: 4 objects to compile again: the compile command changed', self.said)
        check_flags.append('/DMORE')
        self.assertEqual(link(), (True, ['cpu_check.c /O2']))
        self.cc.version('fake clang version 20.1.8')
        self.assertEqual(link(), (True, every))
        write(self.runtime / 'kernel.c', '#error made up\n')
        self.assertEqual(link(), (False, ['kernel.c /O2']))
        self.assertFalse((rt / 'kernel.obj').exists())   # (not the old one either)
        self.assertEqual(link(), (False, ['kernel.c /O2']))   # (still not compiled: tried again)
        self.assertNotIn('to compile again', self.said)

    def test_link_by_hand_links_only_what_is_compiled_for_this_build(self):
        """recomp.py link (link_compiled) links a module folder only when its objects are all compiled for this build,
        so that build/isa.txt, which the link writes, never names today's instruction set over objects compiled for
        another (avx2 objects labelled x86-64-v2 would pass the processor check on a processor without AVX2)."""
        exe, linked = self.dir / 'build' / 'ssbu.exe', []

        def link():
            out = io.StringIO()
            def link(dirs, exe, rt_dir=None):   # (link's record, as test_runtime has it write)
                linked.append(dirs)
                isa.record(str(Path(exe).parent), cc.ISA, False)
                return True
            with mock.patch.object(recomp, 'link', link), contextlib.redirect_stdout(out):
                ok = recomp.link_compiled([str(self.module)], str(exe))
            self.said = out.getvalue()
            return ok
        self.compile()
        isa.record(str(self.module.parent), 'avx2', False)
        self.assertTrue(link())
        self.assertEqual(linked, [[str(self.module)]])
        with mock.patch.object(cc, 'ISA', 'x86-64-v2'):   # (SSBU_ISA=x86-64-v2 set, say)
            self.flags[1:3] = ['/clang:-march=x86-64-v2']
            self.assertFalse(link())
            self.assertIn('main: 3 objects not compiled for this build (the folder is built for avx2, this build is for '
                          'x86-64-v2): compile them first', self.said)
            self.assertIn('main: 3 objects to compile again: the compile command changed', self.said)
            self.assertEqual(len(linked), 1)
            self.compile()
            self.assertTrue(link())
            self.assertEqual(len(linked), 2)
            write(self.module / 'g0001.c', '#include "recomp.h"\nint changed;\n')   # (and not with a file's C changed since)
            self.assertFalse(link())
            self.assertIn('main: 1 object not compiled for this build: compile them first', self.said)
        hand = self.dir / 'build' / 'hand'   # (objects compiled by hand: no manifest.tsv, so no plan to check them by)
        hand.mkdir()
        out = io.StringIO()
        with mock.patch.object(recomp, 'link', lambda *a: linked.append(a) or True), contextlib.redirect_stdout(out):
            self.assertFalse(recomp.link_compiled([str(hand)], str(exe)))
        self.assertIn('not a folder recomp.py generated (no manifest.tsv)', out.getvalue())
        self.assertEqual(len(linked), 2)
        self.assertEqual(self.cc.runs(), [])   # (it compiles nothing itself)


# ---- the Linux build -------------------------------------------------------------------------
class LinuxBuildTests(Folder):
    """build_linux.py's game and runtime jobs, with tools/linux/toolchain.py (its clang the made-up compiler)."""
    def setUp(self):
        super().setUp()
        bin_dir = self.dir / 'llvm' / 'bin'
        bin_dir.mkdir(parents=True)
        with mock.patch.dict(os.environ, {'SSBU_LLVM_BIN': str(bin_dir)}):
            spec = importlib.util.spec_from_file_location('linux_toolchain_under_test', ROOT / 'tools' / 'linux' / 'toolchain.py')
            self.tc = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(self.tc)
        self.cc.name = self.tc.CLANG
        self.tc.RUNTIME = str(self.runtime)
        write(self.runtime / 'host.h', '#pragma once\n#include "module.h"\n')
        write(self.runtime / 'host.c', '#include "host.h"\nint host;\n')
        write(self.runtime / 'cpu_check.c', '#include "cpu_check.h"\n')
        write(self.runtime / 'linux' / 'win32.c', '#include "host.h"\nint win32;\n')
        write(self.runtime / 'linux' / 'include' / 'windows.h', '#pragma once\n')
        self.tree, self.out = self.dir / 'tree', self.dir / 'build-linux'
        main = self.tree / 'build' / 'main'
        write(main / 'manifest.tsv', 'g0000.c\t40000\t5000\ng0001.c\t40000\t60000\n')
        write(main / 'g0000.c', '#include "recomp.h"\nvoid f_7100000000(void) {}\n')
        write(main / 'g0001.c', '#include "recomp.h"\nvoid f_7100001000(void) { __try { } }\n')   # (landing sites: C++)
        write(main / 'g_module.c', '#include "module.h"\n')
        patch = self.cc.patch()
        patch.start()
        self.addCleanup(patch.stop)
        saved = sys.modules.get('toolchain'), sys.argv, list(sys.path)
        sys.modules['toolchain'] = self.tc
        sys.argv = ['build_linux.py', '--port', str(self.tree), '--out', str(self.out), '--jobs', '2']
        try:
            self.ns = runpy.run_path(str(ROOT / 'tools' / 'linux' / 'build_linux.py'), run_name='build_linux_under_test')['main'].__globals__
        finally:
            if saved[0] is None:
                sys.modules.pop('toolchain', None)
            else:
                sys.modules['toolchain'] = saved[0]
            sys.argv, sys.path[:] = saved[1], saved[2]
        self.ns['ISA'] = 'avx2'   # (what an older version's folder, without build-linux/isa.txt, is for)

    def build(self, what='game'):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), warnings.catch_warnings():
            warnings.simplefilter('ignore', ResourceWarning)   # (build_linux.py reads its small files with open().read())
            jobs = self.ns['game_jobs' if what == 'game' else 'runtime_jobs']()
            ok = self.ns['compile_all'](jobs, what)
        self.said = out.getvalue()
        return ok, self.cc.runs()

    def test_game(self):
        every = ['g0000.c -O2', 'g0001_cxx.cpp -O0', 'g_module.c -O1']
        self.assertEqual(self.build(), (True, every))
        self.assertTrue((self.out / 'main' / 'g0001.o').read_text().startswith('object of g0001_cxx.cpp: -c -O0'))
        self.assertIn('-x c++', (self.out / 'main' / 'g0001.o').read_text())
        self.assertEqual(self.build(), (True, []))
        self.assertIn('game: up to date', self.said)
        touch(self.tree / 'build' / 'main' / 'g0001.c')
        self.assertEqual(self.build(), (True, []))
        write(self.tree / 'build' / 'main' / 'g0001.c', '#include "recomp.h"\nvoid f_7100001000(void) { __try { } } \n')
        self.assertEqual(self.build(), (True, ['g0001_cxx.cpp -O0']))
        write(self.runtime / 'lanes.h', '#pragma once\ntypedef struct { u32 s[4]; } V128; /* two */\n')
        self.assertEqual(self.build(), (True, every))
        self.assertIn('game: 3 objects to compile again: the runtime headers changed', self.said)
        self.tc.CFLAGS = [f for f in self.tc.CFLAGS if f != '-mavx2'] + ['-march=x86-64-v2']
        self.assertEqual(self.build(), (True, every))
        self.assertIn('game: 3 objects to compile again: the compile command changed', self.said)
        self.cc.version('fake clang version 20.1.8')
        self.assertEqual(self.build(), (True, every))
        self.assertIn('game: 3 objects to compile again: the compiler changed', self.said)
        write(self.tree / 'build' / 'main' / 'g0000.c', '#include "recomp.h"\nvoid f_7100000000(void) { __try { } }\n')
        self.assertEqual(self.build(), (True, ['g0000_cxx.cpp -O2']))   # (now C++: said as a change to its C)
        self.assertNotIn('to compile again', self.said)

    def test_an_older_version_s_objects_are_compiled_again(self):
        """As on Windows: every older version compiled with -mf16c (isa.LEGACY_CLANG, its toolchain.py's ARCH), which
        today's avx2 does not, so a folder it built is compiled again whole for avx2 (and for x86-64-v2)."""
        every = ['g0000.c -O2', 'g0001_cxx.cpp -O0', 'g_module.c -O1']
        for target in isa.TARGETS:
            with self.subTest(target=target):
                with mock.patch.dict(isa.CLANG, {'avx2': isa.LEGACY_CLANG}):
                    self.ns['ISA'], self.tc.CFLAGS = 'avx2', self.tc.cflags('avx2')   # (the older version's build)
                self.build()
                (self.out / 'main' / objcache.RECORD).unlink()
                self.assertIsNone(isa.recorded(str(self.out)))
                self.ns['ISA'], self.tc.CFLAGS = target, self.tc.cflags(target)
                self.assertEqual(self.build(), (True, every))
                self.assertIn('game: 3 objects to compile again: no record of what they were built from', self.said)
                self.assertNotIn('taken as current', self.said)
                self.assertNotIn('-mf16c', (self.out / 'main' / 'g0000.o').read_text())

    def test_a_folder_an_older_version_built_is_not_compiled_again(self):
        """As on Windows: no record at all, objects newer than their sources and the runtime headers kept (for a build
        whose flags are the ones the older version compiled with, which no target's are today: made up here)."""
        with mock.patch.object(isa, 'LEGACY_CLANG', isa.CLANG['avx2']):
            self.build()
            (self.out / 'main' / objcache.RECORD).unlink()
            self.assertEqual(self.build(), (True, []))
            self.assertIn('3 objects an older version compiled taken as current', self.said)
            self.assertEqual(self.build(), (True, []))

    def test_a_folder_an_older_version_built_for_another_instruction_set(self):
        """As on Windows: built today for x86-64-v2, an older version's folder (avx2, no build-linux/isa.txt) is
        compiled again whole; built for what build-linux/isa.txt names, with its flags the older version's (made up),
        its objects are taken as current."""
        every = ['g0000.c -O2', 'g0001_cxx.cpp -O0', 'g_module.c -O1']
        self.build()
        record = self.out / 'main' / objcache.RECORD
        record.unlink()
        self.ns['ISA'], self.tc.CFLAGS = 'x86-64-v2', self.tc.cflags('x86-64-v2')
        with mock.patch.object(isa, 'LEGACY_CLANG', isa.CLANG['x86-64-v2']):
            self.assertEqual(self.build(), (True, every))
            self.assertIn('game: 3 objects to compile again: no record of what they were built from', self.said)
            record.unlink()
            isa.record(str(self.out), 'x86-64-v2', True)   # (what link writes)
            self.assertEqual(self.build(), (True, []))
            self.assertIn('3 objects an older version compiled taken as current', self.said)
        record.unlink()
        self.assertEqual(self.build(), (True, every))   # (with today's own: not the flags they were compiled with)

    def test_link_only_links_only_what_is_compiled_for_this_build(self):
        """--link-only (link_only) links only when nothing is to be compiled, so that build-linux/isa.txt, which the link
        writes, never names today's instruction set over objects compiled for another: not after the instruction set
        changed, nor with the runtime compiled for another (--runtime-only) in a folder whose record names today's."""
        linked = []
        self.ns['link'] = lambda: linked.append(self.ns['ISA']) or True

        def link_only():
            out = io.StringIO()
            with contextlib.redirect_stdout(out), warnings.catch_warnings():
                warnings.simplefilter('ignore', ResourceWarning)
                ok = self.ns['link_only']()
            self.said = out.getvalue()
            return ok
        self.ns['ISA'], self.tc.CFLAGS = 'avx2', self.tc.cflags('avx2')
        self.build()
        self.build('runtime')
        isa.record(str(self.out), 'avx2', False)
        self.assertTrue(link_only())
        self.assertEqual(linked, ['avx2'])
        self.ns['ISA'], self.tc.CFLAGS = 'x86-64-v2', self.tc.cflags('x86-64-v2')   # (SSBU_ISA=x86-64-v2 set, say)
        self.assertFalse(link_only())
        self.assertIn('--link-only: 8 objects not compiled for this build (the folder is built for avx2, this build is for '
                      'x86-64-v2): build without --link-only', self.said)
        self.build('runtime')   # (--runtime-only: the runtime for x86-64-v2, the game still avx2's)
        self.assertFalse(link_only())
        self.ns['ISA'], self.tc.CFLAGS = 'avx2', self.tc.cflags('avx2')
        self.assertFalse(link_only())   # (the record names avx2, the runtime is x86-64-v2's)
        self.assertIn('--link-only: 5 objects not compiled for this build: build without --link-only', self.said)
        self.assertEqual(linked, ['avx2'])
        self.build('runtime')
        self.assertTrue(link_only())
        self.assertEqual(linked, ['avx2', 'avx2'])
        self.assertEqual(self.cc.runs(), [])   # (it compiles nothing itself)

    def test_hook_slots_are_keyed_on_hookable_h(self):
        """As on Windows: a change to hookable.h compiles again only the files that include it."""
        main = self.tree / 'build' / 'main'
        write(self.runtime / 'hookable.h', '#pragma once\n#include "recomp.h"\n')
        write(main / 'g0000.c', '#include "recomp.h"\n#include "hookable.h"\nvoid f_7100000000(void) {}\n')
        self.build()
        write(self.runtime / 'hookable.h', '#pragma once\n#include "recomp.h"\n/* a slot more */\n')
        self.assertEqual(self.build(), (True, ['g0000.c -O2']))
        self.assertEqual(self.build(), (True, []))

    def test_no_object_with_hook_slots_older_than_hookable_h_taken_as_current(self):
        """As on Windows: with no record, the object of a file with hook slots older than hookable.h is not taken
        as current; the others are (for a build whose flags are the ones the older version compiled with, which no
        target's are today: made up here)."""
        patch = mock.patch.object(isa, 'LEGACY_CLANG', isa.CLANG['avx2'])
        patch.start()
        self.addCleanup(patch.stop)
        main = self.tree / 'build' / 'main'
        write(self.runtime / 'hookable.h', '#pragma once\n#include "recomp.h"\n')
        write(main / 'g0000.c', '#include "recomp.h"\n#include "hookable.h"\nvoid f_7100000000(void) {}\n')
        self.build()
        record = self.out / 'main' / objcache.RECORD
        record.unlink()
        self.assertEqual(self.build(), (True, []))   # (newer than hookable.h: taken as current)
        record.unlink()
        write(self.runtime / 'hookable.h', '#pragma once\n#include "recomp.h"\n/* a slot more */\n', age=-7200)
        self.assertEqual(self.build(), (True, ['g0000.c -O2']))
        self.assertIn('game: 1 object to compile again: no record of what they were built from', self.said)
        self.assertIn('2 objects an older version compiled taken as current', self.said)
        self.assertEqual(self.build(), (True, []))

    def test_landing_sites_found_once(self):
        """Whether a file has landing sites (C++) is kept with its digest: an unchanged file is not read again."""
        self.build()
        read = []
        real = objcache.file_sha
        with mock.patch.object(objcache, 'file_sha', side_effect=lambda p, scan=(): read.append(p) or real(p, scan)):
            self.assertEqual(self.build(), (True, []))
        self.assertFalse([p for p in read if p.endswith('.c')], read)

    def test_a_failed_compile_leaves_no_object(self):
        self.build()
        write(self.tree / 'build' / 'main' / 'g0000.c', '#error made up\n')
        self.assertEqual(self.build(), (False, ['g0000.c -O2']))
        self.assertFalse((self.out / 'main' / 'g0000.o').exists())   # (not the old one either: no link takes it)
        self.assertEqual(self.build(), (False, ['g0000.c -O2']))

    def test_runtime(self):
        every = ['cpu_check.c -O2', 'hle_table.c -O1', 'host.c -O2', 'nro_table.c -O1', 'win32.c -O2', 'wrap_missing.c -O1']
        self.assertEqual(self.build('runtime'), (True, every))
        check = (self.out / 'rt' / 'cpu_check.o').read_text().split()   # (for any x86-64 processor: not the game's flags)
        self.assertIn('-march=x86-64', check)
        self.assertNotIn('baseline', check)
        self.assertEqual([f for f in self.tc.ARCH if f in check], [])
        self.assertEqual(self.build('runtime'), (True, []))
        write(self.runtime / 'linux' / 'include' / 'windows.h', '#pragma once\ntypedef int BOOL;\n')
        self.assertEqual(self.build('runtime'), (True, every))
        self.assertIn('runtime: 6 objects to compile again: the runtime headers changed', self.said)
        self.tc.CFLAGS = self.tc.CFLAGS + ['-DMORE']
        self.assertEqual(self.build('runtime'), (True, [j for j in every if not j.startswith('cpu_check')]))
        self.tc.CHECK_CFLAGS = self.tc.CHECK_CFLAGS + ['-DMORE']
        self.assertEqual(self.build('runtime'), (True, ['cpu_check.c -O2']))


if __name__ == '__main__':
    unittest.main()
