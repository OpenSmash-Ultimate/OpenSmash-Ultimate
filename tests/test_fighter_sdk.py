"""The fighter SDK (ssbu-recomp/fighter_sdk): where its tools look for what they read, the compile and link lines
of fighters and mods (the game's compiler and target, floating point pinned last), and its pure-Python parts
(mangled names, hashes, main_adrp's ELF reading). On a Mac a made-up fighter and the example mod are built with
the game's compiler and run. Made-up inputs only: no game files, nothing derived from the game."""
from pathlib import Path
import ast
import json
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT / 'ssbu-recomp' / 'fighter_sdk'
TOOLS = SDK / 'tools'
sys.path.insert(0, str(TOOLS))
import cityhash    # noqa: E402
import fighter_cc  # noqa: E402
import luaconst    # noqa: E402
import main_adrp   # noqa: E402
import mangle      # noqa: E402
import nrolib      # noqa: E402
import sdk_paths   # noqa: E402

DARWIN = sys.platform == 'darwin'
ARM64 = platform.machine().lower() in ('arm64', 'aarch64')
FUSED = '-ffp-contract=on', '-ffp-contract=fast', '-ffast-math', '-funsafe-math-optimizations'
QUIET = {'PYTHONDONTWRITEBYTECODE': '1'}


def write(path, data=''):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, bytes):
        path.write_bytes(data)
    else:
        path.write_text(data)
    return str(path)


def in_order(part, whole):
    """Whether `part` is in `whole` in the same order (other items between them allowed)."""
    it = iter(whole)
    return all(any(p == w for w in it) for p in part)


# ---------------------------------------------------------------- stand-ins for the root build's toolchain modules

def stub_root(target):
    """A made-up root toolchain module of the shape tools/cc.py, tools/linux/toolchain.py and
    tools/mac/toolchain.py have."""
    if target == 'windows':
        return types.SimpleNamespace(
            CLANG='C:/LLVM/bin/clang-cl.exe', TOOLCHAIN='made up', ISA='avx2', ISA_WHY='made up',
            base_cflags=lambda isa: ['/nologo', '/arch:AVX2', '-mfma', '-fno-strict-aliasing', '/clang:-ffp-contract=off',
                                     '-w', '/winsysroot', 'C:/msvc'],
            LINK_FLAGS=['-fuse-ld=lld', '/winsysroot', 'C:/msvc'], LINK_OPTIONS=['/winsysroot:C:/msvc'])
    if target == 'linux':
        tgt = ['--target=x86_64-linux-gnu', '--sysroot=/made/up/sysroot']
        return types.SimpleNamespace(
            CLANG='/made/up/llvm/bin/clang', ISA='avx2', ISA_WHY='made up', RUNTIME='/made/up/repo/runtime', TARGET=tgt,
            cflags=lambda isa: tgt + ['-mavx2', '-mfma', '-fno-strict-aliasing', '-ffp-contract=off', '-fno-pic', '-fno-plt',
                                      '-w', '-fasynchronous-unwind-tables', '-I', '/made/up/repo/runtime'])
    tgt = ['-arch', 'arm64', '-mmacosx-version-min=14.0', '-isysroot', '/made/up/MacOSX.sdk']
    return types.SimpleNamespace(
        CLANG='/made/up/clang', MIN_MACOS='14.0', RUNTIME='/made/up/repo/runtime', TARGET=tgt,
        CFLAGS=tgt + ['-mcpu=apple-m1', '-fno-strict-aliasing', '-ffp-contract=off', '-fno-stack-protector',
                      '-fvisibility=hidden', '-w'])


def root_flags(target, root):
    if target == 'windows':
        return root.base_cflags(root.ISA)
    if target == 'linux':
        return root.cflags(root.ISA)
    return root.CFLAGS


def kept(target, root):
    """The root's flags a fighter's line must carry: all but -w, -fno-pic and the runtime's include folder."""
    flags, out = list(root_flags(target, root)), []
    runtime = getattr(root, 'RUNTIME', None)
    i = 0
    while i < len(flags):
        if flags[i] in ('-I', '/I') and runtime and i + 1 < len(flags) and flags[i + 1] == runtime:
            i += 2
            continue
        if flags[i] not in ('-w', '-fno-pic'):
            out.append(flags[i])
        i += 1
    return out


