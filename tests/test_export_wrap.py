"""tools/recomp.py: HLE_WRAP_EXPORT(<name>), a native wrap of the function exported under that
name in every module that defines it (hle_export_wraps, resolve_wraps). As for HLE_WRAP the
function stays, as f_<address>_orig, and everything that reaches it reaches the wrapper, which
is handed that original. The modules here are hand-assembled words, the names made up."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
import lift  # noqa: E402
import recomp  # noqa: E402
from test_hook_list import CLANG, MAC_CFLAGS, ON_ARM64_MAC, RET, b, bl  # noqa: E402
from test_wrap_signature import WORDS, runtime_with  # noqa: E402

NAME = 'nn_test_frame_line'
M1, M2 = lift.BASE, lift.BASE + 0x1000000   # two modules that both define NAME


def movz(rd, imm, hw=0):
    return 0xD2800000 | hw << 21 | imm << 5 | rd


def movk(rd, imm, hw):
    return 0xF2800000 | hw << 21 | imm << 5 | rd


def load_address(rd, a):
    """movz/movk words that put a in rd."""
    return [movz(rd, a & 0xFFFF)] + [movk(rd, a >> 16 * hw & 0xFFFF, hw) for hw in (1, 2) if a >> 16 * hw & 0xFFFF]


PUSH, FRAME, POP = 0xA9BF7BFD, 0x910003FD, 0xA8C17BFD   # stp x29, x30, [sp, #-16]!; mov x29, sp; ldp x29, x30, [sp], #16
# module 1: W (0x40), exported as NAME, doubles x0; A calls it, T jumps to it, P calls it through
# a register, S jumps to module 2's NAME through x17 as an import stub does
M1_FUNCS = {
    0x00: [PUSH, FRAME, bl(0x08, 0x40), POP, RET],                                   # A
    0x40: [0x8B000000, RET],                                                         # W: add x0, x0, x0
    0x80: [b(0x80, 0x40)],                                                           # T
    0xC0: [PUSH, FRAME, *load_address(8, M1 + 0x40), 0xD63F0100, POP, RET],         # P: blr x8
    0x100: [*load_address(17, M2 + 0x40), 0xD61F0220],                               # S: br x17
}
# module 2: its own NAME (0x40) adds 3; B calls it
M2_FUNCS = {
    0x00: [PUSH, FRAME, bl(0x08, 0x40), POP, RET],                                   # B
    0x40: [0x91000C00, RET],                                                         # add x0, x0, #3
}


def fake_module(funcs, exports):
    """What forwards, resolve_wraps and module_table read of a module."""
    img = bytearray(0x200)
    for off, words in funcs.items():
        struct.pack_into(f'<{len(words)}I', img, off, *words)
    # (one import, as every real module has: the table's lists are never empty)
    return types.SimpleNamespace(image=bytes(img), exports=exports, relocs=[(0x1F8, 257, 0, 0)],
                                 symbols=[('nn_some_import', 0, 0, 0x12, 0)], mem_end=len(img),
                                 text=types.SimpleNamespace(mem=0, end=len(img)), dyn={}, build_id='0',
                                 u32=lambda a: struct.unpack_from('<I', img, a)[0])


def export(name, off, size=8, info=0x12):
    return (name, off, size, info, 1)


def sizes(funcs):
    return {off: 4 * len(words) for off, words in funcs.items()}


class Reading(unittest.TestCase):
    def wraps(self, source):
        with runtime_with(source) as root, mock.patch.object(recomp, 'ROOT', root):
            return recomp.runtime_wraps()

    def test_both_forms_with_and_without_a_signature(self):
        self.assertEqual(self.wraps('/* signature: 3 words, sha256 0123456789abcdef */\nHLE_WRAP_EXPORT(nn_a) {}\n'
                                    'HLE_WRAP_EXPORT(_ZN2nn1bEv) {}\nHLE_WRAP(7100001000) {}\n'),
                         ({0x7100001000: None}, {'nn_a': (3, '0123456789abcdef'), '_ZN2nn1bEv': None}))
        with runtime_with('HLE_WRAP_EXPORT(nn_a) {}\n') as root, mock.patch.object(recomp, 'ROOT', root):
            self.assertEqual((recomp.hle_wraps(), recomp.hle_export_wraps()), ({}, {'nn_a': None}))

    def test_what_stops_the_build(self):
        for source, why in (('HLE_WRAP_EXPORT(nn-a) {}\n', 'is not'),
                            ('HLE_WRAP_EXPORT( nn_a ) {}\n', 'is not'),
                            ('HLE_WRAP_EXPORT(7100001000) {}\n', 'is not'),   # (not a C name: HLE_WRAP's own)
                            ('HLE_WRAP_EXPORT(café) {}\n', 'is not'),
                            ('HLE_WRAP_EXPORT(nn_a) {}\nHLE_WRAP_EXPORT(nn_a) {}\n', 'wrapped twice'),
                            ('HLE(nn_a) {}\nHLE_WRAP_EXPORT(nn_a) {}\n', 'both replaced'),
                            ('/* signature: 3 words, sha256 0123 */\nHLE_WRAP_EXPORT(nn_a) {}\n', 'not in the form'),
                            ('/* signature: 0123abcd */\nHLE_WRAP_EXPORT(nn_a) {}\n', 'instruction words')):
            with self.subTest(source=source), self.assertRaises(SystemExit) as e:
                self.wraps(source)
            self.assertIn(why, str(e.exception))

    def test_a_use_it_would_not_read_stops_the_build(self):
        """Each of these compiles, and the function would stay unwrapped without a word: an
        export wrap names no module's function, so nothing fails to link either."""
        for source in ('HLE_WRAP_EXPORT (nn_a) {}\n', '  HLE_WRAP_EXPORT(nn_a) {}\n', 'static HLE_WRAP_EXPORT(nn_a) {}\n',
                       'HLE_WRAP (7100001000) {}\n', '\tHLE_WRAP(7100001000) {}\n', 'int x; HLE_WRAP(7100001000) {}\n',
                       'HLE_WRAP_EXPORT\n(nn_a) {}\n', '/*\nHLE_WRAP_EXPORT(nn_a) {}\n*/\n',
                       'HLE_WRAP_EXPORT(nn_b) {}\n#if 0 /* x */\n HLE_WRAP_EXPORT(nn_a) {}\n#endif\n',
                       # (a macro of another name, a name pasted together or cut by a line splice)
                       '#define W HLE_WRAP_EXPORT\nW(nn_a) {}\n', '#define W(k) HLE_WRAP_##k\nW(EXPORT)(nn_a) {}\n',
                       'HLE_WRAP_EX\\\nPORT(nn_a) {}\n', 'HLE_WRAP_EXPORT(nn_\\\na) {}\n', '#define W(x) HLE_WRAP_EXPORT(x)\n',
                       # (inside a string a line splice goes on with)
                       'const char *s = "\\\nHLE_WRAP_EXPORT(nn_a) {}";\n',
                       # (any other name that starts as theirs do: those are theirs)
                       '#define HLE_WRAP_COUNT 2\n'):
            with self.subTest(source=source), self.assertRaises(SystemExit) as e:
                self.wraps(source)
            self.assertIn('at the start of a line, outside comments', str(e.exception))
        # what is no use of one: comments and strings that only mention it
        self.assertEqual(self.wraps('/* HLE_WRAP_EXPORT(nn_x) wraps... */\n// HLE_WRAP(7100002000) {}\n'
                                    'const char *s = "HLE_WRAP_EXPORT /* (";\nHLE_WRAP_EXPORT(nn_a) {}\n'
                                    "char q = '\"'; /* HLE_WRAP(7100003000) */\nHLE_WRAP(7100001000) {}\n"
                                    'void f(void) { printf("HLE_WRAP(%llx) has no original\\n", 1ull); }\n'
                                    'static const char *m = "HLE_WRAP_EXPORT(%s) failed", *n = "a \\" HLE_WRAP_EXPORT(x)";\n'),
                         ({0x7100001000: None}, {'nn_a': None}))

    def test_in_the_order_of_the_sources(self):
        self.assertEqual(list(self.wraps('HLE_WRAP_EXPORT(nn_b) {}\nHLE_WRAP_EXPORT(nn_a) {}\nHLE_WRAP_EXPORT(nn_c) {}\n')[1]),
                         ['nn_b', 'nn_a', 'nn_c'])

    def test_the_names_are_part_of_the_tools_fingerprint(self):
        def fingerprint(source):
            with runtime_with(source) as root, mock.patch.object(recomp, 'ROOT', root):
                return recomp.tools_fingerprint()
        plain = fingerprint('HLE(nn_b) {}\n')
        self.assertNotEqual(fingerprint('HLE(nn_b) {}\nHLE_WRAP_EXPORT(nn_a) {}\n'), plain)
        self.assertNotEqual(fingerprint('HLE(nn_b) {}\nHLE_WRAP_EXPORT(nn_c) {}\n'),
                            fingerprint('HLE(nn_b) {}\nHLE_WRAP_EXPORT(nn_a) {}\n'))
        # (their order too: the order of the wrappers of a function exported under both)
        self.assertNotEqual(fingerprint('HLE_WRAP_EXPORT(nn_a) {}\nHLE_WRAP_EXPORT(nn_c) {}\n'),
                            fingerprint('HLE_WRAP_EXPORT(nn_c) {}\nHLE_WRAP_EXPORT(nn_a) {}\n'))


