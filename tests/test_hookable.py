"""tools/hooks.txt's hookable functions: each kept as one function of its own, out of line, that
the guest reaches only through a slot a hook can take over (runtime/hookable.h, hle_hooks.c).
The lifter calls through the slot at every direct call and tail call, f_<address> (which the
module's table holds for rt_lookup, and so for indirect calls and other modules' import stubs)
calls through it too, and the function is an exact-callee, so a hook sees and changes its
callers' registers (as for any exact-callee, a caller through a pointer's only if that caller is
listed as an exact-caller). tools/hook_inline_check.py checks that in the generated C and in
objects. The functions here are made of hand-assembled words, the lists of made-up addresses."""
from contextlib import redirect_stdout
import io
from pathlib import Path
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
import warnings

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import hook_inline_check  # noqa: E402
import lift  # noqa: E402
import recomp  # noqa: E402
import test_hook_list as hl  # noqa: E402  (its helpers: assemble, b, bl, hooks_file, around, FakeModule)

MAIN, SDK = recomp.MODULES['main'], recomp.MODULES['sdk']
RET = 0xD65F03C0
MOV_X19_5, MOV_X20_6 = 0xD28000B3, 0xD28000D4        # movz x19, #5; movz x20, #6
SUB_SP, ADD_X29, ADD_SP = 0xD10083FF, 0x910043FD, 0x910083FF   # sub sp, sp, #0x20; add x29, sp, #0x10; add sp, sp, #0x20
MOV_X1_X19, MOV_X2_X20 = 0xAA1303E1, 0xAA1403E2      # mov x1, x19; mov x2, x20
ADD_X0_1, ADD_X0_100 = 0x91000400, 0x91019000        # add x0, x0, #1; add x0, x0, #100
BLR_X8 = 0xD63F0100


def caller(at, to):
    """x19 = 5, x20 = 6, a frame, bl to, then returns x19 and x20 in x1 and x2 (x0 as `to` left it)."""
    return [MOV_X19_5, MOV_X20_6, SUB_SP, ADD_X29, hl.bl(at + 0x10, to), MOV_X1_X19, MOV_X2_X20, ADD_SP, RET]


# main: A calls B, the hookable function (x0 + 1); N runs on into B; C calls through x8; G calls T,
# which jumps to B
FUNCS = {0x00: caller(0x00, 0x40),                                     # A
         0x38: [ADD_X0_100, ADD_X0_100],                               # N: runs on into B
         0x40: [ADD_X0_1, RET],                                        # B
         0x80: [MOV_X19_5, MOV_X20_6, SUB_SP, ADD_X29, BLR_X8, MOV_X1_X19, MOV_X2_X20, ADD_SP, RET],   # C
         0xC0: caller(0xC0, 0x100),                                    # G
         0x100: [ADD_X0_100, hl.b(0x104, 0x40)]}                       # T
SIZES = {off: 4 * len(words) for off, words in FUNCS.items()}
B, HOOKED = 0x40, f'{MAIN + 0x40:x}'
CALL_B = f'RT_HOOK(rt_hook_{HOOKED})(c);'
# sdk: P calls B by name through its import stub (0x100), whose GOT slot is 0x200
STUB = hl.STUB
SDK_FUNCS = {0x00: caller(0x00, 0x100), 0x100: STUB}
SDK_SIZES = {off: 4 * len(words) for off, words in SDK_FUNCS.items()}


class SdkModule(hl.FakeModule):
    """What recomp.resolve_hooks reads of sdk: SDK_FUNCS, and a jump-slot relocation binding the
    stub's slot to nn_hooked."""

    def __init__(self):
        super().__init__(hl.assemble(SDK_FUNCS))
        self.text, self.eh_frame_hdr = types.SimpleNamespace(mem=0, end=0x200), (0, 0)
        self.relocs = [(0x200, 1026, 1, 0)]   # R_AARCH64_JUMP_SLOT
        self.symbols = [('', 0, 0, 0, 0), ('nn_hooked', 0, 0, 0x12, 0)]


# (C calls B through a pointer, which cannot be known to reach it: as for any exact-callee, a hook
# sees C's x19-x28 only with C listed as an exact-caller. MacRunTests runs it without, too.)
ONLY_HOOKABLE = f'hookable {HOOKED} nn_hooked\n'
LIST = ONLY_HOOKABLE + f'exact-caller {MAIN + 0x80:x} indirect_caller\n'


def resolved(listing=LIST):
    """What recomp.build hands the Lifter for main and for sdk with this list: ({...}, {...})."""
    main_mod = hl.FakeModule(hl.assemble(FUNCS))
    tails = recomp.tail_graph(main_mod, SIZES)
    with hl.hooks_file(listing):
        imports = recomp.hook_imports({'main': types.SimpleNamespace(exports=[('nn_hooked', B, 8, 0x12, 1)])},
                                      {'main': ({}, tails)})
        callees, callers = recomp.resolve_hooks(main_mod, MAIN, SIZES, 'main', set(FUNCS))
        sdk_callees, sdk_callers = recomp.resolve_hooks(SdkModule(), SDK, SDK_SIZES, 'sdk', set(SDK_FUNCS), imports)
        main = dict(exact_callees=callees, exact_callers=callers, hookable=recomp.hookables(MAIN))
        sdk = dict(exact_callees=sdk_callees, exact_callers=sdk_callers, hookable=recomp.hookables(SDK))
    return main, sdk