class CompileLineTests(unittest.TestCase):
    """fighter_cc's compile and link lines, for each system, with the root module stood in for."""

    def line(self, target, **kw):
        tc = fighter_cc.toolchain(target, stub_root(target))
        obj = 'a' + tc.obj
        return tc, tc.compile_command('a.cpp', obj, ['inc1', 'inc2'], **kw)

    def test_root_flags_reach_the_line(self):
        for target in fighter_cc.TARGETS:
            with self.subTest(target):
                tc, cmd = self.line(target)
                self.assertEqual(cmd[0], stub_root(target).CLANG)
                self.assertTrue(in_order(kept(target, stub_root(target)), cmd), cmd)

    def test_left_out(self):
        for target in fighter_cc.TARGETS:
            with self.subTest(target):
                tc, cmd = self.line(target)
                self.assertNotIn('-w', cmd)                   # warnings are shown
                self.assertNotIn('-fno-pic', cmd)             # a shared library
                self.assertNotIn('/made/up/repo/runtime', cmd)
        self.assertIn('-fPIC', self.line('linux')[1])

    def test_floating_point_pinned_last(self):
        for target, pin in (('windows', ['/clang:-fno-fast-math', '/clang:-ffp-contract=off']),
                            ('linux', ['-fno-fast-math', '-ffp-contract=off']), ('macos', ['-fno-fast-math', '-ffp-contract=off'])):
            with self.subTest(target):
                tc, cmd = self.line(target, force=['f.h'], defines=['FS_X=1'])
                at = cmd.index('a.cpp')
                self.assertEqual(cmd[at - 2:at], pin)        # -fno-fast-math first: it can turn the contraction back on
                self.assertFalse([a for a in cmd[at - 2:] if any(f in a for f in FUSED)], cmd)
                self.assertFalse([a for a in cmd if '-ffast-math' in a or 'fp-contract=fast' in a], cmd)

    def test_windows_line(self):
        tc, cmd = self.line('windows', force=['f.h'], defines=['FS_X=1'])
        for a in ('/c', '/clang:-O1', '/TP', '/std:c++14', '/GR-', fighter_cc.CL_DIAG, '/FIf.h', '/DFS_X=1', '/Foa.obj',
                  '/clang:-Wall', '/clang:-Wno-unused-parameter'):
            self.assertIn(a, cmd)
        self.assertTrue(in_order(['/I', 'inc1', '/I', 'inc2'], cmd))
        # clang-cl reads a bare -Wall as /Wall (every warning there is); -O1 and -std=... are GNU spellings
        self.assertFalse({'-Wall', '-O1', '-std=c++14', '-x', '-o', '/EHsc'} & set(cmd), cmd)

    def test_gnu_line(self):
        for target in ('linux', 'macos'):
            with self.subTest(target):
                tc, cmd = self.line(target, force=['f.h'], defines=['FS_X=1'])
                self.assertEqual(cmd[1:3], ['-c', '-O1'])
                self.assertTrue(in_order(['-x', 'c++', '-std=c++14', '-fno-exceptions', '-fno-rtti', '-Wall'], cmd))
                self.assertTrue(in_order(['-I', 'inc1', '-I', 'inc2', '-include', 'f.h', '-DFS_X=1'], cmd))
                self.assertEqual(cmd[-3:], ['a.cpp', '-o', 'a.o'])

    def test_link_lines(self):
        win = fighter_cc.toolchain('windows', stub_root('windows'))
        self.assertEqual(win.link_command(['a.obj', 'b.obj'], 'x.dll'),
                         ['C:/LLVM/bin/clang-cl.exe', '/nologo', '-fuse-ld=lld', '/winsysroot', 'C:/msvc', '/LD', 'a.obj', 'b.obj',
                          '/Fex.dll', '/link', '/winsysroot:C:/msvc'])
        lin = fighter_cc.toolchain('linux', stub_root('linux'))
        self.assertEqual(lin.link_command(['a.o'], 'x.so'),
                         ['/made/up/llvm/bin/clang', '--target=x86_64-linux-gnu', '--sysroot=/made/up/sysroot', '-shared',
                          '-fuse-ld=lld', 'a.o', '-o', 'x.so'])
        mac = fighter_cc.toolchain('macos', stub_root('macos'))
        self.assertEqual(mac.link_command(['a.o'], 'x.dylib', rsp='objs.rsp'),
                         ['/made/up/clang', *stub_root('macos').TARGET, '-dynamiclib', '@objs.rsp', '-o', 'x.dylib'])

    def test_library_names(self):
        self.assertEqual([fighter_cc.library('mario', t) for t in fighter_cc.TARGETS], ['mario.dll', 'mario.so', 'mario.dylib'])
        self.assertEqual([fighter_cc.toolchain(t, stub_root(t)).obj for t in fighter_cc.TARGETS], ['.obj', '.o', '.o'])
        self.assertEqual(fighter_cc.library('x'), 'x' + fighter_cc.EXT[fighter_cc.host()])

    def test_runtime_include_only(self):
        flags = ['-I', '/made/up/runtime', '-I', '/other', '-w', '-fno-pic', '-fno-plt']
        self.assertEqual(fighter_cc.keep(flags, '/made/up/runtime'), ['-I', '/other', '-fno-plt'])
        self.assertEqual(fighter_cc.keep(flags), ['-I', '/made/up/runtime', '-I', '/other', '-fno-plt'])

    def test_response_file(self):
        with tempfile.TemporaryDirectory(prefix='fighter-sdk-test-') as tmp:
            tc = fighter_cc.toolchain('windows', stub_root('windows'))
            path = tc.response_file(['C:\\a b\\x.obj', '/c/y.obj'], os.path.join(tmp, 'link.rsp'))
            self.assertEqual(Path(path).read_text(), '"C:/a b/x.obj"\n"/c/y.obj"')

    def test_unknown_target(self):
        with self.assertRaises(SystemExit):
            fighter_cc.toolchain('amiga', stub_root('linux'))


class RootModuleTests(unittest.TestCase):
    """The same with the root build's real modules, where they load on this machine."""

    def test_root_flags_reach_the_line(self):
        for target in fighter_cc.TARGETS:
            with self.subTest(target):
                try:
                    root = fighter_cc.load_root(target)
                    flags = list(root_flags(target, root))
                except (Exception, SystemExit) as e:     # (no compiler for that system here)
                    self.skipTest('%s: %s' % (target, str(e).splitlines()[0] if str(e) else type(e).__name__))
                self.assertTrue(any('-ffp-contract=off' in f for f in flags), flags)   # the game is built pinned too
                tc = fighter_cc.toolchain(target, root)
                cmd = tc.compile_command('a.cpp', 'a' + tc.obj)
                self.assertTrue(in_order(kept(target, root), cmd), cmd)
                self.assertEqual(cmd[cmd.index('a.cpp') - 1], ('/clang:' if target == 'windows' else '') + '-ffp-contract=off')

    def test_loaded_once_under_its_own_name(self):
        try:
            root = fighter_cc.load_root('windows')
        except (Exception, SystemExit) as e:
            self.skipTest(str(e))
        self.assertIs(fighter_cc.load_root('windows'), root)
        self.assertIs(sys.modules['fighter_sdk_root_windows'], root)

    def test_no_repository(self):
        with mock.patch.object(sdk_paths, 'REPO', None):
            with self.assertRaises(sdk_paths.Missing) as cm:
                fighter_cc.load_root('windows')
        self.assertIn('SSBU_REPO', str(cm.exception))


