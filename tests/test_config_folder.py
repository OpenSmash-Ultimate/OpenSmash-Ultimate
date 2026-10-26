"""The settings folder (runtime/hle_config.c, config_folder.h): config/ with its version.ini, made on the
first start from WindowSmash's files in the port's folder and an ssbu-recomp install's in its build
folder, once; and controllers.ini read in either port's schema (hle_pad.c), rewritten from
ssbu-recomp's in this port's terms. Through tools/dev/configtest.c, on made-up folders: a new
install, each old layout, both at once, a second start, a folder that cannot be written. Built
with the platform's compiler as tests/test_kernel_threads.py builds kerneltest.c; skipped where
there is none."""
from pathlib import Path
import os
import platform
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

if os.name == 'nt':
    HAVE_CLANG = os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None
else:
    HAVE_CLANG = shutil.which('clang') is not None and platform.machine() in (('arm64',) if sys.platform == 'darwin' else ('x86_64', 'AMD64'))

SEP = os.sep
GUID = '0300000012340000abcd000000000000'   # (made up)

# ssbu-recomp's controllers.ini, made up, as its program writes it (CRLF)
SR_PADS = '\r\n'.join([
    '; controllers.ini: pads and keyboard (made up)',
    '[general]',
    'layout = auto',
    'deadzone = 0.15',
    'outer_zone = 0.95',
    'trigger_threshold = 0.4',
    'rumble = on',
    'rumble_strength = 0.5',
    'gamecube_style = pro',
    'ignore = 0300000000000000aaaa000000000000',
    'ignore = 0300000000000000bbbb000000000000',
    '',
    '[players]',
    'player1 = auto',
    f'player2 = {GUID}',
    'player3 = none',
    '',
    '[keyboard]',
    'player = 2',
    'A = Z',
    'LSTICK_LEFT = LEFT',
    '',
    f'[device {GUID}]',
    'name = Made-up pad',
    'layout = labels            ; positional | labels | auto',
    'deadzone = 0.2',
    'rumble = off',
    'rumble_strength = 0.8',
    'map.A = south, button7',
    'map.STICK_L = leftstick',
    'map.ZR = righttrigger+',
    'map.LSTICK_LEFT = dpleft',
    'axis.LX = leftx',
    'invert.LY = on',
    '',
    '[type playstation]',
    'rumble_strength = 0.6',
    'map.B = east',
    '',
    '[type joystick]',
    'deadzone = 0.3',
    '']).encode()

# what the game makes of it
SR_PADS_READ = f'''keyboard_player=2 background=1 rumble=1
player 2 = {GUID}
player 3 = none
[device default] layout=0 style=1 deadzone=15 rumble=50 trigger_threshold=40
[device {GUID}] layout=1 style=-1 deadzone=20 rumble=0 trigger_threshold=-1
  A = south
  LSTICK = leftstick
  ZR = righttrigger
[device ps3] layout=-1 style=-1 deadzone=-1 rumble=60 trigger_threshold=-1
  B = east
[device ps4] layout=-1 style=-1 deadzone=-1 rumble=60 trigger_threshold=-1
  B = east
[device ps5] layout=-1 style=-1 deadzone=-1 rumble=60 trigger_threshold=-1
  B = east
'''

# the same read on top of this port's default controllers.ini, whose [device 03004271...]
# section (a Mayflash adapter's inverted C-stick, flipped back) rides along
SR_PADS_READ_DEFAULTS = SR_PADS_READ.replace(
    '[device default] layout=0 style=1 deadzone=15 rumble=50 trigger_threshold=40\n',
    '[device default] layout=0 style=1 deadzone=15 rumble=50 trigger_threshold=40\n'
    '[device 03004271790000004418000000016800] layout=-1 style=-1 deadzone=-1 rumble=-1 trigger_threshold=-1\n')

SR_GRAPHICS = '\r\n'.join([
    '; graphics.ini (made up)',
    'renderer = vulkan',
    'window = borderless',
    'width = 1600',
    'height = 900',
    'vsync = auto',
    'gpu = 1',
    'resolution_scale = 0.75',
    'stats_overlay = off',
    'player_name = Made Up Name',
    'online_direct = off',
    'online_server = lobby.example:39500',
    '']).encode()

# WindowSmash's files, made up, in the port's folder
WS_FILES = {
    'display.ini': b'; display.ini (made up)\r\nwindow = fullscreen\r\nwidth = 1920\r\n',
    'controllers.ini': b'[general]\nkeyboard_player = 3\n\n[device default]\ndeadzone = 5\n',
    'launcher.ini': b'show_launcher = 0\r\n',
    'online_name.txt': b'Made-up Player\n',
    'online_direct.txt': b'off\n',
}
SERVER = b'# the lobby (made up)\nlobby.example:39421\n'


