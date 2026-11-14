"""runtime/gpu_wait_budget.h: a frame's waits for new shaders and pipelines (gpu_vk.c) share one
budget, each wait also kept to its own cap, and a new frame starts with the whole budget again."""
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

MS = 1000000
SOURCE = r'''#include <stdio.h>
#include "gpu_wait_budget.h"
#define MS 1000000ull
#define CHECK(x) do { if (!(x)) { printf("failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)
int main(void) {
    WaitBudget b = {~0ull, 0, 6 * MS};
    CHECK(wait_budget_until(&b, 1, 1000, 100 * MS) == 1000 + 6 * MS);   /* (the budget, under the 100 ms cap) */
    wait_budget_spend(&b, 4 * MS);
    CHECK(wait_budget_until(&b, 1, 2000, 100 * MS) == 2000 + 2 * MS);   /* (what is left of it) */
    CHECK(wait_budget_until(&b, 1, 3000, 1 * MS) == 3000 + 1 * MS);     /* (a wait's own cap, when smaller) */
    wait_budget_spend(&b, 3 * MS);
    CHECK(wait_budget_until(&b, 1, 4000, 100 * MS) == 4000);            /* (spent: no wait) */
    CHECK(wait_budget_until(&b, 2, 5000, 100 * MS) == 5000 + 6 * MS);   /* (the next frame: the whole budget) */
    WaitBudget none = {~0ull, 0, 0};
    CHECK(wait_budget_until(&none, 1, 7000, 100 * MS) == 7000);         /* (SSBU_VK_WAIT_BUDGET=0) */
    puts("ok");
    return 0;
}
'''


@unittest.skipUnless(HAVE, 'Windows with clang-cl only')
class WaitBudget(unittest.TestCase):
    def test_budget(self):
        with tempfile.TemporaryDirectory(prefix='waitbudget ') as tmp:
            src, exe = os.path.join(tmp, 'waitbudget.c'), os.path.join(tmp, 'waitbudget.exe')
            Path(src).write_text(SOURCE)
            r = subprocess.run([cc.CLANG, '/nologo', *cc.LINK_FLAGS, '/I', str(ROOT / 'runtime'), src, '/Fe' + exe,
                                '/Fo' + tmp + os.sep, '/link', *cc.LINK_OPTIONS], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
            self.assertEqual((r.returncode, r.stdout.strip()), (0, 'ok'), r.stdout)


if __name__ == '__main__':
    unittest.main()
