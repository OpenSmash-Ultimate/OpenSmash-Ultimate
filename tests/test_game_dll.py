"""--game-dll (tools/game_dll.py, runtime/game_dll.h): the game's modules linked into a library of their
own, which the runtime program loads at start. Here without a real build: the two files game_dll.py
writes (from a made-up runtime/, and from the real one, which runtime/recomp.h and game_dll.h must
agree with), the Windows build's runtime and link in that mode and without it (tools/recomp.py with
tools/cc.py, the made-up compiler of test_objcache.py), the Linux build's (tools/linux/build_linux.py)
and the macOS build's (tools/mac/build_mac.py, its toolchain stood in for), build_game.py's switch and
tools/dev/runtime_compile_check.py's. On a Mac both files are compiled too, and game_tables.c for
Windows with stand-in C headers. The builds themselves, and the program's check of the library, run
in tools/mac/boottest (a Mac) and .github/workflows/runtime-compile.yml. No game files."""
from pathlib import Path
import contextlib
import importlib.util
import io
import os
import re
import runpy
import subprocess
import sys
import tempfile
import types
import unittest
import warnings
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools' / 'dev'))
import cc  # noqa: E402
import game_dll  # noqa: E402
import recomp  # noqa: E402
import runtime_compile_check as check  # noqa: E402
import test_mac_build as mac_tests  # noqa: E402  (build_mac.py loaded with its toolchain stood in for)
import test_objcache as made_up  # noqa: E402  (the made-up compiler)
import test_runtime_compile_check as check_tests  # noqa: E402  (its complete link list)

DARWIN = sys.platform == 'darwin'
CORE = [name for name, _head, _args in game_dll.CORE]


def made_up_runtime(folder):
    """A runtime/ with two HLE functions, an HLE_WRAP (its signature comment before it, as recomp.py
    wants it) and an HLE_WRAP_EXPORT; and what the builds do not read: one not at the start of a
    line, a header's, macOS's ._ file."""
    made_up.write(Path(folder, 'hle_a.c'), 'HLE(nnFoo)\nvoid x(void) {}\nHLE(nnBar)\n')
    made_up.write(Path(folder, 'hle_b.c'), '/* signature: 16 words, sha256 0123456789abcdef */\nHLE_WRAP(7100001234)\n'
                                           'HLE_WRAP_EXPORT(nnWrapped)\n  HLE(notAtTheStart)\n')
    made_up.write(Path(folder, '._hle_a.c'), 'HLE(appledouble)\n')
    made_up.write(Path(folder, 'hle.h'), 'HLE(header)\n')
    return folder


