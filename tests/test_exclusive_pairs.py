"""Exclusive pairs - ldxp, ldaxp, stxp, stlxp - in the lifter (tools/lift.py) and runtime/recomp.h's 16-byte
helpers (ld128, lda128, cas128). The instruction words are assembled here from their bit fields.

  their C   two W registers are an 8-byte exclusive, two X registers a 16-byte one; acquire loads read with lda;
            the CONSTRAINED UNPREDICTABLE forms get the outcome tools/lift.py names; compare-and-swap (casp,
            cas: LSE, which the console's CPU does not have) still traps
  run       sequences of them built with the game build's compiler and flags for this OS (tools/dev/difftest.py's
            toolchain; on an Apple silicon Mac also the Linux build's x86-64 flags under Rosetta 2), at -O2 and
            -O0, and outside Windows the lifted C as C++ too:
              - each sequence's registers, status and memory as the Arm ARM has them
              - threads counting on one 16-byte and one 8-byte counter with ldaxp / stlxp and ldxp / stxp loops
                lose no update
              - ldxp, while other threads store, always reads both doublewords of one store
              - on an arm64 Mac, the same words run on the CPU itself give the same results
tools/dev/difftest.py --synthetic runs such sequences against unicorn as well (difftest_words.SEQUENCES)."""
from pathlib import Path
import importlib.util
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import lift  # noqa: E402

_spec = importlib.util.spec_from_file_location('difftest', ROOT / 'tools' / 'dev' / 'difftest.py')
difftest = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(difftest)

RET, CLREX = 0xD65F03C0, 0xD503305F


def ldxp(sf, o0, rt, rt2, rn):
    """ldxp (o0 = 1: ldaxp) of two W (sf = 0) or X registers."""
    return 1 << 31 | sf << 30 | 0x08600000 | 31 << 16 | o0 << 15 | rt2 << 10 | rn << 5 | rt


def stxp(sf, o0, rs, rt, rt2, rn):
    """stxp (o0 = 1: stlxp), status in Ws."""
    return 1 << 31 | sf << 30 | 0x08200000 | rs << 16 | o0 << 15 | rt2 << 10 | rn << 5 | rt


def casp(sz, l, o0, rs, rt, rn):
    """casp (l: acquire, o0: release) of two W (sz = 0) or X registers."""
    return sz << 30 | 0x08200000 | l << 22 | rs << 16 | o0 << 15 | 31 << 10 | rn << 5 | rt


def lifted(*words):
    """The C of one function made of these words and ret (the register cache off: every register in the context)."""
    img = struct.pack(f'<{len(words) + 1}I', *words, RET) + bytes(0x100)
    cached, lift.REGCACHE = lift.REGCACHE, False
    try:
        text, reasons, _callees = lift.Lifter(img, {0}).lift_function(0, 4 * (len(words) + 1))
    finally:
        lift.REGCACHE = cached
    return text, reasons


def body(*words):
    lines = lifted(*words)[0].splitlines()
    return '\n'.join(line.strip() for line in lines[2:-2])   # (no signature, locals, return or brace)


X_LOAD = '{{ ex_a = {base}; u128 p_ = {ld}128(ex_a); ex_v = (u64)p_; ex_w = (u64)(p_ >> 64); {rt} = ex_v; {rt2} = ex_w; }}'


