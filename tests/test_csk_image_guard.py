"""The CSK Collection's picture loads with an old loader (runtime/hle_skyline.c image_load): a call to
main+0x323c4e0 from the plugin's code that repeats the last call's loader and arguments (what its hook
kept, maybe from a screen that is gone) is left out, the original not called; the game's calls, and the
plugin's calls with a loader of their own, reach it with their registers unchanged. No game files needed:
the function is compiled from the runtime's source with a made-up Cpu."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

CSK_LO, CSK_HI = 0x7266000000, 0x7266200000   # (made-up: the plugin's image)
FROM_GAME = 0x7101861E60 + 0x40               # a return address in main
FROM_CSK = CSK_LO + 0x10F1A4                  # a return address in the plugin


def function_source(name):
    source = (ROOT / "runtime/hle_skyline.c").read_text(encoding="utf-8")
    begin = source.index(f"static void {name}(Cpu *c)")
    end = source.index("\n}\n", begin) + 3
    return source[begin:end]


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32;
typedef struct { u64 x[31]; u64 sp; } Cpu;
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
static void AcquireSRWLockExclusive(SRWLOCK *l) { (void)l; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { (void)l; }
static unsigned long GetCurrentThreadId(void) { return 11; }
static const char *rt_describe(u64 a, char *buf, size_t n) { snprintf(buf, n, "%%#llx", (unsigned long long)a); return buf; }
static u64 image_load_original = 0x7100001000ull, csk_lo = 0x%(lo)xull, csk_hi = 0x%(hi)xull;
static int csk_guard = 1, csk_guard_log;
static SRWLOCK image_load_lock = SRWLOCK_INIT;
static struct { u64 x0, x2; u32 w3; } image_load_last;
static u64 image_load_game;
static int image_load_watch;
static u32 csk_replays;
static int calls;
static u64 seen[31];
static void rt_call(Cpu *c, u64 at) {   /* (the original: notes the registers it was given, returns 0x5a in x0) */
    if (at != image_load_original) { printf("wrong target\n"); exit(1); }
    calls++;
    memcpy(seen, c->x, sizeof seen);
    c->x[0] = 0x5a;
}
%(function)s
/* argv: guard (0/1), then calls "from x0 x2 w3": prints for each "called|left out" and whether the
 * registers the original saw (or, left out, those after) were the ones passed */
int main(int argc, char **argv) {
    csk_guard = atoi(argv[1]);
    for (int i = 2; i + 3 < argc; i += 4) {
        Cpu c;
        for (int r = 0; r < 31; r++) c.x[r] = 0x1000 + r;
        c.x[30] = strtoull(argv[i], 0, 16);
        c.x[0] = strtoull(argv[i + 1], 0, 16);
        c.x[2] = strtoull(argv[i + 2], 0, 16);
        c.x[3] = strtoull(argv[i + 3], 0, 16);
        Cpu in = c;
        int before = calls;
        image_load(&c);
        if (calls != before) printf("called %%s %%llx\n", memcmp(seen, in.x, sizeof seen) ? "changed" : "same",
                                    (unsigned long long)c.x[0]);
        else printf("left-out %%s\n", memcmp(c.x, in.x, sizeof in.x) ? "changed" : "same");
    }
    return 0;
}
'''


class ImageLoadGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("C compiler not installed")
        cls.tmp = tempfile.TemporaryDirectory(prefix="csk image guard test ")
        src = Path(cls.tmp.name) / "guard.c"
        src.write_text(HARNESS % {"function": function_source("image_load"), "lo": CSK_LO, "hi": CSK_HI},
                       encoding="utf-8")
        cls.exe = Path(cls.tmp.name) / ("guard.exe" if os.name == "nt" else "guard")
        subprocess.run([compiler, "-std=c11", "-Wall", "-Werror", "-o", str(cls.exe), str(src)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_calls(self, *calls, guard=1):
        args = [str(self.exe), str(guard)] + [f"{v:x}" for call in calls for v in call]
        out = subprocess.run(args, check=True, capture_output=True, text=True).stdout
        return out.strip().split("\n")

    def test_a_call_from_the_game_reaches_the_function_unchanged(self):
        self.assertEqual(self.run_calls((FROM_GAME, 0x1234560000, 0x77, 2)), ["called same 5a"])

    def test_the_plugin_repeating_the_last_call_is_left_out(self):
        # the game loads with loader A on one screen; on a later screen the plugin calls again with A
        self.assertEqual(self.run_calls((FROM_GAME, 0x1234560000, 0x77, 2), (FROM_CSK, 0x1234560000, 0x77, 2)),
                         ["called same 5a", "left-out same"])

    def test_the_game_still_reaches_it_after_a_left_out_call(self):
        self.assertEqual(self.run_calls((FROM_GAME, 0x1234560000, 0x77, 2), (FROM_CSK, 0x1234560000, 0x77, 2),
                                        (FROM_GAME, 0x2234560000, 0x77, 2)),
                         ["called same 5a", "left-out same", "called same 5a"])

    def test_the_plugin_with_a_loader_of_its_own_reaches_it(self):
        self.assertEqual(self.run_calls((FROM_GAME, 0x1234560000, 0x77, 2), (FROM_CSK, 0x3234560000, 0x10, 1)),
                         ["called same 5a", "called same 5a"])

    def test_with_the_guard_off_every_call_reaches_it(self):
        self.assertEqual(self.run_calls((FROM_GAME, 0x1234560000, 0x77, 2), (FROM_CSK, 0x1234560000, 0x77, 2),
                                        guard=0),
                         ["called same 5a", "called same 5a"])


if __name__ == "__main__":
    unittest.main()