def find_clang():
    for name in ('clang', 'clang-19', 'clang-18', 'clang-17'):
        path = shutil.which(name)
        if path:
            return path
    return None


@unittest.skipUnless(find_clang(), 'no clang on the PATH')
class DriverTests(unittest.TestCase):
    """What clang makes of the Windows and Linux lines (-###: the options reach the compiler proper; nothing is
    compiled, so no Windows or Linux system files are needed)."""

    def cc1(self, target, driver):
        root = stub_root(target)
        if target == 'windows':
            root.base_cflags = lambda isa: ['/nologo', '/arch:AVX2', '-mfma', '-fno-strict-aliasing', '/clang:-ffp-contract=off', '-w']
            root.LINK_FLAGS, root.LINK_OPTIONS = [], []
        tc = fighter_cc.toolchain(target, root)
        with tempfile.TemporaryDirectory(prefix='fighter-sdk-test-') as tmp:
            src = write(os.path.join(tmp, 'a.cpp'), 'float f(float a, float b, float c) { return a * b + c; }\n')
            cmd = tc.compile_command(src, os.path.join(tmp, 'a' + tc.obj), [tmp])
            r = subprocess.run([find_clang(), *driver, '-###', *cmd[1:]], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        lines = [l for l in r.stderr.splitlines() if '"-cc1"' in l]
        self.assertEqual(len(lines), 1, r.stderr[-2000:])
        return lines[0]

    def test_windows(self):
        line = self.cc1('windows', ['--driver-mode=cl', '--target=x86_64-pc-windows-msvc'])
        for a in ('"-ffp-contract=off"', '"-O1"', '"+fma"', '"-std=c++14"', '"-fdiagnostics-format" "clang"'):
            self.assertIn(a, line)
        self.assertTrue('"+avx2"' in line or '"-target-cpu" "haswell"' in line, line)     # /arch:AVX2
        for a in ('"-ffp-contract=on"', '"-ffp-contract=fast"', '"-funsafe-math-optimizations"', '"-menable-no-infs"',
                  '"-fcxx-exceptions"', '"-Weverything"'):
            self.assertNotIn(a, line)

    def test_linux(self):
        line = self.cc1('linux', [])
        for a in ('"-ffp-contract=off"', '"-O1"', '"+fma"', '"-std=c++14"', '"-mrelocation-model" "pic"', '"x86_64-unknown-linux-gnu"'):
            self.assertIn(a, line)
        for a in ('"-ffp-contract=on"', '"-ffp-contract=fast"', '"-funsafe-math-optimizations"', '"-fcxx-exceptions"'):
            self.assertNotIn(a, line)


# ---------------------------------------------------------------- where the tools look

class PathTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='fighter-sdk-test-')
        self.tmp = Path(self._tmp.name)
        self.work = self.tmp / 'build' / 'work'
        self.work.mkdir(parents=True)
        self.repo = self.tmp / 'repo'

    def tearDown(self):
        self._tmp.cleanup()

    def test_repository(self):
        write(self.repo / 'tools' / 'cc.py')
        write(self.repo / 'tools' / 'isa.py')
        deep = self.repo / 'a' / 'b' / 'c'
        deep.mkdir(parents=True)
        self.assertEqual(sdk_paths.find_repo(deep, env={}), str(self.repo))
        self.assertEqual(sdk_paths.find_repo(deep, env={'SSBU_REPO': str(self.tmp)}), str(self.tmp))
        self.assertIsNone(sdk_paths.find_repo(self.work, env={}))
        self.assertEqual(sdk_paths.REPO, str(ROOT))                    # this repository, found from the tools

    def test_work_folder(self):
        self.assertEqual(sdk_paths.work_dir({'SSBU_FIGHTER_WORK': str(self.work)}), str(self.work))
        self.assertEqual(sdk_paths.work_dir({}), sdk_paths.TREE)
        self.assertEqual(sdk_paths.TREE, str(ROOT / 'ssbu-recomp'))

    def test_main_elf(self):
        with self.assertRaises(sdk_paths.Missing) as cm:
            sdk_paths.main_elf(env={}, work=str(self.work))
        text = str(cm.exception)
        for v in sdk_paths.VERSIONS:
            self.assertIn(os.path.join(str(self.tmp / 'build' / 'binaries'), v, 'main.elf'), text)
        self.assertIn('RECOMP_BIN', text)
        self.assertIn('"Reference data"', text)
        new = write(self.tmp / 'build' / 'binaries' / '13.0.5' / 'main.elf', b'made up')
        self.assertEqual(sdk_paths.main_elf(env={}, work=str(self.work)), new)
        old = write(self.tmp / 'build' / 'binaries' / '13.0.4' / 'main.elf', b'made up')
        self.assertEqual(sdk_paths.main_elf(env={}, work=str(self.work)), old)      # 13.0.4 first, as before
        named = write(self.tmp / 'bin' / 'main.elf', b'made up')
        self.assertEqual(sdk_paths.main_elf(env={'RECOMP_BIN': str(self.tmp / 'bin')}, work=str(self.work)), named)
        self.assertEqual(sdk_paths.main_elf(new, env={'RECOMP_BIN': str(self.tmp / 'bin')}), new)
        with self.assertRaises(sdk_paths.Missing) as cm:
            sdk_paths.main_elf(str(self.tmp / 'nowhere.elf'))
        self.assertIn('nowhere.elf', str(cm.exception))

    def test_missing_stops_the_tool(self):
        self.assertTrue(issubclass(sdk_paths.Missing, SystemExit))     # a message, not a traceback
        e = sdk_paths.missing('A thing', ['/a', '/b'], 'Make it.')
        self.assertEqual(str(e).splitlines()[:4], ['A thing was not found. Looked for:', '  /a', '  /b', 'Make it.'])

    def test_program(self):
        with self.assertRaises(sdk_paths.Missing) as cm:
            sdk_paths.program('ssbu_s2.exe', 'S2_EXE', work=str(self.work))
        self.assertIn('S2_EXE', str(cm.exception))
        exe = write(self.work / 'ssbu_s2.exe')
        self.assertEqual(sdk_paths.program('ssbu_s2.exe', 'S2_EXE', work=str(self.work)), exe)

    def test_reference(self):
        parts = ('skyline-smash', 'src', 'cpp.rs')
        with self.assertRaises(sdk_paths.Missing) as cm:
            sdk_paths.reference(*parts, env={'SSBU_FIGHTER_REFERENCE': str(self.tmp / 'mine')}, work=str(self.work), repo=str(self.repo))
        text = str(cm.exception)
        for root in (self.tmp / 'mine', self.repo / 'deps' / 'reference', self.tmp / 'build' / 'reference'):
            self.assertIn(os.path.join(str(root), *parts), text)
        beside = write(self.tmp / 'build' / 'reference' / os.path.join(*parts))
        self.assertEqual(sdk_paths.reference(*parts, env={}, work=str(self.work), repo=str(self.repo)), beside)
        deps = write(self.repo / 'deps' / 'reference' / os.path.join(*parts))
        self.assertEqual(sdk_paths.reference(*parts, env={}, work=str(self.work), repo=str(self.repo)), deps)
        mine = write(self.tmp / 'mine' / os.path.join(*parts))
        self.assertEqual(sdk_paths.reference(*parts, env={'SSBU_FIGHTER_REFERENCE': str(self.tmp / 'mine')},
                                             work=str(self.work), repo=str(self.repo)), mine)

    def test_userdata(self):
        with self.assertRaises(sdk_paths.Missing) as cm:
            sdk_paths.userdata(env={}, work=str(self.work))
        self.assertIn('SSBU_FIGHTER_USERDATA', str(cm.exception))
        (self.tmp / 'build' / 'userdata').mkdir()
        self.assertEqual(sdk_paths.userdata(env={}, work=str(self.work)), str(self.tmp / 'build' / 'userdata'))
        self.assertEqual(sdk_paths.userdata(env={'SSBU_FIGHTER_USERDATA': str(self.tmp)}, work=str(self.work)), str(self.tmp))

    def test_staged_tree(self):
        # setup.py's staging moves tools/nro to ro_study and mods/x to x
        with self.assertRaises(sdk_paths.Missing):
            sdk_paths.nro_tools(tree=str(self.work))
        write(self.work / 'ro_study' / 'arc_extract.py')
        self.assertEqual(sdk_paths.nro_tools(tree=str(self.work)), str(self.work / 'ro_study'))
        write(self.work / 'mods_n12_src' / 'row.txt')
        self.assertEqual(sdk_paths.tree_file(os.path.join('mods', 'mods_n12_src', 'row.txt'), os.path.join('mods_n12_src', 'row.txt'),
                                             tree=str(self.work)), str(self.work / 'mods_n12_src' / 'row.txt'))
        self.assertEqual(sdk_paths.nro_tools(), str(ROOT / 'ssbu-recomp' / 'tools' / 'nro'))   # the repository's own

    def test_script_modules_staged(self):
        ro = self.work / 'ro_study'
        write(ro / 'nro_map.json', json.dumps([{'name': 'lua2cpp_madeup', 'base': '7200000000', 'file': 'lua2cpp_madeup.nro'}]))
        self.assertEqual(sdk_paths.nro_modules(env={}, work=str(self.work), repo=str(self.repo)),
                         [{'name': 'lua2cpp_madeup', 'base': '7200000000', 'file': str(ro / 'nro' / 'lua2cpp_madeup.nro')}])

    def test_script_modules_of_a_game_build(self):
        dump = self.repo / 'dump' / 'nro'
        write(dump / 'bases.txt', 'lua2cpp_madeup 0x7200000000 0x7200100000 00aa\nlua2cpp_gone 0x7300000000 0x7300100000 00bb\nnot a row\n')
        for n in ('lua2cpp_madeup', 'lua2cpp_unplaced', 'other'):
            write(dump / (n + '.nro'), b'not a module')                 # (made-up bytes, in a temporary folder)
        mods = sdk_paths.nro_modules(env={}, work=str(self.work), repo=str(self.repo))
        self.assertEqual(mods, [{'name': 'lua2cpp_madeup', 'base': '0x7200000000', 'file': str(dump / 'lua2cpp_madeup.nro')},
                                {'name': 'lua2cpp_unplaced', 'base': '0', 'file': str(dump / 'lua2cpp_unplaced.nro')}])
        # SSBU_FIGHTER_NRO comes first
        mine = self.tmp / 'mine'
        write(mine / 'bases.txt', '')
        write(mine / 'lua2cpp_x.nro', b'not a module')
        self.assertEqual([m['name'] for m in sdk_paths.nro_modules(env={'SSBU_FIGHTER_NRO': str(mine)}, work=str(self.work),
                                                                   repo=str(self.repo))], ['lua2cpp_x'])

    def test_no_script_modules(self):
        with self.assertRaises(sdk_paths.Missing) as cm:
            sdk_paths.nro_modules(env={}, work=str(self.work), repo=str(self.repo))
        text = str(cm.exception)
        for p in (self.work / 'ro_study' / 'nro_map.json', self.repo / 'dump' / 'nro' / 'bases.txt'):
            self.assertIn(str(p), text)
        self.assertIn('SSBU_FIGHTER_NRO', text)

    def test_unknown_module(self):
        with mock.patch.object(nrolib, '_map', [{'name': 'lua2cpp_madeup', 'base': '0', 'file': str(self.tmp / 'x.nro')}]):
            with self.assertRaises(sdk_paths.Missing) as cm:
                nrolib.Nro('nobody')
        self.assertIn('lua2cpp_nobody', str(cm.exception))