def lifted(funcs=FUNCS, base=MAIN, regcache=True, **lists):
    """{offset: C} of funcs, lifted with these lists."""
    lifter = lift.Lifter(hl.assemble(funcs), set(funcs), base=base, **lists)
    cached, lift.REGCACHE = lift.REGCACHE, regcache
    try:
        out = {}
        for off, words in funcs.items():
            text, reasons, _callees = lifter.lift_function(off, 4 * len(words))
            assert not reasons, reasons
            out[off] = text
        return out
    finally:
        lift.REGCACHE = cached


def emit(outdir, funcs=FUNCS, base=MAIN, **lists):
    """(the file recomp.build writes for funcs, what hook_inline_check found in it in the worker)."""
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    image = outdir / 'image.bin'
    image.write_bytes(hl.assemble(funcs))
    with warnings.catch_warnings():   # (_init leaves its file to the mmap, as a worker does)
        warnings.simplefilter('ignore', ResourceWarning)
        recomp._init(str(image), set(funcs), base, lists.pop('forward', {}), set(), {}, lists.get('exact_callees'),
                     lists.get('exact_callers'), lists.get('hookable'))
    cached, lift.REGCACHE = lift.REGCACHE, True
    try:
        name, *_rest, problems = recomp._emit((0, sorted((o, 4 * len(w)) for o, w in funcs.items()), str(outdir)))
    finally:
        lift.REGCACHE = cached
        recomp._state['img'].close()
        recomp._state.clear()
    return (outdir / name).read_text(encoding='utf-8'), problems


TABLE = f'    {{0x{MAIN + 0x40:X}ull, f_{HOOKED}}},\n'   # B's row of g_module.c's table (recomp.module_table)
# what the checks read of main's g_module.c: B's row of the table, and the module's base
G_MODULE = (f'GuestFn f_{HOOKED};\nstatic const RtFunc funcs[] = {{\n{TABLE}}};\n'
            f'const RtModule rt_module_main = {{\n    "main", 0x{MAIN:X}ull, 0x1000ull,\n}};\n')


def command(folder, *args):
    """(hook_inline_check.py's exit status on a module's folder, what it printed), with LIST."""
    out = io.StringIO()
    with hl.hooks_file(LIST), redirect_stdout(out):
        return hook_inline_check.main([str(folder), *args]), out.getvalue()


