"""runtime/crash_address.h: a crash at an address no loaded module holds (host.c crash_report, SSBU_CRASH_UNLOADED).
A made-up DLL is loaded, a function's address in it taken, the DLL unloaded: crash_unknown_address must say what the
memory there is now and name the DLL among the unloaded modules, marked as the one the pc and a stack word are in;
with only an address touched, no whole list, just the unloaded module that held it."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

if os.name == 'nt':
    import cc  # noqa: E402
    HAVE = os.path.exists(cc.CLANG)
else:
    HAVE = False

DLL = r'''__declspec(dllexport) int made_up_hook(int x) { return x * 3 + 1; }
'''

SOURCE = r'''#include <stdio.h>
#include <string.h>
static char text[16384];
static int used;
#define CRASH_OUT(...) (used += snprintf(text + used, sizeof text - used, __VA_ARGS__))
#include "crash_address.h"
#define CHECK(x) do { if (!(x)) { printf("failed: %s (line %d)\n%s\n", #x, __LINE__, text); return 1; } } while (0)
int main(int argc, char **argv) {
    HMODULE m = LoadLibraryA(argv[1]);
    CHECK(m != NULL);
    CrashAddr hook = (CrashAddr)(uintptr_t)GetProcAddress(m, "made_up_hook");
    CHECK(hook && crash_in_module(hook));
    CHECK(FreeLibrary(m) && !crash_in_module(hook));
    volatile CrashAddr stack[8] = {0, 1, hook + 0x10, 3};   /* (a return address into it, as a hook's caller leaves) */
    crash_unknown_address(hook, 0, (CrashAddr)(uintptr_t)stack, (CrashAddr)(uintptr_t)(stack + 8));
    CHECK(strstr(text, "the faulting pc 0x") && (strstr(text, ": free") || strstr(text, ": reserved")));
    CHECK(strstr(text, "unloaded modules (oldest first"));
    char want[64];
    snprintf(want, sizeof want, "<- the faulting pc (+%#llx)", hook - (CrashAddr)(uintptr_t)m);
    const char *line = strstr(text, "crashaddr_dll.dll");
    CHECK(line && strstr(line, want) && strstr(line, "<- a stack word"));
    /* only an address touched: its region, and the one unloaded module that held it - not the whole list */
    used = 0; text[0] = 0;
    crash_unknown_address(0, hook + 8, 0, 0);
    CHECK(strstr(text, "the address touched 0x") && !strstr(text, "the faulting pc") && !strstr(text, "oldest first"));
    CHECK(strstr(text, "unloaded module crashaddr_dll.dll") && strstr(text, "<- the address touched (+"));
    /* a heap pointer: one region line, nothing unloaded */
    used = 0; text[0] = 0;
    void *heap = HeapAlloc(GetProcessHeap(), 0, 64);
    crash_unknown_address(0, (CrashAddr)(uintptr_t)heap, 0, 0);
    CHECK(strstr(text, "the address touched 0x") && strstr(text, "committed private") && !strstr(text, "unloaded module"));
    puts("ok");
    return 0;
}
'''


@unittest.skipUnless(HAVE, 'Windows with clang-cl only')
class CrashAddress(unittest.TestCase):
    def test_unloaded_dll_named(self):
        with tempfile.TemporaryDirectory(prefix='crashaddr ') as tmp:
            dll_src, dll = os.path.join(tmp, 'crashaddr_dll.c'), os.path.join(tmp, 'crashaddr_dll.dll')
            src, exe = os.path.join(tmp, 'crashaddr.c'), os.path.join(tmp, 'crashaddr.exe')
            Path(dll_src).write_text(DLL)
            Path(src).write_text(SOURCE)
            r = subprocess.run([cc.CLANG, '/nologo', '/LD', *cc.LINK_FLAGS, dll_src, '/Fe' + dll, '/Fo' + tmp + os.sep,
                                '/link', *cc.LINK_OPTIONS], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([cc.CLANG, '/nologo', *cc.LINK_FLAGS, '/I', str(ROOT / 'runtime'), src, '/Fe' + exe,
                                '/Fo' + tmp + os.sep, '/link', *cc.LINK_OPTIONS], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([exe, dll], capture_output=True, text=True, timeout=30)
            self.assertEqual((r.returncode, r.stdout.strip()), (0, 'ok'), r.stdout)


if __name__ == '__main__':
    unittest.main()
