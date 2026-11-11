"""Rollback netplay's netcode and link (runtime/hle_rb_net.c, runtime/hle_rb_link.c; SSBU_RB, docs/TECHNICAL.md)
through tools/dev/rbnettest.c and tools/dev/rblinktest.c, with no game. rbnettest: two to four made-up
peers run a toy fight over a link that drops, delays and reorders messages, each rolling back on its
wrong guesses; it passes when every peer's confirmed states agree frame by frame and equal a run with
every pad known - with input delays 0, 2 and 4, a peer that runs slower for a while, three and four
players through the host, a player leaving a 3- or 4-player match - and it must fail with rollbacks
turned off (the test can fail). rblinktest (Windows: the link is winsock's): the link on this PC's
loopback, listening on 127.0.0.1 only (SSBU_RB_BIND) so that no firewall asks - a waiting game found,
a game following the other to another port, strangers dropped, the host with several players.
Built with clang-cl on Windows (as tests/test_kernel_waits.py builds waittest.c), rbnettest with clang
elsewhere; skipped without one. Both from Piaka11's WindowSmash PR #103 (GPL-3.0).
R7, the link through the netplay relay (runtime/hle_rb_relay.c, SSBU_RB_NET=relay) through
tools/dev/rbrelaytest.c (Windows): two of them, each the game's netplay client (runtime/net_relay.c)
with the rollback link on it, trade real HELLO and PADS messages on loopback - in a private room
(one hosting its relay), in the lobby (tools/ssbu_relay.py) in one Local Wireless room, straight once
punched and through the lobby with direct packets blocked (SSBU_NET_TEST_BLOCK), and in the lobby
with no room shared (version 3's RB messages); players numbered by the room's order, by hand, or by
the host."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
import cc  # noqa: E402

if os.name == 'nt':
    HAVE_CLANG = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
else:
    HAVE_CLANG = shutil.which('clang') is not None


def build(folder, name, sources, libs=()):
    srcs = [str(ROOT / s) for s in sources]
    if os.name == 'nt':
        exe = os.path.join(folder, name + '.exe')
        cmd = [cc.CLANG, '/nologo', '/O2', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, *srcs, '/Fe' + exe, '/Fo' + folder + os.sep,
               '/link', *cc.LINK_OPTIONS, *libs]
    else:
        exe = os.path.join(folder, name)
        cmd = ['clang', '-O2', '-w', *srcs, '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(f'{name} did not build:\n' + r.stdout + r.stderr)
    return exe


@unittest.skipUnless(HAVE_CLANG, 'needs clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools), or clang')
class NetcodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='rbnettest ')
        cls.exe = build(cls.dir, 'rbnettest', ['tools/dev/rbnettest.c', 'runtime/hle_rb_net.c'])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def run_test(self, *args):
        return subprocess.run([self.exe, *map(str, args)], capture_output=True, text=True, timeout=300)

    def passes(self, *args):
        r = self.run_test(*args)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'frames compared with the run with all pads known: PASS\n')
        return r

    def test_two_players_each_delay(self):
        for delay in (0, 2, 4):
            with self.subTest(delay=delay):
                self.passes(delay)

    def test_a_slow_peer(self):
        """The frame advantage holds the other back while one runs slower for a while."""
        self.assertIn('(B slow)', self.passes(2, 'slow').stdout)
        self.passes(2, 'slow', 3)

    def test_three_and_four_players(self):
        for players in (3, 4):
            for delay in (0, 2, 4):
                with self.subTest(players=players, delay=delay):
                    self.assertIn(f'{players} players', self.passes(delay, '-', players).stdout)

    def test_a_player_leaves(self):
        """The host cuts the last player off, and the players left stay the same."""
        for players in (3, 4):
            for delay in (0, 2, 4):
                with self.subTest(players=players, delay=delay):
                    self.assertIn('the last one leaves', self.passes(delay, 'leave', players).stdout)

    def test_without_rollbacks_it_fails(self):
        """The test can fail: with the wrong guesses kept, the peers go apart."""
        for args in ((2, 'norollback'), (2, 'norollback', 3)):
            with self.subTest(args=args):
                r = self.run_test(*args)
                self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)


@unittest.skipUnless(os.name == 'nt' and HAVE_CLANG, 'the link is winsock\'s: Windows, with clang-cl')
class LinkTests(unittest.TestCase):
    def test_loopback(self):
        with tempfile.TemporaryDirectory(prefix='rblinktest ') as folder:
            exe = build(folder, 'rblinktest', ['tools/dev/rblinktest.c', 'runtime/hle_rb_link.c'],
                        ['ws2_32.lib', 'ole32.lib', 'oleaut32.lib'])
            env = {**os.environ, 'SSBU_RB_BIND': '127.0.0.1'}
            env.pop('SSBU_RB_UPNP', None)   # (the router left alone)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=120, env=env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('link test: PASS', r.stdout)
            self.assertIn('UPnP off', r.stderr)


@unittest.skipUnless(os.name == 'nt' and HAVE_CLANG, 'the rollback build is Windows\'s: Windows, with clang-cl')
class RelayLinkTests(unittest.TestCase):
    """Two games' rollback links through the netplay relay (rbrelaytest: net_relay.c + hle_rb_relay.c)."""
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='rbrelaytest ')
        srcs = [str(ROOT / s) for s in ('tools/dev/rbrelaytest.c', 'runtime/hle_rb_relay.c', 'runtime/hle_rb_link.c', 'runtime/hle_rb_net.c')]
        cls.exe = os.path.join(cls.dir, 'rbrelaytest.exe')
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, *srcs, '/Fe' + cls.exe, '/Fo' + cls.dir + os.sep, '/link', *cc.LINK_OPTIONS]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('rbrelaytest did not build:\n' + r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def setUp(self):
        self.games = []

    def tearDown(self):
        for g in self.games:
            g.close()

    def game(self, relay, **kw):
        from test_net_client_v3 import Game
        g = Game(self.exe, relay, **kw)
        self.games.append(g)
        return g

    def both(self, a, b, line, wait):
        """The same command to both at once (each waits for the other); their answers."""
        a.send(line)
        b.send(line)
        return a.answer(wait + 10), b.answer(wait + 10)

    def logs(self, *games):
        return ''.join(f'--- {g.name}:\n' + ''.join(g.log) for g in games)

    def stats(self, g):
        keys = ('out_data', 'out_lobby', 'in_direct', 'in_relayed', 'in_lobby', 'sent', 'received', 'dropped')
        return dict(zip(keys, map(int, g.ask('stats').split()[1:])))

    def play(self, a, b, who_a, who_b, players=2, numbers=None):
        """Open both links, number them (the room's order), shake hands: the hello answers."""
        for g, who in ((a, who_a), (b, who_b)):
            self.assertEqual(g.ask(f'open {who} {players}'), 'open 1', self.logs(g))
        if numbers is not None:
            got = self.both(a, b, 'number 10', 10)
            self.assertEqual(got, tuple(f'number {n}' for n in numbers), self.logs(a, b))
        got = self.both(a, b, 'hello 20', 20)
        self.assertTrue(all(x.startswith('hello OK') for x in got), (got, self.logs(a, b)))
        return got

    def pads(self, a, b, frames=3000):
        """PADS of frames 1..frames both ways, each pad checked, every one acknowledged."""
        got = self.both(a, b, f'pads {frames} 30', 30)
        for x in got:
            self.assertTrue(x.startswith(f'pads OK {frames} 0 '), (got, self.logs(a, b)))

    def lobby_room(self, server, host, guest):
        """host makes a Local Wireless room, guest joins it (as test_net_client_v3's direct test)."""
        import secrets
        import ssbu_relay as R
        from test_ssbu_relay import info_of
        from test_ssbu_relay_v3 import until
        session = secrets.token_bytes(16)
        host.ldn(R.BROADCASTS[0], R.LM_ADVERTISE, info_of(session, host.ip))
        self.assertTrue(until(lambda: session in server.lobby.listings))
        guest.ldn(host.ip, R.LM_JOIN, session + b'Guest'.ljust(32, b'\0') + b'\0\0')
        host.ldn(R.BROADCASTS[0], R.LM_ADVERTISE, info_of(session, host.ip, [guest.ip]))
        host.ldn(guest.ip, R.LM_ACCEPT, info_of(session, host.ip, [guest.ip]))
        self.assertTrue(until(lambda: guest.ip in server.lobby.listings[session].nodes))

    def lobby_pair(self, **more):
        from test_ssbu_relay import Server
        server = Server()
        self.addCleanup(server.stop)
        relay = f'127.0.0.1:{server.port}'
        host = self.game(relay, protocol=3, room='rbroom', name='Host', **more)
        host.join()
        guest = self.game(relay, protocol=3, room='rbroom', name='Guest', **more)
        guest.join()
        self.assertLess(host.ip, guest.ip)   # (the lobby hands addresses out in turn)
        return server, host, guest

    def test_private_room(self):
        """A private room's relay in the host's game: numbered by the room's order (the host's game first),
        every message through the relay (a private room has no direct paths)."""
        from test_net_client_v3 import free_udp_port
        port = free_udp_port()
        host = self.game(None, name='Host', SSBU_NET_HOST=1, SSBU_NET_PORT=port, SSBU_NET_BIND='127.0.0.1')
        host.join()
        guest = self.game(f'127.0.0.1:{port}', name='Guest')
        self.assertEqual(guest.join()[4], 2)   # (two members)
        self.assertEqual(self.play(host, guest, 'auto', 'auto', numbers=(0, 1)), ('hello OK 0', 'hello OK 1'))
        self.pads(host, guest)
        for g in (host, guest):
            st = self.stats(g)
            self.assertGreater(st['in_relayed'], 40, st)
            self.assertEqual((st['in_direct'], st['in_lobby'], st['out_lobby']), (0, 0, 0), st)
            self.assertIn("[rb] link: the room's players by address: 10.13.0.2, 10.13.0.3", ''.join(g.log))

    def test_online_match_numbers(self):
        """SSBU_RB_NET=online: the link opens with no number; an Online match's set-up numbers it (player 1 the
        gathering's owner) with the other game's address, then the handshake and the pads go as in a room, and
        the match's end forgets the number."""
        from test_net_client_v3 import free_udp_port
        port = free_udp_port()
        host = self.game(None, name='Host', SSBU_NET_HOST=1, SSBU_NET_PORT=port, SSBU_NET_BIND='127.0.0.1')
        host.join()
        guest = self.game(f'127.0.0.1:{port}', name='Guest')
        guest.join()
        self.assertEqual(host.ask(f'online 0 {guest.ip}'), 'online 1 0', self.logs(host))
        self.assertEqual(guest.ask(f'online 1 {host.ip}'), 'online 1 1', self.logs(guest))
        got = self.both(host, guest, 'hello 20', 20)
        self.assertEqual(got, ('hello OK 0', 'hello OK 1'), self.logs(host, guest))
        self.pads(host, guest)
        self.assertIn("[rb] link: an Online match's players: this game, ", ''.join(host.log))
        self.assertEqual(host.ask('offline'), 'offline -1')

    def test_recent_peers(self):
        """The players the game's sockets sent to lately (Pia's in an Online match): most recent first, the lobby's
        own addresses (10.13.0.1, Pia's second NAT check server 10.13.255.254, the broadcast) and this game left out."""
        from test_net_client_v3 import free_udp_port
        port = free_udp_port()
        g = self.game(None, name='Host', SSBU_NET_HOST=1, SSBU_NET_PORT=port, SSBU_NET_BIND='127.0.0.1')
        g.join()
        for ip in (0x0A0D0001, 0x0A0DFFFE, 0x0A0DFFFF, 0x0A0D0005, 0x0A0D0007, g.ip, 0x0A0D0005, 0xC0A80001):
            self.assertEqual(g.ask(f'note {ip}'), 'ok')
        self.assertEqual(g.ask('recent 30000'), f'recent 2 {0x0A0D0005} {0x0A0D0007}')

    def test_lobby_room_direct(self):
        """In one Local Wireless room of the lobby: numbered by address; the messages through the lobby
        introduce the two games, which then send straight to each other."""
        server, host, guest = self.lobby_pair()
        self.lobby_room(server, host, guest)
        self.play(host, guest, 'auto', 'auto', numbers=(0, 1))
        for g in (host, guest):
            self.assertEqual(g.peers(8, 1)[0], 1, self.logs(host, guest))   # (punched through)
        self.pads(host, guest)
        for g in (host, guest):
            st = self.stats(g)
            self.assertGreater(st['in_direct'], 40, st)
            self.assertLess(st['in_lobby'] + st['out_lobby'], 20, st)   # (a room's player: no RB, but the hellos before the host is known)

    def test_lobby_room_blocked(self):
        """The same with every direct packet dropped (as a router that lets nothing through): all of it
        through the lobby, delivered all the same."""
        server, host, guest = self.lobby_pair(SSBU_NET_TEST_BLOCK=0, SSBU_NET_TEST_NAT=1)
        self.lobby_room(server, host, guest)
        self.play(host, guest, 'auto', 'auto', numbers=(0, 1))
        self.pads(host, guest)
        for g in (host, guest):
            st = self.stats(g)
            self.assertEqual(g.peers()[0], 0)
            self.assertGreater(st['in_relayed'], 40, st)
            self.assertEqual(st['in_direct'], 0, st)
            self.assertLess(st['in_lobby'] + st['out_lobby'], 20, st)

    @unittest.skipUnless(os.environ.get('SSBU_EOS_TEST_DIR'), 'EOS: SSBU_EOS_TEST_DIR names a folder with EOSSDK-Win64-Shipping.dll and eos.txt (never in the repository)')
    def test_lobby_room_eos(self):
        """With every direct packet dropped and EOS beside the program (runtime/net_eos.c): the players tell each other
        their EOS ids through the lobby, and the room's traffic goes through EOS instead of the lobby's relay. The
        guest makes a new Device ID (SSBU_EOS_NEWID): two EOS players on one PC."""
        from test_ssbu_relay import Server
        for f in ('EOSSDK-Win64-Shipping.dll', 'eos.txt'):
            shutil.copy(os.path.join(os.environ['SSBU_EOS_TEST_DIR'], f), self.dir)
        server = Server()
        self.addCleanup(server.stop)
        relay = f'127.0.0.1:{server.port}'
        host = self.game(relay, protocol=3, room='rbroom', name='Host', SSBU_NET_TEST_BLOCK=0, SSBU_NET_TEST_NAT=1)
        host.join()
        guest = self.game(relay, protocol=3, room='rbroom', name='Guest', SSBU_NET_TEST_BLOCK=0, SSBU_NET_TEST_NAT=1, SSBU_EOS_NEWID=1)
        guest.join()
        self.lobby_room(server, host, guest)
        self.play(host, guest, 'auto', 'auto', numbers=(0, 1))
        import time
        deadline = time.time() + 30
        while time.time() < deadline and not all('through EOS' in ''.join(g.log) for g in (host, guest)):
            time.sleep(0.5)
        self.pads(host, guest)
        for g in (host, guest):
            st = self.stats(g)
            self.assertIn('through EOS', ''.join(g.log), self.logs(host, guest))
            self.assertGreater(st['in_direct'], 40, st)    # (EOS's packets count as direct ones: the lobby's relay and direct packets are out)
            self.assertLess(st['in_relayed'], st['in_direct'], st)

    def test_lobby_code_no_room(self):
        """Players of one lobby code (version 3) in no room together: the host by hand, the joiner numbered
        by the host; the messages go as the lobby's RB."""
        server, host, guest = self.lobby_pair()
        self.assertEqual(self.play(host, guest, '0', '-1'), ('hello OK 0', 'hello OK 1'))
        self.pads(host, guest)
        for g in (host, guest):
            st = self.stats(g)
            self.assertGreater(st['in_lobby'], 40, st)
            self.assertGreater(st['out_lobby'], 40, st)
            self.assertEqual((st['in_direct'], st['in_relayed']), (0, 0), st)
        self.assertGreater(server.lobby.counts['rollback relayed'], 80)


if __name__ == '__main__':
    unittest.main()
