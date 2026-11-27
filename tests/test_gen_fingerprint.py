"""What an update builds again (tools/modcache.py, tools/recomp.py, tools/build_game.py, tools/skyline.py), without
game files:
  - the generator's fingerprint goes by what its sources do (SSBU_GEN_FINGERPRINT): a comment, a docstring, the
    layout or recomp.py's compile and link steps (modcache.DRIVER_ONLY, which nothing the generator runs reaches)
    write no module's C again; code the generator runs does; a stamp from the fingerprint by the bytes stays current
    while those bytes are today's;
  - a release install asks recomp.py for the release's prebuilt runtime first (SSBU_PREBUILT_RUNTIME);
  - the commit file the updater writes at every update, and the launchers' own tools, are no reason to build
    (SSBU_UPDATE_GATE)."""
import ast
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import build_game  # noqa: E402
import modcache  # noqa: E402
import recomp  # noqa: E402

# what the generator runs, from which no DRIVER_ONLY function may be reached
GENERATION = ('build', '_build', '_generate', '_emit', '_init', 'image', 'hook_imports', 'tools_fingerprint',
              'module_fingerprint', 'module_fingerprints', 'c_current', 'build_all')


def top_level(source):
    tree = ast.parse(source)
    return {n.name: n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))}


class DriverOnly(unittest.TestCase):
    def test_each_is_a_function_of_recomp_py(self):
        defs = top_level((ROOT / 'tools' / 'recomp.py').read_text(encoding='utf-8'))
        for name in modcache.DRIVER_ONLY['recomp.py']:
            self.assertIn(name, defs, f'{name} is not a function of recomp.py: take it out of modcache.DRIVER_ONLY')

    def test_nothing_the_generator_runs_reaches_them(self):
        """A DRIVER_ONLY function is left out of the fingerprint: one the generator ran (directly or through another)
        could change the C without a new fingerprint. build_all calls compile_dirs and link after the C is written: it
        is in the fingerprint itself, so its own calls of them are not followed."""
        defs = top_level((ROOT / 'tools' / 'recomp.py').read_text(encoding='utf-8'))
        driver = set(modcache.DRIVER_ONLY['recomp.py'])
        refs = {k: {x.id for x in ast.walk(v) if isinstance(x, ast.Name)} & set(defs) for k, v in defs.items()}
        seen, todo = set(), [r for r in GENERATION if r != 'build_all']
        while todo:
            n = todo.pop()
            if n not in seen:
                seen.add(n)
                todo += refs.get(n, ())
        self.assertEqual(seen & driver, set())
        for name in GENERATION:
            self.assertNotIn(name, driver)


class Meaning(unittest.TestCase):
    CODE = 'X = 1\n\ndef f(a):\n    """says what f does"""\n    return a + X  # (a comment)\n'

    def test_comments_docstrings_and_layout_are_not_in_it(self):
        base = modcache.meaning('lift.py', self.CODE.encode())
        for same in ('"""a module docstring"""\nX = 1\n\n\ndef f(a):\n    return a + X\n',
                     'X = 1\ndef f(a):\n    """another word"""\n    # a new comment\n    return (a +\n            X)\n',
                     self.CODE.replace('\n', '\r\n')):
            self.assertEqual(modcache.meaning('lift.py', same.encode()), base, same)
        for other in (self.CODE.replace('a + X', 'a - X'), self.CODE.replace('X = 1', 'X = 2'),
                      self.CODE + 'def g():\n    pass\n'):
            self.assertNotEqual(modcache.meaning('lift.py', other.encode()), base, other)

    def test_recomp_pys_driver_functions_are_not_in_it(self):
        code = self.CODE + '\ndef link(dirs):\n    return 1\n'
        base = modcache.meaning('recomp.py', code.encode())
        self.assertEqual(modcache.meaning('recomp.py', code.replace('return 1', 'return 2').encode()), base)
        self.assertNotEqual(modcache.meaning('lift.py', code.replace('return 1', 'return 2').encode()),
                            modcache.meaning('lift.py', code.encode()))   # (another file's link is its own)
        self.assertNotEqual(modcache.meaning('recomp.py', code.replace('a + X', 'a * X').encode()), base)

    def test_a_source_that_does_not_parse_is_its_bytes(self):
        self.assertEqual(modcache.meaning('lift.py', b'def (\r\n'), b'def (\n')


