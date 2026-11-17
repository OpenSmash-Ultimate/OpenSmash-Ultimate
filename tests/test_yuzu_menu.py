"""The real compatibility backend in the existing settings menu, no game data."""
from pathlib import Path
import os
import tempfile
import unittest

from test_yuzu_client import ROOT, RoomFixture
from test_lobby_menu import Menu, build


@unittest.skipUnless(os.name == 'nt', 'Windows compatibility client')
class YuzuMenuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not all((ROOT / 'build/yuzu' / name).is_file() for name in ('enet.dll', 'libzstd.dll')):
            raise unittest.SkipTest('run python tools/yuzu_client_deps.py first')
        cls.folder = tempfile.TemporaryDirectory(prefix='yuzu menu ')
        try:
            cls.exe = build(cls.folder.name, yuzu=True)
        except BaseException:
            cls.folder.cleanup()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def test_compatibility_pages_are_read_only_and_keep_native_settings(self):
        room = RoomFixture(); self.addCleanup(room.close)
        menu = Menu(self.exe, self.folder.name, args=['--online'], name='OpenSmash',
                    files={'online_server.txt': 'native.example:39420 friends\n',
                           'online_protocol.txt': '3\n', 'online_rollback.txt': 'on\n'},
                    SSBU_NET_BACKEND='yuzu', SSBU_YUZU_SERVER=f'127.0.0.1:{room.port}',
                    SSBU_YUZU_PASSWORD='test-password', SSBU_YUZU_TOKEN='',
                    SSBU_YUZU_LIBDIR=str(ROOT / 'build/yuzu'), SSBU_YUZU_PROXY_PROFILE='eden',
                    SSBU_RB='0', SSBU_RUNAHEAD='0')
        self.addCleanup(menu.close)
        self.assertEqual(menu.net(), (0, 0, 0))  # no native lobby/protocol in this mode
        menu.key('ESC'); menu.type('3')
        self.assertEqual(menu.labels(), ['Now', 'Connection', 'Your name', 'Room protocol'])
        self.assertIn('yuzu room:', menu.row('Now')['value'])
        self.assertEqual(menu.row('Your name')['value'], 'OpenSmash')
        self.assertTrue(all(row['kind'] == menu.row('Now')['kind'] for row in menu.rows()))
        for _ in range(8):   # (Restart game / Quit game under the rows ask to end the game: not this page's settings)
            if not menu.cursor()[2]:
                menu.key('RIGHT'); menu.key('ENTER')
            menu.key('DOWN')
        self.assertEqual(menu.cursor()[2], 1)   # (nothing to choose here: the cursor goes to Restart game)
        self.assertEqual(menu.labels(), ['Now', 'Connection', 'Your name', 'Room protocol'])
        menu.type('5')
        self.assertEqual(menu.labels(), ['yuzu room'])
        self.assertIn('Local Wireless', menu.row('yuzu room')['help'])
        menu.key('ENTER')
        self.assertEqual(menu.file('online_server.txt'), 'native.example:39420 friends')
        self.assertEqual(menu.file('online_protocol.txt'), '3')
        self.assertEqual(menu.file('online_rollback.txt'), 'on')
        self.assertEqual(room.packets(6), [])  # menu navigation starts no game session


if __name__ == '__main__':
    unittest.main()