class LiftTests(unittest.TestCase):
    # (encoding, what it is, the C it must become)
    FORMS = [
        (0xC87F0C22, 'ldxp x2, x3, [x1]', X_LOAD.format(base='X(1)', ld='ld', rt='X(2)', rt2='X(3)')),
        (0xC87F8C22, 'ldaxp x2, x3, [x1]', X_LOAD.format(base='X(1)', ld='lda', rt='X(2)', rt2='X(3)')),
        (0xC87F8FE2, 'ldaxp x2, x3, [sp]', X_LOAD.format(base='SP', ld='lda', rt='X(2)', rt2='X(3)')),
        (0x887F0C22, 'ldxp w2, w3, [x1]', '{ ex_a = X(1); ex_v = ld64(ex_a); X(2) = (u32)(ex_v); X(3) = (u32)(ex_v >> 32); }'),
        (0x887F8C22, 'ldaxp w2, w3, [x1]', '{ ex_a = X(1); ex_v = lda64(ex_a); X(2) = (u32)(ex_v); X(3) = (u32)(ex_v >> 32); }'),
        (0x887F8FFF, 'ldaxp wzr, w3, [sp]', '{ ex_a = SP; ex_v = lda64(ex_a); (void)(ex_v); X(3) = (u32)(ex_v >> 32); }'),
        (0xC8241825, 'stxp w4, x5, x6, [x1]',
         '{ u32 ok_ = (X(1) == ex_a && cas128(ex_a, ex_v, ex_w, X(5), X(6))); ex_a = 0; X(4) = (u32)(!ok_); }'),
        (0xC8249BE5, 'stlxp w4, x5, x6, [sp]',
         '{ u32 ok_ = (SP == ex_a && cas128(ex_a, ex_v, ex_w, X(5), X(6))); ex_a = 0; X(4) = (u32)(!ok_); }'),
        (0xC83F9825, 'stlxp wzr, x5, x6, [x1]',
         '{ u32 ok_ = (X(1) == ex_a && cas128(ex_a, ex_v, ex_w, X(5), X(6))); ex_a = 0; (void)(!ok_); }'),
        (0x88241825, 'stxp w4, w5, w6, [x1]',
         '{ u32 ok_ = (X(1) == ex_a && cas64(ex_a, ex_v, (u64)(u32)X(6) << 32 | (u32)X(5))); ex_a = 0; X(4) = (u32)(!ok_); }'),
        (0x8824FC25, 'stlxp w4, w5, wzr, [x1]',
         '{ u32 ok_ = (X(1) == ex_a && cas64(ex_a, ex_v, (u64)0u << 32 | (u32)X(5))); ex_a = 0; X(4) = (u32)(!ok_); }'),
    ]

    def test_forms(self):
        for word, name, want in self.FORMS:
            with self.subTest(name):
                self.assertEqual(body(word), want)

    def test_every_encoding(self):
        """Any registers, either size, with and without acquire / release: lifted, never a trap; a W pair is
        8 bytes, an X pair 16; the load reads with lda exactly when it is an acquire; a store compares with
        what the load read and writes its status last."""
        for sf in (0, 1):
            for o0 in (0, 1):
                for rt, rt2, rn, rs in ((2, 3, 1, 4), (30, 17, 31, 0), (31, 5, 9, 12), (7, 31, 31, 31)):
                    with self.subTest(sf=sf, o0=o0, regs=(rt, rt2, rn, rs)):
                        for word in (ldxp(sf, o0, rt, rt2, rn), stxp(sf, o0, rs, rt, rt2, rn)):
                            self.assertFalse(lifted(word)[1])
                        load = body(ldxp(sf, o0, rt, rt2, rn))
                        self.assertTrue(load.startswith(f"{{ ex_a = {'SP' if rn == 31 else f'X({rn})'}; "))
                        self.assertIn(f"{'lda' if o0 else 'ld'}{128 if sf else 64}(ex_a)", load)
                        store = body(stxp(sf, o0, rs, rt, rt2, rn))
                        self.assertIn('cas128(ex_a, ex_v, ex_w, ' if sf else 'cas64(ex_a, ex_v, ', store)
                        self.assertTrue(store.endswith(f"ex_a = 0; {'(void)' if rs == 31 else f'X({rs}) = (u32)'}(!ok_); }}"))

    def test_constrained_unpredictable_forms(self):
        """ldxp of one register twice gets the high half (an UNKNOWN value), as ldp; a status register that is
        also the base or a data register is written after both are read: the original values are stored to the
        original address."""
        self.assertEqual(body(0xC87F0822), X_LOAD.format(base='X(1)', ld='ld', rt='X(2)', rt2='X(2)'))   # ldxp x2, x2, [x1]
        self.assertEqual(body(0xC8210C22),   # stxp w1, x2, x3, [x1]
                         '{ u32 ok_ = (X(1) == ex_a && cas128(ex_a, ex_v, ex_w, X(2), X(3))); ex_a = 0; X(1) = (u32)(!ok_); }')
        self.assertEqual(body(0xC8220C22),   # stxp w2, x2, x3, [x1]
                         '{ u32 ok_ = (X(1) == ex_a && cas128(ex_a, ex_v, ex_w, X(2), X(3))); ex_a = 0; X(2) = (u32)(!ok_); }')

    def test_compare_and_swap_still_traps(self):
        """casp / caspa / caspl / caspal and cas are LSE (ARMv8.1): the console's Cortex-A57 has none."""
        words = [casp(sz, l, o0, 0, 2, 4) for sz in (0, 1) for l in (0, 1) for o0 in (0, 1)]
        words += [0xC8A07C22, 0x88E0FC22, 0x48A07C82, 0x08E0FC82]   # cas x0, x2, [x1]; casal w0, w2, [x1]; cash; casalb
        for word in words:
            with self.subTest(f'{word:08x}'):
                text, reasons = lifted(word)
                self.assertIn('rt_trap(c, 0x7100000000ull, "unsupported");', text)
                self.assertEqual(dict(reasons), {('compare-and-swap (LSE)', word): 1})

    def test_second_doubleword_only_where_used(self):
        """ex_w, the second doubleword an X pair's load read, is a local of the functions that have one; every
        other function's locals are as they were."""
        locals_ = 'u32 nf = 0, zf = 0, cf = 0, vf = 0; u64 ex_a = 0, ex_v = 0'
        for words, extra in (((0xC87F0C22,), ', ex_w = 0'), ((0xC8241825,), ', ex_w = 0'),
                             ((0x887F0C22, 0x88241825), ''), ((0xC85F7C22, 0xC8037C22), ''), ((0x91000442,), '')):
            with self.subTest(words=[f'{w:08x}' for w in words]):
                self.assertEqual(lifted(*words)[0].splitlines()[1].strip(), locals_ + extra + ';')