def build(folder):
    srcs = ['tools/dev/configtest.c', 'runtime/hle_config.c', 'runtime/hle_settings.c']
    if os.name == 'nt':
        exe = os.path.join(folder, 'configtest.exe')
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, *srcs, '/Fe' + exe, '/Fo' + folder + os.sep,
               '/link', *cc.LINK_OPTIONS]
    else:
        exe = os.path.join(folder, 'configtest')
        layer = ['runtime/linux/win32.c']   # (the platform layer)
        if sys.platform == 'darwin':
            layer += ['runtime/macos/memory.c', 'runtime/macos/platform.c']
        else:
            layer += ['-lpthread']
        cmd = ['clang', '-O1', '-w', '-I', 'runtime/macos/include', '-I', 'runtime/linux/include', '-I', 'runtime', *srcs, *layer,
               '-lm', '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError('configtest.c did not build:\n' + r.stdout + r.stderr)
    return exe


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def tree(top):
    """{relative path: bytes} of every file under top."""
    return {p.relative_to(top).as_posix(): p.read_bytes() for p in sorted(top.rglob('*')) if p.is_file()}


@unittest.skipUnless(HAVE_CLANG, 'needs clang on x86-64 Linux or an Apple silicon Mac, or clang-cl on Windows')
class ConfigTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.mkdtemp(prefix='configtest ')
        cls.exe = build(cls.build_dir)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.build_dir, ignore_errors=True)

    def setUp(self):
        t = tempfile.TemporaryDirectory(prefix='config folder ')
        self.addCleanup(t.cleanup)
        self.top = Path(t.name) / 'Win Smash'
        self.top.mkdir()

    def run_test(self, *args, ok=True):
        env = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
        r = subprocess.run([self.exe, *args], cwd=self.top, capture_output=True, text=True, env=env, timeout=60)
        if ok:
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r

    def paths(self):
        """The game's paths of the files, after its first call (which makes the folder)."""
        r = self.run_test('paths')
        got = dict(line.split(': ', 1) for line in r.stdout.splitlines())
        return got, r.stderr

    def dump(self, path):
        return self.run_test('dump', str(path)).stdout

    def version(self):
        return (self.top / 'config' / 'version.ini').read_text()