class Generated(unittest.TestCase):
    """game_tables.c and game_funcs.c, from a made-up runtime/."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='game dll ')
        self.addCleanup(self.temp.cleanup)
        self.runtime = made_up_runtime(self.temp.name)

    def test_the_runtime_functions_as_the_builds_find_them(self):
        self.assertEqual(game_dll.runtime_functions(self.runtime), (['nnBar', 'nnFoo'], [0x7100001234], ['nnWrapped']))
        self.assertEqual(game_dll.imports(self.runtime),
                         [(n, 'core') for n in CORE] + [('hle_nnBar', 'hle'), ('hle_nnFoo', 'hle'),
                                                        ('hle_wrap_7100001234', 'wrap'), ('hle_wrap_nnWrapped', 'export')])

    def test_the_librarys_side(self):
        text = game_dll.tables_text(self.runtime)
        self.assertIn('#include "game_dll.h"\n', text)
        self.assertIn('static RtGameFn *rt_game_slots[12];\n', text)
        # each runtime function is one of its name, which calls through its slot as typed as its declaration
        self.assertIn('GuestFn *rt_lookup(u64 guest_addr) { return ((__typeof__(&rt_lookup))rt_game_slots[0])(guest_addr); }\n',
                      text)
        self.assertIn('#ifdef _WIN32\nint rt_eh_filter(struct _EXCEPTION_POINTERS *e, u64 sp, u64 lp) '
                      '{ return ((__typeof__(&rt_eh_filter))rt_game_slots[3])(e, sp, lp); }\n#else\n'
                      'int rt_eh_filter(const RtEhSignal *e, u64 sp, u64 lp) '
                      '{ return ((__typeof__(&rt_eh_filter))rt_game_slots[3])(e, sp, lp); }\n#endif\n', text)
        self.assertIn('_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) '
                      '{ ((__typeof__(&rt_trap))rt_game_slots[4])(c, pc, why); __builtin_unreachable(); }\n', text)
        self.assertIn('void rt_svc(Cpu *c, u32 number) { ((__typeof__(&rt_svc))rt_game_slots[5])(c, number); }\n', text)
        self.assertIn('u64 rt_ticks(void) { return ((__typeof__(&rt_ticks))rt_game_slots[6])(); }\n', text)
        self.assertIn('void hle_nnBar(Cpu *c) { ((GuestFn *)rt_game_slots[8])(c); }\n', text)
        self.assertIn('void hle_wrap_7100001234(Cpu *c) { ((GuestFn *)rt_game_slots[10])(c); }\n', text)
        self.assertIn('void hle_wrap_nnWrapped(Cpu *c, GuestFn *orig) '
                      '{ ((void (*)(Cpu *, GuestFn *))rt_game_slots[11])(c, orig); }\n', text)
        imports = re.search(r'static const RtGameImport imports\[\] = \{\n(.*?)\};', text, re.S).group(1)
        self.assertEqual(re.findall(r'\{"(\w+)", &rt_game_slots\[(\d+)\]\}', imports),
                         [(name, str(slot)) for slot, (name, _k) in enumerate(game_dll.imports(self.runtime))])
        self.assertTrue(imports.endswith('    {0, 0}\n'))
        # its hook slots listed, not added to a runtime's; a wrapped function's original, or a stub of it
        self.assertIn('void rt_hook_add(RtHook *h) { h->next = hooks; hooks = h; }\n', text)
        self.assertIn(f'void rt_game_missing(Cpu *c) {{ rt_trap(c, 0, "{game_dll.MISSING}"); }}\n', text)
        self.assertIn('#ifdef _WIN32\nGuestFn f_7100001234_orig;\n'
                      '#pragma comment(linker, "/alternatename:f_7100001234_orig=rt_game_missing")\n#else\n'
                      '__attribute__((weak)) void f_7100001234_orig(Cpu *c) { rt_game_missing(c); }\n#endif\n', text)
        self.assertIn('static const RtGameOrig origs[] = {\n    {0x7100001234ull, f_7100001234_orig},\n    {0, 0}\n};\n', text)
        self.assertIn('static const RtModule *const boot[] = {&rt_module_main, &rt_module_subsdk0, &rt_module_sdk};\n', text)
        # one name exported: the function that hands all of it over
        self.assertEqual(text.count('RT_GAME_EXPORT'), 1)
        self.assertIn('RT_GAME_EXPORT const RtGameTables *rt_game_tables(void) {\n', text)
        self.assertIn('(RtGameTables){RT_GAME_ABI, RT_GAME_SIZES, RT_GAME_ISA, boot, rt_nro_modules, rt_nro_count,\n'
                      '                       12, imports, origs, 1, &hooks};\n', text)
        self.assertNotIn('appledouble', text)
        self.assertNotIn('notAtTheStart', text)
        self.assertNotIn('header', text)

    def test_the_programs_side(self):
        text = game_dll.funcs_text(self.runtime)
        self.assertIn('#include "host.h"\n#include "game_dll.h"\n', text)
        rows = re.search(r'const RtGameFunc rt_game_funcs\[\] = \{\n(.*?)\};', text, re.S).group(1).splitlines()
        self.assertEqual(rows, [f'    {{"{n}", (RtGameFn *){n}, 0, 0}},' for n in CORE]
                         + ['    {"hle_wrap_7100001234", (RtGameFn *)hle_wrap_7100001234, 0x7100001234ull, &orig_7100001234},',
                            '    {"hle_wrap_nnWrapped", (RtGameFn *)hle_wrap_nnWrapped, 0, 0},', '    {0, 0, 0, 0}'])
        self.assertIn('const u32 rt_game_func_count = 10;\n', text)
        # the original the runtime's wrapper calls, which the library has: until it is bound, the runtime's stub
        self.assertIn('static GuestFn *orig_7100001234 = rt_wrap_missing;\nvoid f_7100001234_orig(Cpu *c) { orig_7100001234(c); }\n',
                      text)
        self.assertNotIn('hle_nnFoo', text)   # (the HLE functions are bound from hle_table)


class RealRuntime(unittest.TestCase):
    """The interface against the real runtime/: what it declares and defines, as the builds find it."""
    def test_the_same_functions_as_recomp_py_finds(self):
        self.assertEqual(game_dll.runtime_functions(RUNTIME),
                         (sorted(recomp.hle_names()), sorted(recomp.hle_wraps()), sorted(recomp.hle_export_wraps())))

    def test_core_is_what_recomp_h_declares(self):
        """Each CORE function is declared so in runtime/recomp.h (rt_unwind_jumpto, which the generated C declares
        itself, is defined so in the runtime), and recomp.h declares no other runtime function the C could call: one
        added there is one the library calls through a slot of its own (and RT_GAME_ABI changes with it)."""
        header = (RUNTIME / 'recomp.h').read_text()
        for name, head, _args in game_dll.CORE:
            for h in head if isinstance(head, tuple) else (head,):
                if name == 'rt_unwind_jumpto':
                    self.assertIn(h + ' {', (RUNTIME / 'hle_unwind.c').read_text())
                else:
                    self.assertIn(h + ';', header)
        declared = set(re.findall(r'^(?!static)(?:_Noreturn )?\w[\w ]*?[ *](rt_\w+)\([^;{]*\);', header, re.M))
        self.assertEqual(declared, set(CORE) - {'rt_unwind_jumpto'})

    def test_the_header_names_what_the_builds_do(self):
        header = (RUNTIME / 'game_dll.h').read_text()
        self.assertIn(f'#define RT_GAME_ENTRY "{game_dll.ENTRY}"\n', header)
        for name in (game_dll.DLL, game_dll.SO, game_dll.DYLIB):
            self.assertIn(f'#define RT_GAME_LIBRARY "{name}"\n', header)
        self.assertGreaterEqual(game_dll.abi(str(RUNTIME)), 1)
        self.assertIn(f'#ifdef {game_dll.DEFINE}\n', (RUNTIME / 'host.c').read_text())

    def test_the_real_runtimes_imports(self):
        every = game_dll.imports(RUNTIME)
        names, addresses, exported = game_dll.runtime_functions(RUNTIME)
        self.assertEqual(len(every), len(CORE) + len(names) + len(addresses) + len(exported))
        self.assertEqual(len({n for n, _k in every}), len(every))   # (each name once: the program binds by name)
        text = game_dll.tables_text(RUNTIME)
        self.assertIn(f'static RtGameFn *rt_game_slots[{len(every)}];\n', text)
        self.assertEqual(text.count('/alternatename:'), len(addresses))

    @unittest.skipUnless(DARWIN, 'the Command Line Tools')
    def test_both_files_compile_on_this_mac(self):
        tc = mac_tests.real_toolchain()
        with tempfile.TemporaryDirectory(prefix='game dll ') as tmp:
            for name, text, includes in ((game_dll.TABLES, game_dll.tables_text(RUNTIME), [RUNTIME]),
                                         (game_dll.FUNCS, game_dll.funcs_text(RUNTIME),
                                          [RUNTIME / 'macos' / 'include', RUNTIME / 'linux' / 'include', RUNTIME])):
                src = made_up.write(Path(tmp, name), text)
                code, out = tc.run([tc.CLANG, *tc.TARGET, '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                                    *[a for i in includes for a in ('-I', str(i))], str(src)])
                self.assertEqual(code, 0, f'{name}:\n{out}')

    @unittest.skipUnless(DARWIN, 'the Command Line Tools')
    def test_the_librarys_side_compiles_for_windows(self):
        """game_tables.c compiled for x64 Windows with stand-ins for the C library's headers (the real ones are the
        Windows SDK's, which only the Windows job has): the originals' /alternatename and the one export."""
        tc = mac_tests.real_toolchain()
        with tempfile.TemporaryDirectory(prefix='game dll ') as tmp:
            inc = Path(tmp, 'include')
            made_up.write(inc / 'excpt.h', '#pragma once\n')
            made_up.write(inc / 'math.h', '#pragma once\n' + ''.join(f'double {f}(double{", double" * n});\nfloat {f}f(float{", float" * n});\n'
                                                                      for f, n in (('fma', 2), ('sqrt', 0), ('fabs', 0),
                                                                                   ('trunc', 0), ('floor', 0), ('ceil', 0),
                                                                                   ('round', 0), ('rint', 0), ('nearbyint', 0),
                                                                                   ('copysign', 1), ('fmin', 1), ('fmax', 1)))
                          + '#define NAN __builtin_nan("")\n#define INFINITY __builtin_inf()\n#define isnan __builtin_isnan\n'
                            '#define isinf __builtin_isinf\n#define signbit __builtin_signbit\n')
            made_up.write(inc / 'setjmp.h', '#pragma once\ntypedef struct { unsigned long long part[2]; } _SETJMP_FLOAT128;\n'
                                            'typedef _SETJMP_FLOAT128 jmp_buf[16];\nint _setjmp(jmp_buf);\n'
                                            '#define setjmp(b) _setjmp(b)\n__declspec(noreturn) void longjmp(jmp_buf, int);\n')
            made_up.write(inc / 'string.h', '#pragma once\n#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\n'
                                            'void *memset(void *, int, size_t);\n')
            src = made_up.write(Path(tmp, game_dll.TABLES), game_dll.tables_text(RUNTIME))
            obj = Path(tmp, 'game_tables.obj')
            code, out = tc.run([tc.CLANG, '-target', 'x86_64-pc-windows-msvc', '-fms-extensions', '-c', '-O1', '-mavx2', '-mfma',
                                '-Wall', '-Wextra', '-Werror', '-nostdlibinc', '-isystem', str(inc), '-I', str(RUNTIME),
                                str(src), '-o', str(obj)])
            self.assertEqual(code, 0, out)
            data = obj.read_bytes()
            self.assertEqual(data.count(b'/EXPORT:rt_game_tables'), 1)
            self.assertEqual(data.count(b'=rt_game_missing'), len(game_dll.runtime_functions(RUNTIME)[1]))


class WindowsLink(made_up.Folder):
    """tools/recomp.py's link with --game-dll (recomp.GAME_DLL), the made-up compiler as clang-cl and cc.link /
    cc.link_library recorded: the runtime compiled to load the library, its objects linked into the program, the
    game's (with the library's tables and the NROs' list) into build/ssbu_game.dll; and back."""
    def setUp(self):
        super().setUp()
        made_up_runtime(self.runtime)
        made_up.write(self.runtime / 'host.h', '#pragma once\n#include "module.h"\n')
        made_up.write(self.runtime / 'host.c', '#include "host.h"\nint host;\n')
        made_up.write(self.runtime / 'kernel.c', '#include "host.h"\nint kernel;\n')
        made_up.write(self.runtime / 'cpu_check.c', '#include "cpu_check.h"\n')
        self.module = self.dir / 'build' / 'main'
        for name in ('g0000.obj', 'g_module.obj'):   # (compiled: recomp.py compile)
            made_up.write(self.module / name, 'object\n')
        self.flags = ['/nologo', '/arch:AVX2', '-mfma', '/I', str(self.runtime)]   # cc.GAME_CFLAGS
        self.rt_flags = self.flags + ['/DSSBU_HAVE_SDL3']   # cc.CFLAGS
        for patch in (mock.patch.object(cc, 'CLANG', self.cc.name), mock.patch.object(cc, 'GAME_CFLAGS', self.flags),
                      mock.patch.object(cc, 'ISA', 'avx2'), mock.patch.object(cc, 'CFLAGS', self.rt_flags),
                      mock.patch.object(cc, 'RUNTIME', str(self.runtime)), mock.patch.object(cc, 'SDL3_DLL', None),
                      mock.patch.object(cc, 'CHECK_CFLAGS', ['/nologo', '/I', str(self.runtime)]), self.cc.patch()):
            patch.start()
            self.addCleanup(patch.stop)
        self.rt, self.exe = self.dir / 'build' / 'rt', self.dir / 'build' / 'ssbu.exe'
        self.dll = self.dir / 'build' / game_dll.DLL

    def link(self, dll):
        self.linked, self.libraries = [], []

        def link(objs, exe, options=()):
            self.linked.append((objs, list(options)))
            Path(exe).write_bytes(b'MZ')

        def link_library(objs, path):
            self.libraries.append((objs, path))
            Path(path).write_bytes(b'MZ library')
        out = io.StringIO()
        with mock.patch.object(recomp, 'GAME_DLL', dll), mock.patch.object(cc, 'link', link), \
                mock.patch.object(cc, 'link_library', link_library), contextlib.redirect_stdout(out):
            ok = recomp.link([str(self.module)], str(self.exe), str(self.rt))
        self.said = out.getvalue()
        return ok, self.cc.runs()

    def obj(self, name):
        return str(self.rt / name)

    def test_the_program_and_the_library(self):
        ok, runs = self.link(True)
        self.assertTrue(ok, self.said)
        self.assertEqual(runs, ['cpu_check.c /O2', 'game_funcs.c /O1', 'game_tables.c /O1', 'hle_a.c /O2', 'hle_b.c /O2',
                                'hle_table.c /O1', 'host.c /O2', 'kernel.c /O2', 'nro_table.c /O1'])
        for name in ('host', 'kernel', 'hle_a', 'hle_table', 'game_funcs'):   # (the runtime, compiled to load the library)
            self.assertIn('/DSSBU_GAME_DLL', (self.rt / f'{name}.obj').read_text(), name)
            self.assertIn('/DSSBU_HAVE_SDL3', (self.rt / f'{name}.obj').read_text(), name)
        for name in ('game_tables', 'nro_table', 'cpu_check'):   # (the library's, compiled as the game is; the check's own)
            self.assertNotIn('/DSSBU_GAME_DLL', (self.rt / f'{name}.obj').read_text(), name)
        self.assertNotIn('/DSSBU_HAVE_SDL3', (self.rt / 'game_tables.obj').read_text())
        self.assertIn('/arch:AVX2 -mfma', (self.rt / 'game_tables.obj').read_text())   # (the game's instruction set)
        self.assertEqual((self.rt / game_dll.TABLES).read_text(), game_dll.tables_text(str(self.runtime)))
        self.assertEqual((self.rt / game_dll.FUNCS).read_text(), game_dll.funcs_text(str(self.runtime)))
        [(objs, options)] = self.linked
        self.assertEqual(sorted(objs), sorted(self.obj(n) for n in ('host.obj', 'kernel.obj', 'hle_a.obj', 'hle_b.obj',
                                                                      'cpu_check.obj', 'hle_table.obj', 'game_funcs.obj')))
        self.assertEqual(options, [])   # (no stub originals: game_funcs.c has them)
        self.assertEqual(self.libraries, [([self.obj('game_tables.obj'), self.obj('nro_table.obj'),
                                            str(self.module / 'g0000.obj'), str(self.module / 'g_module.obj')], str(self.dll))])
        self.assertIn(f'linked 4 objects into {self.dll}', self.said)
        self.assertEqual(self.link(True), (True, []))

    def test_back_and_forth(self):
        """Without the switch the build is what it was: the runtime compiled again (its command changed), the game's
        objects and the NROs' list linked into the program with the stub originals, and no library left beside it."""
        self.link(False)
        default = self.linked
        self.link(True)
        self.assertTrue(self.dll.exists())
        ok, runs = self.link(False)
        self.assertTrue(ok, self.said)
        self.assertEqual(runs, ['hle_a.c /O2', 'hle_b.c /O2', 'hle_table.c /O1', 'host.c /O2', 'kernel.c /O2'])
        self.assertIn('runtime: 5 objects to compile again: the compile command changed', self.said)
        self.assertEqual(self.linked, default)
        [(objs, options)] = self.linked
        self.assertEqual(objs[-3:], [self.obj('nro_table.obj'), str(self.module / 'g0000.obj'), str(self.module / 'g_module.obj')])
        self.assertNotIn(self.obj('game_funcs.obj'), objs)
        self.assertEqual(options, [f'/alternatename:f_{a:x}_orig=rt_wrap_missing' for a in sorted(recomp.hle_wraps())])
        self.assertEqual(self.libraries, [])
        self.assertFalse(self.dll.exists())
        self.assertNotIn('/DSSBU_GAME_DLL', (self.rt / 'host.obj').read_text())
        ok, runs = self.link(True)
        self.assertEqual(runs, ['game_funcs.c /O1', 'game_tables.c /O1', 'hle_a.c /O2', 'hle_b.c /O2', 'hle_table.c /O1',
                                'host.c /O2', 'kernel.c /O2'])   # (its own two kept from before)

    def test_the_librarys_link(self):
        """cc.link_library: the objects in a response file beside the DLL, linked as the program is but with none of its
        libraries, placed as it is (/HIGHENTROPYVA:NO) and with a map; a failed link raises, its errors said."""
        dll = str(self.dll)
        command = cc.library_command(dll + '.rsp', dll)
        self.assertEqual(command, [cc.CLANG, '/nologo', '/LD', *cc.LINK_FLAGS, '@' + dll + '.rsp', '/Fe' + dll, '/link',
                                   *cc.LINK_OPTIONS, '/HIGHENTROPYVA:NO', '/MAP:' + str(self.dir / 'build' / 'ssbu_game.map')])
        self.assertEqual([a for a in cc.link_command('x.rsp', 'x.exe') if a in cc.SYSTEM_LIBS], list(cc.SYSTEM_LIBS))
        self.assertFalse(set(cc.SYSTEM_LIBS) & set(command))
        objs = [str(self.dir / 'a b' / 'g0000.obj'), str(self.dir / 'g_module.obj')]
        self.dll.parent.mkdir(parents=True, exist_ok=True)
        ran = []
        with mock.patch.object(subprocess, 'run', lambda args, **kw: ran.append(args) or subprocess.CompletedProcess(args, 0, '', '')):
            self.assertEqual(cc.link_library(objs, dll), dll)
        self.assertEqual(ran, [cc.library_command(dll + '.rsp', dll)])
        self.assertEqual(Path(dll + '.rsp').read_text(), '\n'.join(f'"{o}"' for o in objs))
        failed = subprocess.CompletedProcess([], 1, 'warning: made up\nlld-link: error: undefined symbol: rt_made_up\n', '')
        with mock.patch.object(subprocess, 'run', lambda args, **kw: failed), self.assertRaises(RuntimeError) as e:
            cc.link_library(objs, dll)
        self.assertEqual(str(e.exception), "the game library's link failed\nlld-link: error: undefined symbol: rt_made_up")