class HookList(unittest.TestCase):
    def parse(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'hooks.txt'
            path.write_text(text, encoding='utf-8')
            return recomp.hook_list(str(path))

    def refused(self, text, why):
        with self.assertRaises(SystemExit) as e:
            self.parse(text)
        self.assertIn(why, str(e.exception))

    def test_reads_hookable_entries(self):
        self.assertEqual(self.parse('hookable 7100001000 app::Thing::run\n'
                                    'hookable 7109000020 16 words, sha256 0123456789abcdef other  # why\n'
                                    'exact-caller 7100001000 app::Thing::run\n'),
                         {('hookable', 0x7100001000): None, ('hookable', 0x7109000020): (16, '0123456789abcdef'),
                          ('exact-caller', 0x7100001000): None})

    def test_refuses_what_it_cannot_read(self):
        for line, why in (('hookable 7100001000\n', 'not `exact-callee|exact-caller|hookable'),
                          ('Hookable 7100001000 name\n', 'not `exact-callee|exact-caller|hookable'),
                          ('hook-able 7100001000 name\n', 'not `exact-callee|exact-caller|hookable'),
                          ('hookable 7100001000 name extra\n', 'not `exact-callee|exact-caller|hookable'),
                          ('hookable 7100001002 name\n', 'not a function address'),
                          ('hookable 7200000000 name\n', 'not a function address'),   # a fighter module's
                          ('hookable 7100001000 0 words, sha256 0123456789abcdef name\n', 'at least one word'),
                          ('hookable 7100001000 9lives\n', 'not a function name'),
                          ('hookable 7100001000 name # d10083ff\n', 'instruction words'),
                          ('hookable 7100001000 a\nhookable 0x7100001000 b\n', 'listed twice'),
                          # a hookable function is an exact-callee already
                          ('hookable 7100001000 a\nexact-callee 7100001000 a\n', 'list it once, as hookable'),
                          ('exact-callee 7100001000 a\nhookable 7100001000 a\n', 'list it once, as hookable')):
            with self.subTest(line=line):
                self.refused(line, why)

    def test_the_fingerprint_follows_the_kind(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'hooks.txt'

            def fingerprint(text):
                path.write_text(text, encoding='utf-8')
                with mock.patch.object(recomp, 'HOOKS', str(path)):
                    return recomp.tools_fingerprint()
            self.assertNotEqual(fingerprint('hookable 7100001000 name\n'), fingerprint('exact-callee 7100001000 name\n'))

    def test_the_repositorys_list_has_none(self):
        """(The game's C is generated as before: tests/test_hook_list.py checks the list parses.)"""
        self.assertFalse([e for e in recomp.hook_list() if e[0] == 'hookable'])


class Resolve(unittest.TestCase):
    def test_a_hookable_function_is_an_exact_callee(self):
        """With #15's closure: what jumps to it (T) or runs on into it (N), and the import stubs
        bound to its name in other modules (sdk's), are exact-callees too."""
        main, sdk = resolved()
        self.assertEqual((main['exact_callees'], main['exact_callers'], main['hookable']), ({0x38, 0x40, 0x100}, {0x80}, {0x40}))
        self.assertEqual((sdk['exact_callees'], sdk['exact_callers'], sdk['hookable']), ({0x100}, set(), set()))

    def test_no_function_there_stops_the_build(self):
        with hl.hooks_file(f'hookable {MAIN + 0x44:x} b\n'), self.assertRaises(SystemExit) as e:
            recomp.resolve_hooks(hl.FakeModule(hl.assemble(FUNCS)), MAIN, SIZES, 'main', set(FUNCS))
        self.assertIn('hookable 7100000044: no function starts there', str(e.exception))

    def test_setjmp_cannot_be_hookable(self):
        """A call to setjmp becomes the host's setjmp: no slot could stand in for it."""
        with hl.hooks_file(f'hookable {HOOKED} b\n'):
            self.assertEqual(recomp.hookables(MAIN, {0x80}), {0x40})
            with self.assertRaises(SystemExit) as e:
                recomp.hookables(MAIN, {0x40})
        self.assertIn("become the host's setjmp", str(e.exception))


class Lifting(unittest.TestCase):
    def test_calls_and_tail_calls_go_through_the_slot(self):
        out = lifted(**resolved()[0])
        self.assertIn(CALL_B, out[0x00])
        self.assertIn(f'TAIL(RT_HOOK(rt_hook_{HOOKED}))', out[0x100])                    # T's b
        self.assertIn(f'{{ c->x[0] = x_0;  TAIL(RT_HOOK(rt_hook_{HOOKED})) }}', out[0x38])   # N runs on into it
        for off, text in out.items():
            self.assertNotIn(f'f_{HOOKED}(c)', text)
            self.assertNotIn(f'TAIL(f_{HOOKED})', text)

    def test_the_function_stays_out_of_line_behind_its_slot(self):
        text = lifted(**resolved()[0])[B]
        self.assertTrue(text.startswith(f'GuestFn f_{HOOKED}_body;\n'
                                        f'RtHook rt_hook_{HOOKED} = {{0x{HOOKED.upper()}ull, f_{HOOKED}_body, f_{HOOKED}_body}};\n'
                                        f'RT_HOOK_ADD(rt_hook_{HOOKED})\n'
                                        f'void f_{HOOKED}(Cpu *c) {{ TAIL(RT_HOOK(rt_hook_{HOOKED})) }}\n'
                                        f'RT_HOOKABLE void f_{HOOKED}_body(Cpu *c) {{\n'), text)
        self.assertEqual(text.split('{\n', 1)[1], lifted()[B].split('{\n', 1)[1])   # the same body

    def test_its_callers_registers_are_exact(self):
        """A hook in the slot sees x19/x20 as A set them, x29 and SP, and what it writes comes back."""
        out = lifted(**resolved()[0])
        every = ({'19', '20', '29', '30', 'sp'}, {'1', '2', '19', '20', '29', '30', 'sp'})   # (A uses no other register)
        self.assertEqual(hl.around(out[0x00], CALL_B), every)
        self.assertEqual(hl.around(out[0xC0], f'f_{MAIN + 0x100:x}(c);'), every)   # G's call to T, which jumps to it
        # the slot alone, without the exact-callee closure: the ABI's registers only
        self.assertEqual(hl.around(lifted(hookable={B})[0x00], CALL_B), ({'29', '30', 'sp'}, {'1', '2', '30'}))

    def test_a_function_replaced_or_wrapped_natively(self):
        """Its slot starts at what f_<address> would have been: the native function, or the wrapper
        (whose original, f_<address>_orig, it calls directly, as before)."""
        native = lifted(forward={B: 'hle_thing'}, hookable={B})[B]
        self.assertTrue(native.startswith(f'GuestFn hle_thing;\nGuestFn f_{HOOKED}_body;\n'))
        self.assertTrue(native.endswith(f'RT_HOOKABLE void f_{HOOKED}_body(Cpu *c) {{ hle_thing(c); }}'), native)
        wrapped = lifted(forward={B: ('wrap', f'hle_wrap_{HOOKED}')}, hookable={B})[B]
        self.assertIn(f'RT_HOOKABLE void f_{HOOKED}_body(Cpu *c) {{ hle_wrap_{HOOKED}(c); }}\nvoid f_{HOOKED}_orig(Cpu *c) {{\n',
                      wrapped)
        # wrapped by exported name (HLE_WRAP_EXPORT): the slot starts at the wrapper, handed the original
        named = lifted(forward={B: ('wrap', 'hle_wrap_nn_a', 'export')}, hookable={B})[B]
        self.assertIn(f'void f_{HOOKED}(Cpu *c) {{ TAIL(RT_HOOK(rt_hook_{HOOKED})) }}\n', named)
        self.assertIn(f'RT_HOOKABLE void f_{HOOKED}_body(Cpu *c) {{ hle_wrap_nn_a(c, f_{HOOKED}_orig); }}\n'
                      f'void f_{HOOKED}_orig(Cpu *c) {{\n', named)
        # under two names: each wrapper hands on to the next, the last to the original
        two = lifted(forward={B: ('wrap', 'hle_wrap_nn_a', 'export', 'hle_wrap_nn_b')}, hookable={B})[B]
        self.assertIn(f'static void f_{HOOKED}_w1(Cpu *c) {{ hle_wrap_nn_b(c, f_{HOOKED}_orig); }}\n', two)
        self.assertIn(f'RT_HOOKABLE void f_{HOOKED}_body(Cpu *c) {{ hle_wrap_nn_a(c, f_{HOOKED}_w1); }}\n', two)

    def test_without_the_register_cache(self):
        out = lifted(regcache=False, hookable={B})
        self.assertIn(f'X(30) = 0x7100000014ull; RT_HOOK(rt_hook_{HOOKED})(c);', out[0x00])
        self.assertIn(f'RT_HOOKABLE void f_{HOOKED}_body(Cpu *c) {{', out[B])

    def test_an_empty_list_changes_nothing(self):
        """No slot, no include, nothing new: the file recomp.build writes is what it was."""
        self.assertEqual(lifted(hookable=set()), lifted())
        self.assertEqual(lifted(regcache=False, hookable=set()), lifted(regcache=False))
        with tempfile.TemporaryDirectory() as tmp:
            text, problems = emit(tmp)
        bodies = lifted()
        before = ('#include "recomp.h"\n\n' + ''.join(f'GuestFn f_{MAIN + a:x};\n' for a in sorted(FUNCS))
                  + '\n' + '\n\n'.join(bodies[a] for a in sorted(FUNCS)) + '\n')   # (_emit before hookable functions)
        self.assertEqual((text, problems), (before, []))
        self.assertNotIn('hook', text.lower())


class InlineCheck(unittest.TestCase):
    """tools/hook_inline_check.py on the generated C, as recomp.build runs it on every file."""

    def files(self, **lists):
        with tempfile.TemporaryDirectory() as tmp:
            text, problems = emit(tmp, **(lists or resolved()[0]))
        self.assertEqual(problems, [])
        return {'g0000.c': text, 'g_module.c': G_MODULE}

    def problems(self, files):
        return hook_inline_check.check_c(files, {MAIN + B})

    def test_the_generated_c_passes(self):
        files = self.files()
        self.assertIn('#include "hookable.h"\n', files['g0000.c'])
        self.assertIn(f'extern RtHook rt_hook_{HOOKED};\n', files['g0000.c'])
        self.assertEqual(self.problems(files), [])
        self.assertEqual(hook_inline_check.check_c({'g0000.c': files['g0000.c']}, {MAIN + B}, whole=False), [])

    def test_a_file_that_only_calls_it(self):
        """(The function is defined in another of the module's files.)"""
        with tempfile.TemporaryDirectory() as tmp:
            funcs = {0: FUNCS[0]}
            text, problems = emit(tmp, funcs, hookable={B}, exact_callees={B})
        self.assertEqual(problems, [])
        self.assertIn(f'extern RtHook rt_hook_{HOOKED};\n', text)
        self.assertNotIn(f'f_{HOOKED}_body', text)
        self.assertEqual(hook_inline_check.check_c({'g0001.c': text}, {MAIN + B}, whole=False), [])
        self.assertIn(f'f_{HOOKED}_body: defined 0 times, not once', self.problems({'g0001.c': text}))

    def test_what_it_finds(self):
        files = self.files()
        g = files['g0000.c']
        body, fwd, slot = f'f_{HOOKED}_body', f'f_{HOOKED}', f'rt_hook_{HOOKED}'
        for what, changed, why in (
                ('a direct call', g.replace(CALL_B, f'{fwd}(c);', 1), f'calls {fwd} directly'),
                ('a direct tail call', g.replace(f'TAIL(RT_HOOK({slot}))', f'TAIL({fwd})', 1), f'calls {fwd} directly'),
                ('a call of the function itself', g.replace(CALL_B, f'{body}(c);', 1), f'{body} named other than in its slot'),
                ('inlinable', g.replace(f'RT_HOOKABLE void {body}', f'void {body}'), f'{body} named other than in its slot'),
                ('no slot', g.replace(f'void {fwd}(Cpu *c) {{ TAIL(RT_HOOK({slot})) }}', f'void {fwd}(Cpu *c) {{ TAIL({body}) }}'),
                 f'{fwd} does not call through its slot'),
                ('not on the list', g.replace(f'RT_HOOK_ADD({slot})\n', ''), f'without RT_HOOK_ADD({slot})'),
                ('no header', g.replace('#include "hookable.h"\n', ''), 'without #include "hookable.h"'),
                ('defined twice', g + '\n' + lifted(hookable={B})[B] + '\n', f'{body}: defined 2 times')):
            with self.subTest(what):
                self.assertNotEqual(changed, g)
                self.assertTrue(any(why in p for p in self.problems(dict(files, **{'g0000.c': changed}))),
                                self.problems(dict(files, **{'g0000.c': changed})))
        table = files['g_module.c'].replace(f'{fwd}}}', f'{body}}}')
        self.assertIn(f'{body} named other than in its slot', '\n'.join(self.problems(dict(files, **{'g_module.c': table}))))

    def test_the_lifter_changed_finds_its_way_into_the_check(self):
        """A lifter that called the function directly, or left it inlinable, stops the build."""
        main = resolved()[0]
        for what, patch in (('direct', mock.patch.object(lift.Lifter, 'callee', lambda self, t: f'f_{self.base + t:x}')),
                            ('inlinable', mock.patch.object(lift.Lifter, 'entry', lambda self, s, e=lift.Lifter.entry:
                                                            e(self, s).replace('RT_HOOKABLE ', '')))):
            with self.subTest(what), patch, tempfile.TemporaryDirectory() as tmp:
                _text, problems = emit(tmp, **main)
                self.assertTrue(problems)

    def test_a_lifter_not_given_the_list_stops_the_build(self):
        """recomp._emit reads tools/hooks.txt itself for the check, so a build whose Pool hands the
        Lifter no hookable set (its arguments out of step with _init's) stops, not generates
        plain calls."""
        with hl.hooks_file(LIST), tempfile.TemporaryDirectory() as tmp:
            text, problems = emit(tmp, **dict(resolved()[0], hookable=None))
        self.assertNotIn('rt_hook_', text)
        self.assertTrue(any(f'calls f_{HOOKED} directly' in p for p in problems), problems)
        self.assertTrue(any(f'f_{HOOKED} does not call through its slot' in p for p in problems), problems)

    def test_the_command(self):
        """On a module's folder: 1 for a problem in the C; 2, not 0, when a file that defines or
        calls a hookable function has no object to read. (Compiled: 0 with its object.)"""
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for name, text in self.files().items():
                (folder / name).write_text(text, encoding='utf-8')
            status, out = command(folder)
            self.assertEqual(status, 2, out)
            self.assertIn('g0000.c: no object, or no objdump to read it with: not checked', out)
            self.assertIn('1 files that define or call a hookable function have no object checked', out)
            g = (folder / 'g0000.c').read_text(encoding='utf-8')
            (folder / 'g0000.c').write_text(g.replace(CALL_B, f'f_{HOOKED}(c);', 1), encoding='utf-8')
            status, out = command(folder)
            self.assertEqual(status, 1, out)
            self.assertIn(f'calls f_{HOOKED} directly', out)

    def test_the_header(self):
        text = (ROOT / 'runtime' / 'hookable.h').read_text(encoding='utf-8')
        self.assertEqual(hook_inline_check.check_header(text), [])
        for old, new, why in (('__attribute__((noinline, section', '__attribute__((section', 'not noinline'),
                              ('#define RT_HOOKABLE __attribute__((noinline))', '#define RT_HOOKABLE', 'not noinline'),
                              ('noinline, section(".text$rt_hook")', 'noinline', 'section of its own'),
                              ('__ATOMIC_ACQUIRE)', '__ATOMIC_RELAXED)', 'with acquire')):
            with self.subTest(old):
                self.assertIn(old, text)
                self.assertTrue(any(why in p for p in hook_inline_check.check_header(text.replace(old, new, 1))))


# ---- compiled -----------------------------------------------------------------------------
CLANG = shutil.which('clang')
ON_ARM64_MAC = sys.platform == 'darwin' and platform.machine() == 'arm64' and CLANG is not None
MAC_CFLAGS = hl.MAC_CFLAGS
# A direct call of the function itself, as a later change (or LTO) might make one: noinline keeps
# the function out of line there too. (Not something the lifter writes: the check names it.)
PROBE = f'\nvoid probe_direct(Cpu *c) {{ f_{HOOKED}_body(c); c->x[3] = 1; }}\n'
# What recomp.h includes, for clang to compile it for Windows where there are no Windows headers
# (a compile only: nothing is linked)
WINDOWS_STUBS = {
    'excpt.h': '',
    'math.h': '#define NAN __builtin_nanf("")\n#define isinf(x) __builtin_isinf(x)\n#define signbit(x) __builtin_signbit(x)\n'
              'float fmaf(float, float, float); double fma(double, double, double); float fabsf(float); double fabs(double);\n',
    'setjmp.h': 'typedef struct { unsigned long long b[32]; } jmp_buf[1];\nint setjmp(jmp_buf);\n'
                '__declspec(noreturn) void longjmp(jmp_buf, int);\n',
    'string.h': 'void *memcpy(void *, const void *, __SIZE_TYPE__); void *memset(void *, int, __SIZE_TYPE__);\n'
                'int memcmp(const void *, const void *, __SIZE_TYPE__);\n'}


def native_compile(src, obj):
    """The platform's compile of a generated file at -O2, as its build does it: clang-cl and the
    build's flags on Windows; clang elsewhere. None if there is no compiler."""
    if os.name == 'nt':
        import cc
        if not (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG)):
            return None
        cmd = [cc.CLANG, '/c', '/O2', *cc.CFLAGS, str(src), '/Fo' + str(obj)]
    elif CLANG is None:
        return None
    elif sys.platform == 'darwin':
        cmd = [CLANG, *(MAC_CFLAGS if platform.machine() == 'arm64' else ['-w']), '-O2', '-c', '-I', str(ROOT / 'runtime'),
               str(src), '-o', str(obj)]
    else:
        cmd = [CLANG, '-O2', '-c', '-fno-strict-aliasing', '-ffp-contract=off', '-fno-pic', '-w', '-I', str(ROOT / 'runtime'),
               str(src), '-o', str(obj)]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=300)


