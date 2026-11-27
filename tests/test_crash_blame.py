"""runtime/crash_blame.h: which guest modules a crash report's first line may name as a plugin (host.c crash_blame,
SSBU_CRASH_BLAME): any but the game's own (main, sdk, subsdk<n>, rtld, lua2cpp_<name>), and never an address."""
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
#include "crash_blame.h"
#define CHECK(x) do { if (!(x)) { printf("failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)
#define P(s) plugin_module(s, strlen(s))
int main(void) {
    CHECK(!P("main") && !P("sdk") && !P("rtld") && !P("subsdk0") && !P("subsdk9"));
    CHECK(!P("lua2cpp_common") && !P("lua2cpp_mario"));
    CHECK(!P("0x7100001234") && !P("native:memcpy") && !P(""));
    CHECK(P("smashline") && P("arcropolis") && P("cupheadplugin_") && P("ultimate_mod_menu") && P("link_improved"));
    CHECK(P("mainmenu") && P("sdk_tools") && P("n_0"));
    CHECK(plugin_module("smashline+0x3faf0", 9) && !plugin_module("main+0x10", 4));   /* (a part of rt_describe's text) */
    puts("ok");
    return 0;
}
'''


@unittest.skipUnless(HAVE, 'Windows with clang-cl only')
class CrashBlame(unittest.TestCase):
    def test_plugin_module(self):
        with tempfile.TemporaryDirectory(prefix='crashblame ') as tmp:
            src, exe = os.path.join(tmp, 'crashblame.c'), os.path.join(tmp, 'crashblame.exe')
            Path(src).write_text(SOURCE)
            r = subprocess.run([cc.CLANG, '/nologo', *cc.LINK_FLAGS, '/I', str(ROOT / 'runtime'), src, '/Fe' + exe,
                                '/Fo' + tmp + os.sep, '/link', *cc.LINK_OPTIONS], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
            self.assertEqual((r.returncode, r.stdout.strip()), (0, 'ok'), r.stdout)


if __name__ == '__main__':
    unittest.main()
