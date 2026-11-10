"""Memory ordering of the lifted code (tools/lift.py) on the host: litmus tests, threads running guest
programs whose orderings the Arm ARM forbids, on runtime/recomp.h's lda / sta / cas helpers and the
fence the lifter emits for dmb / dsb. The instruction words are listed here, each named.

  MP    message passing: str data; stlr flag || ldar flag; ldr data - the flag seen, the data must be
        too. Also with dmb ishst / dmb ishld, and with an exclusive pair as the flag (ldxp / stlxp
        releasing, ldaxp acquiring)
  SB    store buffering: str x; dmb ish; ldr y || str y; dmb ish; ldr x - not both 0. Also stlr x;
        ldar y (an stlr is ordered before a later ldar)
  LB    load buffering: ldar y; str x || ldar x; str y - not both 1. Also with dmb ish
  IRIW  independent reads of independent writes: stlr x || stlr y || ldar x; ldar y || ldar y; ldar x
        - the readers must not see the two writes in opposite orders (ARMv8 is multi-copy atomic).
        Also with plain accesses and dmb ish between the reads
  lock  four threads counting under a lock taken with ldaxr / stxr and released with stlr: no update
        lost
The same with plain ldr / str and no barrier are controls: SB's must show reorderings (on Arm and
x86-64 alike a store can wait in a store buffer behind a later load), which shows the harness can
observe one; the others' counts are printed only (x86-64 never reorders those).

Built with the game build's compiler and flags for this OS (tools/dev/difftest.py's toolchain; on an
Apple silicon Mac also the Linux build's x86-64 flags under Rosetta 2), at -O2 and -O0, as
tests/test_exclusive_pairs.py is. Made-up programs only."""
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

RET = 0xD65F03C0
STR_X2_X0, STR_X2_X1 = 0xF9000002, 0xF9000022            # str x2, [x0] / [x1]
LDR_X3_X0, LDR_X3_X1, LDR_X4_X0, LDR_X4_X1 = 0xF9400003, 0xF9400023, 0xF9400004, 0xF9400024
STLR_X2_X0, STLR_X2_X1 = 0xC89FFC02, 0xC89FFC22          # stlr x2, [x0] / [x1]
LDAR_X3_X0, LDAR_X3_X1, LDAR_X4_X1 = 0xC8DFFC03, 0xC8DFFC23, 0xC8DFFC24
DMB_ISH, DMB_ISHST, DMB_ISHLD = 0xD5033BBF, 0xD5033ABF, 0xD50339BF
LDXP_X5_X6_X1 = 0xC87F1825                                # ldxp x5, x6, [x1]
STLXP_W7_X2_X2_X1 = 0xC8278822                            # stlxp w7, x2, x2, [x1]
LDAXP_X3_X6_X1 = 0xC87F9823                               # ldaxp x3, x6, [x1]
LDAXR_W5_X0, STXR_W6_W2_X0 = 0x885FFC05, 0x88067C02       # ldaxr w5, [x0]; stxr w6, w2, [x0]
LDR_X7_X1, ADD_X7_1, STR_X7_X1 = 0xF9400027, 0x910004E7, 0xF9000027   # ldr x7, [x1]; add x7, x7, #1; str x7, [x1]
STLR_WZR_X0, SUBS_X8_1 = 0x889FFC1F, 0xF1000508           # stlr wzr, [x0]; subs x8, x8, #1


def cbz(rt, by, sf=1, nz=0):
    """cbz (nz: cbnz) to `by` instructions from here."""
    return sf << 31 | 0x34000000 | nz << 24 | (by & 0x7FFFF) << 5 | rt


def b_ne(by):
    return 0x54000001 | (by & 0x7FFFF) << 5