def windows_compile(src, obj, stubs):
    """clang compiling for Windows as clang-cl, /O2 (which puts each function in a COMDAT: /Gy),
    with stand-in system headers."""
    return subprocess.run([CLANG, '--driver-mode=cl', '--target=x86_64-pc-windows-msvc', '/c', '/O2', '-w', '/I', str(stubs),
                           '/I', str(ROOT / 'runtime'), str(src), '/Fo' + str(obj)], capture_output=True, text=True, timeout=300)


def relocations_overflowed(data):
    """A COFF object with each section's relocations written as a section with more than 65,535
    has them: moved to the end, after a first record holding their count (which counts it too),
    with the section's count 0xFFFF and IMAGE_SCN_LNK_NRELOC_OVFL set."""
    out = bytearray(data)
    (nsec,), (opt,) = struct.unpack_from('<H', data, 2), struct.unpack_from('<H', data, 16)
    for i in range(nsec):
        at = 20 + opt + 40 * i
        (relocs,), (nrel,), (flags,) = (struct.unpack_from(f, data, at + o) for f, o in (('<I', 24), ('<H', 32), ('<I', 36)))
        if nrel:
            struct.pack_into('<I', out, at + 24, len(out))
            out += struct.pack('<IIH', nrel + 1, 0, 0) + data[relocs:relocs + 10 * nrel]
            struct.pack_into('<H', out, at + 32, 0xFFFF)
            struct.pack_into('<I', out, at + 36, flags | hook_inline_check.IMAGE_SCN_LNK_NRELOC_OVFL)
    return bytes(out)


