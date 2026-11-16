from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from nca import RomfsImage, portable_romfs_path, resolve_romfs_path, write_layers


class PortablePaths(unittest.TestCase):
    def test_rename(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'original'
            modules = root / 'dump/modules'
            modules.mkdir(parents=True)
            cfg = modules / 'romfs.cfg'
            source = modules / 'data.bin'
            source.write_bytes(b'game')
            write_layers(str(modules / 'romfs.layers'), 4, [(0, 0, 0)])
            cfg.write_text(chr(10).join(['layered', 'romfs.layers', portable_romfs_path(str(cfg), str(source)), '']))
            moved = Path(temp) / 'renamed'
            root.rename(moved)
            image = RomfsImage(str(moved / 'dump/modules/romfs.cfg'))
            try:
                self.assertEqual(image.read(0, 4), b'game')
            finally:
                image.close()

    def test_external_and_legacy(self):
        with tempfile.TemporaryDirectory() as temp:
            modules = Path(temp) / 'build/dump/modules'
            modules.mkdir(parents=True)
            cfg = str(modules / 'romfs.cfg')
            external = str(Path(temp) / 'external.nca')
            self.assertEqual(portable_romfs_path(cfg, external), external)
            local = modules / 'base.nca'
            local.write_bytes(b'base')
            legacy = str(Path(temp) / 'old/dump/modules/base.nca')
            self.assertEqual(resolve_romfs_path(cfg, legacy), str(local))
            self.assertEqual(resolve_romfs_path(cfg, external), external)
