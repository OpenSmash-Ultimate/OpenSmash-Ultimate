"""Rollback netplay in the game's own Online mode (SSBU_RB_NET=online: runtime/hle_rb_core.c "the game's Online
mode", docs/MULTIPLAYER.md "Rollback in Online mode"): what ties it into the runtime, read from the sources.
Proven on the game headless (two games on one PC, Quickplay through the Online menu against a local lobby with the
NEX game server: both reach the fight, the final fight hashes of both games identical frame by frame, the results
and GSP reported, both back in Quickplay): see the PR and plan/JOURNAL.md. The link's part (an Online match's
numbers, the players the game's sockets talk to) is run for real by tests/test_rb_net.py (tools/dev/rbrelaytest.c).

- an Online match (melee mode 14, set up from the match set-up) is played with rollback when both games agree in
  time: the network player becomes a local one on the slot of its number; the game's own set-up comes back as the
  fight ends (its results, the GSP and the next search run as without rollback); otherwise the game's own netcode;
- the guest threads that wait in the sockets' poll park there during a savestate's hold (NEX's thread: a hold
  that waited for its poll took ~95 ms a save);
- a load puts back what the game's sockets took since its save (datagrams, NEX's stream bytes) and leaves out what
  the game sends again on a stream (the NEX connection and Pia's session survive rollbacks);
- a lost player or a desync writes the log's last [rb] lines to reports\\rollback_<time>.txt."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent.parent


def src(name):
    return (ROOT / 'runtime' / name).read_text(encoding='utf-8')


class OnlineMatch(unittest.TestCase):
    def test_switch(self):
        core = src('hle_rb_core.c')
        self.assertIn('net_online = e && !strncmp(e, "online", 6)', core)
        self.assertIn('ok = rbl_open_relay(RBL_ONLINE, net_n);', core, 'the link opens with no number')
        self.assertIn('if (net_online) net_armed = net_armed && online_setup(st);', core, 'only an agreed Online match is armed')

    def test_no_controls_left_from_the_last_fight(self):
        core = src('hle_rb_core.c')
        start = core.index('rbn_reset(net_mode == 2 ? net_n : 2')
        self.assertIn('net_live_pad = (RbnPad){0};', core[start:core.index('rbn_local(key, net_live == 1', start)],
                      'a fight\'s first local pad starts empty, not as the last fight ended')

    def test_setup(self):
        core = src('hle_rb_core.c')
        self.assertIn('#define ONLINE_MELEE 14', core)
        self.assertIn('#define ARENA_MELEE 20', core)
        self.assertIn('if (mode != ONLINE_MELEE && mode != ARENA_MELEE)', core,
                      'other matches (not Quickplay or a Battle Arena) are played alone or as a room match')
        self.assertIn('peers = net_recent_peers(seen, 8, 30000);', core, 'the other game: the one Pia talks to')
        self.assertIn('if (peers != 1)', core, 'more than 2 players: the game\'s own netcode')
        self.assertIn('int own = ld32(st + 0x78) == 0 && !ld8(st + 0x451) ? 0 : 1;', core, 'player 1 is the network player or this game')
        self.assertIn('while (!hs_agreed && now_ms() - t0 < ONLINE_WAIT_MS)', core, 'a bounded wait for the other side')
        self.assertIn('online_forget("no rollback on the other side");', core)
        # each panel: this game's own on its number's slot, the network player a local one
        self.assertIn('slot = type == 0 && !ld8(st + 0x451) ? (u32)net_local : (u32)idx;', core)
        self.assertIn('st32(st + 0x210, slot); st8(st + 0x451, 0);', core)
        self.assertIn('if (ch_have_mine && net_online) {', core, 'the game server\'s fighters and colours are kept')

    def test_game_setup_back(self):
        core = src('hle_rb_core.c')
        self.assertIn('f_71017880c0_orig(c);', core, 'the game\'s own record built again from its panel')
        self.assertIn('memcpy(online_rec[idx], P(u8, rec), ONLINE_REC); online_rec_have[idx] = 1;', core)
        self.assertIn('online_restore();   /* (an Online match: the game\'s own set-up back for its results) */', core)
        self.assertIn('memcpy(P(u8, MELEE_INFO), online_hdr, ONLINE_HDR);', core)
        # a Local Wireless room's match: its results on rollback's set-up, every player local (#180)
        self.assertIn('online_match = 1; online_lw = mode != ONLINE_MELEE && mode != ARENA_MELEE;', core,
                      'Quickplay and Battle Arena matches get the game set-up back; Local Wireless room matches do not')
        self.assertIn('if (online_lw) {', core)
        self.assertIn('else if (online_lw) st8(MELEE_INFO, LOCAL_MELEE);', core,
                      'a Local Wireless room fight is a local melee: the room mode split on C-stick inputs (#181)')
        self.assertIn('getenv("SSBU_RB_LW_RESTORE")', core)
        self.assertIn('if (net_online) { online_forget("the results are over"); net_armed = 0; }', core,
                      'the next match may have other players')

    def test_poll_parks(self):
        net, kernel = src('hle_net.c'), src('kernel.c')
        self.assertIn('if (c && kernel_rb_park_guest(NULL)) {', net)
        self.assertIn('kernel_rb_park_guest(c->thread);', net)
        self.assertIn('if (!kernel_rb_wake_hook) kernel_rb_wake_hook = net_wake;', net, 'a hold wakes the poll')
        self.assertIn('if (kernel_rb_wake_hook) kernel_rb_wake_hook();', kernel)
        self.assertIn('int kernel_rb_park_guest(void *kthread) {', kernel)

    def test_sockets_through_a_load(self):
        net, online, state = src('hle_net.c'), src('hle_online.c'), src('hle_rb_state.c')
        self.assertIn('s->net = net_rb_save();', state)
        self.assertIn('net_rb_load(s->net);', state)
        self.assertIn('free(s->net);', state)
        self.assertIn('if (rb_keep) keep_taken((int)(s - socks), s->port, d);', net, 'a datagram taken is kept')
        self.assertIn('d->next = s->head; s->head = d;', net, 'and put back in front at a load')
        self.assertIn('if (rb_keep) { hist_put(&st->hist, st->taken, st->buf, n); st->taken += n; }', online)
        self.assertIn('for (u64 k = 0; k < back; k++) st->buf[k] = st->hist[(o->s[i].taken + k) % HIST];', online)
        self.assertIn('if (st->shist && (u8)*p == st->shist[st->resend % HIST]) { p++; left--; st->resend++;', online,
                      'what is sent again is left out while it matches')

    def test_sockets_closed_or_made_since_a_save(self):
        net = src('hle_net.c')
        self.assertIn('if (s && rb_keep && s->type == SOCK_DGRAM_) { s->open = 2;', net, 'a socket closed is kept for a load')
        self.assertIn('if (socks[i].open == 2 && socks[i].closed_n > r->closed) {', net, 'and open again after one')
        self.assertIn('if (socks[i].open && socks[i].opened_n > r->opened && socks[i].type == SOCK_DGRAM_) {', net,
                      'one made since the save is gone again')
        self.assertIn('socks[fd - FD_BASE].open != 1) return NULL;', net, 'a kept one is closed for the game')

    def test_no_dialog_in_test_runs(self):
        core = src('hle_rb_core.c')
        self.assertIn('if ((off && off[0] == \'0\') || getenv("SSBU_SEQ") || (headless && headless[0] == \'1\')) return 0;', core)

    def test_online_delay(self):
        core = src('hle_rb_core.c')
        self.assertIn('net_delay = net_online ? rbn_online_delay_for_rtt(worst) : rbn_delay_for_rtt(worst);', core)

    def test_report(self):
        core = src('hle_rb_core.c')
        self.assertIn('#define fprintf rb_fprintf', core, 'every [rb] line is kept')
        self.assertIn('reports\\\\rollback_%04d%02d%02d_%02d%02d%02d.txt', core)
        self.assertIn('rb_keep_dump(why_all);', core, 'a lost player')
        self.assertEqual(core.count('rb_keep_dump("a desync");'), 2, 'a desync')
        self.assertNotIn('click Collect logs', core, 'no such button in this port')

    def test_net_sim_on_the_relay_link(self):
        relay = src('hle_rb_relay.c')
        self.assertIn('getenv("SSBU_NET_SIM")', relay)
        self.assertIn('if (sim_loss > 0 && sim_rand() % 10000 < (unsigned)(sim_loss * 100))', relay)


class NexServer(unittest.TestCase):
    def test_close_reason_logged(self):
        prudp = (ROOT / 'tools' / 'nex' / 'prudp.py').read_text(encoding='utf-8')
        server = (ROOT / 'tools' / 'nex' / 'server.py').read_text(encoding='utf-8')
        self.assertIn('c.why = why', prudp)
        self.assertIn('self.info(f"{self.name(conn)} left" + (f" ({why})" if why else ""))', server)


if __name__ == '__main__':
    unittest.main()