def run_tool(script, *args, env=None, cwd=None):
    return subprocess.run([sys.executable, str(TOOLS / script), *args], capture_output=True, text=True, timeout=600,
                          env={**os.environ, **QUIET, **(env or {})}, cwd=cwd)


class ToolCommandTests(unittest.TestCase):
    """The tools' own command lines when something is missing (no compiler or game file needed)."""

    def test_usage(self):
        for script in ('build_fighter.py', 'build_mod.py'):
            with self.subTest(script):
                r = run_tool(script)
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertIn('python fighter_sdk/tools/' + script, r.stdout)

    def test_missing_inputs(self):
        with tempfile.TemporaryDirectory(prefix='fighter-sdk-test-') as tmp:
            env = {'SSBU_FIGHTER_WORK': tmp}
            r = run_tool('build_fighter.py', 'madeup', env=env)
            self.assertEqual(r.returncode, 1)
            self.assertIn('The sources of fighter madeup was not found', r.stderr)
            self.assertIn(os.path.join(tmp, 'fighters', 'madeup'), r.stderr)
            self.assertNotIn('Traceback', r.stderr)
            write(os.path.join(tmp, 'fighters', 'madeup', 'acmd', 'game.cpp'))
            r = run_tool('build_fighter.py', 'madeup', env=env)
            self.assertIn('The generated SDK header lua_bind.h was not found', r.stderr)
            self.assertIn('gen_sdk.py', r.stderr)
            r = run_tool('build_mod.py', os.path.join(tmp, 'nothing.cpp'), tmp, env=env)
            self.assertIn('The mod source was not found', r.stderr)
            r = run_tool('main_adrp.py', '0x100', os.path.join(tmp, 'main.elf'), env=env)
            self.assertIn('main.elf was not found', r.stderr)
            self.assertNotIn('Traceback', r.stderr)
            files = [os.path.relpath(os.path.join(d, f), tmp) for d, _, fs in os.walk(tmp) for f in fs]
            self.assertEqual(files, [os.path.join('fighters', 'madeup', 'acmd', 'game.cpp')])   # nothing written