class Compiled(unittest.TestCase):
    """hook_inline_check.check_object on the made-up module compiled at -O2: the function's own
    code is there, its forwarder and every caller load the slot, nothing else names it, and on
    Windows it is in no COMDAT. Then with RT_HOOKABLE's noinline taken out, or with calls that
    skip the slot, it says so."""

    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp())
        cls.lists = resolved()[0]
        cls.text = emit(cls.dir / 'emit', **cls.lists)[0] + PROBE
        (cls.dir / 'stubs').mkdir()
        for name, text in WINDOWS_STUBS.items():
            (cls.dir / 'stubs' / name).write_text(text, encoding='utf-8')

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def build(self, name, text, compile_, header=None):
        """The object compiled from text (in a folder of its own, with hookable.h replaced by header
        if given: a quoted include looks beside the file first), or a skip if it cannot be built."""
        folder = self.dir / self._testMethodName / name
        folder.mkdir(parents=True)
        if header is not None:
            (folder / 'hookable.h').write_text(header, encoding='utf-8')
        src, obj = folder / 'g0000.c', folder / ('g0000.obj' if compile_ is not native_compile or os.name == 'nt' else 'g0000.o')
        src.write_text(text, encoding='utf-8')
        r = compile_(src, obj) if compile_ is native_compile else compile_(src, obj, self.dir / 'stubs')
        if r is None:
            self.skipTest('no compiler')
        if r.returncode:
            # (the compiler, not the hookable function: a file without one does not build either)
            plain = folder / 'plain.c'
            plain.write_text(emit(folder / 'plain', **dict(self.lists, hookable=set()))[0], encoding='utf-8')
            if (compile_(plain, obj) if compile_ is native_compile else compile_(plain, obj, self.dir / 'stubs')).returncode:
                self.skipTest(f'this compiler does not build the generated C here:\n{r.stderr[:500]}')
            self.fail(r.stdout + r.stderr)
        code = hook_inline_check.object_code(str(obj))
        if code is None:
            self.skipTest('no objdump to read the object with')
        return str(obj), code

    def check(self, compile_):
        header = (ROOT / 'runtime' / 'hookable.h').read_text(encoding='utf-8')
        body, fwd, slot = hook_inline_check.names(MAIN + B)
        obj, (funcs, refs, comdat) = self.build('as-is', self.text, compile_)
        self.assertIn(body, funcs)                                        # its own code, standalone
        self.assertNotIn(body, comdat)
        self.assertTrue(slot in refs[fwd] and body not in refs[fwd], refs[fwd])
        for caller in (f'f_{MAIN:x}', f'f_{MAIN + 0x100:x}', f'f_{MAIN + 0x38:x}'):   # A's call, T's jump, N running on
            self.assertTrue(slot in refs[caller] and body not in refs[caller] and fwd not in refs[caller], (caller, refs[caller]))
        self.assertIn(body, refs['probe_direct'])                          # called, not inlined, where called directly
        self.assertEqual(hook_inline_check.check_object(obj, {MAIN + B}),
                         [f'{os.path.basename(obj)}: probe_direct names {body}: a call that bypasses the slot'])
        as_is = obj
        # the command, on the module's folder with the object beside its C: 0
        self.assertTrue(self.text.endswith(PROBE))
        folder = Path(self.build('command', self.text[:-len(PROBE)], compile_)[0]).parent
        (folder / 'g_module.c').write_text(G_MODULE, encoding='utf-8')
        self.assertEqual(command(folder), (0, f'{folder}: 1 hookable functions\n'))

        # noinline taken out: the direct call takes the function's code in
        _obj, (funcs, refs, _comdat) = self.build('inline', self.text, compile_, header.replace('noinline, ', '').replace(
            '__attribute__((noinline))', ''))
        self.assertNotIn(body, refs['probe_direct'])
        # calls that skip the slot (the lifter calling the function itself)
        direct = self.text.replace(f'RT_HOOK(rt_hook_{HOOKED})(c);', f'f_{HOOKED}_body(c);').replace(
            '#include "hookable.h"\n', f'#include "hookable.h"\nGuestFn f_{HOOKED}_body;\n')
        self.assertNotEqual(direct, self.text)
        obj, _code = self.build('direct', direct, compile_)
        self.assertIn(f'f_{MAIN:x} names {body}', '\n'.join(hook_inline_check.check_object(obj, {MAIN + B})))
        return header, as_is

    def test_the_platforms_compiler(self):
        self.check(native_compile)

    @unittest.skipIf(os.name == 'nt' or CLANG is None, 'Windows compiles it with clang-cl itself (the test above)')
    def test_for_windows(self):
        """/O2 puts every function in a COMDAT of its own, which /OPT:ICF may fold with an
        identical one; the function's section is not one. And a section with more than 65,535
        relocations, whose count is in its first record, reads the same."""
        header, as_is = self.check(windows_compile)
        obj, (_funcs, _refs, comdat) = self.build('comdat', self.text, windows_compile,
                                                  header.replace(', section(".text$rt_hook")', ''))
        self.assertIn(f'f_{HOOKED}_body', comdat)
        self.assertIn(f'f_{HOOKED}', comdat)   # (as every other function there)
        self.assertIn('in a COMDAT section', '\n'.join(hook_inline_check.check_object(obj, {MAIN + B})))
        data = Path(as_is).read_bytes()
        self.assertEqual(data[:2], b'\x64\x86')   # (AMD64, not the big-object form)
        self.assertEqual(hook_inline_check.coff_code(relocations_overflowed(data)), hook_inline_check.coff_code(data))


