"""A plugin's function hook on Windows (tools/skyline.py stale_hooks, copied_hooks): patched in when
the game starts (runtime/hle_detour.c), so no rebuild, unless clang copied the function into a
caller in its file, which the patch does not reach. On a made-up module compiled with clang-cl."""
from pathlib import Path
import json
import os
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import skyline  # noqa: E402

if os.name == 'nt':
    import cc  # noqa: E402
    HAVE = os.path.exists(cc.CLANG)
else:
    HAVE = False

# f_7100000010 is small and called by f_7100000020 in the same file: clang copies it there.
SOURCE = '''typedef struct Cpu { unsigned long long x[4]; } Cpu;
typedef void GuestFn(Cpu *c);
void f_7100000010(Cpu *c) {
    c->x[0] += 1;
}
void f_7100000020(Cpu *c) {
    f_7100000010(c);
    c->x[1] = c->x[0] * 3;
}
void f_7100000030(Cpu *c) {
    c->x[2] ^= c->x[3];
}
'''


@unittest.skipUnless(HAVE, 'Windows with clang-cl only')
class CopiedHooks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.tree = self.tmp.name
        self.main = os.path.join(self.tree, 'build', 'main')
        os.makedirs(self.main)
        os.makedirs(os.path.join(self.tree, 'dump', 'skyline'))
        Path(self.main, 'g0000.c').write_text(SOURCE)
        self.table('')

    def tearDown(self):
        self.tmp.cleanup()

    def table(self, text):
        Path(self.main, 'g_module.c').write_text('#include "module.h"\n' + text)
        os.utime(Path(self.main, 'g_module.c'), ns=(os.stat(Path(self.main, 'g0000.c')).st_mtime_ns + 10**9,) * 2)

    def hooks(self, *offsets):
        Path(self.tree, 'dump', 'skyline', 'hooks.txt').write_text(''.join(f'main {o:x}\n' for o in offsets))

    def test_copied_function_needs_rebuild(self):
        self.hooks(0x10)
        self.assertEqual(skyline.stale_hooks(self.tree), ['main'])
        self.assertEqual(skyline.copied_hooks(self.tree), ['main'])
        saved = json.loads(Path(self.tree, 'build', 'work', 'inlined.json').read_text())
        self.assertIn('7100000010', saved['main/g0000.c']['copied'])

    def test_function_not_copied_needs_none(self):
        self.hooks(0x30)
        self.assertEqual(skyline.stale_hooks(self.tree), [])

    def test_forwarded_function_needs_none(self):
        # (a runtime wrap: the build forwards it, so no rebuild gives it a dispatcher; it asked after every update)
        Path(self.main, 'g0000.c').write_text(SOURCE + 'GuestFn hle_wrap_7100000040;\n'
                                              'void f_7100000040(Cpu *c) { hle_wrap_7100000040(c); }\n'
                                              'void f_7100000050(Cpu *c) { f_7100000040(c); c->x[1] = 2; }\n')
        self.table('')
        self.hooks(0x40)
        self.assertEqual(skyline.stale_hooks(self.tree), [])

    def test_answer_of_an_earlier_check_not_kept(self):
        self.hooks(0x30)
        cache = Path(self.tree, 'build', 'work', 'stale_hooks.json')
        cache.parent.mkdir(parents=True, exist_ok=True)
        sig = skyline._hooks_sig(self.tree).replace(f'v{skyline.HOOKS_CHECK_VERSION}|', 'v1|')
        cache.write_text(json.dumps({'sig': sig, 'mods': ['main'], 'copied': ['main']}))
        self.assertEqual(skyline.stale_hooks(self.tree), [])

    def test_page_asks_without_compiling(self):
        # (os_mods.status: the mods page never waits on the compiler; the build's check still finds it)
        self.hooks(0x10)
        with skyline.cached_inlining_only():
            self.assertEqual(skyline.stale_hooks(self.tree), [])
            self.assertEqual(skyline.stale_hooks(self.tree), [])   # (kept for the page: no second scan needed)
        self.assertFalse(Path(self.tree, 'build', 'work', 'inlined.json').exists())
        self.assertEqual(skyline.stale_hooks(self.tree), ['main'])
        with skyline.cached_inlining_only():   # (now the compiler's answer is kept: the page has it too)
            self.assertEqual(skyline.copied_hooks(self.tree), ['main'])

    def test_dispatcher_already_there_needs_none(self):
        self.hooks(0x10)
        self.table('GuestFn f_7100000010_orig;\n')
        self.assertEqual(skyline.stale_hooks(self.tree), [])


class LongIdentity(unittest.TestCase):
    def test_long_names_cut_before_choosing(self):
        class M:
            build_id = 'b2'
            def module_name(self):
                return 'smashline_test'
        with tempfile.TemporaryDirectory() as tree:
            p = skyline.Paths(tree)
            os.makedirs(p.nro, exist_ok=True)
            Path(p.nro, 'bases.txt').write_text('a_very_long_ch 7200000000 1000 b1\n')
            name = skyline.identity(M(), 'a_very_long_character', p)
            self.assertLessEqual(len(name), len('smashline_test'))
            self.assertEqual(name, 'a_very_long_c_')
            self.assertEqual(skyline.identity(M(), 'gogeta', p), 'gogeta')


if __name__ == '__main__':
    unittest.main()