class ImportTests(unittest.TestCase):
    def test_without_capstone_or_numpy(self):
        # the tools that read files only import them when they disassemble or compute with them
        code = ('import sys; sys.modules["capstone"] = None; sys.modules["numpy"] = None; sys.path.insert(0, sys.argv[1]); '
                'import nrolib, luaconst, mangle, cityhash, main_adrp, sdk_paths, fighter_cc, build_fighter, build_mod')
        r = subprocess.run([sys.executable, '-c', code, str(TOOLS)], capture_output=True, text=True, timeout=60,
                           env={**os.environ, **QUIET})
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])


# ---------------------------------------------------------------- pure-Python parts

class MangleTests(unittest.TestCase):
    """mangle.parse and nrolib.demangle_simple on made-up names (checked against c++filt when written)."""

    def test_parse(self):
        cases = {
            '_ZN3foo3bar6do_oneEPNS_5ThingEif': (['foo', 'bar', 'do_one'], [('ptr', ('class', 'foo::Thing')), 'i', 'f'], False),
            '_ZNK3foo5Thing4sizeEv': (['foo', 'Thing', 'size'], [], True),
            '_ZN3foo3bar4pairERKNS0_3KeyES3_': (['foo', 'bar', 'pair'], [('ref', ('const', ('class', 'foo::bar::Key')))] * 2, False),
            '_ZN3foo5ThingC2Ev': (['foo', 'Thing', 'Thing'], [], False),
            '_Z5helloi': (['hello'], ['i'], False),
            '_ZN3foo5Thing3getENS_6Hash40E': (['foo', 'Thing', 'get'], [('class', 'foo::Hash40')], False),
            '_ZN3lib8L2CAgent7math_piE': (['lib', 'L2CAgent', 'math_pi'], [], False),
            '_ZNSt3__14swapEv': None,
            '_ZN3foo3barIiE3bazEv': None,
            'plain': None,
        }
        for name, want in cases.items():
            with self.subTest(name):
                got = mangle.parse(name)
                self.assertEqual(None if got is None else (got[0], got[1], got[2]), want)

    def test_demangle_simple(self):
        cases = {'_ZN3foo3bar6do_oneEPNS_5ThingEif': 'foo::bar::do_one', '_ZNK3foo5Thing4sizeEv': 'foo::Thing::size',
                 '_ZN3foo5ThingC2Ev': 'foo::Thing::Thing', '_ZN3foo5ThingD1Ev': 'foo::Thing::~Thing', '_Z5helloi': 'hello',
                 '_ZNSt3__14swapEv': '?', '_ZN3foo3barIiE3bazEv': 'foo::bar?', 'plain': 'plain'}
        for name, want in cases.items():
            with self.subTest(name):
                self.assertEqual(nrolib.demangle_simple(name), want)

    def test_helpers(self):
        self.assertEqual(mangle.gvalue_name('_ZN3lib8L2CAgent7math_piE'), 'math_pi')
        self.assertIsNone(mangle.gvalue_name('_ZN3lib8L2CAgent8math_absERKNS_8L2CValueE'))
        self.assertIsNone(mangle.gvalue_name('_ZN3lib8L2CAgent9math_piE'))         # the length does not fit
        self.assertTrue(mangle.is_static_value(['lib', 'L2CAgent', 'math_abs']))
        self.assertFalse(mangle.is_member(['lib', 'L2CAgent', 'math_abs']))
        self.assertTrue(mangle.is_member(['lib', 'L2CValue', 'as_integer']))
        self.assertFalse(mangle.is_member(['foo', 'Thing', 'get']))
        self.assertEqual([mangle.reg_class(t) for t in ('f', ('const', 'd'), 'i', ('class', 'lib::L2CValue'),
                                                         ('class', 'phx::Vector3f'), ('class', 'phx::Hash40'))],
                         ['f', 'f', 'x', 'lv', '?', 'x'])


