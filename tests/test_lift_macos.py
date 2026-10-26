"""The lifter (tools/lift.py) and the runtime contract (runtime/recomp.h) on a weakly ordered host
such as arm64 macOS: every load-acquire becomes a sequentially consistent load (lda*), dc zva the
rt_zva helper (which adds RT_MEMBASE, as every guest memory helper does), a guest setjmp the host's
cheapest setjmp (RT_SETJMP) - and nothing else changes. On an arm64 Mac the helpers and lifted code
are also built with the macOS build's flags and run against guest memory placed RT_MEMBASE up."""
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import lift  # noqa: E402

BASE = lift.BASE
RET = 0xD65F03C0
# the flags the macOS build gives every object
MAC_CFLAGS = ['-arch', 'arm64', '-mmacosx-version-min=14.0', '-mcpu=apple-m1', '-fno-strict-aliasing', '-ffp-contract=off',
              '-fno-stack-protector', '-fvisibility=hidden', '-w']
CLANG = shutil.which('clang')
ON_ARM64_MAC = sys.platform == 'darwin' and platform.machine() == 'arm64' and CLANG is not None


def lifted(*words, setjmp=None, entries=None, pads=None):
    """The C of one function made of these instruction words (at the module's start), every
    register in the context (the register cache off: its own forms are tests/test_regcache.py's)."""
    img = struct.pack(f'<{len(words)}I', *words) + bytes(0x100)
    lifter = lift.Lifter(img, entries or {0}, setjmp=setjmp, pads=pads)
    cached, lift.REGCACHE = lift.REGCACHE, False
    try:
        text, reasons, _callees = lifter.lift_function(0, 4 * len(words))
    finally:
        lift.REGCACHE = cached
    return text, reasons


def body(*words, **kw):
    """Just the statements of a function of these words followed by ret."""
    text, _reasons = lifted(*words, RET, **kw)
    lines = text.splitlines()
    return '\n'.join(line.strip() for line in lines[2:-2])   # (no signature, locals, return or brace)