# ---- built and run ----------------------------------------------------------------------
# Registers: x1 the address, x2 / x3 what the load reads, x4 the status, x5 / x6 what the store writes, x7 another
# address (another page), x8 what a plain store writes in between.
STR_X8_HIGH = {1: 0xF9000428, 0: 0xB9000428}   # str x8, [x1, #8] / str w8, [x1, #4]: the high half
SEQUENCES = {}
for _sf, _r in ((1, 'x'), (0, 'w')):
    SEQUENCES.update({
        f'ld_{_r}': [ldxp(_sf, 0, 2, 3, 1)],                                    # ldxp x2, x3, [x1]
        f'lda_{_r}': [ldxp(_sf, 1, 2, 3, 1)],                                   # ldaxp x2, x3, [x1]
        f'ldst_{_r}': [ldxp(_sf, 0, 2, 3, 1), stxp(_sf, 0, 4, 5, 6, 1)],        # ...; stxp w4, x5, x6, [x1]: succeeds
        f'ldast_{_r}': [ldxp(_sf, 1, 2, 3, 1), stxp(_sf, 1, 4, 5, 6, 1)],       # ldaxp; stlxp: succeeds
        f'other_{_r}': [ldxp(_sf, 0, 2, 3, 1), stxp(_sf, 0, 4, 5, 6, 7)],       # ...; stxp w4, x5, x6, [x7]: fails
        f'changed_{_r}': [ldxp(_sf, 1, 2, 3, 1), STR_X8_HIGH[_sf], stxp(_sf, 1, 4, 2, 3, 1)],   # ...; str; stlxp w4, x2, x3: fails
        f'clrex_{_r}': [ldxp(_sf, 0, 2, 3, 1), CLREX, stxp(_sf, 0, 4, 5, 6, 1)],   # fails
        # ...; stxp w8, x2, x3 (what was read): succeeds, memory as it was; stxp w4, x5, x6: fails, the first ended the reservation
        f'twice_{_r}': [ldxp(_sf, 0, 2, 3, 1), stxp(_sf, 0, 8, 2, 3, 1), stxp(_sf, 0, 4, 5, 6, 1)],
        f'alone_{_r}': [stxp(_sf, 1, 4, 5, 6, 1)],                              # stlxp alone: fails
        f'sp_{_r}': [ldxp(_sf, 1, 2, 3, 31), stxp(_sf, 0, 4, 5, 6, 31)],        # ldaxp x2, x3, [sp]; stxp w4, x5, x6, [sp]
    })
