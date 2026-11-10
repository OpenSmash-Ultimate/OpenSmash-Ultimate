"""Cheaper savestates (runtime/hle_rb_state.c "the fight's state map", docs/TECHNICAL.md): what ties them into the
runtime, read from the sources. Proven on the game headless (fight hash logs and run-ahead traces against a run
without run-ahead, SSBU_RB_CHECKLOAD): see the PR and plan/JOURNAL.md; these checks keep the pieces in place.

- the state map: in a fight the alias area's pages not written lately are read-only (cold), a write faults into a
  handler that makes its group writable and hot; saves and loads ask the write watch about the hot pages only;
  steady pages (written at every save) are compared instead of asked; a load only asks (no reset);
- whatever the system writes for the game (file reads, socket receives) opens its pages first;
- run-ahead: hidden frames' submissions draw nothing (their pictures are never shown), and a save waits for the
  guest threads to be on their own fibers (a state saved inside a job could not be loaded)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent.parent


def src(name):
    return (ROOT / 'runtime' / name).read_text(encoding='utf-8')


class StateMap(unittest.TestCase):
    def test_switch_and_fight(self):
        state, core = src('hle_rb_state.c'), src('hle_rb_core.c')
        self.assertIn('getenv("SSBU_RB_SM")', state, 'SSBU_RB_SM=0 turns it off')
        self.assertIn('rb_state_map_fight(1);', core, 'on at a fight\'s start')
        self.assertIn('rb_state_map_fight(0);', core, 'off at its end')
        self.assertIn('if (sm_wanted_fight && !sm_on && sm_want > 0) sm_begin(sm_seen_before);', state,
                      'the fight\'s first save turns it on, after its sync')
        self.assertIn('void rb_state_map_fight(int on) {', state)

    def test_protection(self):
        state = src('hle_rb_state.c')
        self.assertIn('AddVectoredExceptionHandler(1, sm_fault)', state, 'first in the chain, before the crash reporter')
        self.assertIn('r->ExceptionInformation[0] != 1', state, 'writes only')
        self.assertIn('PAGE_READONLY, &old)', state, 'cold pages read-only')
        self.assertIn('sm_protect((void *)(uintptr_t)(ALIAS_BASE + (i << 12)), (j - i) << 12, PAGE_READWRITE);', state,
                      'a fault opens the group')
        # a run crossing the alias area's reservations: one VirtualProtect fails whole, so it goes one at a time then
        # (a page left read-only crashed the Online mode's results after a rollback fight)
        self.assertIn('if (VirtualProtect(at, len, prot, &old)) return;', state)
        self.assertIn('if (m.State == MEM_COMMIT) VirtualProtect(p, (SIZE_T)(stop - p), prot, &old);', state)
        self.assertIn('sm_protect(c->base + ((u64)k << 12), (u64)(e - k) << 12, PAGE_READWRITE);', state, 'at the fight end too')
        # a cold page is present for the savestate, and a page committed since is hot
        self.assertIn('if (sm_on && m.State == MEM_COMMIT && m.Protect == PAGE_READONLY && sm_alias(p)) {', state)
        self.assertIn('{ sm_present(p); settle_page(p, 1, to_shadow); }', state)
        self.assertIn('sm_gone(p);', state)
        # the end: every cold page writable again
        self.assertIn('sm_protect(c->base + ((u64)k << 12), (u64)(e - k) << 12, PAGE_READWRITE);', state)

    def test_system_writers(self):
        fs, net, reg, core = src('hle_fs.c'), src('hle_net.c'), src('hle_fighter_registry.c'), src('hle_rb_core.c')
        self.assertIn('rb_guest_writing(buf, size);', fs, 'file reads open their pages')
        self.assertIn('rb_guest_writing(P(void, buf), len);', net, 'socket receives too')
        self.assertIn('if (rb_page_cold(p)) return 1;', reg, 'a cold page is writable for the fighter registry')
        self.assertIn('void rb_guest_writing(void *p, u64 n) { (void)p; (void)n; }', core, 'stand-ins where rollback is not built')
        self.assertIn('int rb_page_cold(const void *p) { (void)p; return 0; }', core)

    def test_asking(self):
        state, watch = src('hle_rb_state.c'), src('gpu_watch.c')
        self.assertIn('sm_ask(to_shadow, &clock, &calls);', state, 'the hot pages only')
        self.assertIn('u64 bits = sm_hot[(b >> 6) + w] & ~sm_steady[(b >> 6) + w];', state, 'steady pages never asked about')
        self.assertIn('jobs[njobs - 1].same = &sm_same[b + k];', state, 'steady pages compared, then copied')
        self.assertIn('if (q->same) { *q->same = !memcmp(q->d2, q->s2, 0x1000); if (*q->same || q->cmp_only) continue; }', state)
        self.assertIn('void watch_rb_dirty(u64 lo, u64 hi, u64 *bits) {', watch, 'a load asks without resetting')
        self.assertIn('kernel_write_watch(WATCH_RENDERER, 0, (void *)at, hi - at, pages, &count, &granularity);', watch)
        self.assertIn('DWORD wf = to_shadow || !sm_on ? WRITE_WATCH_FLAG_RESET : 0;', state)


class RunAhead(unittest.TestCase):
    def test_hidden_frames_draw_nothing(self):
        gpu, queue, core = src('gpu.c'), src('gpu_queue.c'), src('hle_rb_core.c')
        self.assertIn('if (!gpu_job_hidden) gfx_draw(e->regs, e->cb, e->indexed);', gpu)
        self.assertIn('if (!gpu_job_hidden) gfx_clear(e->regs, arg);', gpu)
        self.assertIn('j->hide = j->fast && gpu_hide_fast;', queue, 'decided when the frame submits')
        self.assertIn('gpu_job_hidden = j->hide;', queue)
        self.assertIn('gpu_hide_fast = hide && !draw_hidden;', core, 'SSBU_RA_DRAW_HIDDEN=1 draws them')
        self.assertIn('gpu_hide_fast = 0;', core, 'off when the fight ends')

    def test_save_on_own_fibers(self):
        state, core = src('hle_rb_state.c'), src('hle_rb_core.c')
        self.assertIn('if ((rb_save_home_only || save_home) && away_from_home(c->thread)) {', state)
        self.assertIn('getenv("SSBU_RB_SAVE_HOME")', state, 'net rollback saves wait for the jobs too (off: =0)')
        self.assertIn('getenv("SSBU_RB_SAVE_RETRY_MS")', state)
        self.assertIn('rb_save_home_only = 1;\n        int r = rb_save(c, frame);\n        rb_save_home_only = 0;', core,
                      'run-ahead\'s saves only')


if __name__ == '__main__':
    unittest.main()
