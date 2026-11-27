"""gpu_vk.c's compiling threads: the landing past their stacks' top (SSBU_VK_STACK_LANDING) and the
skip list (SSBU_VK_SKIP_PIPE), compiled out of the source into small test programs (Windows: Win32 threads)."""
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


def block(start, end):
    source = (ROOT / 'runtime/gpu_vk.c').read_text()
    a = source.index(start)
    return source[a:source.index(end, a)]


@unittest.skipUnless(os.name == 'nt', 'the compiling threads are Win32 threads')
class StackLandingTests(unittest.TestCase):
    def build_and_run(self, code, env=None):
        with tempfile.TemporaryDirectory() as directory:
            src, exe = Path(directory) / 't.c', Path(directory) / 't.exe'
            src.write_text(code)
            result = subprocess.run([cc.CLANG, '/nologo', '/O2', *cc.LINK_FLAGS, str(src), '/Fo' + directory + os.sep,
                                     '/Fe' + str(exe), '/link', *cc.LINK_OPTIONS], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return subprocess.run([str(exe)], capture_output=True, text=True, env={**os.environ, **(env or {})})

    def landing_program(self):
        landing = block('static int stack_landing_on(void) {', '/* What each compiling thread is making')
        return PRELUDE + landing + r'''
static volatile LONG faults, fine;
static __declspec(noinline) unsigned past(const u8 *p) { return *(volatile const unsigned *)p; }
static DWORD WINAPI worker(void *arg) {
    (void)arg;
    const u8 *top = (const u8 *)((NT_TIB *)NtCurrentTeb())->StackBase;
    __try { volatile unsigned v = past(top + 4); (void)v; InterlockedIncrement(&fine); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&faults); }
    Sleep(300);   /* (all of them alive at once, their stacks side by side, as the makers') */
    return 0;
}
int main(void) {
    HANDLE t[24];
    for (int i = 0; i < 24; i++) t[i] = compile_thread(64ull << 20, STACK_SIZE_PARAM_IS_A_RESERVATION, worker, NULL);
    WaitForMultipleObjects(24, t, TRUE, INFINITE);
    printf("%ld %ld\n", fine, faults);
    return 0;
}
'''

    def test_landing_makes_the_bytes_past_the_top_readable(self):
        run = self.build_and_run(self.landing_program())
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.stdout.split(), ['24', '0'])

    def test_off_is_as_before(self):
        # (without it a read just past a 64 MB stack's top faults, as the driver's did: the test proves the case is real)
        run = self.build_and_run(self.landing_program(), {'SSBU_VK_STACK_LANDING': '0'})
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.stdout.split(), ['0', '24'])

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