# loops of x5 rounds: (x3:x2) += 1 on the pair at x1, X and W; and (x2, x3) = (x2 + 1, ~(x2 + 1))
LOOP_END = [0x35FFFF84, 0xF10004A5, 0x54FFFF41]   # cbnz w4, (the load); subs x5, x5, #1; b.ne (the load)
SEQUENCES['count_x'] = [ldxp(1, 1, 2, 3, 1), 0xB1000442, 0x9A1F0063, stxp(1, 1, 4, 2, 3, 1)] + LOOP_END   # adds x2, x2, #1; adc x3, x3, xzr
SEQUENCES['count_w'] = [ldxp(0, 0, 2, 3, 1), 0x31000442, 0x1A1F0063, stxp(0, 0, 4, 2, 3, 1)] + LOOP_END   # adds w2, w2, #1; adc w3, w3, wzr
SEQUENCES['flip_x'] = [ldxp(1, 0, 2, 3, 1), 0x91000442, 0xAA2203E3, stxp(1, 1, 4, 2, 3, 1)] + LOOP_END    # add x2, x2, #1; mvn x3, x2
# run on the CPU as well: not changed_* (whether a CPU's own store clears its monitor is IMPLEMENTATION DEFINED) nor
# sp_* (native code keeps its own stack)
HARDWARE = [n for n in SEQUENCES if n.split('_')[0] in ('ld', 'lda', 'ldst', 'ldast', 'other', 'clrex', 'twice', 'alone')]


def module_c():
    """The generated C of SEQUENCES, each a function ending in ret, as tools/recomp.py writes a file (with the
    register cache), and the C name of each."""
    img, starts = b'', {}
    for name, words in SEQUENCES.items():
        starts[name] = len(img)
        img += struct.pack(f'<{len(words) + 1}I', *words, RET)
    lifter = lift.Lifter(img + bytes(0x100), set(starts.values()))
    bodies = []
    for name, words in SEQUENCES.items():
        text, reasons, callees = lifter.lift_function(starts[name], 4 * (len(words) + 1))
        if reasons or callees:
            raise AssertionError(f'{name}: {dict(reasons)} {callees}')
        bodies.append(text)
    names = {name: f'f_{lift.BASE + start:x}' for name, start in starts.items()}
    return ('#include "recomp.h"\n\n' + ''.join(f'GuestFn {n};\n' for n in names.values()) + '\n'
            + '\n\n'.join(bodies) + '\n'), names


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#include "recomp.h"

/* what the recompiled code could call in the runtime */
GuestFn *rt_lookup(u64 a) { fprintf(stderr, "rt_lookup(%#llx)\n", (unsigned long long)a); exit(3); }
GuestFn *rt_lookup_cached(u64 a, RtCache *k) { (void)k; return rt_lookup(a); }
_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) { (void)c; fprintf(stderr, "trap at %#llx: %s\n", (unsigned long long)pc, why); exit(4); }

