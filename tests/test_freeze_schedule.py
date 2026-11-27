"""runtime/freeze_schedule.h: when the freeze watchdog (debug_watchdog.c) writes a report. By default the first
after 15 s with no frame (a stall of 5-15 s is a log line), with SSBU_FREEZE_CONFIRM=0 at 5, 30 and 120 s as
before."""
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
#include "freeze_schedule.h"
#define CHECK(x) do { if (!(x)) { printf("failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)
/* the watchdog's looks every 500 ms from t0 to t1, frames last presented at `last`: the actions seen */
static int run(FreezeSchedule *s, unsigned long long t0, unsigned long long t1, unsigned long long last, int *reports,
               unsigned long long *first_len) {
    int other = FREEZE_NONE;
    for (unsigned long long t = t0; t <= t1; t += 500) {
        unsigned long long len = 0;
        int a = freeze_schedule_tick(s, t, last, &len);
        if (a == FREEZE_REPORT) { if (!(*reports)++) *first_len = len; }
        else if (a != FREEZE_NONE) other = a;
    }
    return other;
}
int main(void) {
    FreezeSchedule s; int n; unsigned long long len, first;
    /* default: 15 s, 30 s, 120 s */
    freeze_schedule_init(&s, NULL);
    CHECK(!s.old && s.at[0] == 15000 && s.at[1] == 30000 && s.at[2] == 120000);
    /* a 6 s stall: no report, one stall line with its length when frames come again */
    n = 0; first = 0;
    CHECK(run(&s, 1000, 7000, 1000, &n, &first) == FREEZE_NONE && n == 0);
    CHECK(freeze_schedule_tick(&s, 7500, 7200, &len) == FREEZE_STALL_ENDED && len == 6200);
    CHECK(freeze_schedule_tick(&s, 8000, 7900, &len) == FREEZE_NONE);
    /* a hang: reports at 15, 30 and 120 s, then frames again */
    n = 0; first = 0;
    CHECK(run(&s, 10000, 10000 + 130000, 10000, &n, &first) == FREEZE_NONE && n == 3 && first == 15000);
    CHECK(freeze_schedule_tick(&s, 141000, 140900, &len) == FREEZE_RESUMED && len == 130900);
    /* a short gap (under 5 s): nothing */
    CHECK(freeze_schedule_tick(&s, 150000, 146000, &len) == FREEZE_NONE);
    CHECK(freeze_schedule_tick(&s, 150500, 150400, &len) == FREEZE_NONE);
    /* SSBU_FREEZE_CONFIRM=0 and =5: the old schedule, a report for a 6 s stall, no stall line */
    const char *old[] = {"0", "5"};
    for (int i = 0; i < 2; i++) {
        freeze_schedule_init(&s, old[i]);
        CHECK(s.old && s.at[0] == 5000 && s.at[1] == 30000 && s.at[2] == 120000);
        n = 0; first = 0;
        CHECK(run(&s, 1000, 7000, 1000, &n, &first) == FREEZE_NONE && n == 1 && first == 5000);
        CHECK(freeze_schedule_tick(&s, 7500, 7200, &len) == FREEZE_RESUMED);
        n = 0;
        CHECK(run(&s, 8000, 13000, 7200, &n, &first) == FREEZE_NONE && n == 1);
        CHECK(freeze_schedule_tick(&s, 13500, 13400, &len) == FREEZE_RESUMED);
    }
    /* longer: 40 s -> 40, 80, 120; 90 s -> 90, 180, 360; nonsense -> the default */
    freeze_schedule_init(&s, "40");
    CHECK(!s.old && s.at[0] == 40000 && s.at[1] == 80000 && s.at[2] == 120000);
    freeze_schedule_init(&s, "90");
    CHECK(s.at[0] == 90000 && s.at[1] == 180000 && s.at[2] == 360000);
    freeze_schedule_init(&s, "-3");
    CHECK(s.at[0] == 15000);
    freeze_schedule_init(&s, "");
    CHECK(s.at[0] == 15000);
    puts("ok");
    return 0;
}
'''


@unittest.skipUnless(HAVE, 'Windows with clang-cl only')
class FreezeSchedule(unittest.TestCase):
    def test_schedule(self):
        with tempfile.TemporaryDirectory(prefix='freeze ') as tmp:
            src, exe = os.path.join(tmp, 'freeze.c'), os.path.join(tmp, 'freeze.exe')
            Path(src).write_text(SOURCE)
            r = subprocess.run([cc.CLANG, '/nologo', *cc.LINK_FLAGS, '/I', str(ROOT / 'runtime'), src, '/Fe' + exe,
                                '/Fo' + tmp + os.sep, '/link', *cc.LINK_OPTIONS], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
            self.assertEqual((r.returncode, r.stdout.strip()), (0, 'ok'), r.stdout)


if __name__ == '__main__':
    unittest.main()
