"""tests/netproto.py, the lobby protocol the client tests speak by hand: the same numbers as the lobby server's
(ssbu_relay.py, when a checkout of it is there: OPENSMASH_SERVER_DIR) and as the game's client (runtime/net.h)."""
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
import netproto as P  # noqa: E402

NAMES = ('PROTOCOL', 'JOIN', 'WELCOME', 'REJECT', 'PING', 'PONG', 'DATA', 'MEMBERS', 'LEAVE', 'LIST', 'LISTING', 'LIST_END',
         'PEERS', 'BCAST', 'PUNCH', 'DIRECT', 'NAME_LEN', 'BUILD_LEN', 'JOIN_SIZE', 'V1_JOIN_SIZE', 'SUBNET', 'BROADCASTS',
         'V3', 'RB', 'ROOMS', 'ROOM_INFO', 'ROOMS_END', 'MATCH', 'MATCH_STATE', 'V3_JOIN_SIZE', 'LOBBY', 'INTRODUCES',
         'HAS_ROOMS', 'HAS_MATCH', 'HAS_RB', 'MS_IDLE', 'MS_SEARCHING', 'MS_MATCHED', 'MS_TIMEOUT', 'MS_PEER_LEFT',
         'MS_TOGETHER', 'MS_EXPIRED', 'LDN_PORT', 'LM_ADVERTISE', 'LM_JOIN', 'LM_ACCEPT', 'LM_REJECT', 'LM_KEEP', 'LM_LEAVE',
         'LM_GONE', 'LM_KICK', 'NI_SIZE', 'NI_SESSION', 'NI_POLICY', 'NI_NODE_MAX', 'NI_NODE_COUNT', 'NI_NODES', 'NODE_SIZE')


class Protocol(unittest.TestCase):
    def test_client_header(self):
        """The game's own numbers for the versions (runtime/net.h)."""
        h = (ROOT / 'runtime' / 'net.h').read_text(encoding='utf-8')
        self.assertEqual(int(re.search(r'#define NET_PROTOCOL_V3 (\d+)', h).group(1)), P.V3)

    @unittest.skipUnless(P.HAVE_LOBBY, P.NO_LOBBY)
    def test_same_as_the_lobby(self):
        import ssbu_relay as R
        for name in NAMES:
            with self.subTest(name):
                self.assertEqual(getattr(P, name), getattr(R, name))
        self.assertEqual(P.PEER.format, R.PEER.format)
        self.assertEqual(P.BUILD.encode(), R.REQUIRED_BUILD)
        self.assertEqual(P.reject_detail(b'X' * 40, 7, P.mods((b'A', 1))), R.reject_detail(b'X' * 40, 7, P.mods((b'A', 1))))
        from test_ssbu_relay import info_of
        self.assertEqual(P.info_of(b'S' * 16, 0x0A0D0105, [0x0A0D0106]), info_of(b'S' * 16, 0x0A0D0105, [0x0A0D0106]))


if __name__ == '__main__':
    unittest.main()