static _Alignas(64) u8 mem[3 * 4096];
#define GUEST(p) ((u64)(uintptr_t)(p) - RT_MEMBASE)   /* the guest address of host memory p */
static u64 get64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }
static void put64(u8 *p, u64 v) { memcpy(p, &v, 8); }
static int bad;
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); bad++; } } while (0)

#define JUNK 0xDEAD0000BEEF0000ull
#define V5 0x5555666677778888ull
#define V6 0x9999AAAABBBBCCCCull
#define V8 0x0123456789ABCDEFull
static void setup(Cpu *c, u8 *at) {
    memset(c, 0, sizeof *c);
    c->x[1] = GUEST(at); c->x[7] = GUEST(at + 4096); c->sp = GUEST(at);
    c->x[2] = c->x[3] = c->x[4] = JUNK;
    c->x[5] = V5; c->x[6] = V6; c->x[8] = V8;
}

static void each_sequence(void) {
    u8 *b = mem + 64, *o = mem + 64 + 4096;   /* (16-byte aligned) */
    const u64 A0 = 0x1111222233334444ull, A1 = 0xFEDCBA9876543210ull, B = 0x76543210FEDCBA98ull;   /* (no half V8's) */
    Cpu c;
#define RESET(first) (put64(b, first), put64(b + 8, A1), put64(b + 16, 0x77), put64(o, 0x1234), put64(o + 8, 0x5678), setup(&c, b))
#define MEM(lo, hi) (get64(b) == (lo) && get64(b + 8) == (hi) && get64(b + 16) == 0x77 && get64(o) == 0x1234 && get64(o + 8) == 0x5678)
    /* two X registers: Xt the doubleword at the address, Xt2 the next */
    RESET(A0); FN_ld_x(&c);      CHECK(c.x[2] == A0 && c.x[3] == A1 && c.x[4] == JUNK && MEM(A0, A1));
    RESET(A0); FN_lda_x(&c);     CHECK(c.x[2] == A0 && c.x[3] == A1 && c.x[4] == JUNK && MEM(A0, A1));
    RESET(A0); FN_ldst_x(&c);    CHECK(c.x[2] == A0 && c.x[3] == A1 && c.x[4] == 0 && MEM(V5, V6));
    RESET(A0); FN_ldast_x(&c);   CHECK(c.x[2] == A0 && c.x[3] == A1 && c.x[4] == 0 && MEM(V5, V6));
    RESET(A0); FN_other_x(&c);   CHECK(c.x[4] == 1 && MEM(A0, A1));    /* (o: as it was) */
    RESET(A0); FN_changed_x(&c); CHECK(c.x[4] == 1 && MEM(A0, V8));
    RESET(A0); FN_clrex_x(&c);   CHECK(c.x[4] == 1 && MEM(A0, A1));
    RESET(A0); FN_twice_x(&c);   CHECK(c.x[2] == A0 && c.x[3] == A1 && c.x[8] == 0 && c.x[4] == 1 && MEM(A0, A1));
    RESET(A0); FN_alone_x(&c);   CHECK(c.x[4] == 1 && MEM(A0, A1));
    RESET(A0); FN_sp_x(&c);      CHECK(c.x[2] == A0 && c.x[3] == A1 && c.x[4] == 0 && MEM(V5, V6) && c.sp == GUEST(b));
    /* two W registers: one doubleword, Wt its low half; the registers' upper halves cleared */
    RESET(B); FN_ld_w(&c);       CHECK(c.x[2] == 0xFEDCBA98u && c.x[3] == 0x76543210u && c.x[4] == JUNK && MEM(B, A1));
    RESET(B); FN_lda_w(&c);      CHECK(c.x[2] == 0xFEDCBA98u && c.x[3] == 0x76543210u && c.x[4] == JUNK && MEM(B, A1));
    RESET(B); FN_ldst_w(&c);     CHECK(c.x[2] == 0xFEDCBA98u && c.x[3] == 0x76543210u && c.x[4] == 0 && MEM((V6 << 32) | (u32)V5, A1));
    RESET(B); FN_ldast_w(&c);    CHECK(c.x[4] == 0 && MEM((V6 << 32) | (u32)V5, A1));
    RESET(B); FN_other_w(&c);    CHECK(c.x[4] == 1 && MEM(B, A1));
    RESET(B); FN_changed_w(&c);  CHECK(c.x[4] == 1 && MEM((V8 << 32) | (u32)B, A1));
    RESET(B); FN_clrex_w(&c);    CHECK(c.x[4] == 1 && MEM(B, A1));
    RESET(B); FN_twice_w(&c);    CHECK(c.x[8] == 0 && c.x[4] == 1 && MEM(B, A1));
    RESET(B); FN_alone_w(&c);    CHECK(c.x[4] == 1 && MEM(B, A1));
    RESET(B); FN_sp_w(&c);       CHECK(c.x[2] == 0xFEDCBA98u && c.x[3] == 0x76543210u && c.x[4] == 0 && MEM((V6 << 32) | (u32)V5, A1));
}

