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

    def test_names(self):
        out, _ = self.run_test('parse', 'vulkan', 'parse', 'Vulkan', 'parse', 'vk', 'parse', 'd3d11', 'parse', 'DXVK', 'parse', 'metal',
                               'parse', 'auto', 'parse', 'opengl', 'parse', '')
        self.assertEqual(out, ['parse vulkan -> 1 vulkan', 'parse Vulkan -> 1 vulkan', 'parse vk -> 1 vulkan', 'parse d3d11 -> 0 d3d11',
                               'parse DXVK -> 0 d3d11', 'parse metal -> 0 d3d11', 'parse auto -> 0 d3d11', 'parse opengl -> -1 -',
                               'parse  -> -1 -'])

    def test_default_is_unchanged(self):
        """No renderer key, or the default's name: Direct3D 11, and the start says nothing."""
        for ini in ('window = windowed\n', 'renderer = d3d11\n', 'renderer = auto   ; the default\n'):
            out, err = self.run_test('chosen', 'start', ini=ini)
            self.assertEqual(out, ['chosen d3d11', 'start d3d11'], ini)
            self.assertEqual([l for l in err if 'renderer' in l], [], ini)

    def test_vulkan_asked(self):
        """Vulkan checked (whatever this PC has), one line about it, and Direct3D 11 draws."""
        out, err = self.run_test('chosen', 'start', ini='renderer = vulkan\n')
        self.assertEqual(out[0], 'chosen vulkan')
        self.assertEqual(out[1], 'start d3d11')   # (the Vulkan back end does not draw yet: Direct3D 11 does)
        said = [l for l in err if l.startswith('[gfx] renderer = vulkan')]
        self.assertEqual(len(said), 1, err)
        self.assertTrue(said[0].endswith('Direct3D 11 draws'), said)
        self.assertTrue('Vulkan works here: ' in said[0] or 'Vulkan cannot be used here: ' in said[0], said)

    def test_vulkan_without_loader(self):
        out, err = self.run_test('start', ini='renderer = vulkan\n', SSBU_VULKAN_LOADER='no-such-vulkan-loader.dll')
        self.assertEqual(out, ['start d3d11'])
        self.assertIn('[gfx] renderer = vulkan: Vulkan cannot be used here: no Vulkan loader (no-such-vulkan-loader.dll): the graphics '
                      'driver installs it. Direct3D 11 draws', err)

    def test_environment_wins(self):
        out, _ = self.run_test('chosen', ini='renderer = vulkan\n', renderer='d3d11')
        self.assertEqual(out, ['chosen d3d11'])
        out, _ = self.run_test('chosen', ini='renderer = d3d11\n', renderer='vulkan')
        self.assertEqual(out, ['chosen vulkan'])

    def test_unknown_value(self):
        out, err = self.run_test('chosen', ini='renderer = opengl\n')
        self.assertEqual(out, ['chosen d3d11'])
        self.assertTrue(any("renderer is d3d11 or vulkan, not 'opengl': d3d11 it is" in l for l in err), err)

    def test_default_file(self):
        """The display.ini the game writes names the key, both values, and the default."""
        out, _ = self.run_test('default')
        text = '\n'.join(out)
        self.assertIn('; renderer = d3d11 | vulkan', text)
        self.assertIn('\nrenderer = d3d11', text)
        # (and it reads back as the default: the file is written when there is none)
        d = Path(tempfile.mkdtemp(dir=self.folder))
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SSBU_')}
        r = subprocess.run([self.exe, 'chosen'], cwd=d, env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(r.stdout.split(), ['chosen', 'd3d11'])
        self.assertIn('renderer = d3d11', (d / 'display.ini').read_text())


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