class Coff(unittest.TestCase):
    def test_relocations_past_65535(self):
        """A section with more than 65,535 relocations keeps their count in its first record (a
        COFF object made here: one code section, f calling g three times)."""
        sym = struct.Struct('<8sIhHBB')
        syms = [sym.pack(b'f', 0, 1, 0x20, 2, 0), sym.pack(b'g', 0, 0, 0x20, 2, 0)]   # f in section 1; g undefined
        relocs = b''.join(struct.pack('<IIH', 4 * i, 1, 4) for i in range(3))         # three calls of g
        code = b'\xc3' * 16
        at_code = 20 + 40
        at_relocs, at_syms = at_code + len(code), at_code + len(code) + len(relocs)
        head = struct.pack('<HHIIIHH', 0x8664, 1, 0, at_syms, len(syms), 0, 0)
        section = struct.pack('<8sIIIIIIHHI', b'.text', 0, 0, len(code), at_code, at_relocs, 0, len(relocs) // 10, 0,
                              0x60000020)
        data = head + section + code + relocs + b''.join(syms) + struct.pack('<I', 4)
        self.assertEqual(hook_inline_check.coff_code(data), ({'f'}, {'f': {'g'}}, set()))
        self.assertEqual(hook_inline_check.coff_code(relocations_overflowed(data)), ({'f'}, {'f': {'g'}}, set()))


