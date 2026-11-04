"""Direct paths through routers that change ports (runtime/net_relay.c, "direct connections"; the lobby's
side in tools/ssbu_relay.py): two games' netplay clients (tools/dev/netv3test.c) in one room of the real
lobby on 127.0.0.1, each behind a router simulated in the client (SSBU_NET_TEST_NAT):
  1 cone, port-restricted   2 symmetric, random ports   3 symmetric, ports in turn   4 cone, address-restricted
The case players reported (a router that gives the lobby one port and the other player another, so
"no direct path ... through the lobby"): with the address learnt from the hellos (2 with 4), the ports
guessed from the lobby's NAT port (3 with 1) and a port mapping asked of the router (2 with 1, NAT-PMP
answered by a fake gateway here) the direct path now forms; without them (SSBU_NET_TEST_LEARN=0, a lobby
without a NAT port, no gateway) it does not, and two routers that change ports at random fall back to
the lobby cleanly, saying why. Made-up data only, loopback only; waits are for conditions."""
import os
import re
import secrets
import socket
import struct
import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_net_client_v3 import BCAST, ClientTest, Game, HAVE_CLANG, R, Server, info_of, until  # noqa: E402


class FakeGateway:
    """A router's NAT-PMP server (RFC 6886) on 127.0.0.1: maps any UDP port to the same outside port; a
    PCP request (version 2) gets NAT-PMP's "unsupported version", as a NAT-PMP-only router answers."""
    def __init__(self):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(('127.0.0.1', 0))
        self.s.settimeout(0.2)
        self.port = self.s.getsockname()[1]
        self.asked, self.pcp_asked, self.on = [], 0, True
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while self.on:
            try:
                q, at = self.s.recvfrom(1100)
            except OSError:
                continue
            if len(q) >= 2 and q[0] == 2:
                self.pcp_asked += 1
                self.s.sendto(struct.pack('>BBHI', 0, 128 + (q[1] & 127), 1, 1), at)
            elif len(q) >= 12 and q[0] == 0 and q[1] == 1:
                inner, want, life = struct.unpack_from('>HHI', q, 4)
                self.asked.append((inner, want, life))
                self.s.sendto(struct.pack('>BBHIHHI', 0, 129, 0, 1, inner, inner if life else 0, life), at)

    def close(self):
        self.on = False
        self.s.close()


