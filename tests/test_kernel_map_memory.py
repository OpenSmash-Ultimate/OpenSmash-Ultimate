"""The native kernel's memory mappings (runtime/kernel.c's svcMapMemory and svcUnmapMemory) through
tools/dev/maptest.c, with no game: a mapping's pages at its destination and back in its source after
the unmap, the destination freed and the renderer told, mappings side by side and from neighbouring
sources, more mappings than slots, sources in the alias area and in a module, the status line's
counts; with SSBU_MAP_GUARD=1 the source also inaccessible while mapped (on macOS its 16 KiB pages
wholly inside it), and only where the switch says, and kernel_read64 refusing it; without it (unset,
or another value) accessible, as before. Built with the platform's compiler: clang-cl on Windows (as
tests/test_clock.py builds clocktest.c), clang with the platform layer elsewhere (runtime/linux/win32.c, with runtime/macos/'s
memory and platform parts on macOS; the Windows types from runtime/macos/include on both). Skipped
where there is no such compiler, and on hosts the platform layer is not written for (Linux on
x86-64 and macOS on Apple silicon only, as the game)."""
from pathlib import Path
import os
import platform
import shutil
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


def build(folder):
    src = str(ROOT / 'tools' / 'dev' / 'maptest.c')
    if os.name == 'nt':
        exe = os.path.join(folder, 'maptest.exe')
        # (as cc.link links the game: kernel_init reserves the guest's areas at fixed addresses, and with
        # high-entropy ASLR Windows can put a heap or a thread stack there first)
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, src, '/Fe' + exe, '/Fo' + os.path.join(folder, 'maptest.obj'),
               '/link', *cc.LINK_OPTIONS, '/HIGHENTROPYVA:NO']
    else:
        exe = os.path.join(folder, 'maptest')
        layer = ['runtime/linux/win32.c']   # (the platform layer)
        if sys.platform == 'darwin':
            layer += ['runtime/macos/memory.c', 'runtime/macos/platform.c']
        else:
            layer += ['-lpthread']
        cmd = ['clang', '-O1', '-w', '-I', 'runtime/macos/include', '-I', 'runtime/linux/include', '-I', 'runtime', src, *layer, '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError('maptest.c did not build:\n' + r.stdout + r.stderr)
    return exe


@unittest.skipUnless(HAVE_CLANG, 'needs clang on x86-64 Linux or an Apple silicon Mac, or clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class KernelMapMemoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='maptest ')
        cls.exe = build(cls.dir)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def maptest(self, mode=None, switch=None):
        env = {k: v for k, v in os.environ.items() if k != 'SSBU_MAP_GUARD'}
        if switch is not None:
            env['SSBU_MAP_GUARD'] = switch
        r = subprocess.run([self.exe, *([mode] if mode else [])], capture_output=True, text=True, env=env, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'\d+ checks, 0 failed')
        return r.stdout + r.stderr

    def test_copies(self):
        out = self.maptest()
        self.assertRegex(out, r'\[status\] memory mappings: 3016 made \(\d+\.\d MiB in all\), 0 mapped now \(at most 2 at once, of 1024\); \d+\.\d ms copying\n')
        self.assertNotIn('SSBU_MAP_GUARD', out)

    def test_guard(self):
        out = self.maptest('guard', '1')
        self.assertEqual(out.count('[kernel] a memory mapping\'s source is inaccessible while it is mapped (SSBU_MAP_GUARD=1)'), 1, out)
        self.assertIn('; 3010 sources made inaccessible while mapped (SSBU_MAP_GUARD)\n', out)

    def test_other_values_leave_the_source_accessible(self):
        for value in ('0', 'yes'):
            self.assertNotIn('SSBU_MAP_GUARD', self.maptest(None, value))


if __name__ == '__main__':
    unittest.main()
