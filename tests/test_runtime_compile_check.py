"""tools/dev/runtime_compile_check.py without compiling anything: the made-up game's tree it lays out
(tools/mac/boottest's modules), the build's commands on each system, the runtime sources a program
must have (by its link's list, against LEFT_OUT, and none in a folder no build looks in), and how a
run ends (a failed step, a build without its program or without a source, the line for the job's
page). The real build runs in CI (.github/workflows/runtime-compile.yml) on Windows, Linux and macOS."""
from pathlib import Path
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools' / 'dev'))
import runtime_compile_check as check  # noqa: E402

TOOLS = str(ROOT / 'tools')
BOOTTEST = ROOT / 'tools' / 'mac' / 'boottest'


class Folder(unittest.TestCase):
    def setUp(self):
        self.top = tempfile.mkdtemp(prefix='runtime check ')
        self.addCleanup(shutil.rmtree, self.top, ignore_errors=True)
        self.tree = os.path.join(self.top, 'made-up game')


class Tree(Folder):
    def test_the_tree_is_boottests_game(self):
        check.make_tree(self.tree)
        for m in ('main', 'subsdk0', 'sdk'):
            for name in ('g0000.c', 'g_module.c'):
                self.assertEqual(Path(self.tree, 'build', m, name).read_text(), (BOOTTEST / m / name).read_text())
            rows = [line.split('\t') for line in Path(self.tree, 'build', m, 'manifest.tsv').read_text().splitlines()]
            self.assertEqual([r[0] for r in rows], ['g0000.c'])
            self.assertTrue(os.path.isfile(os.path.join(self.tree, 'dump', 'modules', m + '.reloc.bin')))

    def test_laid_out_again_the_files_keep_their_dates(self):
        check.make_tree(self.tree)
        src = os.path.join(self.tree, 'build', 'main', 'g0000.c')
        os.utime(src, (1_000_000_000, 1_000_000_000))
        check.make_tree(self.tree)   # (an older date than its object's: not compiled again)
        self.assertEqual(os.path.getmtime(src), 1_000_000_000)


class Steps(unittest.TestCase):
    tree = os.path.join('top', 'made-up game')

    def dirs(self):
        return [os.path.join(self.tree, 'build', m) for m in ('main', 'subsdk0', 'sdk')]

    def test_windows_compiles_each_module_then_links_with_the_runtime(self):
        recomp = os.path.join(TOOLS, 'recomp.py')
        exe = os.path.join(self.tree, 'build', 'ssbu.exe')
        link = ['py', recomp, 'link', '--dirs', ','.join(self.dirs()), '--out', exe]
        self.assertEqual(check.steps(self.tree, 'windows', '6', python='py'),
                         [(f'compile {m}', ['py', recomp, 'compile', d, '--jobs', '6'])
                          for m, d in zip(('main', 'subsdk0', 'sdk'), self.dirs())] + [('compile the runtime and link', link)])
        self.assertEqual(check.program(self.tree, 'windows'), exe)

    def test_linux_builds_vkd3d_shader_only_when_it_is_not_ready(self):
        build = ('compile and link', ['py', os.path.join(TOOLS, 'linux', 'build_linux.py'), '--port', self.tree,
                                      '--out', os.path.join(self.tree, 'build-linux'), '--jobs', '3'])
        self.assertEqual(check.steps(self.tree, 'linux', 3, vkd3d_ready=True, python='py'), [build])
        self.assertEqual(check.steps(self.tree, 'linux', 3, vkd3d_ready=False, python='py'),
                         [('vkd3d-shader', ['py', os.path.join(TOOLS, 'linux', 'build_vkd3d.py'), 'linux']), build])
        self.assertEqual(check.program(self.tree, 'linux'), os.path.join(self.tree, 'build-linux', 'ssbu'))

    def test_macos_builds_the_app_in_the_tree(self):
        out = os.path.join(self.tree, 'build-mac')
        self.assertEqual(check.steps(self.tree, 'macos', 3, python='py'),
                         [('compile and link', ['py', os.path.join(TOOLS, 'mac', 'build_mac.py'), '--port', self.tree,
                                                '--out', out, '--jobs', '3'])])
        self.assertEqual(check.program(self.tree, 'macos'), os.path.join(out, 'WindowSmash.app', 'Contents', 'MacOS', 'ssbu'))

    def test_without_jobs_each_build_takes_its_own(self):
        for host in ('windows', 'linux', 'macos'):
            for _what, command in check.steps(self.tree, host, None, vkd3d_ready=False):
                self.assertNotIn('--jobs', command)