class LinuxBuild(made_up.Folder):
    """build_linux.py with --game-dll, as test_objcache.py's Linux tests run it (tools/linux/toolchain.py, its clang
    the made-up compiler): the game compiled position-independent with its names hidden, the runtime to load the
    library, the library's own two files, and the two links."""
    def setUp(self):
        super().setUp()
        made_up_runtime(self.runtime)
        made_up.write(self.runtime / 'host.h', '#pragma once\n#include "module.h"\n')
        made_up.write(self.runtime / 'host.c', '#include "host.h"\nint host;\n')
        made_up.write(self.runtime / 'cpu_check.c', '#include "cpu_check.h"\n')
        made_up.write(self.runtime / 'linux' / 'win32.c', '#include "host.h"\nint win32;\n')
        made_up.write(self.runtime / 'linux' / 'include' / 'windows.h', '#pragma once\n')
        os.remove(self.runtime / 'hle_a.c')   # (the build's HLE functions are the real runtime's: one file of them here)
        os.remove(self.runtime / 'hle_b.c')
        self.tree, self.out = self.dir / 'tree', self.dir / 'build-linux'
        main = self.tree / 'build' / 'main'
        made_up.write(main / 'manifest.tsv', 'g0000.c\t40000\t5000\ng0001.c\t40000\t60000\n')
        made_up.write(main / 'g0000.c', '#include "recomp.h"\nvoid f_7100000000(void) {}\n')
        made_up.write(main / 'g0001.c', '#include "recomp.h"\nvoid f_7100001000(void) { __try { } }\n')   # (C++)
        made_up.write(main / 'g_module.c', '#include "module.h"\n')
        patch = self.cc.patch()
        patch.start()
        self.addCleanup(patch.stop)

    def load(self, dll):
        bin_dir = self.dir / 'llvm' / 'bin'
        bin_dir.mkdir(parents=True, exist_ok=True)
        with mock.patch.dict(os.environ, {'SSBU_LLVM_BIN': str(bin_dir)}):
            spec = importlib.util.spec_from_file_location('linux_toolchain_under_test', ROOT / 'tools' / 'linux' / 'toolchain.py')
            tc = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(tc)
        self.cc.name = tc.CLANG
        tc.RUNTIME = str(self.runtime)
        saved = sys.modules.get('toolchain'), sys.argv, list(sys.path)
        sys.modules['toolchain'] = tc
        sys.argv = ['build_linux.py', '--port', str(self.tree), '--out', str(self.out), '--jobs', '2', *(['--game-dll'] if dll else [])]
        try:
            ns = runpy.run_path(str(ROOT / 'tools' / 'linux' / 'build_linux.py'), run_name='build_linux_under_test')['main'].__globals__
        finally:
            if saved[0] is None:
                sys.modules.pop('toolchain', None)
            else:
                sys.modules['toolchain'] = saved[0]
            sys.argv, sys.path[:] = saved[1], saved[2]
        ns['ISA'] = 'avx2'
        ns['RUNTIME'] = str(self.runtime)
        return ns

    def build(self, ns, what):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), warnings.catch_warnings():
            warnings.simplefilter('ignore', ResourceWarning)
            jobs = ns['game_jobs' if what == 'game' else 'runtime_jobs']()
            ok = ns['compile_all'](jobs, what)
        self.said = out.getvalue()
        return ok, self.cc.runs()

    def link(self, ns):
        ran = []

        def run(command):
            ran.append(command)
            Path(command[command.index('-o') + 1]).write_bytes(b'\x7fELF')
            return 0, ''
        ns.update(run=run, HOST_LINUX=False,
                  subprocess=types.SimpleNamespace(run=lambda *a, **kw: subprocess.CompletedProcess(a, 0, '', '')))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            ok = ns['link']()
        self.said = out.getvalue()
        return ok, ran

    def listed(self, rsp):
        return [line.strip('"') for line in Path(rsp).read_text().splitlines()]

    def test_the_game_is_compiled_for_a_shared_library(self):
        ns = self.load(True)
        self.assertEqual(self.build(ns, 'game'), (True, ['g0000.c -O2', 'g0001_cxx.cpp -O0', 'g_module.c -O1']))
        for name in ('g0000.o', 'g0001.o', 'g_module.o'):
            args = (self.out / 'main' / name).read_text().split()
            self.assertIn('-fvisibility=hidden', args)
            self.assertGreater(args.index('-fPIC'), args.index('-fno-pic'), name)   # (the last one counts)
        self.assertEqual(self.build(self.load(False), 'game'), (True, ['g0000.c -O2', 'g0001_cxx.cpp -O0', 'g_module.c -O1']))
        self.assertNotIn('-fPIC', (self.out / 'main' / 'g0000.o').read_text())   # (without it, as before)
        self.assertIn('the compile command changed', self.said)

    def test_the_runtime_and_the_librarys_own(self):
        default = ['cpu_check.c -O2', 'hle_table.c -O1', 'host.c -O2', 'nro_table.c -O1', 'win32.c -O2', 'wrap_missing.c -O1']
        self.assertEqual(self.build(self.load(False), 'runtime'), (True, default))
        ns = self.load(True)
        self.assertEqual(self.build(ns, 'runtime'),
                         (True, ['game_funcs.c -O1', 'game_tables.c -O1', 'hle_table.c -O1', 'host.c -O2', 'nro_table.c -O1',
                                 'win32.c -O2']))
        self.assertEqual(sorted(os.listdir(self.out / 'rt')),   # (the program is linked from every object there)
                         ['cpu_check.o', 'game_funcs.o', 'hle_table.o', 'host.o', 'objcache.json', 'win32.o'])
        self.assertEqual(sorted(os.listdir(self.out / 'game')), ['game_tables.o', 'nro_table.o', 'objcache.json'])
        self.assertIn('-DSSBU_GAME_DLL', (self.out / 'rt' / 'host.o').read_text())
        tables = (self.out / 'game' / 'game_tables.o').read_text().split()
        self.assertNotIn('-DSSBU_GAME_DLL', tables)
        self.assertGreater(tables.index('-fPIC'), tables.index('-fno-pic'))
        self.assertEqual(self.build(ns, 'runtime'), (True, []))
        self.assertEqual(self.build(self.load(False), 'runtime'),
                         (True, ['hle_table.c -O1', 'host.c -O2', 'nro_table.c -O1', 'win32.c -O2', 'wrap_missing.c -O1']))
        self.assertNotIn('game_funcs.o', os.listdir(self.out / 'rt'))

    def test_the_links(self):
        ns = self.load(True)
        self.build(ns, 'game')
        self.build(ns, 'runtime')
        ok, ran = self.link(ns)
        self.assertTrue(ok, self.said)
        program, library = ran
        rsp = str(self.out / 'ssbu.rsp')
        self.assertEqual(self.listed(rsp), [str(self.out / 'rt' / n).replace('\\', '/') for n in
                                            ('cpu_check.o', 'game_funcs.o', 'hle_table.o', 'host.o', 'win32.o')])
        self.assertIn('-no-pie', program)
        lib = str(self.out / game_dll.SO)
        self.assertEqual(self.listed(lib + '.rsp'), [str(p).replace('\\', '/') for p in (
            self.out / 'game' / 'game_tables.o', self.out / 'game' / 'nro_table.o', self.out / 'main' / 'g0000.o',
            self.out / 'main' / 'g0001.o', self.out / 'main' / 'g_module.o')])
        self.assertEqual(library[library.index('-o') + 1], lib)
        for a in ('-shared', '-fuse-ld=lld', '@' + lib + '.rsp', '-lstdc++', '-Wl,-z,defs', '-Wl,-z,noexecstack', '-Wl,--build-id'):
            self.assertIn(a, library)
        self.assertNotIn('-no-pie', library)
        self.assertNotIn('-l:libSDL2-2.0.so.0', library)   # (none of the runtime's libraries)
        self.assertTrue(os.path.isfile(lib))
        ok, ran = self.link(self.load(False))   # (without the switch: one link, of everything, and no library left)
        self.assertEqual(len(ran), 1)
        self.assertFalse(os.path.exists(lib))
        self.assertIn(str(self.out / 'main' / 'g0000.o').replace('\\', '/'), self.listed(rsp))


