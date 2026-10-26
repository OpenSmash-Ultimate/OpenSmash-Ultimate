"""The settings menu's Lobby page (runtime/gpu_menu.c): the online lobby's room browser and
matchmaking in netplay version 3, and Online > Netplay version (online_protocol.txt), through
tools/dev/lobbymenutest.c (the menu and the game's netplay client with no game around them, driven
line by line), against fake lobbies that answer by hand and the real one (tools/ssbu_relay.py) on
127.0.0.1. Checks what the page shows (the rooms, with why one can't be joined; matchmaking's states;
version 2's "needs netplay version 3"), what its keys do and send, which version the game asks for,
and that the other pages keep their rows. Made-up data only, loopback only; waits are for conditions.

Built as tests/test_net_client_v3.py builds netv3test.c, with runtime/net_relay.c and
runtime/hle_settings.c beside it; off Windows with a stand-in d3d11.h (gpu_overlay.h names three of
its types). Skipped where that test is."""
from pathlib import Path
import os
import queue
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
import cc  # noqa: E402
import ssbu_relay as R  # noqa: E402
from test_net_client_v3 import FAKE_COOKIE, FAKE_IP, HAVE_CLANG, SIM, FakeLobby, V2OnlyLobby, free_udp_port  # noqa: E402
from test_ssbu_relay import BUILD, OTHER_BUILD, Server  # noqa: E402
from test_ssbu_relay_v3 import V3Player, mods, until  # noqa: E402

D3D11_STAND_IN = ('typedef struct ID3D11Device ID3D11Device;\ntypedef struct ID3D11DeviceContext ID3D11DeviceContext;\n'
                  'typedef struct ID3D11RenderTargetView ID3D11RenderTargetView;\n')
LOBBY_PAGE = 4   # (the Lobby page's number in the menu, less one)
NO_ANSWER = 'no answer from the lobby (A: ask again)'


