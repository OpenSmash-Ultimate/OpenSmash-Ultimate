"""tools/build_commit.py (SSBU_BUILD_COMMIT_CHECK): the commit a build was made from, written into the program and the
game library after the link, and tools/skyline.py needs_build / tools/build_game.py build_is_current building again
when it is not the sources' one - a build its dates call current although an update never built it - but not after an
update of only what is not built (#637's fast path)."""
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import build_commit  # noqa: E402
import game_dll  # noqa: E402

A, B, C = 'a' * 40, 'b' * 40, 'c' * 40
UNSET = b'SSBU-BUILD-COMMIT:' + b'?' * 40


def program(path, value=UNSET, pad=1000):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'MZ' + b'x' * pad + value + b'\0' + b'y' * pad)
    return path


class Tree(unittest.TestCase):
    """A made-up install: runtime/ and tools/ files, its sources' commit, build/ssbu.exe."""
    def setUp(self):
        self.tree = Path(tempfile.mkdtemp(prefix='build commit '))
        self.addCleanup(shutil.rmtree, self.tree, True)
        self.env = mock.patch.dict(os.environ, {'SSBU_BUILD_COMMIT_CHECK': '1'})
        self.env.start()
        self.addCleanup(self.env.stop)
        for rel, text in (('runtime/host.c', 'int host;\n'), ('tools/recomp.py', '# the recompiler\n'),
                          ('tools/modmanager/app.py', '# the Mods app\n'), ('tools/launcher.ps1', '# a launcher\n')):
            self.put(rel, text)
        self.sources(A)
        self.exe = program(self.tree / 'build' / 'ssbu.exe')
        self.said = []

    def put(self, rel, text, age=None):
        p = self.tree / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding='utf-8')
        if age is not None:
            t = p.stat().st_mtime - age
            os.utime(p, (t, t))
        return p

    def sources(self, commit):
        self.put('tools/source_version.txt', f'{commit} 2026-11-28\n')

    def reason(self):
        return build_commit.reason(str(self.tree), str(self.exe), log=self.said.append)

    def record(self):
        return build_commit.record(str(self.exe), tree=str(self.tree))


class StampAndRead(Tree):
    def test_written_after_the_link_and_read_back(self):
        self.assertEqual(build_commit.read(str(self.exe)), ('', 2 + 1000 + 18))   # (the place, never written)
        self.assertEqual(self.record(), A)
        at = 2 + 1000 + 18
        self.assertEqual(build_commit.read(str(self.exe)), (A, at))
        self.assertEqual(build_commit.read(str(self.exe), at), (A, at))
        self.assertEqual(build_commit.read(str(self.exe), 5), (A, at))   # (a wrong record: looked for)
        rec = build_commit.load(str(self.exe.parent))
        self.assertEqual((rec['commit'], rec['exe_at']), (A, str(at)))
        self.assertEqual(rec['inputs'], build_commit.inputs(str(self.tree)))
        self.assertEqual(self.exe.read_bytes().count(A.encode()), 1)

    def test_the_library_too_when_linked(self):
        library = program(self.tree / 'build' / 'ssbu_game.dll', pad=50)
        build_commit.record(str(self.exe), str(library), tree=str(self.tree))
        self.assertEqual(build_commit.read(str(library))[0], A)
        self.sources(B)
        build_commit.record(str(self.exe), tree=str(self.tree))   # (a kept library: its own commit stays)
        self.assertEqual(build_commit.read(str(library))[0], A)
        rec = build_commit.load(str(self.exe.parent))
        self.assertEqual((rec['commit'], rec['library']), (B, A))

    def test_a_program_of_before_has_no_place(self):
        self.exe.write_bytes(b'MZ an old runtime')
        self.assertEqual(build_commit.read(str(self.exe)), (None, None))
        self.assertIsNone(self.record())
        self.assertFalse((self.exe.parent / build_commit.RECORD).exists())

    def test_off(self):
        with mock.patch.dict(os.environ, {'SSBU_BUILD_COMMIT_CHECK': '0'}):
            self.assertIsNone(self.record())
            self.assertEqual(build_commit.read(str(self.exe))[0], '')
            self.exe.write_bytes(b'MZ an old runtime')
            self.assertIsNone(self.reason())

    def test_the_sources_commit(self):
        self.assertEqual(build_commit.source_commit(str(self.tree)), A)
        self.put('tools/source_version.txt', '$Format:%H %cs$\n')
        self.assertIsNone(build_commit.source_commit(str(self.tree)))   # (no git here)
        self.assertRegex(build_commit.source_commit(str(ROOT)) or 'a' * 40, '^[0-9a-f]{40}$')