class AcquireTests(unittest.TestCase):
    # (encoding, what it is, the load it must become)
    ACQUIRE = [
        (0xC8DFFC20, 'ldar x0, [x1]', 'X(0) = lda64(X(1));'),
        (0x88DFFC20, 'ldar w0, [x1]', 'X(0) = lda32(X(1));'),
        (0x48DFFC20, 'ldarh w0, [x1]', 'X(0) = lda16(X(1));'),
        (0x08DFFC20, 'ldarb w0, [x1]', 'X(0) = lda8(X(1));'),
        (0xC8DF7C20, 'ldlar x0, [x1]', 'X(0) = lda64(X(1));'),
        (0xC8DFFC3F, 'ldar xzr, [x1]', '(void)(lda64(X(1)));'),
        (0xC8DFFFE0, 'ldar x0, [sp]', 'X(0) = lda64(SP);'),
        (0xC85FFC22, 'ldaxr x2, [x1]', '{ ex_a = X(1); ex_v = lda64(ex_a); X(2) = ex_v; }'),
        (0x885FFC22, 'ldaxr w2, [x1]', '{ ex_a = X(1); ex_v = lda32(ex_a); X(2) = ex_v; }'),
        (0x485FFC22, 'ldaxrh w2, [x1]', '{ ex_a = X(1); ex_v = lda16(ex_a); X(2) = ex_v; }'),
        (0x085FFC22, 'ldaxrb w2, [x1]', '{ ex_a = X(1); ex_v = lda8(ex_a); X(2) = ex_v; }'),
        (0xC87F8820, 'ldaxp x0, x2, [x1]',
         '{ ex_a = X(1); u128 p_ = lda128(ex_a); ex_v = (u64)p_; ex_w = (u64)(p_ >> 64); X(0) = ex_v; X(2) = ex_w; }'),
        (0x887F8820, 'ldaxp w0, w2, [x1]', '{ ex_a = X(1); ex_v = lda64(ex_a); X(0) = (u32)(ex_v); X(2) = (u32)(ex_v >> 32); }'),
    ]

    def test_acquire_loads_are_lda(self):
        for word, name, want in self.ACQUIRE:
            with self.subTest(name):
                got = body(word)
                self.assertEqual(got, want)
                self.assertIsNone(re.search(r'\bld(8|16|32|64)\(', got), 'a plain load is left')

    def test_every_acquire_encoding(self):
        """Any register numbers, any size: an o2 load (ldar, ldlar) or an o0 exclusive load (ldaxr)
        reads with lda of its size; a plain exclusive load (ldxr) keeps ld."""
        for size in range(4):
            for o2, o0 in ((1, 1), (1, 0), (0, 1), (0, 0)):
                for rn, rt in ((1, 0), (30, 17), (31, 5)):
                    word = size << 30 | 0b001000 << 24 | o2 << 23 | 1 << 22 | 0x1F << 16 | o0 << 15 | 0x1F << 10 | rn << 5 | rt
                    got = body(word)
                    bits = 8 << size
                    with self.subTest(word=f'{word:08x}'):
                        if o2 or o0:
                            self.assertIn(f'lda{bits}(', got)
                            self.assertIsNone(re.search(r'\bld(8|16|32|64)\(', got))
                        else:
                            self.assertIn(f'ex_v = ld{bits}(ex_a);', got)
                            self.assertNotIn('lda', got)

    def test_plain_and_release_forms_unchanged(self):
        same = [
            (0xC85F7C22, 'ldxr x2, [x1]', '{ ex_a = X(1); ex_v = ld64(ex_a); X(2) = ex_v; }'),
            (0x885F7C22, 'ldxr w2, [x1]', '{ ex_a = X(1); ex_v = ld32(ex_a); X(2) = ex_v; }'),
            (0xC89FFC20, 'stlr x0, [x1]', 'sta64(X(1), (u64)X(0));'),
            (0x089FFC20, 'stlrb w0, [x1]', 'sta8(X(1), (u8)X(0));'),
            (0xC803FC22, 'stlxr w3, x2, [x1]', '{ u32 ok_ = (X(1) == ex_a && cas64(ex_a, ex_v, X(2))); ex_a = 0; X(3) = (u32)(!ok_); }'),
            (0xC8037C22, 'stxr w3, x2, [x1]', '{ u32 ok_ = (X(1) == ex_a && cas64(ex_a, ex_v, X(2))); ex_a = 0; X(3) = (u32)(!ok_); }'),
            (0xF9400020, 'ldr x0, [x1]', 'X(0) = ld64(X(1) + 0x0ull);'),
            (0xB9400420, 'ldr w0, [x1, #4]', 'X(0) = ld32(X(1) + 0x4ull);'),
            (0x39400420, 'ldrb w0, [x1, #1]', 'X(0) = ld8(X(1) + 0x1ull);'),
            (0x3DC00420, 'ldr q0, [x1, #16]', 'ldv128(c, 0, X(1) + 0x10ull);'),
            (0xD50B7E23, 'dc civac, x3', ';'),
        ]
        for word, name, want in same:
            with self.subTest(name):
                self.assertEqual(body(word), want)

    def test_unlifted_forms_still_trap(self):
        """Compare-and-swap (LSE, ARMv8.1) and the RCpc loads (ARMv8.3), not on the console's CPU, were
        never lifted: they stay traps, never plain loads. (Exclusive pairs: tests/test_exclusive_pairs.py.)"""
        for word, name in ((0x4860FC82, 'caspal x0, x1, x2, x3, [x4]'), (0xC8E0FC22, 'casal x0, x2, [x1]'),
                           (0xB8BFC020, 'ldapr w0, [x1]'), (0x38BFC020, 'ldaprb w0, [x1]')):
            with self.subTest(name):
                text, reasons = lifted(word, RET)
                self.assertIn('rt_trap(c, 0x7100000000ull, "unsupported");', text)
                self.assertNotRegex(text, r'\blda?(8|16|32|64)\(')
                self.assertEqual(sum(reasons.values()), 1)