def complete_link(tree, host, drop=()):
    """A link list with an object of every source of the real runtime/ the host's program should have
    (as the builds name them), less those in drop; with the made-up game's and a generated table's."""
    rt, objs = check.runtime_folder(tree, host), [os.path.join(tree, 'build', 'main', 'g0000.o')]
    for rel in check.runtime_sources(check.RUNTIME):
        folder, _, name = rel.rpartition('/')
        if rel in check.LEFT_OUT[host] or rel in drop or (folder and folder not in check.PLATFORMS[host]):
            continue
        objs.append(os.path.join(rt, folder if host == 'macos' else '', os.path.splitext(name)[0] + '.o'))
    return objs + [os.path.join(rt, 'hle_table.o')]


class Sources(Folder):
    """source_problems on a made-up runtime/ (and LEFT_OUT), linked() and link_list()."""
    LEFT_OUT = {'windows': {'smoke.c': 'a test'}, 'linux': {'smoke.c': 'a test', 'gone.c': 'removed'},
                'macos': {'smoke.c': 'a test', 'linux/window.c': 'Linux only'}}

    def setUp(self):
        super().setUp()
        self.runtime = os.path.join(self.top, 'runtime')
        for rel in ('host.c', 'smoke.c', 'new.c', 'host.h', '._host.c', 'linux/win32.c', 'linux/window.c', 'linux/eh.cpp',
                    'macos/fiber.c', 'macos/fiber.S', 'macos/window.m', 'core/moved.c', 'linux/include/stray.c',
                    'macos/include/windows.h'):
            os.makedirs(os.path.dirname(os.path.join(self.runtime, rel)), exist_ok=True)
            Path(self.runtime, rel).write_text('')
        patch = mock.patch.object(check, 'LEFT_OUT', self.LEFT_OUT)
        patch.start()
        self.addCleanup(patch.stop)

    def problems(self, host, have):
        return check.source_problems(self.runtime, host, set(have))

    MOVED = [f'runtime/{rel}: {check.MISPLACED}' for rel in ('core/moved.c', 'linux/include/stray.c')]

    def not_in(self, host, rel):
        return (f'runtime/{rel}: not in the {host} program: add it to the build\'s runtime sources, or to LEFT_OUT in '
                'tools/dev/runtime_compile_check.py with why')

    def test_the_sources_listed(self):
        self.assertEqual(check.runtime_sources(self.runtime),
                         ['host.c', 'new.c', 'smoke.c', 'core/moved.c', 'linux/eh.cpp', 'linux/win32.c', 'linux/window.c',
                          'linux/include/stray.c', 'macos/fiber.S', 'macos/fiber.c', 'macos/window.m'])

    def test_linux_has_its_folder_and_the_top(self):
        have = [('', 'host'), ('', 'new'), ('', 'win32'), ('', 'window'), ('', 'eh')]
        self.assertEqual(self.problems('linux', have),
                         self.MOVED + ['runtime/gone.c: in LEFT_OUT for linux, but there is no such file: take it off LEFT_OUT'])
        self.assertEqual(self.problems('linux', [h for h in have if h != ('', 'window')]),
                         self.MOVED + [self.not_in('linux', 'linux/window.c'),
                                       'runtime/gone.c: in LEFT_OUT for linux, but there is no such file: take it off LEFT_OUT'])

    def test_a_source_left_out_on_purpose_must_be_left_out(self):
        self.assertEqual(self.problems('windows', [('', 'host'), ('', 'new'), ('', 'smoke')]),
                         self.MOVED + ['runtime/smoke.c: in the windows program, though LEFT_OUT says it is left out (a test): '
                                       'take it off LEFT_OUT'])

    def test_windows_has_only_the_top(self):
        self.assertEqual(self.problems('windows', [('', 'host')]), self.MOVED + [self.not_in('windows', 'new.c')])

    def test_macos_by_folder_and_with_two_sources_of_one_name(self):
        have = [('', 'host'), ('', 'new'), ('linux', 'win32'), ('linux', 'eh'), ('macos', 'fiber_c'), ('macos', 'fiber_S'),
                ('macos', 'window')]
        self.assertEqual(self.problems('macos', have), self.MOVED)
        # (runtime/linux's objects in a folder of their own: one of that name at the top is another source's)
        moved = [h for h in have if h not in (('macos', 'fiber_S'), ('linux', 'win32'))] + [('', 'win32')]
        self.assertEqual(self.problems('macos', moved),
                         self.MOVED + [self.not_in('macos', 'linux/win32.c'), self.not_in('macos', 'macos/fiber.S')])

    def test_linked_are_the_runtime_folders_objects(self):
        rt = os.path.join(self.top, 'rt')
        objs = [os.path.join(rt, 'host.obj'), os.path.join(rt, 'macos', 'fiber_S.o'), os.path.join(self.top, 'main', 'g0000.obj'),
                os.path.join(self.top, 'rt2', 'x.o'), rt]
        self.assertEqual(check.linked(objs, rt), {('', 'host'), ('macos', 'fiber_S')})

    def test_link_lists(self):
        tree = self.tree
        self.assertIsNone(check.link_list(tree, 'windows'))
        exe = check.program(tree, 'windows')
        os.makedirs(os.path.dirname(exe))
        Path(exe + '.rsp').write_text('"C:\\made up\\rt\\host.obj"\n"C:\\made up\\main\\g0000.obj"')   # (cc.link's)
        self.assertEqual(check.link_list(tree, 'windows'), ['C:\\made up\\rt\\host.obj', 'C:\\made up\\main\\g0000.obj'])
        os.makedirs(os.path.join(tree, 'build-linux'))
        Path(tree, 'build-linux', 'ssbu.rsp').write_text('"/made up/rt/host.o"\n"/made up/main/g0000.o"')   # (build_linux.py's)
        self.assertEqual(check.link_list(tree, 'linux'), ['/made up/rt/host.o', '/made up/main/g0000.o'])
        os.makedirs(os.path.join(tree, 'build-mac'))
        Path(tree, 'build-mac', 'ssbu.filelist').write_text('/made up/obj/rt/host.o\n/made up/obj/main/g0000.o\n')   # (build_mac's)
        self.assertEqual(check.link_list(tree, 'macos'), ['/made up/obj/rt/host.o', '/made up/obj/main/g0000.o'])