class MacBuild(unittest.TestCase):
    """build_mac.py with --game-dll, its toolchain stood in for (test_mac_build.py's)."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mac game dll ')
        self.addCleanup(self.temp.cleanup)
        self.tree = Path(self.temp.name) / 'tree'
        build = self.tree / 'build'
        mac_tests.fake_module(build, 'main', [('g0000.c', 100, 50, 'void f(void) {}\n'),
                                              ('g0001.c', 30000, 25000, 'y; __try { }\n')])
        mac_tests.fake_module(build, 'subsdk0', [('g0000.c', 1, 1, '')])
        mac_tests.fake_module(build, 'sdk', [('g0000.c', 1, 1, '')])
        mac_tests.fake_module(build, 'fighter', [('g0000.c', 1, 1, '')], nro=True)
        self.ns = mac_tests.load_build_mac(['--port', str(self.tree), '--game-dll'], mac_tests.stub_toolchain())
        self.default = mac_tests.load_build_mac(['--port', str(self.tree)], mac_tests.stub_toolchain())

    def rel(self, path):
        return os.path.relpath(path, self.ns['OBJ'])

    def test_settings(self):
        self.assertEqual(self.ns['RUNTIME_DEFINES'], self.default['RUNTIME_DEFINES'] + ['-DSSBU_GAME_DLL'])
        self.assertEqual(self.ns['LIBRARY'], str(self.tree / 'build-mac' / 'WindowSmash.app' / 'Contents' / 'Frameworks' /
                                                 'ssbu_game.dylib'))
        self.assertEqual((self.ns['CXX_LANG'], self.default['CXX_LANG']), ('c++dll', 'c++'))

    def test_the_games_cxx_shares_its_types_by_name(self):
        for ns, extra in ((self.ns, ['-fvisibility-ms-compat']), (self.default, [])):
            by_obj = {self.rel(j[1]): j for j in ns['game_jobs']()}
            self.assertEqual(by_obj[os.path.join('main', 'g0001.o')][3], ['-I', str(ROOT / 'runtime'), *extra])
            self.assertEqual(by_obj[os.path.join('main', 'g0000.o')][3], ['-I', str(ROOT / 'runtime')])
        runtime = {os.path.basename(j[0]): j[3] for j in self.ns['runtime_jobs']()}
        self.assertIn('-fvisibility-ms-compat', runtime['eh.cpp'])   # (the runtime's that throws)
        self.assertNotIn('-fvisibility-ms-compat', runtime['host.c'])
        self.assertNotIn('-fvisibility-ms-compat', {os.path.basename(j[0]): j[3] for j in self.default['runtime_jobs']()}['eh.cpp'])

    def test_generated_files(self):
        self.assertEqual([(os.path.basename(s), self.rel(o)) for s, o, _f in self.ns['generated_sources']()],
                         [('hle_table.c', os.path.join('rt', 'gen', 'hle_table.o')),
                          ('game_funcs.c', os.path.join('rt', 'gen', 'game_funcs.o'))])
        library = self.ns['library_sources']()
        self.assertEqual([(os.path.basename(s), self.rel(o), f) for s, o, f in library],
                         [('game_tables.c', os.path.join('game', 'game_tables.o'), ['-I', str(ROOT / 'runtime')]),
                          ('nro_table.c', os.path.join('game', 'nro_table.o'), ['-I', str(ROOT / 'runtime')])])
        self.assertIn('rt_module_fighter', Path(library[1][0]).read_text())
        self.assertEqual(Path(library[0][0]).read_text(), game_dll.tables_text(str(ROOT / 'runtime')))
        self.assertTrue({o for _s, o, _f in library} <= {j[1] for j in self.ns['runtime_jobs']()})
        self.assertEqual([os.path.basename(s) for s, _o, _f in self.default['generated_sources']()],
                         ['hle_table.c', 'wrap_missing.c', 'nro_table.c'])

    def test_the_librarys_link(self):
        ran = []
        self.ns['run'] = lambda command: ran.append(command) or Path(command[command.index('-o') + 1]).write_bytes(b'') or (0, '')
        objs = [os.path.join(self.ns['OBJ'], 'game', 'game_tables.o'), os.path.join(self.ns['OBJ'], 'main', 'g0000.o')]
        Path(self.ns['OUT']).mkdir(parents=True)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(self.ns['link_library'](objs))
        out = Path(self.ns['OUT'])
        [command] = ran
        self.assertEqual(command, ['clang', *self.ns['TARGET'], '-dynamiclib', '-Xlinker', '-filelist', '-Xlinker',
                                   str(out / 'ssbu_game.filelist'), '-o', str(out / 'ssbu_game.partial'), '-lc++',
                                   '-Wl,-no_deduplicate', '-Wl,-exported_symbol,_rt_game_tables', '-install_name',
                                   '@rpath/ssbu_game.dylib'])
        self.assertEqual((out / 'ssbu_game.filelist').read_text(), ''.join(o + '\n' for o in objs))
        self.assertTrue(os.path.isfile(self.ns['LIBRARY']))
        self.assertFalse((out / 'ssbu_game.partial').exists())
        self.ns['run'] = lambda command: (1, 'Undefined symbols for architecture arm64:\n  "_rt_made_up", referenced from:\n')
        with contextlib.redirect_stdout(io.StringIO()) as said:
            self.assertFalse(self.ns['link_library'](objs))
        self.assertIn("the game library's link failed", said.getvalue())


class BuildGame(unittest.TestCase):
    """build_game.py --game-dll: handed to the build, and a program is current only with its library (or, without the
    switch, without one)."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='build game dll ')
        self.addCleanup(self.temp.cleanup)
        self.port = Path(self.temp.name) / 'port'
        self.calls = []
        self.ns = runpy.run_path(str(ROOT / 'tools' / 'build_game.py'), run_name='build_game_dll')['build'].__globals__
        self.ns.update(ROOT=str(self.port), log=lambda msg='': None,
                       run=lambda args, failure: self.calls.append([str(a) for a in args]))

    def test_the_library_beside_the_program(self):
        for linux, macos, exe, library in ((True, False, ('build-linux', 'ssbu'), ('build-linux', 'ssbu_game.so')),
                                           (False, True, ('build-mac', 'WindowSmash.app', 'Contents', 'MacOS', 'ssbu'),
                                            ('build-mac', 'WindowSmash.app', 'Contents', 'Frameworks', 'ssbu_game.dylib')),
                                           (False, False, ('build', 'ssbu.exe'), ('build', 'ssbu_game.dll'))):
            self.ns.update(LINUX=linux, MACOS=macos, EXE=str(self.port.joinpath(*exe)))
            self.assertEqual(self.ns['game_library'](), str(self.port.joinpath(*library)))

    def test_handed_to_the_build(self):
        """To build_linux.py and build_mac.py, which compile and link; not to the C's generation (recomp.py), which is
        the same either way."""
        tc = types.SimpleNamespace(VKD3D_BUILD=str(self.port / 'vkd3d'), ISA='avx2')
        fake_subprocess = types.SimpleNamespace(**{k: getattr(subprocess, k) for k in dir(subprocess) if not k.startswith('_')})
        fake_subprocess.Popen = lambda args, **kw: None   # (caffeinate, on a Mac)
        self.ns.update(subprocess=fake_subprocess, linux_toolchain=lambda: tc)
        for dll in (False, True):
            self.calls.clear()
            self.ns['build_linux_program'](4, dll)
            self.ns['build_mac_program'](4, dll)
            by_tool = {}
            for c in self.calls:
                by_tool.setdefault(Path(c[1]).name, []).append(c)
            self.assertEqual(sorted(by_tool), ['build_linux.py', 'build_mac.py', 'build_vkd3d.py', 'recomp.py'])
            for name, commands in by_tool.items():
                for command in commands:
                    self.assertEqual('--game-dll' in command, dll and name in ('build_linux.py', 'build_mac.py'), command)

    def test_current_with_its_library(self):
        """On macOS (build_is_current's other checks are test_mac_build.py's): the program alone is current without the
        switch; with it, only with a library newer than what it is built from; and a library is not current without it."""
        dump = self.port / 'dump'
        for n in ('runtime/host.c', 'tools/recomp.py', 'tools/mac/build_mac.py', 'dump/exefs/main'):
            made_up.write(self.port / n, '', age=100)
        exe = self.port / 'build-mac' / 'WindowSmash.app' / 'Contents' / 'MacOS' / 'ssbu'
        made_up.write(exe, '', age=0)
        made_up.write(exe.parent.parent / 'Frameworks' / 'SDL3.framework' / 'SDL3', '')
        self.ns.update(LINUX=False, MACOS=True, EXE=str(exe), TOOLS=str(self.port / 'tools'), DUMP=str(dump),
                       EXEFS=str(dump / 'exefs'))
        current = self.ns['build_is_current']
        self.assertTrue(current())
        self.assertFalse(current(True))   # (no library)
        library = made_up.write(self.ns['game_library'](), '', age=200)   # (older than the runtime)
        self.assertFalse(current(True))
        os.utime(library, None)
        self.assertTrue(current(True))
        self.assertFalse(current())   # (a library a build without the switch would leave: it removes it)