class SystemTests(unittest.TestCase):
    def test_dc_zva_is_rt_zva(self):
        for rt in (0, 3, 30):
            with self.subTest(rt=rt):
                got = body(0xD50B7420 | rt)
                self.assertEqual(got, f'rt_zva(X({rt}));')
        self.assertEqual(body(0xD50B743F), 'rt_zva(0ull);')
        self.assertNotIn('memset', lifted(0xD50B7423, RET)[0])

    def test_setjmp_is_rt_setjmp(self):
        """A call to setjmp (bl to an entry the recompiler knows as setjmp) is the host's, in place."""
        got = body(0x94000010, setjmp={0x40}, entries={0, 0x40})   # bl +0x40
        self.assertEqual(got, 'if (!RT_SETJMP(*rt_setjmp(c))) X(0) = 0;')
        other = body(0x94000010, entries={0, 0x40})                 # the same call to an ordinary function
        self.assertEqual(other, 'X(30) = 0x7100000004ull; f_7100000040(c);')
        # and the register cache (tools/regcache.py) finds it: everything spilled before, read back after a longjmp
        self.assertEqual(lift.mark_syncs(got), '@F@ if (!RT_SETJMP(*rt_setjmp(c))) X(0) = 0; else { @A@ }')

    def test_recomp_h_lda_is_seq_cst(self):
        """C's acquire would be ldapr on arm64 (RCpc): too weak for the guest's ldar after stlr."""
        text = (ROOT / 'runtime' / 'recomp.h').read_text(encoding='utf-8')
        self.assertIn('static inline T lda##N(u64 a) { return __atomic_load_n((T *)RT_HOST(a), __ATOMIC_SEQ_CST); }', text)
        # (and off macOS the offset is no code at all)
        self.assertRegex(text, r'#else\n#define RT_MEMBASE 0ull\n#define RT_HOST\(a\) \(\(void \*\)\(uintptr_t\)\(a\)\)\n#endif')

    def test_recomp_h_pair_helpers_are_seq_cst(self):
        """ldaxp after stlr, off x86-64 (tests/test_exclusive_pairs.py): lda128 is a fence, then a seq_cst load (a
        seq_cst ldp alone is ldp; dmb, which may pass an earlier stlr); stxp's cas128 is seq_cst, as cas is."""
        text = (ROOT / 'runtime' / 'recomp.h').read_text(encoding='utf-8')
        arm = text.partition('#define RT_CX16')[2].partition('#else\n')[2].partition('#endif')[0]
        self.assertRegex(arm, r'static inline u128 lda128\(u64 a\) \{\n    __atomic_thread_fence\(__ATOMIC_SEQ_CST\);.*\n'
                              r'    return __atomic_load_n\(\(u128 \*\)RT_HOST\(a\), __ATOMIC_SEQ_CST\);\n\}')
        self.assertIn('(u128)hi << 64 | lo, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);', arm)


# ---- built and run on an arm64 Mac ------------------------------------------------------
# A small module: F (offset 0) increments a guest word with an ldaxr/stlxr loop, reads it back
# with ldar, zeroes a block (dc zva), copies a q register and a 16-byte ld1/st1, calls setjmp
# and tail-calls G (offset 0x40), which stores the word and reads it with ldarb/ldarh/ldar. H
# (offset 0x80) calls G inside a call range with a landing pad: a C++ (exception) file. K (0xA0)
# calls setjmp, then L (0xC0, native: it longjmps back, as hle_setjmp.c does) and on the second
# return adds 100 to the value.
F_WORDS = [0xC85FFC22, 0x91000442, 0xC803FC22, 0x35FFFFA3,   # ldaxr x2, [x1]; add x2, x2, #1; stlxr w3, x2, [x1]; cbnz w3, F
           0xC8DFFC24, 0xD50B7425,                           # ldar x4, [x1]; dc zva, x5
           0x3DC00420, 0x3D800820, 0x4C4070C1, 0x4C0070E1,   # ldr q0, [x1, #16]; str q0, [x1, #32]; ld1 {v1.16b}, [x6]; st1 {v1.16b}, [x7]
           0x9400000E, 0x14000005]                           # bl setjmp (0x60); b G (0x40)
G_WORDS = [0xF9000424, 0x08DFFC29, 0x48DFFC2A, 0x88DFFC2B, RET]   # str x4, [x1, #8]; ldarb w9 / ldarh w10 / ldar w11, [x1]
H_WORDS = [0x97FFFFF0, RET, 0xD2800000, RET]                 # bl G; ret; (pad) mov x0, #0; ret
K_WORDS = [0x97FFFFF0, 0xB5000060, 0x94000006, RET,         # bl setjmp; cbnz x0, +12; bl L; ret
           0x91019000, RET]                                  # add x0, x0, #100; ret