def build(folder):
    src = ['tools/dev/lobbymenutest.c', 'runtime/net_relay.c', 'runtime/hle_settings.c']
    if os.name == 'nt':   # (as tests/test_net_client_v3.py builds netv3test.c)
        exe = os.path.join(folder, 'lobbymenutest.exe')
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, *src, '/Fe' + exe, '/Fo' + folder + os.sep, '/link', *cc.LINK_OPTIONS]
    else:
        exe = os.path.join(folder, 'lobbymenutest')
        Path(folder, 'd3d11.h').write_text(D3D11_STAND_IN)
        layer = ['runtime/linux/win32.c']
        layer += ['runtime/macos/memory.c', 'runtime/macos/platform.c'] if sys.platform == 'darwin' else ['-lpthread']
        cmd = ['clang', '-O1', '-w', '-I', folder, '-I', 'runtime/macos/include', '-I', 'runtime/linux/include', '-I', 'runtime', *src, *layer,
               '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError('lobbymenutest.c did not build:\n' + r.stdout + r.stderr)
    return exe


class Menu:
    """One game's settings menu and netplay client (a lobbymenutest), in a folder of its own for the
    settings files, configured as the launchers configure the game: a command in, its answer out."""
    def __init__(self, exe, top, relay=None, protocol=None, room='menu', name='Alpha', args=(), files=None, **more):
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SSBU_') and k.upper() != 'LOBBYMENUTEST_BUILD'}
        env.update(SSBU_NET_UPNP='0', LOBBYMENUTEST_BUILD=BUILD)
        for k, v in (('SSBU_NET_RELAY', relay), ('SSBU_NET_PROTOCOL', protocol), ('SSBU_NET_ROOM', room), ('SSBU_NET_NAME', name)):
            if v is not None:
                env[k] = str(v)
        env.update({k: str(v) for k, v in more.items()})
        self.dir = Path(tempfile.mkdtemp(prefix='menu ', dir=top))
        for f, text in (files or {}).items():
            (self.dir / f).write_text(text)
        self.name = name
        self.p = subprocess.Popen([exe, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env,
                                  cwd=self.dir)
        self.answers, self.log = queue.Queue(), []
        for stream, keep in ((self.p.stdout, self.answers.put), (self.p.stderr, self.log.append)):
            threading.Thread(target=lambda s=stream, k=keep: [k(line) for line in s], daemon=True).start()

    def answer(self, wait=15.0):
        try:
            return self.answers.get(timeout=wait).rstrip('\n')
        except queue.Empty:
            raise AssertionError(f'{self.name}: no answer; its log:\n' + ''.join(self.log)) from None

    def ask(self, line, wait=15.0):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        return self.answer(wait)

    def ok(self, line):
        got = self.ask(line)
        assert got == 'ok', (line, got)

    def key(self, name, n=1):
        self.ok(f'key {name} {n}')

    def type(self, text):
        self.ok(f'type {text}')

    def frames(self, secs):
        self.ok(f'frame {secs}')

    def page(self):
        """(open, page, the cursor's row, [rows]): each row a dict of id, kind, label, value, note, help."""
        head = self.ask('rows').split()
        is_open, page, sel, n = map(int, head[1:])
        out = []
        for _ in range(n):
            ids, label, value, note, helptext = self.answer().split('|')
            rid, kind = map(int, ids.split()[1:])
            out.append(dict(id=rid, kind=kind, label=label, value=value, note=note, help=helptext))
        return is_open, page, sel, out

    def rows(self):
        return self.page()[3]

    def row(self, label):
        """The row of that label (None: none)."""
        return next((r for r in self.rows() if r['label'] == label), None)

    def labels(self):
        return [r['label'] for r in self.rows()]

    def at(self):
        """The row under the cursor."""
        _, _, sel, rows = self.page()
        return rows[sel]

    def value_until(self, label, cond, wait=5.0):
        """Waits until the value of that row passes cond: the value (asserting it came)."""
        got = []
        ok = until(lambda: got.append((self.row(label) or {}).get('value')) or cond(got[-1]), wait)
        assert ok, f'{self.name}: {label} is {got[-1]!r}; its log:\n' + ''.join(self.log)
        return got[-1]

    def draw(self):
        n = int(self.ask('draw').split()[1])
        return [self.answer() for _ in range(n)]

    def toast(self):
        return self.ask('toast')[len('toast '):]

    def net(self, wait=5):
        """(version asked for, version spoken (0: not joined), joined to a lobby)."""
        return tuple(map(int, self.ask(f'net {wait}', wait + 10).split()[1:]))

    def file(self, name):
        p = self.dir / name
        return p.read_text().strip() if p.exists() else None

    def open_lobby(self):
        self.key('ESC')
        self.type('5')

    def go_to(self, label):
        """Moves the cursor to that row (down)."""
        for _ in range(40):
            if self.at()['label'] == label:
                return
            self.key('DOWN')
        raise AssertionError(f'{self.name}: no row {label}')

    def close(self):
        if self.p.poll() is None:
            try:
                self.p.stdin.write('quit\n')
                self.p.stdin.flush()
                self.p.wait(10)
            except (OSError, subprocess.TimeoutExpired):
                self.p.kill()
                self.p.wait(10)
        for f in (self.p.stdin, self.p.stdout, self.p.stderr):
            try:
                f.close()
            except OSError:
                pass


class AnsweringLobby:
    """A fake lobby that answers a JOIN with WELCOME (these flags, in the JOIN's version) and each PING
    with PONG (these counts) on a thread of its own, and keeps every other packet for the test, which
    answers it by hand."""
    def __init__(self, flags=31, players=4, rooms=2, searching=3, code_players=9):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(('127.0.0.1', 0))
        self.s.settimeout(0.05)
        self.address = f'127.0.0.1:{self.s.getsockname()[1]}'
        self.flags, self.counts = flags, (players, rooms, searching, code_players)
        self.got, self.at, self.version, self.stop = queue.Queue(), None, None, False
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop:
            try:
                p, at = self.s.recvfrom(65536)
            except (socket.timeout, ConnectionResetError):
                continue
            except OSError:
                return
            if len(p) < 4 or p[:2] != b'SN':
                continue
            if p[3] == R.JOIN:
                self.at, self.version = at, p[2]
                self.s.sendto(b'SN' + bytes((p[2], R.WELCOME)) + p[4:12] + struct.pack('>IQB', FAKE_IP, FAKE_COOKIE,
                              self.flags if p[2] == R.V3 else 3) + b'\0', at)
            elif p[3] == R.PING:
                players, rooms, searching, code_players = self.counts
                self.s.sendto(b'SN' + bytes((p[2], R.PONG)) + p[12:20] + struct.pack('>HHB', players, rooms, 1) +
                              (struct.pack('>HH', searching, code_players) if p[2] == R.V3 else b''), at)
            else:
                self.got.put(p)

    def next(self, kind, wait=3.0):
        """The body (after the header) of the next packet of that kind (the others before it passed over)."""
        try:
            while True:
                p = self.got.get(timeout=wait)
                if p[3] == kind:
                    return p[4:]
        except queue.Empty:
            raise AssertionError(f'no packet of kind {kind} came') from None

    def kinds(self, wait=0.6):
        out = []
        try:
            while True:
                out.append(self.got.get(timeout=wait)[3])
        except queue.Empty:
            return out

    def send(self, kind, body):
        self.s.sendto(b'SN' + bytes((R.V3, kind)) + body, self.at)

    def rooms(self, seq, *rooms):
        """Answers a ROOMS (seq: its body's last 4 bytes) with these ROOM_INFOs, then ROOMS_END."""
        for r in rooms:
            self.send(R.ROOM_INFO, seq + r)
        self.send(R.ROOMS_END, seq + bytes((len(rooms),)))

    def close(self):
        self.stop = True
        self.thread.join(2)
        self.s.close()


def room(host, name, players=2, most=4, flags=1 | 8, build=BUILD, sim=SIM, mod_set=b'\0'):
    """A ROOM_INFO's body after its seq (flags 1 open, 2 yours, 4 mods known, 8 of your key)."""
    return (struct.pack('>IBBB', host, players, most, flags) + name.encode().ljust(32, b'\0') + build.encode().ljust(40, b'\0') +
            struct.pack('>I', sim) + mod_set)


def match_sent(lobby, op, other_than=None):
    """The search id of the next MATCH of that op (1 a search, 0 a cancel), passing over the ones before
    it (a search is sent again every 2 s) and, for a search, the ones of the search other_than."""
    while True:
        body = lobby.next(R.MATCH)
        if body[8] == op and body[9:13] != other_than:
            return body[9:13]


def match_state(sid, state, role=0, flags=0, peer=0, name=b'', searching=1, players=4, waited=0):
    """A MATCH_STATE's body (states 0 idle, 1 searching, 2 matched, 3 timeout, 4 left, 5 together, 6 expired)."""
    return sid + struct.pack('>BBBI', state, role, flags, peer) + name.ljust(32, b'\0') + struct.pack('>HHH', searching, players, waited)


@unittest.skipUnless(HAVE_CLANG, 'needs clang on x86-64 Linux or an Apple silicon Mac, or clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class MenuTest(unittest.TestCase):
    exe = None

    @classmethod
    def setUpClass(cls):
        if MenuTest.exe is None:   # (built once for every class here)
            MenuTest.folder = tempfile.mkdtemp(prefix='lobbymenutest ')
            MenuTest.exe = build(MenuTest.folder)

    def setUp(self):
        self.menus, self.fakes = [], []

    def tearDown(self):
        for m in self.menus:
            m.close()
        for f in self.fakes:
            f.close()

    def menu(self, relay=None, **kw):
        m = Menu(self.exe, self.folder, relay, **kw)
        self.menus.append(m)
        return m

    def lobby(self, **kw):
        f = AnsweringLobby(**kw)
        self.fakes.append(f)
        return f

    def v3(self, **kw):
        """A menu joined to a fake lobby of version 3, on the Lobby page: (menu, lobby)."""
        lobby = self.lobby(**kw)
        m = self.menu(lobby.address, protocol=3)
        self.assertEqual(m.net(), (3, 3, 1))
        m.open_lobby()
        return m, lobby


def tearDownModule():
    if MenuTest.exe is not None:
        shutil.rmtree(MenuTest.folder, ignore_errors=True)


class Pages(MenuTest):
    """The pages, their keys, and the other pages' rows as before."""
    def test_page_keys(self):
        m = self.menu()   # (offline)
        m.key('ESC')
        self.assertEqual(m.page()[:2], (1, 0))
        m.type('5')
        self.assertEqual(m.page()[1], LOBBY_PAGE)
        m.key('TAB')
        self.assertEqual(m.page()[1], 0)   # (after the Lobby: Display again)
        m.key('SHTAB')
        self.assertEqual(m.page()[1], LOBBY_PAGE)
        m.type('4')
        self.assertEqual(m.page()[1], 3)
        m.key('TAB')
        self.assertEqual(m.page()[1], LOBBY_PAGE)
        drawn = m.draw()
        for i, name in enumerate(('Display', 'Controllers', 'Online', 'Game', 'Lobby')):
            self.assertIn(f'{i + 1} {name}', drawn)
        self.assertIn('Up/Down: choose   Left/Right: change   A/Enter: select   L/R or Tab, 1-5: page   B/Esc: close', drawn)
        m.key('ESC')
        self.assertEqual(m.page()[0], 0)

    def test_other_pages_as_before(self):
        m = self.menu()
        m.key('ESC')
        display = ['Window', 'Frame pacing', 'Frames queued ahead', 'Hide the mouse pointer', 'Frame rate in the corner', 'Window size',
                   'Render scale'] + (['Graphics card'] if os.name == 'nt' else [])
        self.assertEqual(m.labels(), display)
        m.type('2')
        self.assertEqual(m.labels(), ['Controllers', 'Rumble', 'Keyboard plays as', 'Pads when not focused', 'Open controllers.ini'])
        m.type('3')
        self.assertEqual(m.labels(), ['Now', 'Play online in', 'Your name', 'Direct connections', 'Netplay version'])   # (offline: no code)
        m.type('4')
        self.assertEqual(m.labels(), ['Restart the game', 'Quit the game', 'Esc / Home button', 'F1', 'F2', 'F3', 'F11, Alt+Enter', 'Version'])

    def test_offline(self):
        m = self.menu()
        m.open_lobby()
        is_open, page, sel, rows = m.page()
        self.assertEqual([(r['label'], r['value']) for r in rows], [('Lobby', 'offline'), ('Netplay version', '2')])
        self.assertEqual(sel, 1)   # (the Lobby row is information: the cursor is on the next)
        self.assertIn('choose a lobby in Online > Play online in', rows[0]['help'])


class Version(MenuTest):
    """Online > Netplay version (online_protocol.txt), and which version the game asks for."""
    def test_setting_saved(self):
        m = self.menu()
        m.open_lobby()
        m.key('RIGHT')
        self.assertEqual(m.row('Netplay version')['value'], '3 (being tried out)')
        self.assertEqual(m.file('online_protocol.txt'), '3')
        self.assertIn('Restart now', m.labels())   # (next start)
        self.assertIn('next start', m.draw())
        m.type('3')
        r = m.row('Netplay version')
        self.assertEqual((r['value'], r['note'], r['kind']), ('3 (being tried out)', '', 1))
        self.assertIn('Restart now', m.labels())
        m.go_to('Netplay version')
        m.key('LEFT')
        self.assertEqual(m.file('online_protocol.txt'), '2')
        m.key('RIGHT')
        m.key('ESC')
        m.key('ESC')   # (opened again: read from the file)
        self.assertEqual(m.row('Netplay version')['value'], '3 (being tried out)')

    def join_of(self, **kw):
        """The first JOIN a menu's game sends to a fake lobby."""
        lobby = FakeLobby()
        self.fakes.append(lobby)
        m = self.menu(lobby.address, **kw)
        m.ask('net 0')   # (the client starts when first asked, as the game's overlay asks)
        return lobby.next(R.JOIN)[0], m

    def test_file_asks_for_3(self):
        join, m = self.join_of(files={'online_protocol.txt': '# netplay version\n3\n'})
        self.assertEqual((join[2], len(join)), (3, R.V3_JOIN_SIZE))
        self.assertEqual(m.net(0)[0], 3)

    def test_no_file_as_before(self):
        join, _ = self.join_of()
        self.assertEqual((join[2], len(join)), (2, R.JOIN_SIZE))
        join, _ = self.join_of(files={'online_protocol.txt': '4\n'})   # (not a version: the default)
        self.assertEqual((join[2], len(join)), (2, R.JOIN_SIZE))

    def test_variable_wins(self):
        join, m = self.join_of(protocol=2, files={'online_protocol.txt': '3\n'})
        self.assertEqual(join[2], 2)
        m.key('ESC')
        m.type('3')
        r = m.row('Netplay version')
        self.assertEqual((r['value'], r['note']), ('3 (being tried out)', 'SSBU_NET_PROTOCOL set'))   # (the file's, and what wins)
        join, _ = self.join_of(protocol=3, files={'online_protocol.txt': '2\n'})
        self.assertEqual(join[2], 3)

    def test_online_files(self):
        """Started with --online (Play SSBU.bat): the lobby and code from online_server.txt, the version from online_protocol.txt."""
        lobby = FakeLobby()
        self.fakes.append(lobby)
        m = self.menu(room=None, name=None, args=['--online'],
                      files={'online_server.txt': f'{lobby.address} ourcode\n', 'online_name.txt': 'Alpha\n', 'online_protocol.txt': '3\n'})
        m.ask('net 0')
        join = lobby.next(R.JOIN)[0]
        self.assertEqual((join[2], join[12:20], join[44:49]), (3, b'ourcode\0', b'Alpha'))

    def test_hosting_stays_2(self):
        port = free_udp_port()
        m = self.menu(None, files={'online_protocol.txt': '3\n'}, SSBU_NET_HOST=1, SSBU_NET_PORT=port, SSBU_NET_BIND='127.0.0.1')
        self.assertEqual(m.net(), (2, 2, 0))
        m.open_lobby()
        self.assertEqual(m.row('Lobby')['value'], 'hosting a private room')
        self.assertNotIn('Netplay version', m.labels())
        m.type('3')
        self.assertEqual(m.row('Netplay version')['note'], 'hosting: 2')
        guest = self.menu(f'127.0.0.1:{port}', protocol=3, name='Guest')   # (a private room's relay: version 2, after a REJECT)
        self.assertEqual(guest.net(), (3, 2, 0))
        guest.open_lobby()
        self.assertEqual(guest.row('Lobby')['value'], 'in a private room')


class Version2(MenuTest):
    """Netplay version 2, the default: the page says what version 3 would have, and sends nothing."""
    def test_needs_version_3(self):
        lobby = self.lobby()
        m = self.menu(lobby.address)
        self.assertEqual(m.net(), (2, 2, 1))
        m.open_lobby()
        rows = m.rows()
        self.assertEqual([(r['label'], r['value']) for r in rows], [('Lobby', 'needs netplay version 3'), ('Netplay version', '2')])
        self.assertEqual(rows[0]['help'], "The lobby's room browser and matchmaking need netplay version 3, which is being tried out. To turn it "
                                          "on, set Netplay version to 3 (below, or on the Online page) and restart the game, or start it with "
                                          "SSBU_NET_PROTOCOL=3.")
        self.assertIn('needs netplay version 3', m.draw())
        m.key('ENTER', 3)
        self.assertNotIn(R.ROOMS, lobby.kinds())   # (no ROOMS and no MATCH in version 2)
        self.assertNotIn(R.MATCH, lobby.kinds(0.1))

    def test_lobby_of_version_2(self):
        server = Server(make=V2OnlyLobby)
        self.addCleanup(server.stop)
        m = self.menu(f'127.0.0.1:{server.port}', protocol=3)
        self.assertEqual(m.net(), (3, 2, 1))
        m.open_lobby()
        self.assertEqual(m.labels(), ['Lobby'])
        self.assertEqual(m.row('Lobby')['value'], 'this lobby speaks version 2 only')


class Rooms(MenuTest):
    """The room browser against a fake lobby."""
    def test_states_and_asking_again(self):
        m, lobby = self.v3()
        seq = lobby.next(R.ROOMS)[8:12]   # (asked as the page showed)
        self.assertEqual(m.row('Rooms')['value'], 'asking the lobby...')
        m.value_until('Rooms', lambda v: v == NO_ANSWER)   # (3 s unanswered)
        m.go_to('Rooms')
        m.key('ENTER')
        again = lobby.next(R.ROOMS)[8:12]
        self.assertNotEqual(again, seq)
        lobby.rooms(again, room(FAKE_IP + 7, 'Bravo'))
        self.assertEqual(m.value_until('Rooms', lambda v: v != 'asking the lobby...'), '1, 1 you can join')
        self.assertEqual(m.row('Bravo')['value'], '2/4 players, open')
        m.type('1')
        m.type('5')   # (shown again: asked again)
        third = lobby.next(R.ROOMS)[8:12]
        m.key('ESC')
        m.key('ESC')   # (the menu opened again on the page, the last question unanswered: not asked again)
        self.assertNotIn(R.ROOMS, lobby.kinds(0.3))
        lobby.rooms(third, room(FAKE_IP + 7, 'Bravo', players=3))
        self.assertEqual(m.value_until('Bravo', lambda v: v.startswith('3')), '3/4 players, open')
        m.key('ESC')
        m.key('ESC')   # (answered: asked again)
        lobby.next(R.ROOMS)

    def test_rooms_and_why(self):
        m, lobby = self.v3()
        seq = lobby.next(R.ROOMS)[8:12]
        lobby.rooms(seq, room(FAKE_IP + 1, 'Alpha', flags=1 | 2 | 8, players=1),
                    room(FAKE_IP + 2, 'Bravo'),
                    room(FAKE_IP + 3, 'Golf', players=4, flags=8),
                    room(FAKE_IP + 4, 'Charlie', flags=1, sim=SIM + 1),
                    room(FAKE_IP + 5, 'Delta', flags=1, sim=SIM - 1),
                    room(FAKE_IP + 6, 'Echo', flags=1, build=OTHER_BUILD),
                    room(FAKE_IP + 7, 'Foxtrot', flags=1 | 4, mod_set=mods((b'Alpha', 5))))
        self.assertEqual(m.value_until('Rooms', lambda v: v != 'asking the lobby...'), '7, 2 you can join')
        rows = {r['label']: r for r in m.rows()}
        self.assertEqual([(k, rows[k]['value']) for k in ('Alpha (you)', 'Bravo', 'Golf', 'Charlie', 'Delta', 'Echo', 'Foxtrot')],
                         [('Alpha (you)', '1/4 players, open'), ('Bravo', '2/4 players, open'), ('Golf', '4/4 players, full or closed'),
                          ('Charlie', "can't join: update your WindowSmash"), ('Delta', "can't join: their WindowSmash is older"),
                          ('Echo', "can't join: another game version"), ('Foxtrot', "can't join: other mods")])
        self.assertEqual(rows['Bravo']['help'], "Bravo's room, 2 of 4 players. To join it, close this menu, then Local Wireless > Rooms List and "
                                                "pick Bravo's room.")
        self.assertIn(f'simulation version is {SIM + 1}, yours {SIM}', rows['Charlie']['help'])
        self.assertIn('Yours is the older one: update WindowSmash to join.', rows['Charlie']['help'])
        self.assertIn('Theirs is the older one: the host needs to update.', rows['Delta']['help'])
        self.assertIn(f'build {OTHER_BUILD[:8]}, yours {BUILD[:8]}', rows['Echo']['help'])
        self.assertEqual(rows['Foxtrot']['help'], "Mods you don't have: Alpha")
        for label, said in (('Bravo', "Close this menu, then Local Wireless > Rooms List: Bravo's room"),
                            ('Alpha (you)', 'This is your own room'),
                            ('Golf', "Golf's room is full or closed for now"),
                            ('Charlie', "Can't join Charlie's room: update your WindowSmash")):
            m.go_to(label)
            m.key('ENTER')
            self.assertEqual(m.toast(), said)
        self.assertEqual(m.page()[0], 1)   # (the menu stays open)
        m.go_to('Charlie')   # (its help's second line on a line of its own)
        self.assertTrue(any(line.endswith('needs updating) / Yours is the older one: update WindowSmash to join.') for line in m.draw()))

    def test_cursor_stays_on_its_room(self):
        m, lobby = self.v3()
        lobby.rooms(lobby.next(R.ROOMS)[8:12], room(FAKE_IP + 1, 'Alpha'), room(FAKE_IP + 2, 'Bravo'), room(FAKE_IP + 3, 'Charlie'))
        m.value_until('Rooms', lambda v: v == '3, 3 you can join')
        m.go_to('Rooms')
        m.key('ENTER')
        seq = lobby.next(R.ROOMS)[8:12]
        m.go_to('Charlie')   # (in the list shown until the answer comes)
        lobby.rooms(seq, room(FAKE_IP + 3, 'Charlie'), room(FAKE_IP + 4, 'Delta'), room(FAKE_IP + 1, 'Alpha'), room(FAKE_IP + 2, 'Bravo'))
        m.value_until('Rooms', lambda v: v == '4, 4 you can join')
        self.assertEqual(m.at()['label'], 'Charlie')


class Matchmaking(MenuTest):
    """Find a match against a fake lobby: every state, the cancel, and the messages."""
    def test_states(self):
        m, lobby = self.v3()
        self.assertEqual(m.value_until('Lobby', lambda v: 'players' in v), '4 players, 3 searching')   # (the PONG's counts)
        self.assertEqual(m.at()['label'], 'Find a match')
        self.assertEqual(m.at()['value'], 'not searching')
        m.key('ENTER')
        sid = match_sent(lobby, 1)   # (a search for a room)
        self.assertEqual(m.at()['value'], 'asking the lobby...')
        self.assertEqual(m.labels()[:3], ['Lobby', 'Find a match', 'Stop searching'])
        lobby.send(R.MATCH_STATE, match_state(sid, 1, searching=2, players=5))
        self.assertRegex(m.value_until('Find a match', lambda v: v.startswith('searching')), r'^searching\.\.\. 0:0\d$')
        self.assertEqual(m.row('Lobby')['value'], '5 players, 2 searching')
        m.key('ENTER')
        self.assertEqual(m.toast(), 'Searching already')
        lobby.send(R.MATCH_STATE, match_state(sid, 2, role=2, peer=FAKE_IP + 9, name=b'Bravo'))
        self.assertEqual(m.value_until('Find a match', lambda v: v.startswith('with')), 'with Bravo: they host')
        self.assertEqual(m.toast(), 'Matched with Bravo: they host (their room comes first in Rooms List)')
        self.assertIn('Leave the match', m.labels())
        lobby.send(R.MATCH_STATE, match_state(sid, 2, role=2, flags=1, peer=FAKE_IP + 9, name=b'Bravo'))
        self.assertEqual(m.value_until('Find a match', lambda v: 'open' in v), 'with Bravo: their room is open')
        self.assertEqual(m.toast(), "Bravo's room is open (Local Wireless > Rooms List)")
        self.assertIn('close this menu, then Local Wireless > Rooms List', m.row('Find a match')['help'])
        lobby.send(R.MATCH_STATE, match_state(sid, 4, role=2, peer=FAKE_IP + 9, name=b'Bravo'))
        self.assertEqual(m.value_until('Find a match', lambda v: 'left' in v), 'Bravo cancelled or left')
        self.assertEqual(m.toast(), 'Matchmaking: Bravo cancelled or left')
        self.assertNotIn('Leave the match', m.labels())
        m.go_to('Find a match')
        m.key('ENTER')   # (again: a new search)
        sid = match_sent(lobby, 1, sid)
        lobby.send(R.MATCH_STATE, match_state(sid, 2, role=1, peer=FAKE_IP + 9, name=b'Bravo'))
        self.assertEqual(m.value_until('Find a match', lambda v: v.startswith('with')), 'with Bravo: you host')
        self.assertEqual(m.toast(), 'Matched with Bravo: you host (Local Wireless > Create a Room)')
        m.go_to('Leave the match')
        m.key('ENTER')
        self.assertEqual(m.toast(), 'You left the match')
        self.assertEqual(match_sent(lobby, 0), sid)
        self.assertEqual(m.row('Find a match')['value'], 'not searching')
        self.assertEqual(m.at()['label'], 'Rooms')   # (the row under the one gone)

    def test_ends_said_while_closed(self):
        m, lobby = self.v3()
        m.key('ENTER')
        sid = match_sent(lobby, 1)
        m.key('ESC')
        self.assertEqual(m.page()[0], 0)
        for state, value, said in ((3, 'no opponent found in time', 'Matchmaking: no opponent found in time'),
                                   (5, 'in one room with Bravo', 'Matchmaking: you and Bravo are in one room'),
                                   (6, 'expired: not in one room in time', 'Matchmaking: the match expired (not in one room in time)')):
            lobby.send(R.MATCH_STATE, match_state(sid, 1))
            lobby.send(R.MATCH_STATE, match_state(sid, state, role=1, peer=FAKE_IP + 9, name=b'Bravo'))
            self.assertTrue(until(lambda: m.frames(0.05) or m.toast() == said), said)   # (the menu closed: its frames go on)
            m.key('ESC')
            self.assertEqual(m.value_until('Find a match', lambda v: v != 'not searching'), value)
            m.go_to('Find a match')
            m.key('ENTER')   # (searching again)
            sid = match_sent(lobby, 1, sid)
            m.key('ESC')

    def test_stop_searching(self):
        m, lobby = self.v3()
        m.key('ENTER')
        sid = match_sent(lobby, 1)
        m.go_to('Stop searching')
        m.key('ENTER')
        self.assertEqual(m.toast(), 'Search stopped')
        self.assertEqual(match_sent(lobby, 0), sid)
        self.assertEqual(m.labels()[:3], ['Lobby', 'Find a match', 'Rooms'])


class RealLobby(MenuTest):
    """Menus against tools/ssbu_relay.py."""
    def setUp(self):
        super().setUp()
        self.server = Server(search_for=30.0, match_for=30.0, listed_for=60.0)
        self.relay = f'127.0.0.1:{self.server.port}'

    def tearDown(self):
        super().tearDown()
        self.server.stop()

    def test_two_menus_matched(self):
        a = self.menu(self.relay, protocol=3, name='Alpha')
        b = self.menu(self.relay, protocol=3, name='Bravo')
        self.assertEqual((a.net(), b.net()), ((3, 3, 1), (3, 3, 1)))
        a.open_lobby()
        b.open_lobby()
        a.key('ENTER')
        a.value_until('Find a match', lambda v: v.startswith('searching'))
        b.key('ENTER')
        self.assertEqual(a.value_until('Find a match', lambda v: v.startswith('with')), 'with Bravo: you host')   # (Alpha waited longer)
        self.assertEqual(b.value_until('Find a match', lambda v: v.startswith('with')), 'with Alpha: they host')
        self.assertTrue(until(lambda: a.toast() == 'Matched with Bravo: you host (Local Wireless > Create a Room)'))
        b.go_to('Leave the match')
        b.key('ENTER')
        self.assertEqual(a.value_until('Find a match', lambda v: 'left' in v), 'Bravo cancelled or left')
        self.assertEqual(b.row('Find a match')['value'], 'not searching')

    def test_rooms_listed_with_why(self):
        hosts = {}
        for name, kw in (('Same', {}), ('Newer', dict(sim=SIM + 1)), ('Modded', dict(mod_set=mods((b'Alpha', 1)))),
                         ('Older', dict(build=OTHER_BUILD))):
            p = V3Player(self.server, room='menurooms', name=name, sim=kw.pop('sim', SIM), **kw)
            self.fakes.append(p)
            p.join()
            p.advertise(bytes(16 - len(name)) + name.encode())
            hosts[name] = p
        self.assertTrue(until(lambda: len(self.server.lobby.listings) == 4))
        m = self.menu(self.relay, protocol=3, room='menurooms')
        m.net()
        m.open_lobby()
        self.assertEqual(m.value_until('Rooms', lambda v: v.startswith('4')), '4, 1 you can join')
        rows = m.rows()
        got = {r['label']: r['value'] for r in rows if r['id'] >= 60}
        self.assertEqual([r['label'] for r in rows if r['id'] >= 60][0], 'Same')   # (the rooms of this game's key first)
        self.assertEqual(got, {'Same': '1/8 players, open', 'Newer': "can't join: update your WindowSmash", 'Modded': "can't join: other mods",
                               'Older': "can't join: another game version"})


if __name__ == '__main__':
    unittest.main()
