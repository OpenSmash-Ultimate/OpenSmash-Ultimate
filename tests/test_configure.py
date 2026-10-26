from pathlib import Path
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
PS = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
FONTS = (
    'nintendo_udsg-r_std_003.bfttf', 'nintendo_udsg-r_org_zh-cn_003.bfttf',
    'nintendo_udsg-r_ext_zh-cn_003.bfttf', 'nintendo_udjxh-db_zh-tw_003.bfttf',
    'nintendo_udsg-r_ko_003.bfttf', 'nintendo_ext_003.bfttf',
)


@unittest.skipUnless(PS.exists(), 'Windows PowerShell only')
class ConfigureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='configure-test-')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.install = self.folder / 'runtime with spaces'
        (self.install / 'tools').mkdir(parents=True)
        self.script = self.install / 'tools/Configure-Game-Data.ps1'
        shutil.copyfile(ROOT / 'tools/Configure-Game-Data.ps1', self.script)
        self.romfs = self.folder / 'romfs.bin'
        self.romfs.write_bytes(struct.pack('<10Q', 80, 80, 0, 80, 0, 80, 0, 80, 0, 80) + b'fixture')
        self.fonts = self.folder / 'my fonts'
        self.fonts.mkdir()
        for name in FONTS:
            (self.fonts / name).write_bytes(b'font-fixture')
        self.aoc = self.folder / 'aoc.cfg'
        self.aoc.write_text('1\n')

    def invoke(self, *args, fonts=True):
        given = f"-FontsDirectory '{self.fonts}' " if fonts else ''
        command = f"function Get-Process {{ @() }}; & '{self.script}' -NonInteractive -RomfsPath '{self.romfs}' {given}-AocConfig '{self.aoc}' " + ' '.join(args)
        return subprocess.run([str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command], capture_output=True, text=True)

    def test_setup_and_backup(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        config = self.install / 'dump/modules/romfs.cfg'
        self.assertEqual(config.read_text().splitlines(), [str(self.romfs), '0'])
        self.assertEqual((self.install / 'dump/modules/aoc.cfg').read_bytes(), self.aoc.read_bytes())
        for name in FONTS:
            self.assertEqual((self.install / 'dump/fonts' / name).read_bytes(), b'font-fixture')
        again = self.invoke()
        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        backups = list(config.parent.glob('romfs.cfg.before-setup-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), config.read_bytes())

    def test_dry_run_writes_nothing(self):
        result = self.invoke('-DryRun')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.install / 'dump').exists())

    def test_bad_image_rejected(self):
        self.romfs.write_bytes(bytes(100))
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.install / 'dump').exists())

    def test_missing_font_rejected(self):
        (self.fonts / FONTS[0]).unlink()
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.install / 'dump').exists())

    def open_fonts(self):
        """third_party/fonts_open as the runtime reads it, made up (.ttf for each .bfttf)."""
        folder = self.install / 'third_party/fonts_open'
        folder.mkdir(parents=True)
        for name in FONTS:
            (folder / name.replace('.bfttf', '.ttf')).write_bytes(b'open-font-fixture')
        return folder

    def test_no_firmware_fonts_the_open_ones(self):
        # no fonts given: the game data is set up, nothing goes into dump/fonts, and it says what the game is like then
        self.open_fonts()
        result = self.invoke(fonts=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.install / 'dump/modules/romfs.cfg').read_text().splitlines(), [str(self.romfs), '0'])
        self.assertFalse((self.install / 'dump/fonts').exists())
        said = ' '.join(result.stdout.split())
        self.assertIn('No firmware fonts: the game uses the open fonts of third_party\\fonts_open', said)
        self.assertIn('Chinese, Japanese and Korean text and most symbols are missing', said)
        self.assertIn('Fights are not affected.', said)

    def test_no_firmware_fonts_dry_run(self):
        self.open_fonts()
        result = self.invoke('-DryRun', fonts=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.install / 'dump').exists())

    def test_no_firmware_fonts_and_no_open_ones(self):
        # neither: the game would have no fonts, so nothing is set up
        folder = self.open_fonts()
        (folder / 'nintendo_ext_003.ttf').unlink()
        result = self.invoke(fonts=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('nintendo_ext_003.ttf', result.stdout)
        self.assertFalse((self.install / 'dump').exists())


if __name__ == '__main__':
    unittest.main()