def module_c(*starts):
    """Generated C for those of the module's functions (F 0, G 0x40, H 0x80, K 0xA0, L 0xC0),
    laid out as tools/recomp.py writes a file."""
    img = bytearray(0x100)
    for off, words in ((0, F_WORDS), (0x40, G_WORDS), (0x60, [RET]), (0x80, H_WORDS), (0xA0, K_WORDS), (0xC0, [RET])):
        struct.pack_into(f'<{len(words)}I', img, off, *words)
    sizes = {0: 4 * len(F_WORDS), 0x40: 4 * len(G_WORDS), 0x80: 4 * len(H_WORDS), 0xA0: 4 * len(K_WORDS), 0xC0: 4}
    lifter = lift.Lifter(bytes(img), {0, 0x40, 0x60, 0x80, 0xA0, 0xC0}, forward={0xC0: 'hle_test_longjmp'}, setjmp={0x60},
                         pads={0x80: [(0x80, 0x84, 0x88)]})
    bodies, names = [], set()
    for start in starts:
        text, reasons, callees = lifter.lift_function(start, sizes[start])
        assert not reasons, reasons
        bodies.append(text)
        names |= {start} | set(callees)
    return ('#include "recomp.h"\n\n' + ''.join(f'GuestFn f_{BASE + a:x};\n' for a in sorted(names)) + '\n'
            + '\n\n'.join(bodies) + '\n')