@unittest.skipUnless(HAVE_CLANG, "needs clang on x86-64 Linux or an Apple silicon Mac, or clang-cl on Windows (deps\\llvm)")
class Routers(ClientTest):
    def setUp(self):
        super().setUp()
        self.servers = []

    def tearDown(self):
        super().tearDown()
        for s in self.servers:
            s.stop()

    def room(self, host_nat, guest_nat, nat_port=0, host_env=None, guest_env=None):
        """A lobby (with a NAT port unless nat_port is None), two games behind those routers in one room,
        talking (so introduced): (host, guest)."""
        server = Server(nat_port=nat_port)
        self.servers.append(server)
        relay = f'127.0.0.1:{server.port}'
        host = self.game(relay, protocol=3, name='Host', SSBU_NET_TEST_NAT=host_nat, **(host_env or {}))
        guest = self.game(relay, protocol=3, name='Guest', SSBU_NET_TEST_NAT=guest_nat, **(guest_env or {}))
        host.join()
        guest.join()
        session = secrets.token_bytes(16)
        host.ldn(BCAST, R.LM_ADVERTISE, info_of(session, host.ip))
        self.assertTrue(until(lambda: session in server.lobby.listings))
        self.assertEqual(guest.list(), [host.ip])
        guest.ldn(host.ip, R.LM_JOIN, session + b'Guest'.ljust(32, b'\0') + b'\0\0')
        host.ldn(BCAST, R.LM_ADVERTISE, info_of(session, host.ip, [guest.ip]))
        host.ldn(guest.ip, R.LM_ACCEPT, info_of(session, host.ip, [guest.ip]))
        self.assertTrue(until(lambda: guest.ip in server.lobby.listings[session].nodes))
        host.datagram(guest.ip, b'hello guest')   # (their games talk: they are introduced)
        guest.datagram(host.ip, b'hello host')
        return host, guest

    def assert_direct(self, host, guest):
        for g, other in ((host, guest), (guest, host)):
            direct, peers = g.peers(9, 1)
            self.assertEqual((direct, peers[0][:2]), (1, (other.ip, 'direct')), ''.join(g.log))
        host.datagram(guest.ip, b'straight')
        self.assertTrue(until(lambda: guest.peers()[1][0][2] >= 1), ''.join(guest.log))   # (in_direct)
        self.assertIn(b'straight', [d[4] for d in guest.received()])

    def assert_relayed(self, host, guest, why_host=None, why_guest=None):
        """Neither reaches the other directly after punching; the game traffic still goes, through the lobby;
        each log says why (the patterns given)."""
        for g, why in ((host, why_host), (guest, why_guest)):
            self.assertTrue(until(lambda: any('no direct path to' in line for line in g.log), 12), ''.join(g.log))
            direct, peers = g.peers()
            self.assertEqual((direct, peers[0][1]), (0, 'relayed'), ''.join(g.log))
            if why:
                line = next(line for line in g.log if 'no direct path to' in line)
                self.assertRegex(line, why)
                self.assertIn("through the lobby's relay", line)
        host.datagram(guest.ip, b'through the lobby')
        self.assertTrue(until(lambda: b'through the lobby' in [d[4] for d in guest.received()]))

    # ---- the case players reported: one router changes ports ---------------------------------------
    def test_remapping_router_learnt_from_its_hellos(self):
        """A symmetric router (2) and one that lets in what comes from an address sent to (4): the other side
        learns the port the hellos come from, answers there, and the path forms both ways."""
        host, guest = self.room(2, 4)
        self.assert_direct(host, guest)
        self.assertTrue(any("Host's hellos come from" in line for line in guest.log), ''.join(guest.log))

    def test_remapping_router_without_learning_relays(self):
        """The same routers as the game was (no address learnt): through the lobby, as the players saw."""
        host, guest = self.room(2, 4, host_env=dict(SSBU_NET_TEST_LEARN=0), guest_env=dict(SSBU_NET_TEST_LEARN=0))
        self.assert_relayed(host, guest, 'your router changes ports', "Host's router changes ports")

    def test_router_that_hands_out_ports_in_turn_guessed(self):
        """A sequential symmetric router (3) and a port-restricted one (1): the lobby's NAT port shows the
        first's step, the other punches its next ports, the path forms."""
        host, guest = self.room(3, 1)
        self.assert_direct(host, guest)
        self.assertTrue(any('next ones too' in line for line in guest.log), ''.join(guest.log))
        self.assertTrue(any(re.search(r'Host: direct, via guessed', line) for line in guest.log), ''.join(guest.log))

    def test_ports_in_turn_without_a_nat_port_relays(self):
        """The same routers with a lobby that has no NAT port (as the lobbies before): nothing to guess from."""
        host, guest = self.room(3, 1, nat_port=None)
        self.assert_relayed(host, guest)

    def test_port_mapping_by_nat_pmp(self):
        """A symmetric router (2) that maps the game's port when asked (NAT-PMP; PCP asked first and refused):
        the port-restricted other side (1) reaches it there."""
        gw = FakeGateway()
        self.addCleanup(gw.close)
        host, guest = self.room(2, 1, host_env=dict(SSBU_NET_TEST_GATEWAY=f'127.0.0.1:{gw.port}'))
        self.assert_direct(host, guest)
        self.assertTrue(any('NAT-PMP: the router forwards' in line for line in host.log), ''.join(host.log))
        self.assertGreaterEqual(gw.pcp_asked, 1)
        self.assertTrue(gw.asked and gw.asked[0][0] == gw.asked[0][1] and gw.asked[0][2] == 7200, gw.asked)

    def test_symmetric_and_port_restricted_relay_and_say_why(self):
        """A symmetric router at random ports (2) with no mapping, and a port-restricted one (1): no path any
        trick opens; through the lobby, each log naming whose router, F1 saying what turns it on."""
        host, guest = self.room(2, 1)
        self.assert_relayed(host, guest, 'your router changes ports .*UPnP or NAT-PMP', "Host's router changes ports")
        self.assertIn('Your router changes ports (symmetric NAT)', host.status())
        self.assertIn('relayed (their router changes ports)', guest.status())

    def test_two_symmetric_routers_fall_back_cleanly(self):
        """Two routers that change ports at random: through the lobby, within the packet budget (nothing to
        guess), the session's count saying so at the end."""
        host, guest = self.room(2, 2)
        self.assert_relayed(host, guest, 'both routers change ports', 'both routers change ports')
        for g in (host, guest):
            self.assertFalse(any('guessed ports' in line for line in g.log), ''.join(g.log))
            g.close()
            self.assertTrue(any(re.search(r'direct paths this session: 0 of 1 players introduced; relayed 1 \(.*both 1', line)
                                for line in g.log), ''.join(g.log))


if __name__ == '__main__':
    unittest.main()