# ---- run on an arm64 Mac ------------------------------------------------------------------
HARNESS = r'''
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "hookable.h"

GuestFn f_7100000000, f_7100000038, f_7100000040, f_7100000040_body, f_7100000080, f_71000000c0, f_7109000000;

/* what the recompiled code calls in the runtime: rt_lookup with the module's table (g_module.c),
 * which holds f_7100000040 */
GuestFn *rt_lookup(u64 a) {
    if (a == 0x7100000040ull) return f_7100000040;
    fprintf(stderr, "rt_lookup(%#llx)\n", (unsigned long long)a);
    exit(3);
}
GuestFn *rt_lookup_cached(u64 a, RtCache *k) { GuestFn *fn = rt_lookup(a); k->fn = fn; k->key = a ^ RT_CACHE_MIX(fn); return fn; }
_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) { (void)c; fprintf(stderr, "trap at %#llx: %s\n", (unsigned long long)pc, why); exit(4); }
void rt_svc(Cpu *c, u32 n) { (void)c; fprintf(stderr, "svc %u\n", n); exit(5); }
u64 rt_ticks(void) { return 0; }

/* the hook: sees the caller's x19, x20, x29 and SP, runs the original, then changes x19 and x20 */
static GuestFn *next;
static int hooked;
static u64 seen[4];
static void hook(Cpu *c) {
    hooked++; seen[0] = c->x[19]; seen[1] = c->x[20]; seen[2] = c->x[29]; seen[3] = c->sp;
    next(c);
    c->x[19] = 77; c->x[20] = 88;
}

static const struct { const char *name; GuestFn *fn; } entries[] = {
    {"direct", f_7100000000}, {"tail", f_71000000c0}, {"indirect", f_7100000080}, {"stub", f_7109000000}, {"runs-on", f_7100000038}};

static void run_all(const char *phase) {
    for (int i = 0; i < 5; i++) {
        Cpu c;
        memset(&c, 0, sizeof c);
        c.x[0] = 10; c.x[8] = 0x7100000040ull; c.x[19] = 1; c.x[20] = 2; c.x[29] = 3; c.sp = 0x10000;
        hooked = 0; memset(seen, 0, sizeof seen);
        entries[i].fn(&c);
        printf("%s %s %d %llu %llu %llx %llx %llu %llu %llu\n", phase, entries[i].name, hooked, (unsigned long long)seen[0],
               (unsigned long long)seen[1], (unsigned long long)seen[2], (unsigned long long)seen[3], (unsigned long long)c.x[0],
               (unsigned long long)c.x[1], (unsigned long long)c.x[2]);
    }
}

/* One thread calls A over and over while another puts a hook in and takes it out, 200000 times.
 * Every 8th time it leaves the hook in until a call has gone through it (5 s at most). */
static GuestFn *stress_next;
static u64 stress_hooked;
static int stop, bad_remove, missed;
static void stress_hook(Cpu *c) {
    __atomic_fetch_add(&stress_hooked, 1, __ATOMIC_RELAXED);
    __atomic_load_n(&stress_next, __ATOMIC_RELAXED)(c);
    c->x[19] = 77; c->x[20] = 88;
}
static void *swapper(void *slot) {
    for (int i = 0; i < 200000; i++) {
        u64 before = __atomic_load_n(&stress_hooked, __ATOMIC_RELAXED);
        rt_hook_install((GuestFn **)slot, stress_hook, &stress_next);
        if (i % 8 == 0 && !missed)
            for (time_t end = time(NULL) + 5; __atomic_load_n(&stress_hooked, __ATOMIC_RELAXED) == before; sched_yield())
                if (time(NULL) > end) { missed = 1; break; }
        if (!rt_hook_remove((GuestFn **)slot, stress_hook, stress_next)) bad_remove++;
    }
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    return NULL;
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    mach_vm_address_t at = RT_MEMBASE + 0x7109000000ull;   /* sdk's GOT, which its import stub reads */
    if (mach_vm_allocate(mach_task_self(), &at, 0x10000, VM_FLAGS_FIXED) != KERN_SUCCESS) { fprintf(stderr, "cannot map\n"); return 2; }
    st64(0x7109000200ull, 0x7100000040ull);   /* bound to main's nn_hooked */

    GuestFn **slot = rt_hook_slot(0x7100000040ull);
    CHECK(slot && *slot == f_7100000040_body && rt_hook_body(0x7100000040ull) == f_7100000040_body);
    CHECK(!rt_hook_slot(0x7100000000ull) && !rt_hook_body(0x7100000000ull));
    CHECK(rt_hook_list() && rt_hook_list()->addr == 0x7100000040ull && !rt_hook_list()->next);
    CHECK(!rt_hook_install(NULL, hook, &next));
    run_all("before");
    CHECK(rt_hook_install(slot, hook, &next) && next == f_7100000040_body && *slot == hook);
    run_all("hooked");
    CHECK(!rt_hook_remove(slot, stress_hook, next) && *slot == hook);   /* not the one in the slot */
    CHECK(rt_hook_remove(slot, hook, next) && *slot == f_7100000040_body);
    run_all("after");

    /* each call's x0 is the function's and its x19/x20 A's or the hook's; every call that went
     * through the hook came back with the hook's */
    pthread_t t;
    pthread_create(&t, NULL, swapper, slot);
    u64 calls = 0, wrong = 0, hooked_seen = 0;
    do {
        Cpu c;
        memset(&c, 0, sizeof c);
        c.x[0] = 10; c.sp = 0x10000;
        f_7100000000(&c);
        calls++;
        if (c.x[0] != 11 || !((c.x[1] == 5 && c.x[2] == 6) || (c.x[1] == 77 && c.x[2] == 88))) wrong++;
        hooked_seen += c.x[1] == 77;
    } while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE));
    pthread_join(t, NULL);
    u64 through = __atomic_load_n(&stress_hooked, __ATOMIC_RELAXED);
    printf("stress %llu %d %d\n", (unsigned long long)wrong, bad_remove, !missed && hooked_seen > 0 && hooked_seen == through);
    fprintf(stderr, "(%llu calls while the hook went in and out 200000 times; %llu went through it, %llu came back changed)\n",
            (unsigned long long)calls, (unsigned long long)through, (unsigned long long)hooked_seen);
    CHECK(*slot == f_7100000040_body);
    return 0;
}
'''