HARNESS = r'''
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp.h"

/* what the recompiled code calls in the runtime */
static jmp_buf guest_jb;
static int setjmps;
jmp_buf *rt_setjmp(Cpu *c) { (void)c; setjmps++; return &guest_jb; }
GuestFn *rt_lookup(u64 a) { fprintf(stderr, "rt_lookup(%#llx)\n", (unsigned long long)a); exit(3); }
GuestFn *rt_lookup_cached(u64 a, RtCache *k) { (void)k; return rt_lookup(a); }
_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) { (void)c; fprintf(stderr, "trap at %#llx: %s\n", (unsigned long long)pc, why); exit(4); }
void rt_svc(Cpu *c, u32 n) { (void)c; fprintf(stderr, "svc %u\n", n); exit(5); }
u64 rt_ticks(void) { return 0; }
GuestFn f_7100000000, f_71000000a0;
void hle_test_longjmp(Cpu *c) { c->x[0] = 7; RT_LONGJMP(guest_jb, 7); }   /* (as hle_setjmp.c's longjmp) */

#define G 0x7100100000ull   /* the guest memory used: 64 KiB there */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static u64 host64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }

int main(void) {
    mach_vm_address_t at = RT_MEMBASE + G;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &at, 0x10000, VM_FLAGS_FIXED);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "cannot map guest memory at %#llx: %d\n", (unsigned long long)(RT_MEMBASE + G), kr); return 2; }
    u8 *h = (u8 *)(uintptr_t)(RT_MEMBASE + G);   /* guest address G, as the host sees it */
    CHECK(RT_MEMBASE == 0x10000000000ull && RT_HOST(G) == (void *)h);

    /* every helper reaches guest + RT_MEMBASE */
    st64(G, 0x1122334455667788ull);
    CHECK(host64(h) == 0x1122334455667788ull);
    CHECK(ld8(G) == 0x88 && ld16(G) == 0x7788 && ld32(G) == 0x55667788u && ld64(G) == 0x1122334455667788ull);
    CHECK(lda8(G) == 0x88 && lda16(G) == 0x7788 && lda32(G) == 0x55667788u && lda64(G) == 0x1122334455667788ull);
    st8(G + 1, 0xAA); st16(G + 2, 0xBBCC); st32(G + 4, 0xDDEEFF00u);
    CHECK(host64(h) == 0xDDEEFF00BBCCAA88ull);
    CHECK(cas64(G, 0xDDEEFF00BBCCAA88ull, 5) && host64(h) == 5);
    CHECK(!cas64(G, 4, 6) && host64(h) == 5);
    CHECK(cas32(G, 5, 7) && cas16(G, 7, 9) && cas8(G, 9, 11) && host64(h) == 11);
    sta64(G + 8, 0x0102030405060708ull); sta32(G + 16, 0x0A0B0C0Du); sta16(G + 20, 0x0E0F); sta8(G + 22, 0x10);
    CHECK(host64(h + 8) == 0x0102030405060708ull && host64(h + 16) == 0x00100E0F0A0B0C0Dull);
    memset(h + 0x100, 0xAB, 0x100);
    rt_zva(G + 0x140 + 13);   /* the 64-byte block holding it */
    for (int i = 0x100; i < 0x200; i++) CHECK(h[i] == (i >= 0x140 && i < 0x180 ? 0 : 0xAB));
    Cpu c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 16; i++) c.v[3].b[i] = (u8)(0x30 + i);
    stv128(&c, 3, G + 0x200);
    CHECK(!memcmp(h + 0x200, c.v[3].b, 16));
    stv32(&c, 3, G + 0x210); CHECK(!memcmp(h + 0x210, c.v[3].b, 4));
    ldv64(&c, 4, G + 0x200); CHECK(c.v[4].d[0] == host64(h + 0x200) && c.v[4].d[1] == 0);
    ldv128(&c, 5, G + 0x200); CHECK(!memcmp(c.v[5].b, h + 0x200, 16));
    for (int i = 0; i < 32; i++) h[0x300 + i] = (u8)i;
    rt_ldst_multi(&c, 1, 6, 1, 2, 4, 1, G + 0x300);   /* ld2 {v6.4s, v7.4s}: de-interleaved */
    CHECK(c.v[6].s[0] == 0x03020100u && c.v[7].s[0] == 0x07060504u && c.v[6].s[3] == 0x1B1A1918u);
    rt_ldst_multi(&c, 0, 6, 1, 2, 4, 1, G + 0x340);   /* st2 back */
    CHECK(!memcmp(h + 0x300, h + 0x340, 32));
    rt_ldst_single(&c, 1, 1, 8, 1, 2, 0, 1, G + 0x302);   /* ld1r {v8.8h} */
    CHECK(c.v[8].h[0] == 0x0302 && c.v[8].h[7] == 0x0302);
    rt_ldst_single(&c, 0, 0, 8, 1, 2, 5, 1, G + 0x380);   /* st1 {v8.h}[5] */
    CHECK(h[0x380] == 0x02 && h[0x381] == 0x03 && h[0x382] == 0);

    /* RT_SETJMP / RT_LONGJMP: _setjmp/_longjmp, which leave the signal mask alone */
    static jmp_buf j;
    static volatile int passes;
    sigset_t usr1, now;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    if (RT_SETJMP(j) == 0) { passes++; sigprocmask(SIG_BLOCK, &usr1, NULL); RT_LONGJMP(j, 7); }
    passes++;
    sigprocmask(SIG_SETMASK, NULL, &now);
    CHECK(passes == 2 && sigismember(&now, SIGUSR1));

    /* the lifted module */
    memset(&c, 0, sizeof c);
    c.x[1] = G + 0x400; c.x[5] = G + 0x500 + 7; c.x[6] = G + 0x600; c.x[7] = G + 0x700; c.x[0] = 99;
    u64 start = 41;
    memcpy(h + 0x400, &start, 8);
    for (int i = 0; i < 16; i++) { h[0x410 + i] = (u8)(0x80 + i); h[0x600 + i] = (u8)(0xC0 + i); }
    memset(h + 0x500, 0xCD, 0x80);
    f_7100000000(&c);
    CHECK(host64(h + 0x400) == 42 && c.x[2] == 42 && c.x[3] == 0 && c.x[4] == 42);   /* ldaxr/stlxr loop, ldar */
    CHECK(host64(h + 0x408) == 42);                                                  /* G's store, after the tail call */
    CHECK(c.x[9] == 42 && c.x[10] == 42 && c.x[11] == 42);                           /* ldarb, ldarh, ldar w */
    for (int i = 0x500; i < 0x580; i++) CHECK(h[i] == (i < 0x540 ? 0 : 0xCD));       /* dc zva */
    CHECK(!memcmp(h + 0x420, h + 0x410, 16) && !memcmp(h + 0x700, h + 0x600, 16));    /* q0, ld1/st1 */
    CHECK(setjmps == 1 && c.x[0] == 0);                                              /* setjmp's first return */
    memset(&c, 0, sizeof c);
    c.x[0] = 1234;
    f_71000000a0(&c);
    CHECK(setjmps == 2 && c.x[0] == 107);                                            /* and its second, by longjmp */
    puts("ok");
    return 0;
}
'''