class Fingerprint(unittest.TestCase):
    def setUp(self):
        self.tools = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tools)
        for n in modcache.GENERATOR:
            shutil.copy(ROOT / 'tools' / n, self.tools / n)
        patch = mock.patch.object(modcache, 'TOOLS', str(self.tools))
        patch.start()
        self.addCleanup(patch.stop)

    def edit(self, name, old, new):
        path = self.tools / name
        text = path.read_text(encoding='utf-8')
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding='utf-8', newline='\n')

    def both(self):
        return modcache.fingerprint('extra', legacy=False), modcache.fingerprint('extra', legacy=True)

    def test_the_bytes_way_is_as_before(self):
        import hashlib
        h = hashlib.sha256()
        for n in modcache.GENERATOR:
            h.update(n.encode() + b'\0' + (self.tools / n).read_bytes().replace(b'\r\n', b'\n') + b'\0')
        h.update(b'extra')
        self.assertEqual(modcache.fingerprint('extra', legacy=True), h.hexdigest()[:16])
        with mock.patch.dict(os.environ, {'SSBU_GEN_FINGERPRINT': '0'}):
            self.assertEqual(modcache.fingerprint('extra'), h.hexdigest()[:16])
        with mock.patch.dict(os.environ, {'SSBU_GEN_FINGERPRINT': '1'}):
            self.assertNotEqual(modcache.fingerprint('extra'), h.hexdigest()[:16])

    def test_a_link_step_edit_writes_no_c_again(self):
        meaning, legacy = self.both()
        self.edit('recomp.py', 'def link(dirs, exe, rt_dir=None, steps=None):\n',
                  'def link(dirs, exe, rt_dir=None, steps=None):\n    print("linking")\n')
        self.assertEqual(self.both()[0], meaning)
        self.assertNotEqual(self.both()[1], legacy)   # (as before: every module's C written again)

    def test_a_comment_writes_no_c_again(self):
        meaning, legacy = self.both()
        self.edit('lift.py', '\n', '\n# a comment that changes nothing\n')
        self.assertEqual(self.both()[0], meaning)
        self.assertNotEqual(self.both()[1], legacy)

    def test_the_generators_code_does(self):
        meaning, _legacy = self.both()
        self.edit('recomp.py', 'def write_if_changed(path, text):\n', 'def write_if_changed(path, text):\n    text = text + ""\n')
        self.assertNotEqual(self.both()[0], meaning)
        meaning = self.both()[0]
        self.edit('a64.py', '\n', '\nA_NEW_NAME = 1\n')
        self.assertNotEqual(self.both()[0], meaning)

    def test_the_extra_part(self):
        self.assertNotEqual(modcache.fingerprint('extra', legacy=False), modcache.fingerprint('other', legacy=False))