class Reason(Tree):
    def test_the_build_of_the_sources_commit_is_current(self):
        self.record()
        self.assertIsNone(self.reason())

    def test_an_update_whose_build_was_skipped(self):
        """The two PCs' case: the program the dates call current (newer than every source), its commit not the
        sources' and its inputs another: built again, and said."""
        self.record()
        self.sources(B)
        self.put('runtime/host.c', 'int host = 2;\n', age=3600)   # (older than the program)
        self.assertIsNotNone(self.reason())
        self.assertIn('was not built again after an update', self.said[-1])

    def test_an_update_of_only_what_is_not_built(self):
        """#637's fast path (SSBU_UPDATE_GATE): the launcher, the Mods app and the commit file changed, the program's
        files did not: current, and the new commit listed as one it is current for."""
        self.record()
        self.sources(B)
        self.put('tools/modmanager/app.py', '# the Mods app, new\n')
        self.put('tools/launcher.ps1', '# a launcher, new\n')
        self.assertIsNone(self.reason())
        self.assertIn(B, build_commit.load(str(self.exe.parent))['current'])
        self.assertIsNone(self.reason())   # (and again, from the list)
        self.sources(C)
        self.put('runtime/host.c', 'int host = 3;\n')
        self.assertIsNotNone(self.reason())

    def test_a_build_of_before_is_built_once(self):
        self.exe.write_bytes(b'MZ an old runtime, no marker')
        self.assertIn('does not say which commit', self.reason())
        program(self.exe)
        self.assertIsNone(self.reason())   # (never written: the dates decide, never a build every time)

    def test_not_for_other_programs_nor_unknown_sources(self):
        linux = program(self.tree / 'build-linux' / 'ssbu', b'MZ no marker')
        self.assertIsNone(build_commit.reason(str(self.tree), str(linux), log=self.said.append))
        self.exe.write_bytes(b'MZ an old runtime')
        self.put('tools/source_version.txt', '$Format:%H %cs$\n')
        self.assertIsNone(self.reason())


class NeedsBuild(Tree):
    """tools/skyline.py needs_build and tools/build_game.py build_is_current go by it."""
    def test_needs_build(self):
        import skyline
        import std_plugins
        self.record()
        with mock.patch.object(std_plugins, 'PLUGINS', {}), mock.patch.object(skyline, 'plugin_refresh_reason', lambda t: None), \
                mock.patch.object(skyline, 'game_link_reason', lambda t: None):
            self.assertIsNone(skyline.needs_build(str(self.tree)))
            self.sources(B)
            self.put('runtime/host.c', 'int host = 2;\n', age=3600)
            self.assertIn(f'is commit {A[:10]}', skyline.needs_build(str(self.tree)))
            with mock.patch.dict(os.environ, {'SSBU_BUILD_COMMIT_CHECK': '0'}):
                self.assertIsNone(skyline.needs_build(str(self.tree)))   # (the dates, as before: the case it missed)

    def test_build_is_current(self):
        ns = runpy.run_path(str(ROOT / 'tools' / 'build_game.py'), run_name='build_game_commit')['build'].__globals__
        exefs = self.tree / 'dump' / 'exefs'
        self.put('dump/exefs/main', 'main', age=3600)
        for rel in ('runtime/host.c', 'tools/recomp.py'):
            os.utime(self.tree / rel, (self.exe.stat().st_mtime - 3600,) * 2)
        isa = mock.MagicMock()
        isa.built_for.return_value = 'avx2'
        isa.target.return_value = ('avx2', '')
        ns.update(ROOT=str(self.tree), TOOLS=str(self.tree / 'tools'), EXE=str(self.exe), EXEFS=str(exefs),
                  DUMP=str(self.tree / 'dump'), LINUX=False, MACOS=False, log=self.said.append)
        cc = mock.MagicMock(SDL3_DLL=None)
        with mock.patch.dict(sys.modules, {'isa': isa, 'cc': cc}):
            self.record()
            self.assertTrue(ns['build_is_current']())
            self.sources(B)
            self.put('runtime/host.c', 'int host = 2;\n', age=3600)
            self.assertFalse(ns['build_is_current']())
            with mock.patch.dict(os.environ, {'SSBU_BUILD_COMMIT_CHECK': '0'}):
                self.assertTrue(ns['build_is_current']())