class HashTests(unittest.TestCase):
    # libc++'s std::hash<std::string> of BASE[:n], computed with Apple's libc++ when this test was written
    BASE = 'made_up_name_for_a_test_of_the_hash_0123456789_abcdefghijklmnopqrstuvwxyz_ABCDEFGHIJKLMNOPQRSTUVWXYZ_' * 2
    LIBCXX = {0: 0x9ae16a3b2f90404f, 1: 0x1c68541223dc31b0, 3: 0x6230f893dabb6e02, 4: 0x1737c086f087d43f,
              7: 0x2b7f267d80cda0d0, 8: 0x8716f1c8cb747982, 9: 0x4dad6081985db786, 16: 0x5f83491b41efa596,
              17: 0xac4d12e4b32f8662, 32: 0x674d6a537f07dacb, 33: 0xc22c86859417f67d, 48: 0x92c8b17f3ab49ada,
              64: 0x8e514c53270cafcf, 65: 0x8af254e1359ff7a4, 100: 0x9679a61115a1c3ab, 128: 0x13a0a1577115bc57,
              129: 0x9c27ecf69daee329, 200: 0x0c2f117b6a3d85d9}

    def test_cityhash_as_libcxx(self):
        for n, want in self.LIBCXX.items():
            with self.subTest(length=n):
                self.assertEqual(cityhash.cityhash64(self.BASE[:n]), want)
        self.assertEqual(cityhash.cityhash64(self.BASE[:9].encode()), self.LIBCXX[9])

    @unittest.skipUnless(DARWIN and shutil.which('clang++'), "needs libc++ (macOS's own C++ library)")
    def test_cityhash_against_libcxx(self):
        program = ('#include <cstdio>\n#include <functional>\n#include <string>\nint main() { char b[4096];\n'
                   '  while (fgets(b, sizeof b, stdin)) { std::string s(b); if (!s.empty() && s.back() == \'\\n\') s.pop_back();\n'
                   '    printf("%016zx\\n", std::hash<std::string>()(s)); } }\n')
        words = ['%s_%d' % (self.BASE[n % 50:n % 50 + n % 37], n) * (1 + n % 4) for n in range(300)]
        with tempfile.TemporaryDirectory(prefix='fighter-sdk-test-') as tmp:
            src = write(os.path.join(tmp, 'h.cpp'), program)
            exe = os.path.join(tmp, 'h')
            r = subprocess.run(['clang++', '-std=c++17', '-O1', src, '-o', exe], capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stderr)
            out = subprocess.run([exe], input='\n'.join(words) + '\n', capture_output=True, text=True, timeout=60).stdout.split()
        self.assertEqual(len(out), len(words))
        self.assertEqual(['%016x' % cityhash.cityhash64(w) for w in words], out)
        self.assertTrue(min(len(w) for w in words) <= 8 and max(len(w) for w in words) > 64)   # every length class

    def test_xxh64(self):
        # the public XXH64 test values
        self.assertEqual(luaconst.xxh64(b''), 0xef46db3751d8e999)
        self.assertEqual(luaconst.xxh64('a'), 0xd24ec4f1a98c6e5b)
        self.assertEqual(luaconst.xxh64('abc'), 0x44bc2cf5ad770999)
        self.assertEqual(luaconst.xxh64('Nobody inspects the spammish repetition'), 0xfbcea83c8a378bf1)