class LeftOut(unittest.TestCase):
    """The real LEFT_OUT, against the real runtime/."""
    def test_each_file_left_out_is_there_with_why(self):
        sources = set(check.runtime_sources(check.RUNTIME))
        for host, left_out in check.LEFT_OUT.items():
            for rel, why in left_out.items():
                self.assertIn(rel, sources, host)
                self.assertIn(rel.rpartition('/')[0], ('',) + check.PLATFORMS[host], (host, rel))
                self.assertTrue(why.strip(), (host, rel))

    def test_no_source_is_where_no_build_looks(self):
        self.assertEqual(check.misplaced(check.runtime_sources(check.RUNTIME)), [])


class Run(Folder):
    def main(self, argv, host, made=True, fail=None, isa=None, link=complete_link):
        """main with each build step recorded rather than run: the last one makes the program (made) and
        the link list (link(tree, host), or none), the one named `fail` fails; isa: what the made-up link's
        isa.txt says."""
        self.ran = []

        def run(command, cwd):
            self.ran.append((command, cwd))
            if fail and fail in command[1]:
                return subprocess.CompletedProcess(command, 1)
            if made and len(self.ran) == len(check.steps(self.tree, host, None, vkd3d_ready=False)):
                exe = check.program(self.tree, host)
                os.makedirs(os.path.dirname(exe), exist_ok=True)
                Path(exe).write_bytes(b'\0' * 4096)
                if isa:
                    Path(os.path.dirname(exe), 'isa.txt').write_text(isa + '\nchosen\n')
                if link:
                    listed = {'windows': exe + '.rsp', 'linux': os.path.join(self.tree, 'build-linux', 'ssbu.rsp'),
                              'macos': os.path.join(self.tree, 'build-mac', 'ssbu.filelist')}[host]
                    Path(listed).write_text('\n'.join(f'"{o}"' if host != 'macos' else o for o in link(self.tree, host)))
            return subprocess.CompletedProcess(command, 0)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), mock.patch.object(check, 'vkd3d_ready', return_value=False):
            code = check.main(argv, run=run, host=host)
        return code, out.getvalue()

    def test_the_steps_run_in_order_from_the_checkout(self):
        code, out = self.main(['--tree', self.tree], 'linux', isa='x86-64-v2')
        self.assertEqual(code, 0, out)
        self.assertEqual([c for c, _cwd in self.ran], [c for _w, c in check.steps(self.tree, 'linux', None, vkd3d_ready=False)])
        self.assertEqual({cwd for _c, cwd in self.ran}, {check.REPO})
        self.assertIn('linux, x86-64-v2: the runtime compiled and linked into ssbu', out)
        self.assertTrue(os.path.isfile(os.path.join(self.tree, 'build', 'main', 'g0000.c')))

    def test_a_failed_step_stops_the_run(self):
        code, out = self.main(['--tree', self.tree, '--jobs', '2'], 'windows', fail='recomp.py')
        self.assertEqual(code, 1)
        self.assertEqual(len(self.ran), 1)
        self.assertIn('FAILED: compile main (exit status 1)', out)

    def test_a_build_that_ends_without_its_program_fails(self):
        code, out = self.main(['--tree', self.tree], 'macos', made=False)
        self.assertEqual(code, 1)
        self.assertIn('FAILED: the build ended without', out)

    def test_the_line_for_the_jobs_page(self):
        summary = os.path.join(self.top, 'summary.md')
        with mock.patch.dict(os.environ, {'GITHUB_STEP_SUMMARY': summary}):
            code, out = self.main(['--tree', self.tree], 'macos')
        self.assertEqual(code, 0, out)
        line = Path(summary).read_text()
        self.assertRegex(line, r'^macos, arm64: the runtime compiled and linked into ssbu \(0\.0 MB\) in \d+ s, every source in it '
                               r'but those left out on purpose: difftest\.c, .*linux/window\.c\n$')
        self.assertIn(line.strip(), out)

    def test_a_program_without_a_source_fails(self):
        summary = os.path.join(self.top, 'summary.md')
        for host in ('windows', 'linux', 'macos'):
            with self.subTest(host), mock.patch.dict(os.environ, {'GITHUB_STEP_SUMMARY': summary}):
                code, out = self.main(['--tree', self.tree], host, link=lambda tree, h: complete_link(tree, h, drop=('host.c',)))
                self.assertEqual(code, 1, out)
                problem = f'FAILED ({host}): runtime/host.c: not in the {host} program'
                self.assertIn(problem, out)
                self.assertIn(problem, Path(summary).read_text())

    def test_a_program_without_a_link_list_fails(self):
        code, out = self.main(['--tree', self.tree], 'linux', link=None)
        self.assertEqual(code, 1, out)
        self.assertIn('FAILED (linux): the build left no list of the objects it linked', out)

    def test_not_in_this_checkout(self):
        code, out = self.main(['--tree', check.REPO], 'macos')
        self.assertEqual((code, self.ran), (2, []))
        self.assertIn("this checkout, whose build/ is the real game's", out)

    def test_jobs_is_a_number_of_files(self):
        for jobs in ('many', '0'):
            code, out = self.main(['--tree', self.tree, '--jobs', jobs], 'macos')
            self.assertEqual((code, self.ran), (2, []))
            self.assertIn(f'--jobs {jobs}: not a number of files', out)
        self.assertFalse(os.path.exists(self.tree))


if __name__ == '__main__':
    unittest.main()