class LibraryPlace(unittest.TestCase):
    def test_in_game_tables_unless_off(self):
        runtime = ROOT / 'runtime'
        with mock.patch.dict(os.environ, {'SSBU_BUILD_COMMIT_CHECK': '1'}):
            text = game_dll.tables_text(str(runtime))
        self.assertEqual(text.count('RT_GAME_EXPORT volatile const char rt_game_build_commit[] = "SSBU-BUILD-COMMIT:' + '?' * 40 + '";'), 1)
        with mock.patch.dict(os.environ, {'SSBU_BUILD_COMMIT_CHECK': '0'}):
            self.assertNotIn('rt_game_build_commit', game_dll.tables_text(str(runtime)))


def clang_cl():
    for c in (shutil.which('clang-cl'), str(ROOT / 'deps' / 'llvm' / 'bin' / 'clang-cl.exe')):
        if c and os.path.isfile(c):
            return c
    return None


@unittest.skipUnless(sys.platform == 'win32' and clang_cl(), 'clang-cl on Windows')
class Program(unittest.TestCase):
    """runtime/hle_build_commit.c compiled into a small program: it says no commit until the build writes one, then
    that one, and warns when the sources are at another."""
    def test_said_at_start(self):
        d = Path(tempfile.mkdtemp(prefix='build commit program '))
        self.addCleanup(shutil.rmtree, d, True)
        main = d / 'main.c'
        main.write_text('#include <stdio.h>\nvoid rt_build_commit_boot(void);\nconst char *rt_build_commit(void);\n'
                        'int main(void) { rt_build_commit_boot(); const char *c = rt_build_commit(); printf("report: %s\\n", c ? c : "-"); return 0; }\n')
        exe = d / 'build' / 'ssbu.exe'
        exe.parent.mkdir()
        r = subprocess.run([clang_cl(), '/nologo', '/O2', '/D_CRT_SECURE_NO_WARNINGS', str(main), str(ROOT / 'runtime' / 'hle_build_commit.c'),
                            '/Fe' + str(exe), '/Fo' + str(d) + os.sep], capture_output=True, text=True)
        if r.returncode:
            self.skipTest('no linker here: ' + (r.stdout + r.stderr)[-300:])
        (d / 'tools').mkdir()
        (d / 'tools' / 'source_version.txt').write_text(A + ' 2026-11-28\n')

        def run():
            return subprocess.run([str(exe)], cwd=d, capture_output=True, text=True, timeout=60)
        out = run()
        self.assertIn('[boot] build commit: not recorded; the sources are at aaaaaaaaaa', out.stderr)
        self.assertIn('report: -', out.stdout)
        with mock.patch.dict(os.environ, {'SSBU_BUILD_COMMIT_CHECK': '1'}):
            self.assertIsNotNone(build_commit.stamp(str(exe), A))
        out = run()
        self.assertIn(f'[boot] build commit {A}, the sources\' too', out.stderr)
        self.assertIn(f'report: {A}', out.stdout)
        build_commit.stamp(str(exe), B)
        out = run()
        self.assertIn('WARNING: this build is commit bbbbbbbbbb but the sources are at aaaaaaaaaa', out.stderr)
        (exe.parent / build_commit.RECORD).write_text(f'commit {B}\ncurrent {A}\n')
        self.assertIn('with the same files to build', run().stderr)


if __name__ == '__main__':
    unittest.main()