@unittest.skipUnless(ON_ARM64_MAC, 'needs an arm64 Mac with clang (the Command Line Tools)')
class MacBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp())

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def cc(self, *args):
        r = subprocess.run([CLANG, *MAC_CFLAGS, '-I', str(ROOT / 'runtime'), *args], capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r

    def disassembly(self, obj):   # (with relocations: calls in an object name their targets there)
        r = subprocess.run(['objdump', '-d', '-r', '--no-show-raw-insn', str(obj)], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def test_helpers_and_lifted_code_reach_rt_membase(self):
        gen = self.dir / 'g0000.c'
        gen.write_text(module_c(0, 0x40, 0xA0, 0xC0), encoding='utf-8')
        harness = self.dir / 'harness.c'
        harness.write_text(HARNESS, encoding='utf-8')
        for opt in ('-O2', '-O0'):
            with self.subTest(opt):
                exe = self.dir / f'run{opt}'
                self.cc(opt, str(gen), str(harness), '-o', str(exe))
                r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
                self.assertEqual((r.returncode, r.stdout.strip()), (0, 'ok'), r.stderr)

    def test_generated_code_compiles_as_the_build_does(self):
        """At -O2: as C, and as the C++ wrapper of a file with a landing site (__try)."""
        gen = self.dir / 'g0001.c'
        gen.write_text(module_c(0), encoding='utf-8')   # (G elsewhere: F's tail call stays one)
        obj = self.dir / 'g0001.o'
        self.cc('-O2', '-c', str(gen), '-o', str(obj))
        asm = self.disassembly(obj)
        for insn in ('ldar\t', 'casal\t'):
            self.assertIn(insn, asm)
        self.assertNotIn('ldapr', asm)   # (C's acquire: too weak)
        calls = re.findall(r'\t(b|bl)\t[^\n]*\n\s*[0-9a-f]+:\s+ARM64_RELOC_BRANCH26\s+(\S+)', asm)
        self.assertIn(('b', '_f_7100000040'), calls)   # TAIL: a jump, not a call
        self.assertNotIn(('bl', '_f_7100000040'), calls)
        self.assertIn(('bl', '__setjmp'), calls)       # (Mach-O's name for _setjmp; setjmp's is _setjmp)
        self.assertNotIn(('bl', '_setjmp'), calls)

        pad = self.dir / 'g0002.c'
        text = module_c(0, 0x40, 0x80)
        self.assertIn('__try', text)
        pad.write_text(text, encoding='utf-8')
        wrapper = self.dir / 'g0002_cxx.cpp'
        wrapper.write_text('#include "recomp.h"\nextern "C" {\n#include "g0002.c"\n}\n', encoding='utf-8')
        cxx = self.dir / 'g0002_cxx.o'
        self.cc('-x', 'c++', '-std=gnu++20', '-fexceptions', '-O2', '-I', str(self.dir), '-c', str(wrapper), '-o', str(cxx))
        asm = self.disassembly(cxx)
        self.assertIn('ldarb\t', asm)
        self.assertIn('__setjmp', asm)
        self.assertNotIn('ldapr', asm)

    def test_pair_helpers_keep_the_guest_order(self):
        """ldaxp after stlr (lda128 after sta64): a dmb between the stlr and the ldp. stlxp (cas128): caspal."""
        src = self.dir / 'pair.c'
        src.write_text('#include "recomp.h"\n'
                       'u128 sta_lda(u64 a, u64 b, u64 v) { sta64(a, v); return lda128(b); }\n'
                       'u32 pair_cas(u64 a, u64 e0, u64 e1, u64 v0, u64 v1) { return cas128(a, e0, e1, v0, v1); }\n',
                       encoding='utf-8')
        obj = self.dir / 'pair.o'
        self.cc('-O2', '-c', str(src), '-o', str(obj))
        ops = re.findall(r'^\s+[0-9a-f]+:\s+(\w+)', self.disassembly(obj), re.M)
        self.assertIn('dmb', ops[ops.index('stlr'):ops.index('ldp')], ops)
        self.assertIn('caspal', ops)


if __name__ == '__main__':
    unittest.main()
