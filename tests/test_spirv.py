"""DXBC to SPIR-V for the Vulkan back end in development (runtime/gpu_spirv.c, through vkd3d-shader;
docs/TECHNICAL.md, "A second back end: Vulkan") through tools/dev/spirvtest.c: the binding map
(gpu_spirv.h) on a vertex, a pixel and a compute program with every kind of register gpu_shader.c's
HLSL uses; and the build without vkd3d-shader, which says it has none. tools/vkd3d_lib.py's source
is the Linux build's, pinned by SHA-256 in tools/linux/install_linux.py. Windows only; the
translation tests are skipped where deps/vkd3d has no library (tools/vkd3d_lib.py builds it)."""
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
import vkd3d_lib  # noqa: E402

HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)


def build(folder, flags, libs):
    exe = os.path.join(folder, 'spirvtest.exe')
    cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *flags, 'tools/dev/spirvtest.c', '/Fe' + exe,
           '/Fo' + folder + os.sep, '/link', *cc.LINK_OPTIONS, *libs]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError('spirvtest.c did not build:\n' + r.stdout + r.stderr)
    return exe


class Pins(unittest.TestCase):
    def test_same_source_as_linux(self):
        """vkd3d 2.1 and SPIRV-Headers, from install_linux.py's PARTS (URL and SHA-256 there)."""
        parts, _ = vkd3d_lib.parts()
        self.assertEqual([p[0] for p in parts], ['vkd3d 2.1', 'SPIRV-Headers'])
        for what, url, sha, *_ in parts:
            self.assertRegex(sha, r'^[0-9a-f]{64}$', what)
            self.assertTrue(url.startswith('https://'), what)


@unittest.skipUnless(HAVE_CLANG, 'Windows with clang-cl only (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class WithoutVkd3d(unittest.TestCase):
    def test_says_it_has_none(self):
        folder = tempfile.mkdtemp(prefix='spirvtest ')
        try:
            flags = [f for f in cc.CFLAGS if f != '/DSSBU_HAVE_VKD3D']
            exe = build(folder, flags, [])
            r = subprocess.run([exe, '-self'], capture_output=True, text=True, timeout=60)
            self.assertEqual((r.returncode, r.stdout.strip()), (3, 'no vkd3d-shader in this build'))
        finally:
            shutil.rmtree(folder, ignore_errors=True)


@unittest.skipUnless(HAVE_CLANG and vkd3d_lib.have(), 'Windows with deps/vkd3d\'s library only (tools/vkd3d_lib.py)')
class BindingMap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.mkdtemp(prefix='spirvtest ')
        cls.exe = build(cls.folder, cc.CFLAGS, [vkd3d_lib.LIB])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.folder, ignore_errors=True)

    def test_built_with_vkd3d(self):
        self.assertTrue(cc.HAVE_VKD3D)
        self.assertIn('/DSSBU_HAVE_VKD3D', cc.CFLAGS)
        self.assertIn(vkd3d_lib.LIB, cc.EXTRA_LIBS)

    def test_self(self):
        """b0 -> 0, t<n> -> 32 + n, s<n> -> 160 + n, u<n> -> 192 + n; vertex set 0, pixel set 1, compute set 0."""
        r = subprocess.run([self.exe, '-self'], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        lines = r.stdout.splitlines()
        self.assertEqual(lines[-1], 'self ok')
        got = {tuple(l.split()[i] for i in (0, 2, 4)) for l in lines[:-1]}
        want = {('vs', '0', '0'), ('vs', '0', str(32 + 3)), ('vs', '0', str(32 + 18)), ('vs', '0', str(32 + 97)),
                ('ps', '1', '0'), ('ps', '1', str(32 + 64)), ('ps', '1', '160'), ('ps', '1', str(32 + 1)),
                ('cs', '0', str(32 + 0)), ('cs', '0', '192'), ('cs', '0', '194')}
        self.assertEqual(got, want)


if __name__ == '__main__':
    unittest.main()