def elf(code, vaddr=0x1000, off=0x100, flags=5):
    """A made-up 64-bit ELF file: the header, one PT_LOAD program header, the code."""
    head = bytearray(0x40)
    head[0:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<Q', head, 0x20, 0x40)          # e_phoff
    struct.pack_into('<HH', head, 0x36, 56, 1)        # e_phentsize, e_phnum
    ph = struct.pack('<IIQQQQQQ', 1, flags, off, vaddr, vaddr, len(code), len(code), 0x1000)
    body = bytes(head) + ph
    return body + bytes(off - len(body)) + code


def adrp(rd, pages):
    return 0x90000000 | ((pages & 3) << 29) | ((pages >> 2) << 5) | rd


def add(rd, rn, imm):
    return 0x91000000 | (imm << 10) | (rn << 5) | rd


class MainAdrpTests(unittest.TestCase):
    CODE = struct.pack('<6I', adrp(3, 4), add(3, 3, 0x678),      # x3 = 0x5678 at 0x1000 / 0x1004
                       adrp(4, 4), add(5, 6, 0x678),              # the add reads another register: not the address
                       0xd503201f, 0xd65f03c0)                    # nop, ret

    def test_text_segment(self):
        self.assertEqual(main_adrp.text_segment(elf(self.CODE)), (0x100, 0x1000, len(self.CODE)))
        with self.assertRaises(SystemExit):
            main_adrp.text_segment(elf(self.CODE, flags=6))        # not executable
        with self.assertRaises(SystemExit) as cm:
            main_adrp.text_segment(b'NRO0' + bytes(0x80))
        self.assertIn('64-bit ELF', str(cm.exception))

    def test_find(self):
        try:
            import numpy  # noqa: F401
        except ImportError:
            self.skipTest('needs numpy')
        with tempfile.TemporaryDirectory(prefix='fighter-sdk-test-') as tmp:
            path = write(os.path.join(tmp, 'main.elf'), elf(self.CODE))
            self.assertEqual(main_adrp.find(0x5678, path), [0x1004])
            self.assertEqual(main_adrp.find(main_adrp.BASE + 0x5678, path), [0x1004])
            self.assertEqual(main_adrp.find(0x5670, path), [])


# ---------------------------------------------------------------- a made-up fighter and the example mod, built and run

FIGHTER_SOURCE = '''#include "fighter_sdk.h"

ACMD(madeup, game_fp) {
    float a = madeup::value(0), b = madeup::value(1), c = madeup::value(2);
    madeup::report(a * b + c);
}
'''

# Runs the fighter library's one script with a made-up host: madeup::value(i) answers VALUES[i], madeup::report(v)
# prints v's bits. (1 + 2^-12)^2 - (1 + 2^-11) is 0 when the multiply and the add round apart, as the game does,
# and 2^-24 (0x33800000) when they are fused.
FIGHTER_HARNESS = r'''
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
VALUE, REPORT = int(sys.argv[2]), int(sys.argv[3])
VALUES = [0x3F800800, 0x3F800800, 0xBF801000]
class V(ctypes.Structure): _fields_ = [('d', ctypes.c_uint64 * 2)]
class Ctx(ctypes.Structure): _fields_ = [('x', ctypes.c_uint64 * 32), ('v', V * 32)]
CALL = ctypes.CFUNCTYPE(None, ctypes.POINTER(Ctx), ctypes.c_int)
LOG = ctypes.CFUNCTYPE(None, ctypes.c_char_p)
CTX = ctypes.CFUNCTYPE(ctypes.c_void_p)
CALL_ADDR = ctypes.CFUNCTYPE(None, ctypes.POINTER(Ctx), ctypes.c_uint64)
SCRIPT = ctypes.CFUNCTYPE(None, ctypes.POINTER(Ctx))
class FsHost(ctypes.Structure): _fields_ = [('call', CALL), ('log', LOG), ('ctx', CTX), ('call_addr', CALL_ADDR)]
class FsOverride(ctypes.Structure): _fields_ = [('off', ctypes.c_uint32), ('name', ctypes.c_char_p), ('fn', SCRIPT)]
class FsSym(ctypes.Structure): _fields_ = [('name', ctypes.c_char_p), ('spec', ctypes.c_char_p)]
class FsTable(ctypes.Structure):
    _fields_ = [('syms', ctypes.POINTER(FsSym)), ('sym_count', ctypes.c_int), ('addr', ctypes.POINTER(ctypes.c_uint64)),
                ('ovr', ctypes.POINTER(FsOverride)), ('ovr_count', ctypes.c_int)]
ctx = Ctx()
reported = []
def call(c, slot):
    if slot == VALUE:
        c[0].v[0].d[0], c[0].v[0].d[1] = VALUES[c[0].x[0]], 0
    elif slot == REPORT:
        reported.append(c[0].v[0].d[0] & 0xffffffff)
    else:
        c[0].x[0] = 0
host = FsHost(CALL(call), LOG(lambda t: None), CTX(lambda: ctypes.addressof(ctx)), CALL_ADDR(lambda c, a: None))
table = FsTable()
lib.fighter_bind.argtypes = [ctypes.POINTER(FsHost), ctypes.POINTER(FsTable)]
lib.fighter_bind(ctypes.byref(host), ctypes.byref(table))
agent = (ctypes.c_uint8 * 0x100)()
ctx.x[0] = ctypes.addressof(agent)
for i in range(table.ovr_count):
    print('script', table.ovr[i].name.decode(), hex(table.ovr[i].off), table.sym_count)
table.ovr[0].fn(ctypes.byref(ctx))
print('reported', ' '.join('%08x' % r for r in reported))
'''

# mod_init of the example mod with a made-up hook table; then the replacement it installed runs once.
MOD_HARNESS = r'''
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
class V(ctypes.Structure): _fields_ = [('d', ctypes.c_uint64 * 2)]
class Ctx(ctypes.Structure): _fields_ = [('x', ctypes.c_uint64 * 32), ('v', V * 32)]
HOOK = ctypes.CFUNCTYPE(None, ctypes.POINTER(Ctx), ctypes.c_void_p)
ORIG = ctypes.CFUNCTYPE(None, ctypes.POINTER(Ctx))
INSTALL = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))
INSTALL_NAME = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))
ID = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int)
CALLS = ctypes.CFUNCTYPE(ctypes.c_longlong, ctypes.c_int)
ADDRESS = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_char_p)
CALL = ctypes.CFUNCTYPE(None, ctypes.POINTER(Ctx), ctypes.c_uint64)
LOG = ctypes.CFUNCTYPE(None, ctypes.c_char_p)
ERROR = ctypes.CFUNCTYPE(ctypes.c_char_p)
class HookApi(ctypes.Structure):
    _fields_ = [('size', ctypes.c_uint32), ('version', ctypes.c_uint32), ('install', INSTALL), ('install_name', INSTALL_NAME),
                ('remove', ID), ('enable', ID), ('calls', CALLS), ('address', ADDRESS), ('call', CALL), ('log', LOG), ('error', ERROR)]
hooks, origs = [], []
def original(c):
    origs.append(c[0].x[0])
orig = ORIG(original)
def install_name(name, fn, user, out):
    hooks.append(fn)
    out[0] = ctypes.cast(orig, ctypes.c_void_p).value
    return len(hooks)
api = HookApi(ctypes.sizeof(HookApi), 1, INSTALL(lambda *a: 0), INSTALL_NAME(install_name), ID(lambda i: 1), ID(lambda i: 1),
              CALLS(lambda i: 0), ADDRESS(lambda n: 0), CALL(lambda c, a: None), LOG(lambda t: print('log', t.decode())),
              ERROR(lambda: b''))
lib.mod_init.argtypes = [ctypes.POINTER(HookApi)]
lib.mod_init(ctypes.byref(api))
print('hooks', len(hooks))
ctx = Ctx()
ctx.x[0] = 0x1234
HOOK(hooks[0])(ctypes.byref(ctx), None)
print('original', ' '.join(hex(o) for o in origs))
'''


def fixed_slots():
    """gen_sdk.py's fixed slot names (read from its source: importing it needs numpy)."""
    tree = ast.parse((TOOLS / 'gen_sdk.py').read_text(encoding='utf-8'))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, 'id', None) == 'FIXED' for t in node.targets):
            return [e[0] for e in ast.literal_eval(node.value)]
    raise AssertionError('gen_sdk.py has no FIXED')


