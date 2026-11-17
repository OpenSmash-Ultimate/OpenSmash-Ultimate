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
        self.assertIn('if ((sm_on || lazy_pages) && m.State == MEM_COMMIT && m.Protect == PAGE_READONLY && sm_alias(p)) {', state)
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


class Cooling(unittest.TestCase):
    """A hot page left unwritten for SSBU_RB_SM_COOL saves (30) goes cold again: the hot set is what a fight writes
    lately, not everything it ever wrote (a mario-donkey fight: ~15,500 hot pages a save, ~1,500 written)."""
    def test_cooling(self):
        state = src('hle_rb_state.c')
        self.assertIn('getenv("SSBU_RB_SM_COOL")', state)
        self.assertIn('else sm_streaks(clock); }', state, "after the copies, with the save's clock")
        # never the GPU's pages (its thread is not held) nor the left-out ones (the shadow does not follow them)
        self.assertIn('if (cool_on && !((gpu_word(wi) | skip_word(wi)) & m) && ++sm_idle[a] >= SM_COOL && is_present(c, w * 64 + z)) cool |= m;', state)
        # a write by a thread not held between the questions and the protection: asked again, opened again
        self.assertIn('if (chi) sm_cool_check(clo, chi, save_clock);', state)
        self.assertIn('if (sm_bit(sm_cold, k)) { sm_open_locked(k, k + 1); sm_cool_back++; }', state)
        self.assertIn('getenv("SSBU_RB_SM_COOL_CHECK")', state, 'the test switch comparing cooled pages with the shadow')


class Owners(unittest.TestCase):
    """The state map by owner (runtime/hle_rb_map.c): jemalloc's entry points hooked, the census, the sites left out
    and their live blocks kept with each state."""
    def test_hooks_and_list(self):
        m, fs = src('hle_rb_map.c'), src('hle_fs.c')
        for a in ('0x710392e1f0', '0x710392e290', '0x710392e330', '0x710392eb40'):
            self.assertIn(a, m, 'a jemalloc entry point (13.0.5)')
        self.assertIn('rb_map_install();', fs, 'hooked once the RomFS is mounted')
        self.assertIn('getenv("SSBU_RB_MAP")', m, 'SSBU_RB_MAP=0: no hooks, every page in')
        self.assertIn('getenv("SSBU_RB_MAP_SKIP")', m)
        self.assertIn('getenv("SSBU_RB_MAP_CENSUS")', m)
        self.assertIn('{0x7103816ef4ull, 0},', m, "the graphics module's 16 MB buffers")
        self.assertIn('{0x710355a3b8ull, 0x71000a3fd8ull},', m, 'the 8 MB block')
        # only whole pages of a block are left out
        self.assertIn('u64 lo = (b->p + 0xFFF) & ~0xFFFull, hi = (b->p + b->size) & ~0xFFFull;', m)

    def test_with_the_state(self):
        state = src('hle_rb_state.c')
        self.assertIn('s->map = rb_map_save(ring_count ? slot_at(ring_count - 1)->map : NULL);', state, 'each state keeps the left-out blocks')
        self.assertIn('rb_map_load(s->map);', state, 'a load puts them back')
        self.assertIn('rb_map_free(s->map);', state)
        self.assertIn('if (rb_map_active()) map_pages = rb_map_skip_pages(sm_skip);', state, 'into the left-out set')
        self.assertIn('map_gen_seen == rb_map_gen', state, 'built again when they change')
        self.assertIn('if (out_) { left_out_diff++; continue; }', state, 'the replay check counts them apart')

    def test_young_blocks_stay_in(self):
        m = src('hle_rb_map.c')
        # a block is left out only once every kept state was saved after it was made
        self.assertIn('if (!b->p || b->born >= map_oldest) continue;   /* (young) */', m)
        self.assertIn('old = oldest ? ((const MapSave *)oldest)->stamp : stamp;', m)
        # a load builds the set again only when it differs
        self.assertIn('if (after != before) InterlockedIncrement(&rb_map_gen);', m)
        # the caller's frame record is read only where memory is committed
        self.assertIn('&& readable8(fp + 8))', m)


class RunAhead(unittest.TestCase):
    def test_hidden_frames_draw_nothing(self):
        gpu, queue, core = src('gpu.c'), src('gpu_queue.c'), src('hle_rb_core.c')
        self.assertIn('if (!gpu_job_hidden) gfx_draw(e->regs, e->cb, e->indexed);', gpu)
        self.assertIn('if (!gpu_job_hidden) gfx_clear(e->regs, arg);', gpu)
        self.assertIn('j->hide = gpu_hide_fast == 2 ? rb_fast > 1 : j->fast && gpu_hide_fast;', queue, 'decided when the frame submits')
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