class FolderTests(ConfigTestCase):
    def config(self, name):
        return 'config' + SEP + name

    def test_new_install(self):
        got, _ = self.paths()
        self.assertEqual(got, {'display': self.config('display.ini'), 'controllers': self.config('controllers.ini'),
                               'launcher': self.config('launcher.ini'), 'online_server': 'online_server.txt',
                               'online_name': self.config('online_name.txt'), 'online_direct': self.config('online_direct.txt'),
                               'online_protocol': self.config('online_protocol.txt')})
        self.assertEqual(sorted(tree(self.top)), ['config/version.ini'])
        text = self.version()
        self.assertIn('\nversion = 1\n', text.replace('\r\n', '\n'))
        self.assertTrue(text.replace('\r\n', '\n').rstrip().endswith('[migrated]'), text)   # (nothing copied)

    def test_defaults_written_in_the_folder(self):
        r = self.run_test('load')
        self.assertIn('path: ' + self.config('controllers.ini'), r.stdout)
        self.assertIn('keyboard_player=1 background=1 rumble=1', r.stdout)
        text = (self.top / 'config' / 'controllers.ini').read_text()
        self.assertTrue(text.startswith('; controllers.ini: how pads become players.'), text[:80])
        self.assertFalse((self.top / 'controllers.ini').exists())

    def test_windowsmash_layout(self):
        for name, data in WS_FILES.items():
            write(self.top / name, data)
        write(self.top / 'online_server.txt', SERVER)
        before = tree(self.top)
        got, err = self.paths()
        after = tree(self.top)
        for name in before:   # (the old files stay as they were)
            self.assertEqual(after[name], before[name], name)
        for name, data in WS_FILES.items():
            self.assertEqual(after['config/' + name], data, name)
        self.assertNotIn('config/online_server.txt', after)   # (the port's own file, read where it is)
        self.assertEqual(got['online_server'], 'online_server.txt')
        self.assertIn('windowsmash = display.ini, controllers.ini, launcher.ini, online_name.txt, online_direct.txt', self.version())
        self.assertIn('[config] copied display.ini, controllers.ini', err)
        self.assertIn('keyboard_player=3', self.run_test('load').stdout)

    def test_second_start(self):
        for name, data in WS_FILES.items():
            write(self.top / name, data)
        write(self.top / 'online_server.txt', SERVER)
        self.paths()
        first = tree(self.top)
        write(self.top / 'display.ini', b'window = windowed\n')   # (the old file edited: not copied again)
        (self.top / 'config' / 'online_name.txt').unlink()          # (nor a file gone from the folder)
        got, err = self.paths()
        after = tree(self.top)
        self.assertEqual(after['config/display.ini'], WS_FILES['display.ini'])
        self.assertNotIn('config/online_name.txt', after)
        self.assertEqual(after['config/version.ini'], first['config/version.ini'])
        self.assertNotIn('[config]', err)
        write(self.top / 'config' / 'online_server.txt', b'off\n')   # (one of its own: that one from now on)
        got, _ = self.paths()
        self.assertEqual(got['online_server'], self.config('online_server.txt'))

    def sr_install(self, folder='build'):
        write(self.top / folder / 'config' / 'controllers.ini', SR_PADS)
        write(self.top / folder / 'config' / 'graphics.ini', SR_GRAPHICS)
        write(self.top / folder / 'config' / 'dlc.txt', b'made up\n')
        (self.top / folder / 'work').mkdir(parents=True, exist_ok=True)

    def test_ssbu_recomp_layout(self):
        self.sr_install()
        before = tree(self.top)
        got, err = self.paths()
        after = tree(self.top)
        for name in before:
            self.assertEqual(after[name], before[name], name)
        self.assertEqual(after['config/ssbu-recomp/controllers.ini'], SR_PADS)
        self.assertEqual(after['config/ssbu-recomp/graphics.ini'], SR_GRAPHICS)
        self.assertNotIn('config/ssbu-recomp/dlc.txt', after)   # (not a setting: the DLC the installer found)
        self.assertIn(f'ssbu-recomp = build{SEP}config: controllers.ini, graphics.ini', self.version())
        # controllers.ini in this port's terms, read as ssbu-recomp's was
        self.assertEqual(self.dump(self.top / 'config' / 'controllers.ini'), SR_PADS_READ_DEFAULTS)
        pads = after['config/controllers.ini'].decode()
        self.assertTrue(pads.startswith('; controllers.ini: how pads become players.'), pads[:80])
        for kept in ('outer_zone = 0.95', 'ignore = 0300000000000000aaaa000000000000', 'ignore = 0300000000000000bbbb000000000000',
                     'A = Z', 'name = Made-up pad', '; map.A = south, button7', 'map.LSTICK_LEFT = dpleft', 'axis.LX = leftx',
                     '[type joystick]'):
            self.assertIn(kept, pads)
        self.assertNotIn('\nmap.A =', pads)   # (carried over: A = south)
        # graphics.ini's keys in display.ini; the name and direct connections in their files
        display = after['config/display.ini'].decode().replace('\r\n', '\n')
        for line in ('window = borderless', 'width = 1600', 'height = 900', 'pacing = display', 'render_scale = 0.75', 'overlay = 0'):
            self.assertIn('\n' + line + '\n', display)
        self.assertNotIn('gpu =', display)    # (a number in ssbu-recomp's list of devices)
        self.assertNotIn('lobby.example', display)
        self.assertEqual(after['config/online_name.txt'].decode().strip(), 'Made Up Name')
        self.assertEqual(after['config/online_direct.txt'].decode().strip(), 'off')
        self.assertNotIn('config/online_server.txt', after)   # (ssbu-recomp's lobby talks another protocol)
        self.assertEqual(got['online_server'], 'online_server.txt')
        # and once only
        self.paths()
        self.assertEqual(tree(self.top), after)

    def test_ssbu_recomp_elsewhere(self):
        """Its build folder where its build_location.txt says, or in the staged tree's ssbu-recomp/."""
        other = self.top.parent / 'Big Disk' / 'SSBU build'
        write(other / 'config' / 'graphics.ini', SR_GRAPHICS)
        write(self.top / 'build_location.txt', f'"{other}"\r\n'.encode())
        self.paths()
        self.assertIn(f'ssbu-recomp = {other}{SEP}config: graphics.ini', self.version())
        self.assertIn('width = 1600', (self.top / 'config' / 'display.ini').read_text())
        self.assertFalse((self.top / 'config' / 'controllers.ini').exists())   # (none there: written with the defaults later)

        shutil.rmtree(self.top / 'config')
        (self.top / 'build_location.txt').unlink()
        self.sr_install('ssbu-recomp/build')
        self.paths()
        self.assertIn(f'ssbu-recomp = ssbu-recomp{SEP}build{SEP}config: controllers.ini, graphics.ini', self.version())
        self.assertEqual(self.dump(self.top / 'config' / 'controllers.ini'), SR_PADS_READ_DEFAULTS)

    def test_both_layouts(self):
        """WindowSmash's files are the base; ssbu-recomp's fill in what it has not."""
        self.sr_install()
        for name in ('display.ini', 'controllers.ini'):
            write(self.top / name, WS_FILES[name])
        got, err = self.paths()
        after = tree(self.top)
        self.assertEqual(after['config/display.ini'], WS_FILES['display.ini'])
        self.assertEqual(after['config/controllers.ini'], WS_FILES['controllers.ini'])
        self.assertEqual(after['config/ssbu-recomp/controllers.ini'], SR_PADS)   # (kept)
        self.assertIn('is the one in use; ssbu-recomp', err)
        self.assertEqual(after['config/online_name.txt'].decode().strip(), 'Made Up Name')   # (WindowSmash had none)
        version = self.version()
        self.assertIn('windowsmash = display.ini, controllers.ini\n', version.replace('\r\n', '\n'))
        self.assertIn('ssbu-recomp = build', version)

    def test_newer_folder(self):
        write(self.top / 'config' / 'version.ini', b'version = 2\n')
        write(self.top / 'display.ini', WS_FILES['display.ini'])
        got, err = self.paths()
        self.assertEqual(got['display'], self.config('display.ini'))
        self.assertIn('newer than this program', err)
        self.assertFalse((self.top / 'config' / 'display.ini').exists())

    def test_folder_that_cannot_be_made(self):
        write(self.top / 'display.ini', WS_FILES['display.ini'])
        write(self.top / 'config', b'a file where the folder would go\n')
        got, err = self.paths()
        self.assertEqual(got['display'], 'display.ini')   # (the old layout, this start)
        self.assertEqual(got['online_name'], 'online_name.txt')
        self.assertIn('the settings files stay where they were', err)

    def test_half_done_is_taken_back(self):
        """A start that cannot finish removes what it copied, so the next one copies the files as they are then."""
        for name, data in WS_FILES.items():
            write(self.top / name, data)
        self.sr_install()
        write(self.top / 'config' / 'ssbu-recomp', b'a file where its folder would go\n')
        got, err = self.paths()
        self.assertEqual(got['controllers'], 'controllers.ini')
        self.assertEqual(sorted(p for p in tree(self.top) if p.startswith('config/')), ['config/ssbu-recomp'])
        (self.top / 'config' / 'ssbu-recomp').unlink()
        got, _ = self.paths()
        self.assertEqual(got['controllers'], self.config('controllers.ini'))
        self.assertEqual((self.top / 'config' / 'controllers.ini').read_bytes(), WS_FILES['controllers.ini'])

    @unittest.skipIf(os.name == 'nt' or (hasattr(os, 'geteuid') and os.geteuid() == 0),
                     'a read-only folder: POSIX permissions, as another user than root')
    def test_read_only_folder(self):
        for name, data in WS_FILES.items():
            write(self.top / name, data)
        before = tree(self.top)
        self.top.chmod(0o555)
        self.addCleanup(self.top.chmod, 0o755)
        got, err = self.paths()
        self.assertEqual(got['display'], 'display.ini')
        self.assertEqual(tree(self.top), before)
        self.top.chmod(0o755)
        (self.top / 'config').mkdir()   # (a folder of its own that cannot be written)
        (self.top / 'config').chmod(0o555)
        self.addCleanup((self.top / 'config').chmod, 0o755)
        got, err = self.paths()
        self.assertEqual(got['display'], 'display.ini')
        self.assertEqual(list((self.top / 'config').iterdir()), [])


