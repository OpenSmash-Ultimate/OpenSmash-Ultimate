"""The Linux package (tools/linux/package_linux.py): made from a made-up game tree and build, it
carries third_party/fonts_open, licences included, where the runtime looks for it. No game files."""
from pathlib import Path
import os
import runpy
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'tools' / 'linux' / 'package_linux.py'
OPEN_FONTS = ROOT / 'third_party' / 'fonts_open'
NAMES = ('nintendo_udsg-r_std_003', 'nintendo_udsg-r_org_zh-cn_003', 'nintendo_udsg-r_ext_zh-cn_003',
         'nintendo_udjxh-db_zh-tw_003', 'nintendo_udsg-r_ko_003', 'nintendo_ext_003')


def files_in(folder):
    return {str(p.relative_to(folder)): p.read_bytes() for p in Path(folder).rglob('*') if p.is_file()}


class PackageFontsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='linux-package-test-')
        self.addCleanup(self.temp.cleanup)
        top = Path(self.temp.name)
        self.port, self.build, self.out = top / 'port', top / 'build-linux', top / 'out'
        dump = self.port / 'dump'
        (dump / 'modules').mkdir(parents=True)
        (dump / 'exefs').mkdir()
        (dump / 'exefs' / 'main').write_bytes(b'NSO0 made up')
        romfs = top / 'game' / 'romfs.bin'
        romfs.parent.mkdir()
        romfs.write_bytes(b'made-up romfs')
        (dump / 'modules' / 'romfs.cfg').write_text(f'{romfs}\n0\n')
        self.build.mkdir()
        (self.build / 'ssbu').write_bytes(b'made-up program')
        (self.build / 'ssbu.syms').write_bytes(b'made-up symbols')
        (self.build / 'port.txt').write_text(f'{self.port}\n')
        self.toolchain = top / 'toolchain'
        lib = self.toolchain / 'dxvk-native' / 'usr' / 'lib'
        lib.mkdir(parents=True)
        for so in ('libdxvk_d3d11.so.0', 'libdxvk_dxgi.so.0'):
            (lib / so).write_bytes(b'made-up library')
        self.env = {**os.environ, 'SSBU_LINUX_TOOLCHAIN': str(self.toolchain), 'SSBU_LLVM_BIN': str(top)}
        self.bundle = self.out / 'ssbu-linux'

    def package(self, *args):
        r = subprocess.run([sys.executable, str(SCRIPT), '--port', str(self.port), '--build', str(self.build),
                            '--out', str(self.out), *args], env=self.env, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout

    def test_without_firmware_fonts(self):
        said = self.package('--tar')
        fonts = self.bundle / 'third_party' / 'fonts_open'
        self.assertEqual(files_in(fonts), files_in(OPEN_FONTS))   # (as it is: every font, README and licence)
        for name in ('LICENSE-Apache-2.0.txt', 'LICENSE-OFL-1.1.txt', 'LICENSE-GPL-2.0-or-later.txt', 'README.md'):
            self.assertTrue((fonts / name).is_file(), name)
        for name in NAMES:
            self.assertTrue((fonts / (name + '.ttf')).is_file(), name)
        self.assertFalse((self.bundle / 'dump' / 'fonts').exists())
        self.assertIn('fonts: the open ones of third_party/fonts_open for 6 of the 6', said)
        # where the runtime looks: from the folder it is given (the launchers give dump/modules, from the package's root)
        hle_pl = (ROOT / 'runtime' / 'hle_pl.c').read_text()
        self.assertIn('"%s/../../third_party/fonts_open", rt_data_dir', hle_pl)
        self.assertIn('"$SSBU_EXE" dump/modules', (self.bundle / 'run.sh').read_text())
        self.assertEqual(os.path.normpath(self.bundle / 'dump' / 'modules' / '..' / '..' / 'third_party' / 'fonts_open'),
                         str(fonts))
        with tarfile.open(self.out / 'ssbu-linux.tar') as t:
            names = t.getnames()
        for name in ('LICENSE-OFL-1.1.txt', 'README.md', 'nintendo_ext_003.ttf'):
            self.assertIn('ssbu-linux/third_party/fonts_open/' + name, names)

    def test_with_firmware_fonts(self):
        fonts = self.port / 'dump' / 'fonts'
        fonts.mkdir()
        for name in NAMES:
            (fonts / (name + '.bfttf')).write_bytes(b'made-up firmware font')
        said = self.package()
        self.assertEqual(files_in(self.bundle / 'dump' / 'fonts'), files_in(fonts))
        self.assertEqual(files_in(self.bundle / 'third_party' / 'fonts_open'), files_in(OPEN_FONTS))   # (there too)
        self.assertNotIn('fonts: the open ones', said)

    def test_fonts_left_out(self):
        # (what it stops for: a font in neither folder, which the game does not start without)
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.dict(os.environ, {'SSBU_LINUX_TOOLCHAIN': str(self.toolchain), 'SSBU_LLVM_BIN': tmp}), \
                mock.patch.object(sys, 'argv', ['package_linux.py']), mock.patch.object(sys, 'path', list(sys.path)), \
                mock.patch.dict(sys.modules):   # (its toolchain module, imported here, is not kept for other tests)
            left_out = runpy.run_path(str(SCRIPT), run_name='package_linux_test')['fonts_left_out']
            fonts, open_fonts = Path(tmp) / 'fonts', Path(tmp) / 'open'
            for d in (fonts, open_fonts):
                d.mkdir()
            self.assertEqual(left_out(str(fonts), str(OPEN_FONTS)), (list(NAMES), []))
            (fonts / 'nintendo_ext_003.bfttf').write_bytes(b'')
            (open_fonts / 'nintendo_udsg-r_std_003.ttf').write_bytes(b'')
            self.assertEqual(left_out(str(fonts), str(open_fonts)), (list(NAMES[:5]), list(NAMES[1:5])))


if __name__ == '__main__':
    unittest.main()
