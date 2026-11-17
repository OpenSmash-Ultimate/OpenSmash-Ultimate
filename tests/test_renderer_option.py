"""display.ini's renderer (runtime/renderer.h, runtime/gpu_display.c, runtime/gpu_renderer.c) through
tools/dev/renderertest.c: the names, display.ini's key and SSBU_RENDERER, the default file's text,
and what the renderer's start does with the choice: with the default nothing at all (the
Direct3D 11 path as before), with vulkan a line in the log, and the default still draws while
the Vulkan back end cannot. Windows only (the driver includes gpu_display.c, whose window code is
Windows'); skipped where there is no clang-cl. The Display page's Renderer row is
tests/test_lobby_menu.py's (Pages.test_renderer_row)."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)


@unittest.skipUnless(HAVE_CLANG, 'Windows with clang-cl only (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class RendererOption(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.mkdtemp(prefix='renderertest ')
        cls.exe = os.path.join(cls.folder, 'renderertest.exe')
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, 'tools/dev/renderertest.c', '/Fe' + cls.exe,
               '/Fo' + cls.folder + os.sep, '/link', *cc.LINK_OPTIONS]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('renderertest.c did not build:\n' + r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.folder, ignore_errors=True)

    def run_test(self, *args, ini=None, renderer=None, **more):
        """(stdout lines, stderr lines) of renderertest with that display.ini (None: none), SSBU_RENDERER, and more variables."""
        d = Path(tempfile.mkdtemp(dir=self.folder))
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SSBU_')}
        env['SSBU_DISPLAY'] = str(d / 'display.ini')
        env.update(more)
        if ini is not None:
            (d / 'display.ini').write_text(ini)
        if renderer is not None:
            env['SSBU_RENDERER'] = renderer
        r = subprocess.run([self.exe, *args], cwd=d, env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout.splitlines(), r.stderr.splitlines()

    def test_menu_sees_windowed(self):
        """Windowed shows as windowed, so the menu steps on from it; F11's full-screen mode stays."""
        out, _ = self.run_test('shown', 'mode', '2', 'shown', 'mode', '0', 'shown', ini='window = borderless\n')
        self.assertEqual(out, ['shown 1 1', 'shown 2 2', 'shown 0 2'])

    def test_names(self):
        out, _ = self.run_test('parse', 'vulkan', 'parse', 'Vulkan', 'parse', 'vk', 'parse', 'd3d11', 'parse', 'DXVK', 'parse', 'metal',
                               'parse', 'auto', 'parse', 'direct3d11', 'parse', 'DX11', 'parse', 'opengl', 'parse', '')
        # (d3d11, the value every default display.ini before auto was written with, is auto; direct3d11 is Direct3D 11)
        self.assertEqual(out, ['parse vulkan -> 1 vulkan', 'parse Vulkan -> 1 vulkan', 'parse vk -> 1 vulkan', 'parse d3d11 -> 0 auto',
                               'parse DXVK -> 0 auto', 'parse metal -> 0 auto', 'parse auto -> 0 auto', 'parse direct3d11 -> 2 direct3d11',
                               'parse DX11 -> 2 direct3d11', 'parse opengl -> -1 -', 'parse  -> -1 -'])

    def test_every_display_value_the_menu_writes_reads_back(self):
        """Each Display row's values, written as runtime/gpu_menu.c's change() writes them, are read back by the game:
        a setting changed in the menu is still there at the next start. No warning about any of them."""
        rows = [  # (written to display.ini, what the game then has)
            ('window = windowed\nwidth = 2580\nheight = 1080\n', 'window 0 size 2580x1080'),
            ('window = borderless\n', 'window 1 '), ('window = fullscreen\n', 'window 2 '),
            ('pacing = clock\n', 'pacing 0 '), ('pacing = vrr\n', 'pacing 1 '), ('pacing = display\n', 'pacing 2 '),
            ('latency = 1\n', 'latency 1 '), ('latency = 3\n', 'latency 3 '),
            ('hide_cursor = 0\n', 'cursor 0 '), ('hide_cursor = 1\n', 'cursor 1 '),
            ('overlay = 0\n', 'overlay 0 '), ('overlay = 1\n', 'overlay 1 '),
            ('render_scale = 1\n', 'scale 1 '), ('render_scale = auto\n', 'scale 0 '), ('render_scale = 1.5\n', 'scale 1.5 '),
            ('render_scale = 2\n', 'scale 2 '), ('render_scale = 3\n', 'scale 3 '),
            ('renderer = auto\n', 'renderer auto '), ('renderer = direct3d11\n', 'renderer direct3d11 '), ('renderer = vulkan\n', 'renderer vulkan '),
            ('aspect = 16:9\n', 'aspect 16:9 '), ('aspect = auto\n', 'aspect auto '), ('aspect = 21:9\n', 'aspect 21:9 '), ('aspect = 32:9\n', 'aspect 32:9 '),
            ('view = wide\n', 'view 0 '), ('view = zoomed\n', 'view 1 '), ('view = hybrid\n', 'view 2 '),
            ('experimental_ultrawide_online = 0\n', 'uw_online 0 '), ('experimental_ultrawide_online = 1\n', 'uw_online 1 '),
            ('timer = right\n', 'timer 0 '), ('timer = center\n', 'timer 1 '), ('timer = left\n', 'timer 2 '), ('timer = hidden\n', 'timer 3 '),
            ('gpu = nvidia\n', 'gpu nvidia'), ('language = en_us\nvolume = 80\nrunahead = 0\n', 'settings '),
        ]
        for ini, want in rows:
            out, err = self.run_test('settings', ini=ini)
            self.assertIn(want, out[0] + ' ', ini)
            self.assertEqual([l for l in err if l.startswith('[display]') and 'not' in l or 'unknown' in l], [], ini)

    def test_shape_takes_the_monitors_in_its_range(self):
        """21:9 and 43:18 are the monitor's shape on any 21:9-class monitor (2.22-2.45), 32:9 on a 32:9-class one;
        outside the range the setting's own shape (bars); never past 32:9, never below 16:9."""
        out, _ = self.run_test('shape', '21:9', '2.3889', 'shape', '43:18', '2.3704', 'shape', '21:9', '1.7778',
                               'shape', '32:9', '2.3889', 'shape', '32:9', '3.6', 'shape', 'auto', '2.3889', 'shape', '16:9', '2.3889',
                               'shape', '21:9', '3.7')
        self.assertEqual(out, ['shape 21:9 2.3889 -> 2.389', 'shape 43:18 2.3704 -> 2.370', 'shape 21:9 1.7778 -> 2.333',
                               'shape 32:9 2.3889 -> 3.556', 'shape 32:9 3.6 -> 3.556', 'shape auto 2.3889 -> 2.389',
                               'shape 16:9 2.3889 -> 1.778', 'shape 21:9 3.7 -> 2.333'])

    def test_default_is_unchanged(self):
        """No renderer key, or the default's name: auto (gpu_backend.c's choice, not made here: Direct3D 11 starts),
        and the start says nothing."""
        for ini in ('window = windowed\n', 'renderer = auto   ; the default\n'):
            out, err = self.run_test('chosen', 'start', ini=ini)
            self.assertEqual(out, ['chosen auto', 'start auto'], ini)
            self.assertEqual([l for l in err if 'renderer' in l], [], ini)

    def test_vulkan_asked(self):
        """Vulkan checked (whatever this PC has), one line about it, and Direct3D 11 draws."""
        out, err = self.run_test('chosen', 'start', ini='renderer = vulkan\n')
        self.assertEqual(out[0], 'chosen vulkan')
        self.assertEqual(out[1], 'start auto')   # (gfx_renderer_start alone: the default, Direct3D 11, draws)
        said = [l for l in err if l.startswith('[gfx] renderer = vulkan')]
        self.assertEqual(len(said), 1, err)
        self.assertTrue(said[0].endswith('Direct3D 11 draws'), said)
        self.assertTrue('Vulkan works here: ' in said[0] or 'Vulkan cannot be used here: ' in said[0], said)

    def test_vulkan_without_loader(self):
        out, err = self.run_test('start', ini='renderer = vulkan\n', SSBU_VULKAN_LOADER='no-such-vulkan-loader.dll')
        self.assertEqual(out, ['start auto'])
        self.assertIn('[gfx] renderer = vulkan: Vulkan cannot be used here: no Vulkan loader (no-such-vulkan-loader.dll): the graphics '
                      'driver installs it. Direct3D 11 draws', err)

    def test_environment_wins(self):
        out, _ = self.run_test('chosen', ini='renderer = vulkan\n', renderer='direct3d11')
        self.assertEqual(out, ['chosen direct3d11'])
        out, _ = self.run_test('chosen', ini='renderer = d3d11\n', renderer='vulkan')
        self.assertEqual(out, ['chosen vulkan'])

    def test_unknown_value(self):
        out, err = self.run_test('chosen', ini='renderer = opengl\n')
        self.assertEqual(out, ['chosen auto'])
        self.assertTrue(any("renderer is auto, direct3d11 or vulkan, not 'opengl': auto it is" in l for l in err), err)

    def test_default_file(self):
        """The display.ini the game writes names the key, both values, and the default."""
        out, _ = self.run_test('default')
        text = '\n'.join(out)
        self.assertIn('; renderer = auto | direct3d11 | vulkan', text)
        self.assertIn('\nrenderer = auto', text)
        # (and it reads back as the default: the file is written when there is none)
        d = Path(tempfile.mkdtemp(dir=self.folder))
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SSBU_')}
        r = subprocess.run([self.exe, 'chosen'], cwd=d, env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.stdout.split(), ['chosen', 'auto'])
        self.assertIn('renderer = auto', (d / 'display.ini').read_text())


