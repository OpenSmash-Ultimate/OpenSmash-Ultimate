"""LAN discovery (runtime/net_relay.c "LAN discovery"): a game hosting a private room announces it
on the local network, and a game whose relay is "lan" (SSBU_NET_RELAY=lan, online_server.txt "lan")
finds it and joins it with no address or room code, through tools/dev/netv3test.c (the game's netplay
client with no game around it) as tests/test_net_client_v3.py drives it. The host's relay listens on
127.0.0.1 (SSBU_NET_BIND), so its beacons go to 127.0.0.1 only (unicast: multicast loopback is not to be
counted on everywhere), and the finding game listens there (SSBU_NET_LAN_TEST=1), on a free port
(SSBU_NET_LAN_PORT): nothing reaches the network, and no firewall is asked. Checks the join (the host's
room taken, a datagram through), a host of another build or of another room code not joined (and said),
a host that starts later found, no beacon while direct connections are off, the beacon's bytes, and the
next host taken after the first one leaves. Made-up data only; waits are for conditions.

SSBU_TEST_MULTICAST=1 also runs one over real multicast (239.255.13.13, the PC's own network: the
firewall may ask)."""
from pathlib import Path
import os
import shutil
import socket
import struct
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
from test_net_client_v3 import HAVE_CLANG, SIM, Game, build, free_udp_port  # noqa: E402
from test_ssbu_relay import BUILD, OTHER_BUILD  # noqa: E402
from test_ssbu_relay_v3 import until  # noqa: E402

LAN_OFF, LAN_LOOKING, LAN_NONE, LAN_FOUND, LAN_ANNOUNCING, LAN_QUIET = range(6)   # (net.h's NET_LAN_*)
BEACON_SIZE = 4 + 2 + 8 + 2 + 4 + 1 + 40 + 32 + 32


