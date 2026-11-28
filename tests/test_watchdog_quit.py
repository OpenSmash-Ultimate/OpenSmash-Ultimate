"""runtime/debug_watchdog.c's watchdog_disarm (called by gpu_backend.c gfx_closing: the settings menu's Quit, the window's
close): a close that saves the caches for seconds is not a freeze - no report after it; a close of a game already
frozen (no frame for over 2 s) keeps the watchdog on; SSBU_WATCHDOG_QUIT=0 keeps it on, as before. debug_watchdog.c is
compiled alone with the renderer's and kernel's parts it asks made up; SSBU_FREEZE_CONFIRM=0 (a report after 5 s)."""
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

SOURCE = r'''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
typedef unsigned long long U64;
DWORD gfx_lock_owner(void) { return 0; }
DWORD gfx_last_enter_thread(void) { return 0; }
DWORD kernel_lock_owner(void) { return 0; }
DWORD nv_lock_owner(void) { return 0; }
DWORD present_queue_holder(U64 *since_ms) { *since_ms = 0; return 0; }
void gfx_report_compiles(void (*line)(const char *text)) { (void)line; }
void kernel_thread_states(void (*line)(const char *text)) { (void)line; }
int profile_dump_threads(void (*line)(const char *text)) { (void)line; return 1; }
static int reports;
void rt_report_path(char *path, size_t n, const char *kind, const char *ext) { reports++; snprintf(path, n, "%s_%d.%s", kind, reports, ext); }
int rt_write_dump(const char *path, DWORD tid, EXCEPTION_POINTERS *e, DWORD wait_ms) { (void)path; (void)tid; (void)e; (void)wait_ms; return 0; }
void watchdog_present(void);
void watchdog_disarm(void);
int main(int argc, char **argv) {
    /* frames for 1 s, then (argv[1]) "quit": the close at once; "frozen": the close 3 s into a stall; "none": no close */
    for (int i = 0; i < 20; i++) { watchdog_present(); Sleep(50); }
    if (!strcmp(argv[1], "quit")) watchdog_disarm();
    if (!strcmp(argv[1], "frozen")) { Sleep(3000); watchdog_disarm(); }
    Sleep(strcmp(argv[1], "frozen") ? 7000 : 4500);   /* (past the 5 s report) */
    fflush(stderr);
    printf("reports %d\n", reports);
    return 0;
}
'''


@unittest.skipUnless(HAVE, 'Windows with clang-cl only')
class WatchdogQuit(unittest.TestCase):
    def test_disarm(self):
        with tempfile.TemporaryDirectory(prefix='wdquit ') as tmp:
            src, exe = os.path.join(tmp, 'wdquit.c'), os.path.join(tmp, 'wdquit.exe')
            Path(src).write_text(SOURCE)
            r = subprocess.run([cc.CLANG, '/nologo', '-D_CRT_SECURE_NO_WARNINGS', *cc.LINK_FLAGS, '/I', str(ROOT / 'runtime'), src,
                                str(ROOT / 'runtime' / 'debug_watchdog.c'), '/Fe' + exe, '/Fo' + tmp + os.sep, '/link',
                                *cc.LINK_OPTIONS], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

            def run(how, **env):
                e = {k: v for k, v in os.environ.items() if not k.startswith(('SSBU_WATCHDOG', 'SSBU_FREEZE'))}
                e.update(SSBU_FREEZE_CONFIRM='0', **env)
                r = subprocess.run([exe, how], cwd=tmp, env=e, capture_output=True, text=True, timeout=60)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                return r.stdout.strip(), r.stderr

            out, err = run('quit')
            self.assertEqual(out, 'reports 0', err)
            self.assertIn('[watchdog] off: the game is closing', err)
            out, err = run('quit', SSBU_WATCHDOG_QUIT='0')
            self.assertEqual(out, 'reports 1', 'SSBU_WATCHDOG_QUIT=0: the report as before\n' + err)
            out, err = run('frozen')
            self.assertEqual(out, 'reports 1', 'a game already frozen at its close: its report\n' + err)
            self.assertIn('[watchdog] stays on: no frame for', err)


if __name__ == '__main__':
    unittest.main()