def made_up_fighter(work):
    """fighters/madeup/ and a made-up fighters/sdk_generated/ (what gen_sdk.py writes from the game, here two
    made-up game functions)."""
    slots = fixed_slots() + ['S_madeup_value', 'S_madeup_report']
    gen = os.path.join(work, 'fighters', 'sdk_generated')
    write(os.path.join(gen, 'lua_bind.h'),
          '#pragma once\nenum FsSlot : int {\n' + ''.join('    %s,\n' % s for s in slots) + '    S_COUNT\n};\n'
          'namespace madeup {\ninline float value(int32_t i) { return gcall<float>(S_madeup_value, i); }\n'
          'inline void report(float v) { gcall<void>(S_madeup_report, v); }\n}\n')
    write(os.path.join(gen, 'lua_const.h'), '#pragma once\ninline LuaConst lua_const_at(int32_t index) { return LuaConst{index}; }\n')
    write(os.path.join(gen, 'fs_syms.inc'), ''.join('    { "made_up_%s", "" },\n' % s[2:] for s in slots))
    write(os.path.join(work, 'fighters', 'madeup', 'generated', 'bindings.inc'), 'ACMD_BIND(madeup, game_fp, 0x100)\n')
    write(os.path.join(work, 'fighters', 'madeup', 'acmd', 'game.cpp'), FIGHTER_SOURCE)
    return len(slots) - 2, len(slots) - 1


def mac_toolchain():
    try:
        return fighter_cc.toolchain('macos')
    except (Exception, SystemExit):
        return None


@unittest.skipUnless(DARWIN and mac_toolchain(), "needs a Mac with the Command Line Tools (the build runs where it was checked)")
class MacBuildTests(unittest.TestCase):
    """build_fighter.py and build_mod.py with the game's compiler (tools/mac/toolchain.py), and the results run."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='fighter-sdk-test-')
        self.work = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def build(self):
        r = run_tool('build_fighter.py', 'madeup', env={'SSBU_FIGHTER_WORK': self.work})
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-3000:])
        return r.stdout

    def run_fighter(self, lib, value, report):
        r = subprocess.run([sys.executable, '-c', FIGHTER_HARNESS, lib, str(value), str(report)], capture_output=True,
                           text=True, timeout=60, env={**os.environ, **QUIET})
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        return r.stdout

    def test_fighter(self):
        value, report = made_up_fighter(self.work)
        out = self.build()
        self.assertIn('madeup: compiler ', out)
        self.assertIn('2 source files, 2 compiled', out)
        lib = os.path.join(self.work, 'fighters_bin', 'madeup.dylib')
        self.assertTrue(os.path.isfile(lib))
        stamp = json.loads(Path(self.work, 'obj_fighters', 'madeup', 'compile.json').read_text())
        at = stamp.index('<source>')
        self.assertEqual(stamp[at - 2:at], ['-fno-fast-math', '-ffp-contract=off'])
        self.assertTrue(in_order(kept('macos', fighter_cc.load_root('macos')), stamp))
        names = subprocess.run(['nm', '-gU', lib], capture_output=True, text=True).stdout.split()
        self.assertIn('_fighter_bind', names)            # FS_EXPORT: visible although the game's flags hide symbols
        self.assertIn('script game_fp 0x100', self.run_fighter(lib, value, report))
        self.assertIn('reported 00000000', self.run_fighter(lib, value, report))     # rounded apart, as the game does
        # nothing changed: nothing compiled; another command: everything compiled again
        self.assertIn('2 source files, 0 compiled', self.build())
        Path(self.work, 'obj_fighters', 'madeup', 'compile.json').write_text(json.dumps([a for a in stamp if a != '-fno-fast-math']))
        out = self.build()
        self.assertIn('madeup: compiler ', out)
        self.assertIn('2 source files, 2 compiled', out)
        self.assertEqual(json.loads(Path(self.work, 'obj_fighters', 'madeup', 'compile.json').read_text()), stamp)

    @unittest.skipUnless(ARM64, 'clang fuses on its own on arm64 (on x86-64 only with -mfma)')
    def test_fused_without_the_pin(self):
        # the control: the same fighter compiled without -ffp-contract=off reports the fused result, so
        # test_fighter's 0 says the pin reached the compiler
        value, report = made_up_fighter(self.work)
        self.build()
        tc = fighter_cc.toolchain('macos')
        stamp = json.loads(Path(self.work, 'obj_fighters', 'madeup', 'compile.json').read_text())
        objs = []
        for src in (SDK / 'src' / 'fighter_rt.cpp', Path(self.work, 'fighters', 'madeup', 'acmd', 'game.cpp')):
            obj = os.path.join(self.work, src.stem + '.control.o')
            cmd = [obj if a == '<object>' else str(src) if a == '<source>' else a for a in stamp if a != '-ffp-contract=off']
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            self.assertEqual(r.returncode, 0, r.stderr[-2000:])
            objs.append(obj)
        lib = os.path.join(self.work, 'control.dylib')
        r = subprocess.run(tc.link_command(objs, lib), capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        self.assertIn('reported 33800000', self.run_fighter(lib, value, report))

    def test_mod(self):
        out = os.path.join(self.work, 'mods')
        r = run_tool('build_mod.py', str(SDK / 'examples' / 'mod_example.cpp'), out)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-3000:])
        lib = os.path.join(out, 'mod_example.dylib')
        self.assertEqual(os.listdir(out), ['mod_example.dylib'])
        names = subprocess.run(['nm', '-gU', lib], capture_output=True, text=True).stdout.split()
        self.assertIn('_mod_init', names)
        r = subprocess.run([sys.executable, '-c', MOD_HARNESS, lib], capture_output=True, text=True, timeout=60,
                           env={**os.environ, **QUIET})
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        self.assertIn('hooks 1', r.stdout)
        self.assertIn('log [mod] jump squat number 1 (agent 1234)', r.stdout)
        self.assertIn('original 0x1234', r.stdout)            # the hook ran the function it replaced


if __name__ == '__main__':
    unittest.main()