class Stamps(unittest.TestCase):
    """build_all's check of a module folder's stamp (recomp.c_current with recomp.module_fingerprints' pair)."""
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir)

    def test_either_fingerprint(self):
        modcache.write_stamp(str(self.dir), 'AB12', recomp.MODULES['main'], 'main', 'old0')
        self.assertTrue(recomp.c_current(str(self.dir), 'AB12', 'main', ('new0', ('old0',))))
        self.assertTrue(recomp.c_current(str(self.dir), 'AB12', 'main', ('old0', ())))
        self.assertFalse(recomp.c_current(str(self.dir), 'AB12', 'main', ('new0', ())))
        self.assertFalse(recomp.c_current(str(self.dir), 'CD34', 'main', ('old0', ())))   # (another build id)
        self.assertFalse(recomp.c_current(str(self.dir), 'AB12', 'sdk', ('old0', ())))
        self.assertFalse(recomp.c_current(str(self.dir / 'none'), 'AB12', 'main', ('old0', ())))

    def test_the_pair(self):
        with mock.patch.object(recomp, 'tools_fingerprint', lambda legacy=None: 'bytes' if legacy else 'meaning'), \
                mock.patch.object(recomp, 'module_fingerprint', lambda fp, name, legacy=None: f'{fp}-{name}'):
            with mock.patch.dict(os.environ, {'SSBU_GEN_FINGERPRINT': '1'}):
                self.assertEqual(recomp.module_fingerprints(['main']), {'main': ('meaning-main', ('bytes-main',))})
            with mock.patch.dict(os.environ, {'SSBU_GEN_FINGERPRINT': '0'}):
                self.assertEqual(recomp.module_fingerprints(['main']), {'main': ('meaning-main', ())})


class PrebuiltRuntime(unittest.TestCase):
    """build_game.prebuilt_runtime_first: the release whose runtime recomp.py's link takes when only it changed."""
    def test_a_release_install(self):
        with mock.patch.object(build_game, 'LINUX', False), mock.patch.object(build_game, 'MACOS', False):
            env = {'OPENSMASH_RELEASE_TAG': 'v0.5.4'}
            self.assertEqual(build_game.prebuilt_runtime_first(True, env), 'v0.5.4')
            self.assertEqual(env['SSBU_PREBUILT_RUNTIME'], 'v0.5.4')
            for dll, env in ((False, {'OPENSMASH_RELEASE_TAG': 'v0.5.4'}), (True, {}), (True, {'OPENSMASH_RELEASE_TAG': 'main'}),
                             (True, {'OPENSMASH_RELEASE_TAG': 'v0.5.4', 'SSBU_PREBUILT_RUNTIME': '0'})):
                self.assertIsNone(build_game.prebuilt_runtime_first(dll, env), env)
                self.assertNotEqual(env.get('SSBU_PREBUILT_RUNTIME'), 'v0.5.4')
            self.assertEqual(build_game.prebuilt_runtime_first(True, {'SSBU_PREBUILT_RUNTIME': 'v0.5.3'}), 'v0.5.3')
        for platform in ('LINUX', 'MACOS'):
            with mock.patch.object(build_game, platform, True):
                self.assertIsNone(build_game.prebuilt_runtime_first(True, {'OPENSMASH_RELEASE_TAG': 'v0.5.4'}))


class UpdateGate(unittest.TestCase):
    """tools/skyline.py's needs_build and game_link_reason go by NOT_BUILT: the commit file the updater writes last at
    every update is no reason to build, nor are the launchers' and releases' own tools; the generator and the runtime's
    build are."""
    def load(self, gate):
        import importlib
        with mock.patch.dict(os.environ, {'SSBU_UPDATE_GATE': gate}):
            sys.modules.pop('skyline', None)
            import skyline
            importlib.reload(skyline)
        self.addCleanup(sys.modules.pop, 'skyline', None)
        return skyline

    def test_not_built(self):
        on = self.load('1').NOT_BUILT
        for rel in ('tools/source_version.txt', 'tools/release/make_release.py', 'tools/launcher_fetch.py',
                    'tools/runtime_fetch.py', 'tools/build_avalonia.py', 'tools/rebuild_avalonia_after_update.ps1'):
            self.assertTrue(rel.startswith(on), rel)
            self.assertTrue((ROOT / rel).exists(), rel)
        for rel in ['tools/' + n for n in modcache.GENERATOR] + ['tools/cc.py', 'tools/hooks.txt', 'tools/game_dll.py',
                                                                  'tools/build_game.py', 'runtime/host.c']:
            self.assertFalse(rel.startswith(on), rel)
        off = self.load('0').NOT_BUILT
        self.assertFalse('tools/source_version.txt'.startswith(off))
        self.assertTrue('tools/modmanager/x.py'.startswith(off))