class ControllersTests(ConfigTestCase):
    def test_this_ports_schema(self):
        write(self.top / 'c.ini', b'[general]\nkeyboard_player = 0\nbackground = 0\nrumble = 0\n[players]\n4 = DualSense\n'
                                  b'[device default]\nlayout = labels\ndeadzone = 10\ntrigger_threshold = 30\nZR = rightshoulder, misc1\n'
                                  b'[device ps5]\nrumble = 70\nstyle = pro\n')
        self.assertEqual(self.dump(self.top / 'c.ini'), 'keyboard_player=0 background=0 rumble=0\nplayer 4 = DualSense\n'
                         '[device default] layout=1 style=-1 deadzone=10 rumble=-1 trigger_threshold=30\n  ZR = rightshoulder, misc1\n'
                         '[device ps5] layout=-1 style=1 deadzone=-1 rumble=70 trigger_threshold=-1\n')

    def test_ssbu_recomps_schema(self):
        write(self.top / 'c.ini', SR_PADS)
        r = self.run_test('dump', str(self.top / 'c.ini'))
        self.assertEqual(r.stdout, SR_PADS_READ)
        self.assertNotIn('unknown', r.stderr)   # (what has nothing to stand for here is passed over quietly)
        write(self.top / 'd.ini', b'[general]\nrumble = off\n[keyboard]\nplayer = none\n[type switch]\nlayout = label\n')
        self.assertEqual(self.dump(self.top / 'd.ini'), 'keyboard_player=0 background=1 rumble=0\n'
                         '[device default] layout=-1 style=-1 deadzone=-1 rumble=-1 trigger_threshold=-1\n'
                         + ''.join(f'[device {t}] layout=1 style=-1 deadzone=-1 rumble=-1 trigger_threshold=-1\n'
                                   for t in ('switchpro', 'joyconleft', 'joyconright', 'joyconpair')))

    def test_both_schemas_this_ports_keys_win(self):
        write(self.top / 'c.ini', f'[device {GUID}]\ndeadzone = 12\nrumble = 40\nB = west\n[keyboard]\nplayer = 4\n'
                                  f'[general]\nkeyboard_player = 2\n[device {GUID}]\ndeadzone = 0.3\nrumble_strength = 0.9\nmap.B = east\n'
                                  f'map.X = north\n'.encode())
        self.assertEqual(self.dump(self.top / 'c.ini'), 'keyboard_player=2 background=1 rumble=1\n'
                         '[device default] layout=-1 style=-1 deadzone=-1 rumble=-1 trigger_threshold=-1\n'
                         f'[device {GUID}] layout=-1 style=-1 deadzone=12 rumble=40 trigger_threshold=-1\n  B = west\n  X = north\n')

    def test_conversion_round_trip(self):
        """Rewritten, the file reads as it did; the settings menu's changes then go in as in any of this port's files."""
        write(self.top / 'sr.ini', SR_PADS)
        self.run_test('convert', str(self.top / 'sr.ini'), str(self.top / 'c.ini'))
        self.assertFalse((self.top / 'c.ini.new').exists())
        self.assertEqual(self.dump(self.top / 'c.ini'), SR_PADS_READ_DEFAULTS)
        self.assertEqual(self.dump(self.top / 'sr.ini'), SR_PADS_READ)   # (its own file untouched)
        self.assertEqual((self.top / 'sr.ini').read_bytes(), SR_PADS)
        c = str(self.top / 'c.ini')
        self.run_test('set', c, f'device {GUID}', 'deadzone', '25')   # (as the menu: the pad's dead zone, its player, the keyboard)
        self.run_test('set', c, 'players', '2', '-')
        self.run_test('set', c, 'general', 'keyboard_player', '1')
        self.run_test('set', c, f'device {GUID}', 'A', '-')            # (the pad's buttons back to the defaults)
        got = self.dump(self.top / 'c.ini')
        self.assertIn(f'[device {GUID}] layout=1 style=-1 deadzone=25 rumble=0 trigger_threshold=-1\n  LSTICK', got)
        self.assertNotIn('  A = ', got)   # (nothing of ssbu-recomp's brings it back)
        self.assertNotIn('player 2 =', got)
        self.assertTrue(got.startswith('keyboard_player=1 '), got)
        text = (self.top / 'c.ini').read_text()
        for kept in ('outer_zone = 0.95', 'A = Z', 'axis.LX = leftx', '; map.A = south, button7'):   # (kept through the changes)
            self.assertIn(kept, text)
        self.run_test('convert', str(self.top / 'c.ini'), str(self.top / 'again.ini'))   # (this port's file: as it reads)
        self.assertEqual(self.dump(self.top / 'again.ini'), got)

    def test_unknown_keys_kept_on_rewrite(self):
        write(self.top / 'c.ini', b'[general]\r\nkeyboard_player = 1\r\nmade_up = 7   ; a key of a later version\r\n'
                                  b'[device default]\r\nfuture = x\r\n')
        r = self.run_test('dump', str(self.top / 'c.ini'))
        self.assertIn("unknown setting 'made_up'", r.stderr)
        self.run_test('set', str(self.top / 'c.ini'), 'general', 'keyboard_player', '2')
        self.assertEqual((self.top / 'c.ini').read_bytes(), b'[general]\r\nkeyboard_player = 2\r\nmade_up = 7   ; a key of a later version\r\n'
                                                            b'[device default]\r\nfuture = x\r\n')


if __name__ == '__main__':
    unittest.main()
