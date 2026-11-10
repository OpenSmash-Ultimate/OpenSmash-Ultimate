"""The Vulkan back end's glslang path (docs/TECHNICAL.md, "A second back end: Vulkan"): ssbu_glslang.dll
made by tools/glslang_lib.py, and tools/dev/glslangtest.c running a sample of Prepare Shaders' pack
(dump/aot/programs.pack) through it. Windows with deps/glslang only; the pack part needs a game folder."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

if os.name == 'nt':
    import cc  # noqa: E402
    import glslang_lib  # noqa: E402
    HAVE = glslang_lib.have() and os.path.exists(cc.CLANG)
else:
    HAVE = False
PACK = ROOT / 'dump' / 'aot' / 'programs.pack'


@unittest.skipUnless(HAVE, "Windows with deps/glslang (tools/glslang_lib.py) only")
class GlslangTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.mkdtemp(prefix='glslangtest ')
        cls.dll = os.path.join(cls.folder, glslang_lib.DLL_NAME)
        if not glslang_lib.build(cls.dll, log=lambda *_: None):
            raise AssertionError('ssbu_glslang.dll did not build')
        cls.exe = os.path.join(cls.folder, 'glslangtest.exe')
        r = subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, 'tools/dev/glslangtest.c', 'runtime/gpu_shader.c',
                            '/Fe' + cls.exe, '/Fo' + cls.folder + os.sep, '/link', *cc.LINK_OPTIONS, 'd3dcompiler.lib'],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            raise AssertionError('glslangtest.c did not build:\n' + r.stdout + r.stderr)

    def test_dll_exports(self):
        import ctypes
        lib = ctypes.WinDLL(self.dll)
        self.assertTrue(hasattr(lib, 'ssbu_hlsl_to_spirv'))
        self.assertTrue(hasattr(lib, 'ssbu_spirv_free'))

    @unittest.skipUnless(PACK.exists(), 'no dump/aot/programs.pack (tools/shader_aot.py --dry makes it)')
    def test_pack_sample(self):
        r = subprocess.run([self.exe, str(PACK), '-every', '500'], capture_output=True, text=True, timeout=1200)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr[-3000:])
        self.assertIn('0 failed', r.stdout)


if __name__ == '__main__':
    unittest.main()