# Each program: x0 and x1 the two locations, x2 = 1 the value stored; what it reads in x3 (and x4).
PROGRAMS = {
    'mp_rel_w': [STR_X2_X0, STLR_X2_X1],                  # data at x0, flag at x1
    'mp_acq_r': [LDAR_X3_X1, LDR_X4_X0],
    'mp_dmb_w': [STR_X2_X0, DMB_ISHST, STR_X2_X1],
    'mp_dmb_r': [LDR_X3_X1, DMB_ISHLD, LDR_X4_X0],
    'mp_pair_w': [STR_X2_X0, LDXP_X5_X6_X1, STLXP_W7_X2_X2_X1, cbz(7, -2, 0, 1)],   # (again while the stlxp fails)
    'mp_pair_r': [LDAXP_X3_X6_X1, LDR_X4_X0],
    'mp_plain_w': [STR_X2_X0, STR_X2_X1],
    'mp_plain_r': [LDR_X3_X1, LDR_X4_X0],
    'sb_dmb': [STR_X2_X0, DMB_ISH, LDR_X3_X1],            # own location at x0, the other's at x1
    'sb_rel_acq': [STLR_X2_X0, LDAR_X3_X1],
    'sb_plain': [STR_X2_X0, LDR_X3_X1],
    'lb_acq': [LDAR_X3_X1, STR_X2_X0],
    'lb_dmb': [LDR_X3_X1, DMB_ISH, STR_X2_X0],
    'lb_plain': [LDR_X3_X1, STR_X2_X0],
    'iriw_w_rel': [STLR_X2_X0],
    'iriw_r_acq': [LDAR_X3_X0, LDAR_X4_X1],
    'iriw_w': [STR_X2_X0],
    'iriw_r_dmb': [LDR_X3_X0, DMB_ISH, LDR_X4_X1],
    # x8 rounds of: take the lock at x0 (ldaxr until 0, stxr 1), count at x1, release with stlr wzr
    'lock': [LDAXR_W5_X0, cbz(5, -1, 0, 1), STXR_W6_W2_X0, cbz(6, -3, 0, 1),
             LDR_X7_X1, ADD_X7_1, STR_X7_X1, STLR_WZR_X0, SUBS_X8_1, b_ne(-9)],
}


def module_c():
    """The lifted C of PROGRAMS, each a function ending in ret, as tools/recomp.py writes a file, and the C name of each."""
    img, starts = b'', {}
    for name, words in PROGRAMS.items():
        starts[name] = len(img)
        img += struct.pack(f'<{len(words) + 1}I', *words, RET)
    lifter = lift.Lifter(img + bytes(0x100), set(starts.values()))
    bodies = []
    for name, words in PROGRAMS.items():
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
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#include "recomp.h"

GuestFn *rt_lookup(u64 a) { fprintf(stderr, "rt_lookup(%#llx)\n", (unsigned long long)a); exit(3); }
GuestFn *rt_lookup_cached(u64 a, RtCache *k) { (void)k; return rt_lookup(a); }
_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) { (void)c; fprintf(stderr, "trap at %#llx: %s\n", (unsigned long long)pc, why); exit(4); }

#define GUEST(p) ((u64)(uintptr_t)(p) - RT_MEMBASE)   /* the guest address of host memory p */
#define SLOTS 1024                                     /* locations used in turn, each on its own cache lines */
#define RUNS 100000L
typedef struct { _Alignas(64) u64 v[2]; char pad[48]; } Line;
static Line loc[2][SLOTS];
static u64 out3[4][RUNS], out4[4][RUNS];

/* ---- n threads in lock-step: each runs its program on slot i once all have reached i. The wait is an acquire
 * (it sees thread 0's clearing of the slots before a new pass); the step stores are relaxed: nothing orders a
 * program's own accesses among themselves */
