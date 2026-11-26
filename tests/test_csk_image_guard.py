"""The CSK Collection's picture loads with an old loader (runtime/hle_skyline.c image_load): a call to
main+0x323c4e0 from the plugin's code that repeats the last call's loader and arguments (what its hook
kept, maybe from a screen that is gone) is left out, the original not called; the game's calls, and the
plugin's calls with a loader of their own, reach it with their registers unchanged. A copy of the plugin
loaded under another name (the_csk_collection_: a mod's own build of it) is guarded the same way. No game
files needed: the functions are compiled from the runtime's source with a made-up Cpu."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

CSK_LO, CSK_HI = 0x7266000000, 0x7266200000   # (made-up: the plugin's image)
COPY_LO, COPY_HI = 0x7268000000, 0x7268200000  # (made-up: a second copy's, the_csk_collection_)
FROM_GAME = 0x7101861E60 + 0x40               # a return address in main
FROM_CSK = CSK_LO + 0x10F1A4                  # a return address in the plugin
FROM_COPY = COPY_LO + 0x10F1A4                # the same place in the copy


def function_source(name, head=None):
    source = (ROOT / "runtime/hle_skyline.c").read_text(encoding="utf-8")
    begin = source.index(head or f"static void {name}(Cpu *c)")
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
static u64 image_load_original = 0x7100001000ull;
#define CSK_COPIES 4
static u64 csk_lo[CSK_COPIES] = {0x%(lo)xull, 0x%(lo2)xull}, csk_hi[CSK_COPIES] = {0x%(hi)xull, 0x%(hi2)xull};
static int csk_copies = 2;
#define CSK_NAME "the_csk_collection"
%(from_csk)s
%(csk_place)s
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
    if (!strcmp(argv[1], "names")) {   /* (csk_place: each name after it, 1 or 0) */
        for (int i = 2; i < argc; i++) printf("%%d\n", csk_place(argv[i]));
        return 0;
    }
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
        src.write_text(HARNESS % {"function": function_source("image_load"), "lo": CSK_LO, "hi": CSK_HI,
                                  "lo2": COPY_LO, "hi2": COPY_HI,
                                  "from_csk": function_source(None, "static int from_csk(u64 from)"),
                                  "csk_place": function_source(None, "static int csk_place(const char *described)")},
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

    def test_a_renamed_copy_repeating_the_last_call_is_left_out(self):
        # (0.5.1's reports: the_csk_collection_+0x10f0b8 -> main+0x323c4e0, with no guard for the copy)
        self.assertEqual(self.run_calls((FROM_GAME, 0x1234560000, 0x77, 2), (FROM_COPY, 0x1234560000, 0x77, 2),
                                        (FROM_COPY, 0x3234560000, 0x10, 1)),
                         ["called same 5a", "left-out same", "called same 5a"])

    def test_the_plugin_s_names_with_or_without_the_underscores(self):
        names = ["the_csk_collection+0xe8c88", "the_csk_collection_+0x10f0b8", "the_csk_collection___+0x10",
                 "the_csk_collectionx+0x10", "the_csk_collection", "main+0x323c4e0", "smashline+0x147a8"]
        out = subprocess.run([str(self.exe), "names", *names], check=True, capture_output=True, text=True).stdout
        self.assertEqual(out.split(), ["1", "1", "1", "0", "0", "0", "0"])

    def test_with_the_guard_off_every_call_reaches_it(self):
        self.assertEqual(self.run_calls((FROM_GAME, 0x1234560000, 0x77, 2), (FROM_CSK, 0x1234560000, 0x77, 2),
                                        guard=0),
                         ["called same 5a", "called same 5a"])


if __name__ == "__main__":
    unittest.main()