class Resolving(unittest.TestCase):
    def resolve(self, source, exports, funcs=None, base=M1):
        funcs = funcs or M1_FUNCS
        with runtime_with(source) as root, mock.patch.object(recomp, 'ROOT', root):
            return recomp.forwards(fake_module(funcs, exports), base, set(funcs), sorted(sizes(funcs).items()), 'm1')

    def test_every_module_that_defines_the_name(self):
        wrap = {0x40: ('wrap', f'hle_wrap_{NAME}', 'export')}
        self.assertEqual(self.resolve(f'HLE_WRAP_EXPORT({NAME}) {{}}\n', [export(NAME, 0x40)]), wrap)
        self.assertEqual(self.resolve(f'HLE_WRAP_EXPORT({NAME}) {{}}\n', [export(NAME, 0x40)], M2_FUNCS, M2), wrap)
        # a module that doesn't define it, or only imports it (no section): nothing
        self.assertEqual(self.resolve(f'HLE_WRAP_EXPORT({NAME}) {{}}\n', [export('nn_other', 0x40)]), {})
        self.assertEqual(self.resolve('HLE(nn_other) {}\n', [export(NAME, 0x40)]), {})

    def test_with_a_signature(self):
        funcs = {0x40: WORDS}
        comment = recomp.signature_comment(recomp.signature(fake_module(funcs, []), 0x40, 4 * len(WORDS)))
        self.assertIn(0x40, self.resolve(f'{comment}\nHLE_WRAP_EXPORT({NAME}) {{}}\n', [export(NAME, 0x40)], funcs))
        changed = {0x40: WORDS[:9] + [0xAA0203E0] + WORDS[10:]}   # mov x0, x2
        with self.assertRaises(SystemExit) as e:
            self.resolve(f'{comment}\nHLE_WRAP_EXPORT({NAME}) {{}}\n', [export(NAME, 0x40)], changed)
        self.assertIn('does not begin as its signature says', str(e.exception))

    def test_one_function_under_several_wrapped_names_has_each_wrapper(self):
        """(An alias, or identical code folded into one: nnosGetSystemTick and nn::os::GetSystemTick
        could be one function.) The first in the sources is the one callers reach."""
        exports = [export('nn_alias', 0x40), export(NAME, 0x40), export('nn_third', 0x40)]
        self.assertEqual(self.resolve(f'HLE_WRAP_EXPORT({NAME}) {{}}\nHLE_WRAP_EXPORT(nn_alias) {{}}\n', exports),
                         {0x40: ('wrap', f'hle_wrap_{NAME}', 'export', 'hle_wrap_nn_alias')})
        self.assertEqual(self.resolve(f'HLE_WRAP_EXPORT(nn_third) {{}}\nHLE_WRAP_EXPORT(nn_alias) {{}}\n'
                                      f'HLE_WRAP_EXPORT({NAME}) {{}}\n', exports),
                         {0x40: ('wrap', 'hle_wrap_nn_third', 'export', 'hle_wrap_nn_alias', f'hle_wrap_{NAME}')})
        # a symbol table that has one of the names twice for it: that wrapper once, not inside itself
        self.assertEqual(self.resolve(f'HLE_WRAP_EXPORT({NAME}) {{}}\nHLE_WRAP_EXPORT(nn_alias) {{}}\n',
                                      [export(NAME, 0x40), export('nn_alias', 0x40), export(NAME, 0x40)]),
                         {0x40: ('wrap', f'hle_wrap_{NAME}', 'export', 'hle_wrap_nn_alias')})
        self.assertEqual(self.resolve(f'HLE_WRAP_EXPORT({NAME}) {{}}\n', [export(NAME, 0x40), export(NAME, 0x40)]),
                         {0x40: ('wrap', f'hle_wrap_{NAME}', 'export')})
        # each name's signature is checked against the one function
        funcs = {0x40: WORDS}
        comment = recomp.signature_comment(recomp.signature(fake_module(funcs, []), 0x40, 4 * len(WORDS)))
        with self.assertRaises(SystemExit) as e:
            self.resolve(f'{comment}\nHLE_WRAP_EXPORT({NAME}) {{}}\n/* signature: 2 words, sha256 0123456789abcdef */\n'
                         'HLE_WRAP_EXPORT(nn_alias) {}\n', [export(NAME, 0x40, 44), export('nn_alias', 0x40, 44)], funcs)
        self.assertIn('HLE_WRAP_EXPORT(nn_alias): the function does not begin as its signature says', str(e.exception))

    def test_what_stops_the_build(self):
        source = f'HLE_WRAP_EXPORT({NAME}) {{}}\n'
        for exports, why, extra in (([export(NAME, 0x40, info=0x11)], 'not as a function', ''),
                                    ([export(NAME, 0x44)], 'no function starts', ''),
                                    ([export(NAME, 0x40), export('nn_alias', 0x40)], 'both replaced', 'HLE(nn_alias) {}\n')):
            with self.subTest(why=why), self.assertRaises(SystemExit) as e:
                self.resolve(source + extra, exports)
            self.assertIn(why, str(e.exception))
        with self.assertRaises(SystemExit) as e:   # (and one at an HLE_WRAP's address)
            self.resolve(source + f'HLE_WRAP({M1 + 0x40:x}) {{}}\n', [export(NAME, 0x40)])
        self.assertIn('wrapped already', str(e.exception))

    def test_lifted_the_callers_reach_the_wrapper_and_the_loader_too(self):
        forward = {0x40: ('wrap', f'hle_wrap_{NAME}', 'export')}
        lifter = lift.Lifter(fake_module(M1_FUNCS, []).image, set(M1_FUNCS), forward=forward)
        text = lifter.lift_function(0x40, 8)[0]
        self.assertIn(f'void hle_wrap_{NAME}(Cpu *c, GuestFn *orig);\nGuestFn f_7100000040_orig;\n'
                      f'void f_7100000040(Cpu *c) {{ hle_wrap_{NAME}(c, f_7100000040_orig); }}\n', text)
        self.assertIn('void f_7100000040_orig(Cpu *c)', text)
        self.assertIn('f_7100000040(c);', lifter.lift_function(0, 20)[0])
        self.assertIn('TAIL(f_7100000040)', lifter.lift_function(0x80, 4)[0])
        table = recomp.module_table(fake_module(M1_FUNCS, [export(NAME, 0x40)]), 'm1', M1, sorted(sizes(M1_FUNCS).items()), set())[0]
        self.assertIn('{0x7100000040ull, f_7100000040},', table)
        self.assertNotIn('_orig', table)

    def test_lifted_several_wrappers_one_inside_the_other(self):
        forward = {0x40: ('wrap', 'hle_wrap_nn_a', 'export', 'hle_wrap_nn_b', 'hle_wrap_nn_c')}
        text = lift.Lifter(fake_module(M1_FUNCS, []).image, set(M1_FUNCS), forward=forward).lift_function(0x40, 8)[0]
        self.assertIn('void hle_wrap_nn_a(Cpu *c, GuestFn *orig);\nvoid hle_wrap_nn_b(Cpu *c, GuestFn *orig);\n'
                      'void hle_wrap_nn_c(Cpu *c, GuestFn *orig);\nGuestFn f_7100000040_orig;\n'
                      'static void f_7100000040_w2(Cpu *c) { hle_wrap_nn_c(c, f_7100000040_orig); }\n'
                      'static void f_7100000040_w1(Cpu *c) { hle_wrap_nn_b(c, f_7100000040_w2); }\n'
                      'void f_7100000040(Cpu *c) { hle_wrap_nn_a(c, f_7100000040_w1); }\n', text)
        self.assertIn('void f_7100000040_orig(Cpu *c)', text)


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include "module.h"
#include "host.h"