class PluginC(unittest.TestCase):
    """tools/skyline.py c_stale: a plugin's C older than recomp.py's file, but made by a recompiler that does the same
    (a link-step edit), stays current; a change of what it does, or SSBU_GEN_FINGERPRINT=0, makes it stale again."""
    def test_by_meaning(self):
        import skyline
        tools = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tools)
        for n in set(modcache.GENERATOR) | set(skyline.RECOMPILER):
            shutil.copy(ROOT / 'tools' / n, tools / n)
        table = tools / 'plugin' / 'g_module.c'
        table.parent.mkdir()
        table.write_text('/* C */\n')
        old = (tools / 'recomp.py').stat().st_mtime_ns + 10**9
        os.utime(table, ns=(old, old))
        with mock.patch.object(skyline, 'HERE', str(tools)), mock.patch.object(modcache, 'TOOLS', str(tools)), \
                mock.patch.dict(os.environ, {'SSBU_GEN_FINGERPRINT': '1'}):
            self.assertFalse(skyline.c_stale(str(table)))   # (made after the recompiler's files: current, and marked)
            self.assertTrue((table.parent / 'generator.txt').exists())
            text = (tools / 'recomp.py').read_text(encoding='utf-8')
            (tools / 'recomp.py').write_text(text.replace('def link(dirs, exe, rt_dir=None, steps=None):\n',
                                                          'def link(dirs, exe, rt_dir=None, steps=None):\n    # an edit\n', 1),
                                             encoding='utf-8', newline='\n')
            later = old + 5 * 10**9
            os.utime(tools / 'recomp.py', ns=(later, later))
            self.assertFalse(skyline.c_stale(str(table)))   # (older now, by a recompiler that does the same)
            with mock.patch.dict(os.environ, {'SSBU_GEN_FINGERPRINT': '0'}):
                self.assertTrue(skyline.c_stale(str(table)))
            (tools / 'lift.py').write_text((tools / 'lift.py').read_text(encoding='utf-8') + '\nA_NEW_NAME = 1\n',
                                           encoding='utf-8', newline='\n')
            os.utime(tools / 'lift.py', ns=(later, later))
            self.assertTrue(skyline.c_stale(str(table)))   # (what the lifter does changed)
            self.assertTrue(skyline.c_stale(str(tools / 'none' / 'g_module.c')))


class ObjectDate(unittest.TestCase):
    """recomp._without_date: the game library's own two objects are compared without the compile time clang-cl writes
    into their COFF header (the same code compiled again keeps the library), and nothing else is left out."""
    def test_the_header_time_only(self):
        d = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, d)

        def obj(name, head, rest=b'code'):
            (d / name).write_bytes(head + rest)
            return recomp._without_date(str(d / name))
        x64 = bytes((0x64, 0x86, 3, 0))
        self.assertEqual(obj('a.obj', x64 + b'\x01\x02\x03\x04'), obj('b.obj', x64 + b'\x09\x09\x09\x09'))
        self.assertNotEqual(obj('a.obj', x64 + b'\x01\x02\x03\x04'), obj('b.obj', x64 + b'\x01\x02\x03\x04', b'other'))
        big = bytes((0, 0, 0xFF, 0xFF, 2, 0, 0x64, 0x86))
        self.assertEqual(obj('c.obj', big + b'\x01\x02\x03\x04'), obj('d.obj', big + b'\x05\x06\x07\x08'))
        self.assertNotEqual(obj('e.obj', b'ELF!\x01\x02\x03\x04'), obj('f.obj', b'ELF!\x05\x06\x07\x08'))   # (not COFF: as it is)


if __name__ == '__main__':
    unittest.main()