@unittest.skipUnless(ON_ARM64_MAC, 'needs an arm64 Mac with clang (the Command Line Tools)')
class MacRunTests(unittest.TestCase):
    def run_harness(self, main, sdk):
        """(the harness's lines, main's C, sdk's C), with main and sdk lifted with these lists."""
        with tempfile.TemporaryDirectory() as tmp:
            gen_main = emit(Path(tmp) / 'main', **main)[0]
            gen_sdk = emit(Path(tmp) / 'sdk', SDK_FUNCS, SDK, **sdk)[0]
            harness = Path(tmp) / 'harness.c'
            harness.write_text(HARNESS, encoding='utf-8')
            exe = Path(tmp) / 'run'
            r = subprocess.run([CLANG, *MAC_CFLAGS, '-O2', '-I', str(ROOT / 'runtime'), str(Path(tmp) / 'main' / 'g0000.c'),
                                str(Path(tmp) / 'sdk' / 'g0000.c'), str(ROOT / 'runtime' / 'hle_hooks.c'), str(harness),
                                '-o', str(exe)], capture_output=True, text=True, timeout=300)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout.splitlines(), gen_main, gen_sdk

    def test_a_hook_in_the_slot_takes_every_call(self):
        """Put in at run time, the hook takes the direct call, the tail call, the indirect call, the
        call from sdk through its import stub, and the function running on into it; it sees the
        caller's x19, x20, x29 and SP, changes x19 and x20, and runs the original (x0 + 1). Taken
        out, the calls reach the function again. And it goes in and out while another thread
        calls through the slot, taking calls as it does. (C, the indirect caller, is listed as an
        exact-caller.)"""
        lines, gen_main, gen_sdk = self.run_harness(*resolved())
        self.assertIn(f'RT_HOOK(rt_hook_{HOOKED})', gen_main)
        self.assertIn(f'f_{SDK + 0x100:x}(c);', gen_sdk)   # (sdk calls its stub; the stub, rt_lookup)
        plain = {'direct': '0 0 0 0 0 11 5 6', 'tail': '0 0 0 0 0 111 5 6', 'indirect': '0 0 0 0 0 11 5 6',
                 'stub': '0 0 0 0 0 11 5 6', 'runs-on': '0 0 0 0 0 211 0 0'}
        # (each caller's frame: SP 0x10000 - 0x20, x29 SP + 0x10)
        hooked = {'direct': '1 5 6 fff0 ffe0 11 77 88', 'tail': '1 5 6 fff0 ffe0 111 77 88',
                  'indirect': '1 5 6 fff0 ffe0 11 77 88', 'stub': '1 5 6 fff0 ffe0 11 77 88',
                  'runs-on': '1 1 2 3 10000 211 0 0'}   # (N's own caller is the harness: its registers as it set them)
        self.assertEqual(lines, [f'{phase} {k} {v}' for phase, table in (('before', plain), ('hooked', hooked), ('after', plain))
                                 for k, v in table.items()] + ['stress 0 0 1'])

    def test_a_caller_through_a_pointer_needs_listing_as_an_exact_caller(self):
        """With only the hookable entry, the slot still takes C's call through x8, and the hook
        sees C's x29 and SP and its result comes back in x0, but it finds the context's x19 and
        x20, not C's, and what it writes there is lost. The other calls are exact as before."""
        lines, _main, _sdk = self.run_harness(*resolved(ONLY_HOOKABLE))
        self.assertIn('hooked indirect 1 1 2 fff0 ffe0 11 5 6', lines)
        for k, v in (('direct', '1 5 6 fff0 ffe0 11 77 88'), ('tail', '1 5 6 fff0 ffe0 111 77 88'),
                     ('stub', '1 5 6 fff0 ffe0 11 77 88')):
            self.assertIn(f'hooked {k} {v}', lines)

    def test_without_the_exact_callee_closure_a_hook_sees_stale_registers(self):
        """(What making a hookable function an exact-callee is for: the slot alone, as in #15's
        tests for a wrap.)"""
        lines, _main, _sdk = self.run_harness(dict(hookable={B}), {})
        self.assertIn('hooked direct 1 1 2 fff0 ffe0 11 5 6', lines)   # the context's x19/x20; A keeps its own


if __name__ == '__main__':
    unittest.main()