@unittest.skipUnless(HAVE_CLANG, 'needs the platform compiler (clang-cl on Windows, clang elsewhere)')
class LanDiscovery(unittest.TestCase):
    exe = folder = None

    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.mkdtemp(prefix='lantest ')
        cls.exe = build(cls.folder)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.folder, ignore_errors=True)

    def setUp(self):
        self.games = []
        self.lan_port = free_udp_port()

    def tearDown(self):
        for g in self.games:
            g.close()

    def game(self, relay=None, **kw):
        g = Game(self.exe, relay, **kw)
        self.games.append(g)
        return g

    def host(self, room='lanroom', name='Host', **kw):
        """A game hosting a private room on 127.0.0.1, joined to it, announcing it (unless kw says otherwise)."""
        kw.setdefault('SSBU_NET_LAN_PORT', self.lan_port)
        port = free_udp_port()
        h = self.game(room=room, name=name, SSBU_NET_HOST=1, SSBU_NET_PORT=port, SSBU_NET_BIND='127.0.0.1', **kw)
        h.relay_port = port
        h.join()
        return h

    def finder(self, room='', name='Guest', **kw):
        """A game looking for a room on this network (its relay "lan"), listening on 127.0.0.1."""
        kw.setdefault('SSBU_NET_LAN_PORT', self.lan_port)
        return self.game('lan', room=room, name=name, SSBU_NET_LAN_TEST=1, **kw)

    def lan(self, g, want=-1, wait=0):
        """(state, rooms of its version, others, room) - net_lan, after waiting for that state."""
        st, ours, others, room = g.ask(f'lan {wait} {want}', wait + 10).split()[1:]
        return int(st), int(ours), int(others), room

    def test_found_and_joined(self):
        h = self.host()
        self.assertEqual(self.lan(h)[0], LAN_ANNOUNCING)
        self.assertIn('announced on this network', h.status())
        self.assertTrue(any('announcing room lanroom on this network' in line for line in h.log), ''.join(h.log))
        g = self.finder()
        version, ip, _, _, members = g.join(8)
        self.assertEqual((version, members), (2, 2))
        self.assertEqual(self.lan(g), (LAN_FOUND, 1, 0, 'lanroom'))   # (the host's room, though none was named)
        self.assertIn('room lanroom', g.status())
        self.assertTrue(any("found Host's room lanroom on this network at 127.0.0.1:" in line for line in g.log), ''.join(g.log))
        g.datagram(h.ip, b'hello')   # (the room works: a datagram through the host's relay)
        self.assertTrue(until(lambda: any(d[4] == b'hello' for d in h.received()), 5), ''.join(h.log))

    def test_other_build_not_joined(self):
        self.host(NETV3TEST_BUILD=OTHER_BUILD)
        g = self.finder()
        self.assertEqual(g.ask('join 3', 13), 'notjoined')
        self.assertEqual(self.lan(g, LAN_NONE, 3)[:3], (LAN_NONE, 0, 1))   # (heard, of another version)
        self.assertIn('no room on this network yet', g.status())
        self.assertTrue(any('of another game version or room only' in line for line in g.log), ''.join(g.log))

    def test_room_named(self):
        self.host(room='theirs')
        g = self.finder(room='ours')
        self.assertEqual(g.ask('join 3', 13), 'notjoined')
        self.assertEqual(self.lan(g)[:3], (LAN_NONE, 0, 1))
        self.host(room='ours', name='Second')
        self.assertIsNotNone(g.joined(g.ask('join 8', 18)))
        self.assertEqual(self.lan(g), (LAN_FOUND, 1, 1, 'ours'))

    def test_host_starting_later(self):
        g = self.finder()
        self.assertEqual(g.ask('join 2', 12), 'notjoined')
        self.assertEqual(self.lan(g, LAN_NONE, 3)[:3], (LAN_NONE, 0, 0))
        self.host()
        self.assertIsNotNone(g.joined(g.ask('join 8', 18)))
        self.assertEqual(self.lan(g)[0], LAN_FOUND)

    def test_not_announced_without_direct_connections(self):
        h = self.host(SSBU_NET_DIRECT=0)
        self.assertEqual(self.lan(h)[0], LAN_QUIET)
        self.assertNotIn('announced on this network', h.status())
        h2 = self.host(room='quiet2', SSBU_NET_LAN=0)
        self.assertEqual(self.lan(h2)[0], LAN_QUIET)
        g = self.finder()
        self.assertEqual(g.ask('join 3', 13), 'notjoined')
        self.assertEqual(self.lan(g)[:3], (LAN_NONE, 0, 0))

    def test_beacon_bytes(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.bind(('127.0.0.1', self.lan_port))
            s.settimeout(5)
            h = self.host(name='Beaconer')
            p, at = s.recvfrom(2048)
        self.assertEqual(len(p), BEACON_SIZE)
        self.assertEqual(p[:6], b'SN\x02\x20LB')
        relay_port, sim, players = struct.unpack('>HIB', p[14:21])
        self.assertEqual((relay_port, sim), (h.relay_port, SIM))
        self.assertIn(players, (0, 1))   # (the host's own game: in its room once joined)
        self.assertEqual(p[21:61].rstrip(b'\0').decode(), BUILD)
        self.assertEqual(p[61:93].rstrip(b'\0'), b'lanroom')
        self.assertEqual(p[93:125].rstrip(b'\0'), b'Beaconer')
        self.assertEqual(at[0], '127.0.0.1')

    def test_next_host_after_the_first_leaves(self):
        first = self.host(name='First')
        g = self.finder()
        self.assertIsNotNone(g.joined(g.ask('join 8', 18)))
        self.host(room='second', name='Second')
        first.close()   # (its relay gone: the finder's PINGs go unanswered for 10 s, then it takes the next host heard)
        self.assertTrue(until(lambda: self.lan(g)[3] == 'second' and g.joined(g.ask('join 0', 10)) is not None, 25),
                        ''.join(g.log))

    @unittest.skipUnless(os.environ.get('SSBU_TEST_MULTICAST') == '1', 'real multicast: SSBU_TEST_MULTICAST=1')
    def test_real_multicast(self):
        """Over the PC's own network: the host's relay on every address, the beacon to 239.255.13.13 and the
        broadcast address, the finder in the group."""
        port = free_udp_port()
        h = self.game(room='mcast', name='Host', SSBU_NET_HOST=1, SSBU_NET_PORT=port, SSBU_NET_LAN_PORT=self.lan_port)
        h.join()
        g = self.game('lan', room='', name='Guest', SSBU_NET_LAN_PORT=self.lan_port)
        self.assertIsNotNone(g.joined(g.ask('join 8', 18)), ''.join(g.log))
        self.assertEqual(self.lan(g)[3], 'mcast')


if __name__ == '__main__':
    unittest.main()