/* ---- threads */
typedef struct Job { GuestFn *fn; u64 at, rounds; int reader; } Job;
static int arrived, expected, done;
static u64 reads, torn;
static void work(Job *j) {
    Cpu c;
    memset(&c, 0, sizeof c);
    __atomic_add_fetch(&arrived, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&arrived, __ATOMIC_SEQ_CST) < expected) {}   /* all start together */
    if (!j->reader) { c.x[1] = j->at; c.x[5] = j->rounds; j->fn(&c); return; }
    do {   /* ldxp alone, while the others store (x2, ~x2) */
        c.x[1] = j->at; j->fn(&c); reads++;
        if (c.x[3] != ~c.x[2]) torn++;
    } while (!__atomic_load_n(&done, __ATOMIC_SEQ_CST));
}
#ifdef _WIN32
typedef HANDLE Thread;
static DWORD WINAPI thread_main(LPVOID j) { work((Job *)j); return 0; }
static Thread start(Job *j) {
    HANDLE t = CreateThread(NULL, 0, thread_main, j, 0, NULL);
    if (!t) { fprintf(stderr, "CreateThread: error %lu\n", (unsigned long)GetLastError()); exit(2); }
    return t;
}
static void join(Thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
typedef pthread_t Thread;
static void *thread_main(void *j) { work((Job *)j); return NULL; }
static Thread start(Job *j) { pthread_t t; if (pthread_create(&t, NULL, thread_main, j)) { perror("pthread_create"); exit(2); } return t; }
static void join(Thread t) { pthread_join(t, NULL); }
#endif

#define THREADS 4
#define ROUNDS 100000
static void counters(void) {
    u8 *x = mem + 2 * 4096, *w = x + 128;   /* (other cache lines) */
    put64(x, ~0ull - 999); put64(x + 8, 7);       /* the low doubleword carries into the high after 1000 */
    put64(w, 3ull << 32 | 0xFFFFFF00u);           /* the low word carries after 256 */
    Job jobs[2 * THREADS];
    Thread t[2 * THREADS];
    arrived = 0; expected = 2 * THREADS;
    for (int i = 0; i < THREADS; i++) {
        jobs[i] = (Job){FN_count_x, GUEST(x), ROUNDS, 0};
        jobs[THREADS + i] = (Job){FN_count_w, GUEST(w), ROUNDS, 0};
    }
    for (int i = 0; i < 2 * THREADS; i++) t[i] = start(&jobs[i]);
    for (int i = 0; i < 2 * THREADS; i++) join(t[i]);
    const u64 n = (u64)THREADS * ROUNDS, low = 0xFFFFFF00u + n;
    CHECK(get64(x) == ~0ull - 999 + n && get64(x + 8) == 8);
    CHECK(get64(w) == ((3 + (low >> 32)) << 32 | (u32)low));
    printf("counters: %llu, %llu\n", (unsigned long long)get64(x), (unsigned long long)get64(w));
}

static void whole_reads(void) {
    u8 *x = mem + 2 * 4096 + 1024;
    put64(x, 0); put64(x + 8, ~0ull);
    Job jobs[3] = {{FN_flip_x, GUEST(x), ROUNDS, 0}, {FN_flip_x, GUEST(x), ROUNDS, 0}, {FN_ld_x, GUEST(x), 0, 1}};
    Thread t[3];
    arrived = 0; expected = 3; done = 0; reads = torn = 0;
    for (int i = 0; i < 3; i++) t[i] = start(&jobs[i]);
    join(t[0]); join(t[1]);
    __atomic_store_n(&done, 1, __ATOMIC_SEQ_CST);
    join(t[2]);
    CHECK(get64(x) == 2 * ROUNDS && get64(x + 8) == ~(u64)(2 * ROUNDS));
    CHECK(reads > 0 && torn == 0);
    printf("reads: %llu, torn %llu\n", (unsigned long long)reads, (unsigned long long)torn);
}

/* ---- the CPU: the same words, between loads and stores of x1-x8 (from r[1] .. r[8]), after a clrex (a lifted
 * function starts without a reservation) */
#if defined(__aarch64__) && defined(__APPLE__)
#define HW_LOAD "mov x9, %0\n ldp x1, x2, [x9, #8]\n ldp x3, x4, [x9, #24]\n ldp x5, x6, [x9, #40]\n ldp x7, x8, [x9, #56]\n"
#define HW_STORE "stp x1, x2, [x9, #8]\n stp x3, x4, [x9, #24]\n stp x5, x6, [x9, #40]\n stp x7, x8, [x9, #56]\n"
#define HW_CLOBBERS "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "memory", "cc"
@NATIVE@
static int compared;
static void hardware(const char *name, GuestFn *fn, void (*cpu)(u64 *)) {
    u8 *b = mem + 64, *o = mem + 64 + 4096;
    u8 start_b[16], start_o[16], lifted_b[16], lifted_o[16];
    for (int i = 0; i < 16; i++) { start_b[i] = (u8)(0x31 * i + 7); start_o[i] = (u8)(0x57 * i + 3); }
    memcpy(b, start_b, 16); memcpy(o, start_o, 16);
    Cpu c;
    setup(&c, b);
    fn(&c);
    memcpy(lifted_b, b, 16); memcpy(lifted_o, o, 16);
    u64 r[9];
    for (int k = 0; k < 100; k++) {   /* (a store-exclusive may fail spuriously: a success is what it may give) */
        memcpy(b, start_b, 16); memcpy(o, start_o, 16);
        r[1] = (u64)(uintptr_t)b; r[7] = (u64)(uintptr_t)o;
        r[2] = r[3] = r[4] = JUNK; r[5] = V5; r[6] = V6; r[8] = V8;
        cpu(r);
        if (r[4] != 1) break;
    }
    if (c.x[2] != r[2] || c.x[3] != r[3] || c.x[4] != r[4] || memcmp(lifted_b, b, 16) || memcmp(lifted_o, o, 16)) {
        printf("%s: lifted x2 %#llx x3 %#llx x4 %#llx, the CPU x2 %#llx x3 %#llx x4 %#llx%s\n", name,
               (unsigned long long)c.x[2], (unsigned long long)c.x[3], (unsigned long long)c.x[4],
               (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4],
               memcmp(lifted_b, b, 16) || memcmp(lifted_o, o, 16) ? ", and memory differs" : "");
        bad++;
    }
    compared++;
}
#endif

int main(void) {
    each_sequence();
    counters();
    whole_reads();
#if defined(__aarch64__) && defined(__APPLE__)
@COMPARE@
    printf("hardware: %d sequences\n", compared);
#else
    puts("hardware: none");
#endif
    puts(bad ? "FAILED" : "ok");
    return bad != 0;
}
'''


def harness(names):
    native = '\n'.join(
        f'static void hw_{n}(u64 *r) {{\n    __asm__ volatile(HW_LOAD ".inst {CLREX:#010x}\\n"'
        + ''.join(f' ".inst {w:#010x}\\n"' for w in SEQUENCES[n]) + ' HW_STORE : : "r"(r) : HW_CLOBBERS);\n}'
        for n in HARDWARE)
    compare = '\n'.join(f'    hardware("{n}", FN_{n}, hw_{n});' for n in HARDWARE)
    defines = ''.join(f'#define FN_{n} {c}\n' for n, c in names.items())
    decls = ''.join(f'GuestFn {c};\n' for c in names.values())
    return HARNESS.replace('#include "recomp.h"\n', '#include "recomp.h"\n' + decls + defines, 1) \
                  .replace('@NATIVE@', native).replace('@COMPARE@', compare)


HOST = {'win32': 'windows', 'darwin': 'mac'}.get(sys.platform, 'linux')


def rosetta():
    """Whether this is an Apple silicon Mac that runs x86-64 programs."""
    if sys.platform != 'darwin' or platform.machine() != 'arm64' or not shutil.which('clang'):
        return False
    try:
        return subprocess.run(['arch', '-x86_64', '/usr/bin/true'], capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


class RunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp(prefix='exclusive-pairs-'))
        cls.gen, cls.names = module_c()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def build_and_run(self, name):
        llvm = os.environ.get('SSBU_LLVM_BIN')
        try:
            tc = difftest.toolchain(name)
        except SystemExit as e:
            self.skipTest(f'{name}: {e}')
        finally:   # (the rosetta toolchain sets it for tools/linux/toolchain.py; other tests see it as it was)
            if llvm is None:
                os.environ.pop('SSBU_LLVM_BIN', None)
        out = self.dir / name
        out.mkdir()
        (out / 'gen.c').write_text(self.gen, encoding='utf-8')
        (out / 'gen_cxx.cpp').write_text('#include "recomp.h"\nextern "C" {\n#include "gen.c"\n}\n', encoding='utf-8')
        (out / 'harness.c').write_text(harness(self.names), encoding='utf-8')
        builds = [('c', 'O2'), ('c', 'O0')] + ([('cpp', 'O2')] if name != 'windows' else [])
        for lang, level in builds:
            with self.subTest(toolchain=name, lang=lang, level=level):
                objs = []
                for src in ('gen_cxx.cpp' if lang == 'cpp' else 'gen.c', 'harness.c'):
                    obj = out / f'{Path(src).stem}_{lang}_{level}{tc.obj}'
                    cmd = tc.compile_cmd(str(out / src), str(obj), tc.opts[level])
                    if src.endswith('.cpp'):
                        cmd[1:1] = ['-x', 'c++', '-std=gnu++20', '-fexceptions', '-I', str(out)]
                    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
                    self.assertEqual(r.returncode, 0, f'{" ".join(cmd)}\n{r.stdout}{r.stderr}')
                    objs.append(str(obj))
                exe = out / f'run_{lang}_{level}{tc.exe}'
                link = tc.link_cmd(objs, str(exe))
                r = subprocess.run(link, capture_output=True, text=True, timeout=300)
                self.assertEqual(r.returncode, 0, f'{" ".join(link)}\n{r.stdout}{r.stderr}')
                r = subprocess.run([*tc.run, str(exe)], capture_output=True, text=True, timeout=600)
                hw = f'hardware: {len(HARDWARE)} sequences' if name == 'mac' else 'hardware: none'
                self.assertEqual((r.returncode, r.stdout.splitlines()[-2:]), (0, [hw, 'ok']), r.stdout + r.stderr)

    def test_this_os(self):
        self.build_and_run(HOST)

    @unittest.skipUnless(rosetta(), 'needs an Apple silicon Mac with Rosetta 2 and clang')
    def test_x86_64_under_rosetta(self):
        self.build_and_run('rosetta')


if __name__ == '__main__':
    unittest.main()