typedef struct { GuestFn *fn; int me; } Job;
static struct { volatile long at; char pad[56]; } step[4];
static int nthreads, kind;   /* kind: 0 MP (x0 data, x1 flag), 1 SB / LB (x0 own, x1 other's), 2 IRIW */
static long runs;
static void wait_all(long i) {
    for (int t = 0; t < nthreads; t++) while (__atomic_load_n(&step[t].at, __ATOMIC_ACQUIRE) < i) {}
}
static void work(Job *j) {
    Cpu c;
    memset(&c, 0, sizeof c);
    for (long i = 0; i < runs; i++) {
        long s = i % SLOTS;
        if (s == 0 && i) {   /* a pass done: all wait, thread 0 clears every slot, all wait again */
            __atomic_store_n(&step[j->me].at, 2 * i - 1, __ATOMIC_SEQ_CST);
            wait_all(2 * i - 1);
            if (j->me == 0) memset(loc, 0, sizeof loc);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
        }
        __atomic_store_n(&step[j->me].at, 2 * i, __ATOMIC_RELAXED);
        wait_all(2 * i);
        u64 a = GUEST(&loc[0][s].v[0]), b = GUEST(&loc[1][s].v[0]);
        if (kind == 0 || kind == 2) { c.x[0] = (kind == 2 && (j->me & 1)) ? b : a; c.x[1] = (kind == 2 && (j->me & 1)) ? a : b; }
        else { c.x[0] = j->me ? b : a; c.x[1] = j->me ? a : b; }
        c.x[2] = 1; c.x[3] = c.x[4] = 7;
        j->fn(&c);
        out3[j->me][i] = c.x[3]; out4[j->me][i] = c.x[4];
    }
    __atomic_store_n(&step[j->me].at, 2 * runs + 1, __ATOMIC_SEQ_CST);
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
static void run(int k, long n, int count, GuestFn **fns) {
    Job jobs[4];
    Thread t[4];
    memset(loc, 0, sizeof loc);
    kind = k; runs = n; nthreads = count;
    for (int i = 0; i < count; i++) { step[i].at = -1; jobs[i] = (Job){fns[i], i}; }
    for (int i = 0; i < count; i++) t[i] = start(&jobs[i]);
    for (int i = 0; i < count; i++) join(t[i]);
}

static int bad;
static void result(const char *name, long seen, long of, int forbidden) {
    printf("%-11s %s %ld of %ld\n", name, forbidden ? (seen ? "FORBIDDEN SEEN" : "never") : "control:", seen, of);
    if (forbidden && seen) bad++;
}
static long mp(const char *name, GuestFn *w, GuestFn *r, int forbidden) {   /* the flag seen, the data not */
    long seen = 0;
    run(0, RUNS, 2, (GuestFn *[]){w, r});
    for (long i = 0; i < RUNS; i++) seen += out3[1][i] == 1 && out4[1][i] == 0;
    result(name, seen, RUNS, forbidden);
    return seen;
}
static long sb_lb(const char *name, GuestFn *f, u64 both, int forbidden) {   /* both threads read `both` */
    long seen = 0;
    run(1, RUNS, 2, (GuestFn *[]){f, f});
    for (long i = 0; i < RUNS; i++) seen += out3[0][i] == both && out3[1][i] == both;
    result(name, seen, RUNS, forbidden);
    return seen;
}
static long iriw(const char *name, GuestFn *w, GuestFn *r, int forbidden) {   /* reader 2: x then not y, reader 3: y then not x */
    long seen = 0;
    run(2, RUNS / 2, 4, (GuestFn *[]){w, w, r, r});
    for (long i = 0; i < RUNS / 2; i++) seen += out3[2][i] == 1 && out4[2][i] == 0 && out3[3][i] == 1 && out4[3][i] == 0;
    result(name, seen, RUNS / 2, forbidden);
    return seen;
}

/* ---- the lock */
typedef struct { GuestFn *fn; u64 lock, count, rounds; } LockJob;
static void lock_work(LockJob *j) { Cpu c; memset(&c, 0, sizeof c); c.x[0] = j->lock; c.x[1] = j->count; c.x[2] = 1; c.x[8] = j->rounds; j->fn(&c); }
#ifdef _WIN32
static DWORD WINAPI lock_main(LPVOID j) { lock_work((LockJob *)j); return 0; }
static Thread lock_start(LockJob *j) { return CreateThread(NULL, 0, lock_main, j, 0, NULL); }
#else
static void *lock_main(void *j) { lock_work((LockJob *)j); return NULL; }
static Thread lock_start(LockJob *j) { pthread_t t; if (pthread_create(&t, NULL, lock_main, j)) exit(2); return t; }
#endif
static void lock_counts(GuestFn *fn) {
    static _Alignas(64) u64 lock_word, pad_[7], counter;
    Thread t[4];
    LockJob j = {fn, GUEST(&lock_word), GUEST(&counter), RUNS / 4};
    lock_word = counter = 0;
    (void)pad_;
    for (int i = 0; i < 4; i++) t[i] = lock_start(&j);
    for (int i = 0; i < 4; i++) join(t[i]);
    printf("lock        counted %llu of %ld\n", (unsigned long long)counter, RUNS);
    if (counter != (u64)RUNS || lock_word) bad++;
}

int main(void) {
    mp("MP stlr", FN_mp_rel_w, FN_mp_acq_r, 1);
    mp("MP dmb", FN_mp_dmb_w, FN_mp_dmb_r, 1);
    mp("MP pair", FN_mp_pair_w, FN_mp_pair_r, 1);
    mp("MP plain", FN_mp_plain_w, FN_mp_plain_r, 0);
    sb_lb("SB dmb", FN_sb_dmb, 0, 1);
    sb_lb("SB stlr", FN_sb_rel_acq, 0, 1);
    long sb = 0;
    for (int k = 0; k < 200 && !sb; k++) sb = sb_lb("SB plain", FN_sb_plain, 0, 0);   /* (tried again until it shows: at -O0
                                                                                        * a run of 100,000 can show none) */
    printf("SB plain reorders: %s\n", sb ? "seen" : "NEVER SEEN");
    if (!sb) bad++;
    sb_lb("LB ldar", FN_lb_acq, 1, 1);
    sb_lb("LB dmb", FN_lb_dmb, 1, 1);
    sb_lb("LB plain", FN_lb_plain, 1, 0);
    iriw("IRIW ldar", FN_iriw_w_rel, FN_iriw_r_acq, 1);
    iriw("IRIW dmb", FN_iriw_w, FN_iriw_r_dmb, 1);
    lock_counts(FN_lock);
    puts(bad ? "FAILED" : "ok");
    return bad != 0;
}
'''


def harness(names):
    decls = ''.join(f'GuestFn {c};\n' for c in names.values())
    defines = ''.join(f'#define FN_{n} {c}\n' for n, c in names.items())
    return HARNESS.replace('#include "recomp.h"\n', '#include "recomp.h"\n' + decls + defines, 1)


HOST = {'win32': 'windows', 'darwin': 'mac'}.get(sys.platform, 'linux')


def rosetta():
    """Whether this is an Apple silicon Mac that runs x86-64 programs."""
    if sys.platform != 'darwin' or platform.machine() != 'arm64' or not shutil.which('clang'):
        return False
    try:
        return subprocess.run(['arch', '-x86_64', '/usr/bin/true'], capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def usable_cpus():
    """The processors this process may run on (an affinity mask or a container's limit), not the machine's."""
    if hasattr(os, 'sched_getaffinity'):
        return len(os.sched_getaffinity(0))
    if hasattr(os, 'process_cpu_count'):   # (Python 3.13)
        return os.process_cpu_count() or 1
    return os.cpu_count() or 1


class LiftTests(unittest.TestCase):
    def test_programs_lift(self):
        """Every program lifts without a trap; the orderings come from lda / sta / the exclusive helpers and
        the fence, not from anything left out."""
        text, names = module_c()
        self.assertEqual(len(names), len(PROGRAMS))
        self.assertNotIn('rt_trap', text)
        self.assertIn('__atomic_thread_fence(__ATOMIC_SEQ_CST);', text)
        for helper in ('lda64(', 'sta64(', 'lda128(', 'cas128(', 'lda32('):
            self.assertIn(helper, text)


class RunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp(prefix='litmus-'))
        cls.gen, cls.names = module_c()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def build_and_run(self, name):
        if usable_cpus() < 4:
            self.skipTest('needs 4 processors (IRIW and the lock run four threads at once)')
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
        (out / 'harness.c').write_text(harness(self.names), encoding='utf-8')
        for level in ('O2', 'O0'):
            with self.subTest(toolchain=name, level=level):
                objs = []
                for src in ('gen.c', 'harness.c'):
                    obj = out / f'{Path(src).stem}_{level}{tc.obj}'
                    cmd = tc.compile_cmd(str(out / src), str(obj), tc.opts[level])
                    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
                    self.assertEqual(r.returncode, 0, f'{" ".join(cmd)}\n{r.stdout}{r.stderr}')
                    objs.append(str(obj))
                exe = out / f'litmus_{level}{tc.exe}'
                link = tc.link_cmd(objs, str(exe))
                r = subprocess.run(link, capture_output=True, text=True, timeout=300)
                self.assertEqual(r.returncode, 0, f'{" ".join(link)}\n{r.stdout}{r.stderr}')
                r = subprocess.run([*tc.run, str(exe)], capture_output=True, text=True, timeout=900)
                self.assertEqual((r.returncode, r.stdout.splitlines()[-1:]), (0, ['ok']), r.stdout + r.stderr)

    def test_this_os(self):
        self.build_and_run(HOST)

    @unittest.skipUnless(rosetta(), 'needs an Apple silicon Mac with Rosetta 2 and clang')
    def test_x86_64_under_rosetta(self):
        self.build_and_run('rosetta')


if __name__ == '__main__':
    unittest.main()