@unittest.skipUnless(HAVE_CLANG, 'Windows with clang-cl only (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class VulkanProbe(unittest.TestCase):
    """runtime/gpu_vulkan.c's check through tools/dev/vkprobetest.c, built as the game builds the runtime
    (with the Vulkan headers). On a PC without Vulkan the check must say so, never fail otherwise."""
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.mkdtemp(prefix='vkprobetest ')
        cls.exe = os.path.join(cls.folder, 'vkprobetest.exe')
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, 'tools/dev/vkprobetest.c', '/Fe' + cls.exe,
               '/Fo' + cls.folder + os.sep, '/link', *cc.LINK_OPTIONS]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('vkprobetest.c did not build:\n' + r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.folder, ignore_errors=True)

    def probe(self, *args, **env_more):
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SSBU_')}
        env.update(env_more)
        r = subprocess.run([self.exe, *args], cwd=self.folder, env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        line = r.stdout.splitlines()[-1]
        self.assertRegex(line, r'^probe [01] .+')
        return int(line.split()[1]), line[len('probe 0 '):], r.stderr

    def test_built_with_vulkan(self):
        self.assertTrue(cc.HAVE_VULKAN, 'tools/cc.py did not find third_party/vulkan-headers')
        ok, what, _ = self.probe()
        self.assertNotEqual(what, 'this program was built without Vulkan')

    def test_no_loader(self):
        ok, what, _ = self.probe(SSBU_VULKAN_LOADER='no-such-vulkan-loader.dll')
        self.assertEqual((ok, what), (0, 'no Vulkan loader (no-such-vulkan-loader.dll): the graphics driver installs it'))

    def test_this_pc(self):
        """Whatever this PC has: with a card, the cards listed and one device made, with a window too."""
        ok, what, err = self.probe('--window')
        if not ok:
            self.skipTest('no usable Vulkan here: ' + what)
        self.assertTrue(any(l.startswith('[vulkan] loader ') and 'graphics cards: ' in l for l in err.splitlines()), err)
        self.assertRegex(what, r'^.+ \((discrete|integrated|virtual|software|other), Vulkan \d+\.\d+\.\d+, driver [\d.]+\): a device made, '
                               r'it shows pictures in the game\'s window')
        _, named, err = self.probe('--gpu', 'no-card-is-called-this')
        self.assertIn("[vulkan] display.ini: no graphics card's name contains 'no-card-is-called-this': using the fastest", err)
        self.assertEqual(named.split(' (')[0], what.split(' (')[0])   # (the fastest: the one auto picks)


class VulkanHeaders(unittest.TestCase):
    """third_party/vulkan-headers: every file as its SHA256SUMS lists it (Khronos' Vulkan-Headers v1.4.365,
    verbatim), and nothing else there."""
    def test_files(self):
        import hashlib
        top = ROOT / 'third_party' / 'vulkan-headers'
        listed = {}
        for line in (top / 'SHA256SUMS').read_text().splitlines():
            digest, name = line.split('  ', 1)
            listed[name] = digest
        found = {p.relative_to(top).as_posix() for p in top.rglob('*') if p.is_file() and p.name != 'SHA256SUMS'}
        self.assertEqual(found, set(listed))
        for name, digest in listed.items():
            self.assertEqual(hashlib.sha256((top / name).read_bytes()).hexdigest(), digest, name)
        self.assertIn('#define VK_HEADER_VERSION 365', (top / 'include' / 'vulkan' / 'vulkan_core.h').read_text())


if __name__ == '__main__':
    unittest.main()
