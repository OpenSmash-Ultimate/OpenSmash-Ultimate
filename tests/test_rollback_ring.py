"""The rollback ring (runtime/hle_rollback_ring.c) and the write watch the kernel shares between it and
the renderer (runtime/kernel.c, kernel_watch.h), through tools/dev/ringtest.c, on made-up memory with
no game: with SSBU_ROLLBACK_RING=4, frames saved with only the pages that changed, the newest and older
ones put back byte for byte, the ring's end passed and its oldest and newest put back, the renderer and
the ring each told of every write whichever took the system's bits first, memory unmapped and mapped
again between saves, SSBU_MAP_GUARD left off; without the switch, nothing registered and the
renderer's questions the system's own, and the switch's values. Built with the platform's compiler as
tests/test_kernel_map_memory.py builds maptest.c: clang-cl on Windows, clang with the platform layer
elsewhere. Skipped where there is no such compiler, and on hosts the platform layer is not written for
(Linux on x86-64 and macOS on Apple silicon only, as the game)."""
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
    src = str(ROOT / 'tools' / 'dev' / 'ringtest.c')
    if os.name == 'nt':
        exe = os.path.join(folder, 'ringtest.exe')
        # (kernel_init reserves the guest's areas at fixed addresses: no high-entropy ASLR, as maptest)
        cmd = [cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, src, '/Fe' + exe, '/Fo' + os.path.join(folder, 'ringtest.obj'),
               '/link', *cc.LINK_OPTIONS, '/HIGHENTROPYVA:NO']
    else:
        exe = os.path.join(folder, 'ringtest')
        layer = ['runtime/linux/win32.c']   # (the platform layer)
        if sys.platform == 'darwin':
            layer += ['runtime/macos/memory.c', 'runtime/macos/platform.c']
        else:
            layer += ['-lpthread']
        cmd = ['clang', '-O1', '-w', '-I', 'runtime/macos/include', '-I', 'runtime/linux/include', '-I', 'runtime', src, *layer, '-o', exe]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise AssertionError('ringtest.c did not build:\n' + r.stdout + r.stderr)
    return exe


@unittest.skipUnless(HAVE_CLANG, 'needs clang on x86-64 Linux or an Apple silicon Mac, or clang-cl on Windows (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class RollbackRingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix='ringtest ')
        cls.exe = build(cls.dir)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def ringtest(self, mode=None, **switches):
        env = {k: v for k, v in os.environ.items() if k not in ('SSBU_ROLLBACK_RING', 'SSBU_MAP_GUARD')}
        env.update(switches)
        r = subprocess.run([self.exe, *([mode] if mode else [])], capture_output=True, text=True, env=env, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r'(^|\n)\d+ checks, 0 failed\n$')
        return r.stdout + r.stderr

    def test_ring(self):
        out = self.ringtest(SSBU_ROLLBACK_RING='4', SSBU_MAP_GUARD='1')
        self.assertEqual(out.count('[kernel] the heap, stack and thread-local areas\' writes watched too, and the write watch shared by the renderer '
                                   'and the rollback ring (SSBU_ROLLBACK_RING)\n'), 1, out)
        self.assertIn('[kernel] SSBU_MAP_GUARD=1 left off: the rollback ring reads every mapped page (SSBU_ROLLBACK_RING)\n', out)
        self.assertNotIn('inaccessible while it is mapped', out)

    def test_frame_ends(self):
        """The lines a run gets: the switch, the first save, then one for each 300 saves."""
        out = self.ringtest('run', SSBU_ROLLBACK_RING='3')
        self.assertEqual(out.count('[ring] SSBU_ROLLBACK_RING: the guest\'s writable memory saved at each frame end, the last 3 frames kept; '
                                   'never put back in a run (that needs the game\'s threads stopped first)\n'), 1, out)
        self.assertRegex(out, r'\[ring\] frame 1: \d+ writable pages \(\d+\.\d MiB\), \d+ of them not zeros, saved in \d+\.\d ms; \d+\.\d MiB held\n')
        self.assertRegex(out, r'\[ring\] frames 2-301: a frame \d+ pages written \(at most \d+\), 10 changed \(at most 10\), \d+ compared; '
                              r'a save \d+\.\d\d ms \(at most \d+\.\d\d\); \d+\.\d MiB held for 3 frames of \d+ writable pages\n')
        self.assertEqual(out.count('[ring] frames'), 1, out)

    def test_off(self):
        out = self.ringtest('off')
        self.assertNotIn('[ring]', out)
        self.assertNotIn('SSBU_ROLLBACK_RING', out)


if __name__ == '__main__':
    unittest.main()