class CompileCheck(unittest.TestCase):
    """tools/dev/runtime_compile_check.py --game-dll: the builds' commands with the switch, the library's place and
    its link's list, what each link must and must not have, and the run, which ends with the program loading the
    library (ssbu --game-library)."""
    tree = os.path.join('top', 'made-up game')

    def test_steps_hand_the_switch_to_each_build(self):
        for host in ('windows', 'linux', 'macos'):
            plain = check.steps(self.tree, host, 3, vkd3d_ready=False, python='py')
            split = check.steps(self.tree, host, 3, vkd3d_ready=False, python='py', dll=True)
            self.assertEqual(split[:-1], plain[:-1], host)   # (Windows' compiles, Linux's vkd3d-shader: the same)
            self.assertEqual(split[-1], (plain[-1][0], plain[-1][1] + ['--game-dll']), host)

    def test_where_the_library_and_its_list_are(self):
        t = self.tree
        self.assertEqual(check.library(t, 'windows'), os.path.join(t, 'build', 'ssbu_game.dll'))
        self.assertEqual(check.library(t, 'linux'), os.path.join(t, 'build-linux', 'ssbu_game.so'))
        self.assertEqual(check.library(t, 'macos'), os.path.join(t, 'build-mac', 'WindowSmash.app', 'Contents', 'Frameworks',
                                                                 'ssbu_game.dylib'))
        with tempfile.TemporaryDirectory(prefix='runtime check ') as top:
            tree = os.path.join(top, 'made-up game')
            for host, path, text in (('windows', check.library(tree, 'windows') + '.rsp', '"C:\\a b\\g0000.obj"'),   # (cc.link_library's)
                                     ('linux', check.library(tree, 'linux') + '.rsp', '"/a b/g0000.o"'),   # (build_linux.py's)
                                     ('macos', os.path.join(tree, 'build-mac', 'ssbu_game.filelist'), '/a b/g0000.o\n')):
                self.assertIsNone(check.link_list(tree, host, of_library=True))
                made_up.write(path, text)
                self.assertEqual(check.link_list(tree, host, of_library=True), [text.strip().strip('"')])

    def lists(self, tree, host):
        """A good pair of link lists: the program's the runtime's objects (and a generated one), the library's the
        modules' and its own tables'."""
        rt = check.runtime_folder(tree, host)
        program = [os.path.join(rt, 'host.o'), os.path.join(rt, 'game_funcs.o')]
        library = [os.path.join(rt if host == 'windows' else os.path.join(tree, 'elsewhere'), 'game_tables.o')]
        library += [os.path.join(check.module_folder(tree, host, m), 'g0000.o') for m in check.MODULES]
        return program, library

    def test_what_each_link_has(self):
        tree = os.path.abspath(self.tree)
        for host in ('windows', 'linux', 'macos'):
            with self.subTest(host):
                program, library = self.lists(tree, host)
                self.assertEqual(check.library_problems(check.RUNTIME, tree, host, program, library), [])
                game = library[-1]
                self.assertEqual(check.library_problems(check.RUNTIME, tree, host, program + [game], library[:-1]),
                                 [f"the program has g0000.o, which is not the runtime's (not in {check.runtime_folder(tree, host)}): "
                                  "the game's objects are the library's",
                                  f"the game's library has none of sdk's objects (in {check.module_folder(tree, host, 'sdk')})"])
                host_o = os.path.join(check.runtime_folder(tree, host), 'host.o')
                self.assertEqual(check.library_problems(check.RUNTIME, tree, host, program, library + [host_o]),
                                 ["the game's library has runtime/host.c's object: it is linked without the runtime's "
                                  "(tools/game_dll.py)"])

    def run_check(self, host, top, code=0, output=None, library=True):
        """main --game-dll with the build recorded rather than run: the last step makes the program, the library and their
        lists; the program's run says output (default: that it loaded the library) and ends with code."""
        tree = os.path.join(top, 'made-up game')
        self.ran = []

        def run(command, cwd, **kw):
            self.ran.append(command)
            if command[-1] == '--game-library':
                said = output if output is not None else f'[boot] game library {check.library(tree, host)}: interface 1, 9 ' \
                                                         'runtime functions bound, 1 hook slots, 0 NROs\n'
                return subprocess.CompletedProcess(command, code, said)
            if len(self.ran) == len(check.steps(tree, host, None, vkd3d_ready=False, dll=True)):
                exe = check.program(tree, host)
                made_up.write(exe, 'program')
                program = [o for o in check_tests.complete_link(tree, host) if check.inside(o, check.runtime_folder(tree, host))]
                lib_objs = [os.path.join(check.module_folder(tree, host, m), 'g0000.o') for m in check.MODULES]
                listed = {'windows': exe + '.rsp', 'linux': os.path.join(tree, 'build-linux', 'ssbu.rsp'),
                          'macos': os.path.join(tree, 'build-mac', 'ssbu.filelist')}[host]
                made_up.write(listed, '\n'.join(program))
                if library:
                    made_up.write(check.library(tree, host), 'library')
                    made_up.write(check.library(tree, host) + '.rsp' if host != 'macos' else
                                  os.path.join(tree, 'build-mac', 'ssbu_game.filelist'), '\n'.join(lib_objs))
            return subprocess.CompletedProcess(command, 0)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), mock.patch.object(check, 'vkd3d_ready', return_value=False), \
                mock.patch.dict(os.environ, {'GITHUB_STEP_SUMMARY': ''}):
            result = check.main(['--tree', tree, '--game-dll'], run=run, host=host)
        return result, out.getvalue()

    def test_a_run_ends_with_the_program_loading_the_library(self):
        with tempfile.TemporaryDirectory(prefix='runtime check ') as top:
            for host in ('windows', 'linux', 'macos'):
                with self.subTest(host):
                    code, out = self.run_check(host, top)
                    self.assertEqual(code, 0, out)
                    tree = os.path.join(top, 'made-up game')
                    self.assertIn('--game-dll', self.ran[-2])
                    self.assertEqual(self.ran[-1], [check.program(tree, host), '--game-library'])
                    self.assertRegex(out, rf'\n{host}, [\w?-]+, --game-dll: the runtime compiled and linked into ssbu(\.exe)? '
                                          rf'\(0\.0 MB\) and the made-up game into ssbu_game\.\w+ \(0\.0 MB\) in \d+ s, which the '
                                          r'program loaded \(interface 1, 9 runtime functions bound, 1 hook slots, 0 NROs\), every '
                                          r'source in it but those left out on purpose: ')

    def test_a_library_the_program_refuses_fails(self):
        with tempfile.TemporaryDirectory(prefix='runtime check ') as top:
            code, out = self.run_check('linux', top, code=4, output='[boot] cannot use the game library x: it is built for '
                                                                    'game interface 2, and this runtime\'s is 1\n')
            self.assertEqual(code, 1)
            self.assertIn("it is built for game interface 2", out)   # (the program's own words, shown)
            self.assertIn('FAILED (linux, --game-dll): the program did not load the game\'s library (exit status 4)', out)
            code, out = self.run_check('linux', top, code=0, output='')   # (ended well, but said nothing of the library)
            self.assertEqual(code, 1)

    def test_a_build_without_the_library_fails(self):
        with tempfile.TemporaryDirectory(prefix='runtime check ') as top:
            code, out = self.run_check('macos', top, library=False)
            self.assertEqual(code, 1)
            self.assertIn("FAILED (macos, --game-dll): the build ended without the game's library, "
                          f"{check.library(os.path.join(top, 'made-up game'), 'macos')}", out)
            self.assertNotEqual(self.ran[-1][-1], '--game-library')   # (not started)


if __name__ == '__main__':
    unittest.main()
