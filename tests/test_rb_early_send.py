"""SSBU_RB_EARLY_SEND (runtime/hle_rb_core.c net_early_send): this game's pad goes out at its frame's first read, not a
frame later at the next safe point. Read from the sources: the switch, where the pad is fixed and sent, and that the
value queued is the one the safe point would have queued (both games hold the same pads: no desync can come of it)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent.parent


def core():
    return (ROOT / 'runtime' / 'hle_rb_core.c').read_text(encoding='utf-8')


class EarlySend(unittest.TestCase):
    def test_switch(self):
        c = core()
        self.assertIn('getenv("SSBU_RB_EARLY_SEND")', c)
        self.assertIn('early_on = e && *e ? atoi(e) : 1;', c, 'on by default; 0 = as 0.5.3')

    def test_same_pad_as_the_safe_point(self):
        c = core()
        f = c[c.index('static void net_early_send(u32 key) {'):]
        f = f[:f.index('\n}\n')]
        # the safe point K + 1 queues rbn_local(K + 1, ...) with the pad read in frame K: the early send queues the same
        self.assertIn('rbn_local(key + 1, net_live == 1 ? net_live_pad : net_pad(net_local, key + 1));', f)
        self.assertIn('rbn_local(key, net_live == 1 ? net_live_pad : net_pad(net_local, key));', c)
        # only a real pass of the frame after the safe point, on the safe point's thread, once a frame
        for cond in ('GetCurrentThreadId() != S.thread', 'key != pr_sp_lf', 'key == early_key', 'rb_catchup', 'net_done'):
            self.assertIn(cond, f)
        self.assertIn('net_send(key);', f)

    def test_hooked_after_the_live_pad(self):
        c = core()
        f = c[c.index('static int net_pad_for(u32 id, u32 key, u64 *buttons'):]
        live = f.index('net_live_pad = rbn_rest(l);')
        hook = f.index('net_early_send(key);')
        self.assertLess(live, hook, 'the live pad of this read is taken before it is sent')
        self.assertIn('rb_fast <= 0) net_early_send(key);', f[:hook + 40], 'never from a frame run again')

    def test_safe_point_sends_only_what_was_not_sent(self):
        c = core()
        self.assertIn('if (early_send_on() == 1 && early_key && early_key + 1 == key) early_skipped++;', c)
        self.assertIn('early_key = 0; early_sends = early_skipped = 0;', c, 'reset as a fight starts')


if __name__ == '__main__':
    unittest.main()
