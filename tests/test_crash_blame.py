"""runtime/crash_blame.h: which guest modules a crash report's first line may name as a plugin (host.c crash_blame,
SSBU_CRASH_BLAME): any but the game's own (main, sdk, subsdk<n>, rtld, lua2cpp_<name>), and never an address; and the
known crashes (known_crash, SSBU_CRASH_BLAME_KNOWN): the CSK Collection's null read in the game's code, named only with
the plugin (or a copy of it) loaded, a read below 0x1000, and its frames among the innermost five."""
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
static const char *csk_on(const char *p) { return !strcmp(p, "the_csk_collection") ? "the_csk_collection_" : NULL; }
static const char *csk_off(const char *p) { (void)p; return NULL; }
static int K(const char *const *f, int n, int near_null, const char *(*loaded)(const char *)) {
    const char *m = NULL, *fr = NULL;
    const KnownCrash *c = known_crash(f, n, near_null, loaded, &m, &fr);
    if (!c) return 0;
    if (strcmp(m, "the_csk_collection_") || strcmp(c->plugin, "the_csk_collection")) return -1;
    return !strcmp(fr, "main+0x35c2890") ? 1 : !strcmp(fr, "main+0x35c7aa0") ? 2 : -2;
}
int main(void) {
    const char *g1[] = {"main+0x35c2890", "main+0x17dde90", "main+0x17eeee0"};
    const char *g1b[] = {"main+0x3602380", "main+0x36026c0", "main+0x35c7aa0", "main+0x35e0e50"};
    const char *g1c[] = {"smashline+0x3faf0", "main+0x35c7aa0", "main+0x35e0e50"};
    const char *deep[] = {"main+0x1", "main+0x2", "main+0x3", "main+0x4", "main+0x5", "main+0x35c2890"};
    const char *other[] = {"main+0x35c2894", "main+0x17dde90"};
    CHECK(K(g1, 3, 1, csk_on) == 1 && K(g1b, 4, 1, csk_on) == 2 && K(g1c, 3, 1, csk_on) == 2);
    CHECK(K(g1, 3, 0, csk_on) == 0 && K(g1, 3, 1, csk_off) == 0);   /* (not a null read; the plugin not loaded) */
    CHECK(K(deep, 6, 1, csk_on) == 0 && K(other, 2, 1, csk_on) == 0 && K(g1, 0, 1, csk_on) == 0);

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
