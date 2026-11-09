"""The plugins' code patches (dump/skyline/patches.txt, runtime/hle_skyline.c sky_memcpy): each line
names the plugin that wrote it, and tools/skyline.py prune_patches drops the ones of plugins no longer
loaded, so a build doesn't compile a removed plugin's changes into the game; tools/recomp.py and
skyline.py's stale check read lines with and without the plugin alike. On made-up lists."""
from pathlib import Path
import os
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import recomp  # noqa: E402
import skyline  # noqa: E402

SEED = (ROOT / 'tools' / 'skyline_seed' / 'patches.txt').read_text(encoding='utf-8').splitlines()


class PrunePatches(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.p = skyline.Paths(self.tmp.name)
        os.makedirs(self.p.skyline)
        self.path = os.path.join(self.p.skyline, 'patches.txt')
        self.log = []

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, lines):
        Path(self.path).write_text(''.join(l + '\n' for l in lines), encoding='utf-8')

    def read(self):
        return Path(self.path).read_text(encoding='utf-8').splitlines()

    def test_a_gone_plugins_patches_are_dropped(self):
        lines = [SEED[0],
                 'main 17e0854 f877d936 d503201f smashline_',      # (another build of Smashline: gone)
                 'main 33b6ddc f875791b d503201f goomba_',
                 'lua2cpp_common 1a104 1400002c d503201f smashline',
                 'main 60bf78 b840cd09 52800009 ?']                # (no plugin found: kept)
        self.write(lines)
        skyline.prune_patches(self.p, {'smashline', 'goomba_'}, self.log.append)
        self.assertEqual(self.read(), [lines[0], lines[2], lines[3], lines[4]])
        self.assertEqual(len(self.log), 1)
        self.assertIn('smashline_ (1)', self.log[0])

    def test_an_unnamed_line_is_kept_only_if_the_seed_has_it(self):
        self.write([SEED[0], 'main 3db774 38686928 d503201f'])
        skyline.prune_patches(self.p, {'smashline'}, self.log.append)
        self.assertEqual(self.read(), [SEED[0]])
        self.assertIn('no plugin named (1)', self.log[0])

    def test_nothing_to_drop_leaves_the_file(self):
        lines = [SEED[0], 'main 33b6ddc f875791b d503201f goomba_']
        self.write(lines)
        before = os.path.getmtime(self.path)
        os.utime(self.path, (before - 100, before - 100))
        skyline.prune_patches(self.p, {'goomba_'}, self.log.append)
        self.assertEqual(self.read(), lines)
        self.assertEqual(os.path.getmtime(self.path), before - 100)   # (not rewritten: no build asked for)
        self.assertEqual(self.log, [])

    def test_both_readers_take_the_plugin_field(self):
        self.write(['main 33b6ddc f875791b d503201f goomba_', 'main 60bf78 b840cd09 52800009'])
        old = recomp.TREE
        recomp.TREE = self.tmp.name
        try:
            self.assertEqual(recomp.plugin_patches('main'), [(0x33b6ddc, 0xf875791b, 0xd503201f),
                                                             (0x60bf78, 0xb840cd09, 0x52800009)])
        finally:
            recomp.TREE = old
        # (what recomp.py records a module's C was made with: the same text, so the build isn't stale)
        self.assertEqual(skyline.patch_lines(self.p), {'main': '33b6ddc f875791b d503201f\n60bf78 b840cd09 52800009\n'})


if __name__ == '__main__':
    unittest.main()
