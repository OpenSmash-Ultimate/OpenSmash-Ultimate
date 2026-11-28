"""runtime/debug_perf_session.c: the session's summary (reports/perf_session.json) says clean_exit true when the game
ends by the player's or its own choice (perf_session_clean_exit: the settings menu's Quit, the settings' restart,
svcExitProcess), false from the 30 s writes; OPENSMASH_CLEAN_EXIT=0 writes nothing there, as before."""
from pathlib import Path
import json
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

SOURCE = r'''#include <windows.h>
void perf_frame_shown(void);
void perf_session_write(int clean);
void perf_session_clean_exit(void);
int main(int argc, char **argv) {
    for (int i = 0; i < 5; i++) { perf_frame_shown(); Sleep(2); }
    perf_session_write(0);   /* (a 30 s write: false) */
    if (argc > 1) perf_session_clean_exit();   /* (the Quit) */
    return 0;
}
'''


@unittest.skipUnless(HAVE, 'Windows with clang-cl only')
class PerfCleanExit(unittest.TestCase):
    def test_clean_exit(self):
        with tempfile.TemporaryDirectory(prefix='perfclean ') as tmp:
            src, exe = os.path.join(tmp, 'perfclean.c'), os.path.join(tmp, 'perfclean.exe')
            Path(src).write_text(SOURCE)
            r = subprocess.run([cc.CLANG, '/nologo', '-D_CRT_SECURE_NO_WARNINGS', *cc.LINK_FLAGS, '/I', str(ROOT / 'runtime'), src,
                                str(ROOT / 'runtime' / 'debug_perf_session.c'), '/Fe' + exe, '/Fo' + tmp + os.sep, '/link',
                                *cc.LINK_OPTIONS], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            summary = os.path.join(tmp, 'reports', 'perf_session.json')

            def run(*args, **env):
                if os.path.exists(summary):
                    os.remove(summary)
                e = {k: v for k, v in os.environ.items() if k not in ('OPENSMASH_CLEAN_EXIT', 'SSBU_PERF_SESSION')}
                e.update(env)
                r = subprocess.run([exe, *args], cwd=tmp, env=e, capture_output=True, text=True, timeout=30)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                with open(summary, encoding='utf-8') as f:
                    return json.load(f)

            self.assertIs(run()['clean_exit'], False, 'a session that ends without a clean end: false')
            self.assertIs(run('quit')['clean_exit'], True, "the Quit's clean end: true")
            self.assertIs(run('quit', OPENSMASH_CLEAN_EXIT='0')['clean_exit'], False, 'OPENSMASH_CLEAN_EXIT=0: as before')
            self.assertEqual(run('quit')['frames'], 5)


if __name__ == '__main__':
    unittest.main()
