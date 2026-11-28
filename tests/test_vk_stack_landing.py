"""The compiling threads' landing past their stacks' top (runtime/gpu_vk_landing.c, SSBU_VK_STACK_LANDING) and
gpu_vk.c's skip list (SSBU_VK_SKIP_PIPE), compiled into small test programs (Windows: Win32 threads).

No GeForce RTX 50 card here: a made-up reader stands in for its driver's shader compiler, which reads past the top
of the stack of the thread that asked for the pipeline (0.5.3: the top + 0 to 8; 0.5.4, with #629's 64 KB of zeros:
the top + 0x10004)."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc

PRELUDE = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
'''

# Threads made as gpu_vk.c's are (vk_landing_thread), all alive at once, their stacks side by side as the makers'.
# Each reads the 4 bytes at its stack top + OFFSET; SCAN=1: it reads 4-byte words upward from its top
# until one is all ones (the driver's read as a search), and says where it stopped. The report's lines after.
LANDING_MAIN = r'''
HANDLE vk_landing_thread(SIZE_T stack, DWORD flags, LPTHREAD_START_ROUTINE fn, void *arg);
void vk_landing_report(void (*line)(const char *text));
int vk_landing_status(char *text, size_t n);
static volatile LONG faults, fine;
static volatile unsigned last;
static volatile LONG64 stopped = -1;
static __declspec(noinline) unsigned past(const u8 *p) { return *(volatile const unsigned *)p; }
static DWORD WINAPI worker(void *arg) {
    const u8 *top = (const u8 *)((NT_TIB *)NtCurrentTeb())->StackBase;
    const char *scan = getenv("SCAN");
    __try {
        if (scan && *scan == '1') {
            const u8 *p = top;   /* (from the top: the stack below it has words of all ones of its own) */
            while (past(p) != 0xffffffffu) p += 4;
            stopped = (LONG64)(p - top);
        } else {
            last = past(top + (size_t)arg);
        }
        InterlockedIncrement(&fine);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&faults); }
    Sleep(300);
    return 0;
}
static void say(const char *text) { printf("REPORT %s", text); }
int main(void) {
    const char *o = getenv("OFFSET");
    size_t offset = o ? (size_t)strtoull(o, NULL, 0) : 4;
    HANDLE t[24];
    for (int i = 0; i < 24; i++) t[i] = vk_landing_thread(i == 23 ? 0 : 64ull << 20, i == 23 ? 0 : STACK_SIZE_PARAM_IS_A_RESERVATION, worker, (void *)offset);
    WaitForMultipleObjects(24, t, TRUE, INFINITE);
    printf("%ld %ld %08x %lld\n", fine, faults, last, (long long)stopped);
    vk_landing_report(say);
    char status[300];
    if (vk_landing_status(status, sizeof status)) printf("STATUS %s", status);
    return 0;
}
'''


def block(start, end):
    source = (ROOT / 'runtime/gpu_vk.c').read_text()
    a = source.index(start)
    return source[a:source.index(end, a)]


@unittest.skipUnless(os.name == 'nt', 'the compiling threads are Win32 threads')
class StackLandingTests(unittest.TestCase):
    def build(self, code, directory, *more):
        src, exe = Path(directory) / 't.c', Path(directory) / 't.exe'
        src.write_text(code)
        result = subprocess.run([cc.CLANG, '/nologo', '/O2', *cc.LINK_FLAGS, str(src), *map(str, more), '/Fo' + directory + os.sep,
                                 '/Fe' + str(exe), '/link', *cc.LINK_OPTIONS], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return exe

    def build_and_run(self, code, env=None):
        with tempfile.TemporaryDirectory() as directory:
            exe = self.build(code, directory)
            return subprocess.run([str(exe)], capture_output=True, text=True, env={**os.environ, **(env or {})})

    def landing(self, **env):
        if not hasattr(type(self), 'exe'):
            type(self).dir = tempfile.TemporaryDirectory()
            type(self).exe = self.build(PRELUDE + LANDING_MAIN, type(self).dir.name, ROOT / 'runtime' / 'gpu_vk_landing.c')
        clean = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_VK_STACK_LANDING')}
        run = subprocess.run([str(self.exe)], capture_output=True, text=True, timeout=120, env={**clean, **env})
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        lines = run.stdout.splitlines()
        fine, faults, value, stopped = lines[0].split()
        return int(fine), int(faults), value, int(stopped), '\n'.join(lines[1:])

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, 'dir'):
            cls.dir.cleanup()

    def test_default_reads_past_the_top_are_ones(self):
        fine, faults, value, _s, report = self.landing()
        self.assertEqual((fine, faults, value), (24, 0, 'ffffffff'))
        self.assertIn('SSBU_VK_STACK_LANDING=2, 16 MB, filled with 0xff', report)
        self.assertIn('24 of their own, 0 shared, 0 none', report)

    def test_past_the_first_64_kb_it_grows_as_read(self):
        """0.5.4's read (the top + 0x10004): committed as it is read, filled the same, and said"""
        fine, faults, value, _s, report = self.landing(OFFSET='0x10004')
        self.assertEqual((fine, faults, value), (24, 0, 'ffffffff'))
        self.assertIn('grown 24 times as read, farthest read the top + 0x10004', report)
        self.assertIn('STATUS [status] vulkan: the driver read past a compiling thread', report)
        fine, faults, _v, _s, report = self.landing(OFFSET=hex(15 << 20))
        self.assertEqual((fine, faults), (24, 0))

    def test_past_the_landing_the_fault_is_named(self):
        fine, faults, _v, _s, report = self.landing(OFFSET='0x1000004')   # (16 MB + 4: past it)
        self.assertEqual((fine, faults), (0, 24))
        self.assertIn("a read past a compiling thread's stack top the landing did not take: thread ", report)
        self.assertIn('at the stack top + 0x1000004', report)
        fine, faults, _v, _s, report = self.landing(OFFSET='0x100004', SSBU_VK_STACK_LANDING_MB='1')
        self.assertEqual((fine, faults), (0, 24))
        self.assertIn('1 MB, filled with 0xff', report)

    def test_a_search_for_an_end_stops_at_the_top(self):
        """A reader looking for an all-ones word past the top: the landing's first word"""
        fine, faults, _v, stopped, _r = self.landing(SCAN='1')
        self.assertEqual((fine, faults, stopped), (24, 0, 0))
        fine, faults, _v, _s, report = self.landing(SCAN='1', SSBU_VK_STACK_LANDING_FILL='zero', SSBU_VK_STACK_LANDING_MB='1')
        self.assertEqual((fine, faults), (0, 24))   # (zeros: it reads on to the landing's end, as 0.5.4's did)
        self.assertIn('at the stack top + 0x100000', report)

    def test_fill(self):
        self.assertEqual(self.landing(OFFSET='0x20000', SSBU_VK_STACK_LANDING_FILL='zero')[:3], (24, 0, '00000000'))
        self.assertEqual(self.landing(OFFSET='0x20000', SSBU_VK_STACK_LANDING_FILL='5a')[:3], (24, 0, '5a5a5a5a'))

    def test_629_as_it_was(self):
        """SSBU_VK_STACK_LANDING=1: #629's 64 KB of zeros; past them a fault, which the report now places"""
        fine, faults, value, _s, report = self.landing(SSBU_VK_STACK_LANDING='1')
        self.assertEqual((fine, faults, value), (24, 0, '00000000'))
        self.assertIn('(SSBU_VK_STACK_LANDING=1): 24 of their own, 0 shared, 0 none', report)
        fine, faults, _v, _s, report = self.landing(SSBU_VK_STACK_LANDING='1', OFFSET='0x10004')
        self.assertEqual((fine, faults), (0, 24))
        self.assertIn('at the stack top + 0x10004', report)
        self.assertNotIn('STATUS', report)

    def test_off_is_as_before(self):
        # (without it a read just past a 64 MB stack's top faults, as the driver's did: the test proves the case is real)
        fine, faults, _v, _s, report = self.landing(SSBU_VK_STACK_LANDING='0')
        self.assertEqual((fine, faults), (0, 24))
        self.assertEqual(report, '')

    def test_gpu_vk_starts_its_compiling_threads_so(self):
        source = (ROOT / 'runtime/gpu_vk.c').read_text()
        self.assertIn('return vk_landing_thread(stack, flags, fn, arg);', source)
        self.assertEqual(source.count('compile_thread('), 4)   # (its definition, the makers, the preparation, the warm threads)
        self.assertIn('vk_landing_report(line);', source)

    def skip_program(self):
        skip = block('/* SSBU_VK_SKIP_PIPE: record sums', 'static DWORD WINAPI maker_main')
        return PRELUDE + skip + r'''
int main(void) {
    int a = pipe_skip_listed(0x1a2b3c4d, 0, 0), b = pipe_skip_listed(5, 0x0123456789abcdefull, 0),
        c = pipe_skip_listed(5, 6, 0xfedcba9876543210ull), d = pipe_skip_listed(5, 6, 7), e = pipe_skip_listed(0, 0, 0);
    printf("%d %d %d %d %d %d\n", nskip_pipes, a, b, c, d, e);
    return 0;
}
'''

    def test_skip_list(self):
        run = self.build_and_run(self.skip_program(), {'SSBU_VK_SKIP_PIPE': '1a2b3c4d, 0123456789abcdef;FEDCBA9876543210'})
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.stdout.split()[:6], ['3', '1', '1', '1', '0', '0'])

    def test_skip_list_unset_skips_nothing(self):
        run = self.build_and_run(self.skip_program(), {'SSBU_VK_SKIP_PIPE': ''})
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.stdout.split(), ['0', '0', '0', '0', '0', '0'])


if __name__ == '__main__':
    unittest.main()
