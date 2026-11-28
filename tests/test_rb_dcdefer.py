"""runtime/rb_dcdefer.h: SSBU_RB_DCDEFER's page bits (kernel.c). Pages the game gives back in a rollback fight stay
committed (KEPT); a page given back in the fight (ZERO) is cleared when the game maps it anew, also when a load
brought back a frame from before an earlier clear; at the fight's end the KEPT pages not mapped again are
decommitted (=2: they stay). Made-up page numbers and a made-up memory map, no game files."""
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
#include <string.h>
#include "rb_dcdefer.h"
#define CHECK(x) do { if (!(x)) { printf("failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)
#define PAGES 1024
static uint64_t kept[PAGES / 64], zero[PAGES / 64];
static unsigned char mapped[PAGES];          /* the made-up memory map: 1 = in a region */
static unsigned char cleared[PAGES];         /* pages clear() was given */
static unsigned char released[PAGES];        /* pages release() was given */
static int runs;
static int is_mapped(uint64_t i, void *ctx) { (void)ctx; return mapped[i]; }
static void clear(uint64_t i, uint64_t j, void *ctx) { (void)ctx; runs++; for (; i < j; i++) cleared[i]++; }
static void release(uint64_t i, uint64_t j, void *ctx) { (void)ctx; runs++; for (; i < j; i++) released[i]++; }
static void fresh(DcDefer *d) {
    memset(kept, 0, sizeof kept); memset(zero, 0, sizeof zero); memset(mapped, 0, sizeof mapped);
    memset(cleared, 0, sizeof cleared); memset(released, 0, sizeof released); runs = 0;
    memset(d, 0, sizeof *d); d->kept = kept; d->zero = zero; d->lo = ~0ull;
}
static int count(const unsigned char *a, int lo, int hi) { int n = 0; for (int i = lo; i < hi; i++) n += a[i]; return n; }
int main(void) {
    DcDefer d;
    /* the game maps 100..140, gives back 110..120 in the fight: kept, then maps 100..140 again: only 110..120
       (not mapped before) cleared, in one run */
    fresh(&d);
    for (int i = 100; i < 140; i++) mapped[i] = 1;
    for (int i = 110; i < 120; i++) mapped[i] = 0;
    dcd_mark(&d, 110, 120);
    CHECK(d.now == 10 && d.peak == 10 && d.lo == 110 && d.hi == 120);
    dcd_take(&d, 100, 140, 1, is_mapped, clear, NULL);
    CHECK(runs == 1 && count(cleared, 110, 120) == 10 && count(cleared, 0, PAGES) == 10 && d.now == 0 && d.zeroed == 10);
    CHECK(!dcd_bit(kept, 115) && dcd_bit(zero, 115));   /* (ZERO stays in the fight) */
    for (int i = 110; i < 120; i++) mapped[i] = 1;
    /* a load brings back a frame from before that clear (110..120 not mapped then), the re-run maps it again:
       cleared again */
    for (int i = 110; i < 120; i++) mapped[i] = 0;
    memset(cleared, 0, sizeof cleared); runs = 0;
    dcd_take(&d, 110, 120, 1, is_mapped, clear, NULL);
    CHECK(runs == 1 && count(cleared, 110, 120) == 10);
    /* a page mapped now is never cleared, even with ZERO */
    for (int i = 110; i < 120; i++) mapped[i] = 1;
    memset(cleared, 0, sizeof cleared); runs = 0;
    dcd_take(&d, 100, 140, 1, is_mapped, clear, NULL);
    CHECK(runs == 0 && count(cleared, 0, PAGES) == 0);
    /* the fight's end: nothing kept, ZERO goes */
    CHECK(dcd_end(&d, 1, is_mapped, release, NULL) == 0 && !dcd_bit(zero, 115) && d.now == 0 && d.lo == ~0ull);

    /* the end of a fight with pages still given back: two runs released, a page mapped again by a load is not */
    fresh(&d);
    dcd_mark(&d, 200, 210);
    dcd_mark(&d, 300, 302);
    mapped[205] = 1;   /* (a load brought back a frame where 205 was mapped: the game's own again) */
    CHECK(d.now == 12);
    long long f = dcd_end(&d, 1, is_mapped, release, NULL);
    CHECK(f == 11 && count(released, 200, 205) == 5 && count(released, 206, 210) == 4 && count(released, 300, 302) == 2);
    CHECK(!released[205] && runs == 3 && d.now == 0 && !dcd_bit(kept, 205) && !dcd_bit(zero, 300));

    /* =2: nothing released at the end; the next map outside a fight clears them and drops both bits */
    fresh(&d);
    dcd_mark(&d, 64, 192);   /* (whole words) */
    CHECK(dcd_end(&d, 2, is_mapped, release, NULL) == 0 && count(released, 0, PAGES) == 0 && d.now == 128);
    CHECK(dcd_bit(kept, 100) && dcd_bit(zero, 100));
    dcd_take(&d, 0, PAGES, 0, is_mapped, clear, NULL);
    CHECK(count(cleared, 64, 192) == 128 && d.now == 0 && !dcd_bit(zero, 100) && !dcd_bit(kept, 100));

    /* a real decommit forgets the pages; a map outside the marked span does nothing */
    fresh(&d);
    dcd_mark(&d, 500, 510);
    dcd_unmark(&d, 500, 505);
    CHECK(d.now == 5 && !dcd_bit(kept, 502) && !dcd_bit(zero, 502) && dcd_bit(kept, 507));
    dcd_take(&d, 0, 400, 1, is_mapped, clear, NULL);
    CHECK(runs == 0);
    dcd_take(&d, 0, PAGES, 1, is_mapped, clear, NULL);
    CHECK(runs == 1 && count(cleared, 0, PAGES) == 5 && count(cleared, 505, 510) == 5);
    puts("ok");
    return 0;
}
'''


@unittest.skipUnless(HAVE, 'Windows with clang-cl only')
class DcDeferBits(unittest.TestCase):
    def test_bits(self):
        with tempfile.TemporaryDirectory(prefix='dcdefer ') as tmp:
            src, exe = os.path.join(tmp, 'dcdefer.c'), os.path.join(tmp, 'dcdefer.exe')
            Path(src).write_text(SOURCE)
            r = subprocess.run([cc.CLANG, '/nologo', *cc.LINK_FLAGS, '/I', str(ROOT / 'runtime'), src, '/Fe' + exe,
                                '/Fo' + tmp + os.sep, '/link', *cc.LINK_OPTIONS], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
            self.assertEqual((r.returncode, r.stdout.strip()), (0, 'ok'), r.stdout)


class BeforeTheFight(unittest.TestCase):
    """hle_rb_core.c SSBU_RB_DCDEFER_PRE: on before the set-up's copy (its threads read while the guest's run), the
    fight takes it over, a set-up no fight follows lets it go (read from the source: the copies need the game)."""
    source = (ROOT / 'runtime' / 'hle_rb_core.c').read_text(encoding='utf-8')

    def body(self, head):
        a = self.source.index(head)
        return self.source[a:self.source.index('\n}\n', a)]

    def test_on_before_the_set_up_copy(self):
        pre = self.body('static void net_pre_fight(Cpu *c, u64 frame) {')
        first = pre[pre.index('if (pre_frames == 1) {'):]
        self.assertLess(first.index('dcd_pre_start();'), first.index('rb_save(c, frame)'))
        done = pre[pre.index('pre_done = 1;'):]
        self.assertLess(done.index('dcd_pre_start();'), done.index('rb_save(c, frame)'))

    def test_taken_over_or_let_go(self):
        self.assertIn('kernel_rb_defer_fight(1); dcd_pre_fight(1); }', self.source)
        self.assertIn('else { dcd_pre_fight(0); fprintf(stderr, "[rb] this fight needs no savestates', self.source)
        self.assertIn('dcd_pre_drop("a new set-up");', self.body('static void net_pre_reset(void) {'))
        self.assertIn('dcd_pre_drop("a sudden death\'s hold ended");', self.body('static void sd_hold_end(const char *why) {'))
        start = self.body('static void dcd_pre_start(void) {')
        self.assertIn('if (!dcd_pre_mode() || dcd_pre_on || S.in_fight) return;', start)
        self.assertIn('v = !(e && *e == \'0\');', self.body('static int dcd_pre_mode(void) {'))


if __name__ == '__main__':
    unittest.main()