extern const RtModule rt_module_m1, rt_module_m2;
GuestFn f_7100000040_orig;
static int calls;
static u64 seen;
HLE_WRAP_EXPORT(NAME) { calls++; seen = c->x[0]; orig(c); c->x[0] += 1000; }

GuestFn *rt_lookup(u64 a) {
    const RtModule *m[] = {&rt_module_m1, &rt_module_m2};
    for (int i = 0; i < 2; i++)
        for (u32 k = 0; k < m[i]->func_count; k++)
            if (m[i]->funcs[k].addr == a) return m[i]->funcs[k].fn;
    printf("no function at %llx\n", (unsigned long long)a);
    exit(3);
}
GuestFn *rt_lookup_cached(u64 a, RtCache *cache) { (void)cache; return rt_lookup(a); }
void rt_trap(Cpu *c, u64 pc, const char *why) { (void)c; printf("trap %llx %s\n", (unsigned long long)pc, why); exit(4); }

static u8 stack[4096] __attribute__((aligned(16)));
static void run(u64 a) {
    static Cpu c;
    c.sp = H2G(stack + sizeof stack);
    c.x[0] = 5;
    rt_lookup(a)(&c);
    printf("%llx %llu %d %llu\n", (unsigned long long)a, (unsigned long long)c.x[0], calls, (unsigned long long)seen);
}
int main(void) {
    u64 m1 = rt_module_m1.base, m2 = rt_module_m2.base;
    run(m1); run(m1 + 0x80); run(m1 + 0xC0); run(m1 + 0x100); run(m2); run(m1 + 0x40); run(m2 + 0x40);
    static Cpu c;
    c.x[0] = 5;
    f_7100000040_orig(&c);
    printf("orig %llu %d\n", (unsigned long long)c.x[0], calls);
    return 0;
}
'''


# Module 1 again, its W exported under two names, each wrapped: both wrappers run, the first in
# the sources outermost, then the original (each wrapper adds its digit to x0 after).
HARNESS_TWO = r'''
#include <stdio.h>
#include <stdlib.h>
#include "module.h"
#include "host.h"

extern const RtModule rt_module_m1;
GuestFn f_7100000040_orig;
static char order[16];
static int n;
HLE_WRAP_EXPORT(NAME) { order[n++] = 'A'; orig(c); c->x[0] = c->x[0] * 10 + 1; }
HLE_WRAP_EXPORT(nn_alias) { order[n++] = 'B'; orig(c); c->x[0] = c->x[0] * 10 + 2; }

GuestFn *rt_lookup(u64 a) {
    for (u32 k = 0; k < rt_module_m1.func_count; k++)
        if (rt_module_m1.funcs[k].addr == a) return rt_module_m1.funcs[k].fn;
    printf("no function at %llx\n", (unsigned long long)a);
    exit(3);
}
GuestFn *rt_lookup_cached(u64 a, RtCache *cache) { (void)cache; return rt_lookup(a); }
void rt_trap(Cpu *c, u64 pc, const char *why) { (void)c; printf("trap %llx %s\n", (unsigned long long)pc, why); exit(4); }

static u8 stack[4096] __attribute__((aligned(16)));
static void run(u64 a) {
    static Cpu c;
    c.sp = H2G(stack + sizeof stack);
    c.x[0] = 5;
    n = 0;
    rt_lookup(a)(&c);
    printf("%llx %llu %.*s\n", (unsigned long long)a, (unsigned long long)c.x[0], n, order);
}
int main(void) {
    u64 m1 = rt_module_m1.base;
    run(m1); run(m1 + 0x80); run(m1 + 0xC0); run(m1 + 0x40);
    static Cpu c;
    c.x[0] = 5;
    n = 0;
    f_7100000040_orig(&c);
    printf("orig %llu %d\n", (unsigned long long)c.x[0], n);
    return 0;
}
'''


@unittest.skipUnless(ON_ARM64_MAC, 'needs an arm64 Mac with clang (the Command Line Tools)')
class MacRunTests(unittest.TestCase):
    def generate(self, tmp, name, funcs, base, source=f'HLE_WRAP_EXPORT({NAME}) {{ orig(c); }}\n', exports=(NAME,)):
        """The module's C as build writes it (one chunk and its loader table), wrapped as the runtime says."""
        mod = fake_module(funcs, [export(n, 0x40) for n in exports])
        with runtime_with(source) as root, mock.patch.object(recomp, 'ROOT', root):
            forward = recomp.forwards(mod, base, set(funcs), sorted(sizes(funcs).items()), name)
        out = Path(tmp) / name
        out.mkdir()
        cached = dict(recomp._state)
        recomp._state.update(img=mod.image, args=(set(funcs), base, forward, None, None, None, None))
        try:
            recomp._emit((0, sorted(sizes(funcs).items()), str(out)))
        finally:
            recomp._state.clear()
            recomp._state.update(cached)
        (out / 'table.c').write_text(recomp.module_table(mod, name, base, sorted(sizes(funcs).items()), set())[0],
                                     encoding='utf-8')
        return [str(out / 'g0000.c'), str(out / 'table.c')]

    def build_and_run(self, tmp, sources, harness_text):
        harness = Path(tmp) / 'harness.c'
        harness.write_text(harness_text, encoding='utf-8')
        exe = Path(tmp) / 'run'
        includes = ['-I', str(ROOT / 'runtime' / 'macos' / 'include'), '-I', str(ROOT / 'runtime' / 'linux' / 'include'),
                    '-I', str(ROOT / 'runtime')]
        r = subprocess.run([CLANG, *MAC_CFLAGS, '-O2', *includes, *sources, str(harness), '-o', str(exe)],
                           capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout.splitlines()

    def test_one_function_under_two_wrapped_names_runs_both_wrappers(self):
        with tempfile.TemporaryDirectory() as tmp:
            sources = self.generate(tmp, 'm1', M1_FUNCS, M1, f'HLE_WRAP_EXPORT({NAME}) {{}}\nHLE_WRAP_EXPORT(nn_alias) {{}}\n',
                                    ('nn_alias', NAME))
            self.assertEqual(self.build_and_run(tmp, sources, HARNESS_TWO.replace('NAME', NAME)), [
                f'{M1:x} 1021 AB',         # a direct call: A, which runs B, which runs the original (5 * 2), then B's 2, A's 1
                f'{M1 + 0x80:x} 1021 AB',  # a tail call
                f'{M1 + 0xC0:x} 1021 AB',  # a call through a register
                f'{M1 + 0x40:x} 1021 AB',  # looked up by address
                'orig 10 0',               # the original alone: no wrapper
            ])

    def test_every_way_of_reaching_it_runs_the_wrapper_with_its_own_modules_original(self):
        with tempfile.TemporaryDirectory() as tmp:
            sources = self.generate(tmp, 'm1', M1_FUNCS, M1) + self.generate(tmp, 'm2', M2_FUNCS, M2)
            self.assertEqual(self.build_and_run(tmp, sources, HARNESS.replace('NAME', NAME)), [
                f'{M1:x} 1010 1 5',            # a direct call: the wrapper, then module 1's original (5 * 2)
                f'{M1 + 0x80:x} 1010 2 5',     # a tail call
                f'{M1 + 0xC0:x} 1010 3 5',     # a call through a register
                f'{M1 + 0x100:x} 1008 4 5',    # an import stub's jump to module 2's: module 2's original (5 + 3)
                f'{M2:x} 1008 5 5',            # module 2's own direct call
                f'{M1 + 0x40:x} 1010 6 5',     # looked up by address, as a pointer is
                f'{M2 + 0x40:x} 1008 7 5',
                'orig 10 7',                   # the original alone: no wrapper
            ])


if __name__ == '__main__':
    unittest.main()
