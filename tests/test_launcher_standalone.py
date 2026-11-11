"""OpenSmash.exe on its own (the release's OpenSmash.exe): the first start's folder checks, the
project ZIP's unpacking (ProjectSetup.InstallAsync) and the shortcuts, through the launcher's
test entry points (--fetch-project, --find-project, --make-shortcuts), which open no window. ZIPs are
made here, so nothing is downloaded; the remembered folder goes to OPENSMASH_SETTINGS_DIR and the
shortcuts to a temporary folder, never the real desktop. Skipped off Windows and where the launcher
is not built (toolsuild_launcher.ps1, or dotnet publish into OpenSmash.Launcher/bin/publish)."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent.parent
BUILT = [ROOT / 'OpenSmash.Launcher' / 'bin' / 'publish' / 'OpenSmash.exe', ROOT / 'OpenSmash.exe']
EXE = next((p for p in BUILT if p.exists()), None) if os.name == 'nt' else None
TOP = 'OpenSmash-Ultimate-main/'


def make_zip(path, files):
    with zipfile.ZipFile(path, 'w') as z:
        for name, data in files.items():
            z.writestr(name, data)


GOOD = {TOP + 'README.md': 'readme', TOP + 'tools/recomp.py': '# recomp',
        TOP + 'tools/install_windowsmash.ps1': '# installer', TOP + 'docs/assets/windowsmash.ico': '',
        TOP + 'media/opensmash.ico': ''}


@unittest.skipUnless(EXE, 'needs Windows and a built OpenSmash.exe')
class StandaloneLauncherTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp()).resolve()   # (the long name: the exe prints full paths)
        self.env = dict(os.environ, OPENSMASH_SETTINGS_DIR=str(self.dir / 'settings'))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def run_exe(self, *args, exe=None):
        r = subprocess.run([str(exe or EXE), *args], capture_output=True, text=True, timeout=120, env=self.env)
        return r.returncode, r.stdout.strip().splitlines()

    def fetch(self, folder, files):
        z = self.dir / 'src.zip'
        make_zip(z, files)
        return self.run_exe('--fetch-project', str(folder), '--from', str(z))

    def test_unpacks_into_a_new_folder_and_remembers_it(self):
        target = self.dir / 'games' / 'OpenSmash'
        code, out = self.fetch(target, GOOD)
        self.assertEqual(code, 0, out)
        self.assertEqual(out[-1], 'ok ' + str(target))
        self.assertTrue((target / 'tools' / 'install_windowsmash.ps1').is_file())
        self.assertEqual((target / 'README.md').read_text(), 'readme')
        self.assertTrue((target / 'OpenSmash.exe').is_file())            # the launcher copied in
        self.assertFalse((target / '.opensmash-download').exists())      # no leftovers
        self.assertFalse((target / 'OpenSmash-Ultimate-main').exists())  # the ZIP's top folder is unwrapped
        remembered = (self.dir / 'settings' / 'project_folder.txt').read_text(encoding='utf-8').strip()
        self.assertEqual(remembered, str(target))
        # an exe somewhere else, with no project around it, finds the remembered folder
        lone = self.dir / 'Downloads'
        lone.mkdir()
        shutil.copy2(EXE, lone / 'OpenSmash.exe')
        code, out = self.run_exe('--find-project', exe=lone / 'OpenSmash.exe')
        self.assertEqual((code, out), (0, [str(target)]))

    def test_build_output_finds_its_repo(self):
        if EXE.parent == ROOT:
            self.skipTest('built into the root')
        code, out = self.run_exe('--find-project')
        self.assertEqual((code, out), (0, [str(ROOT)]))

    def test_folder_with_only_the_exe_is_used(self):
        target = self.dir / 'OpenSmash'
        target.mkdir()
        shutil.copy2(EXE, target / 'OpenSmash.exe')
        code, out = self.fetch(target, GOOD)
        self.assertEqual(code, 0, out)
        self.assertTrue((target / 'tools' / 'recomp.py').is_file())

    def test_folder_with_other_files_is_refused_and_left_alone(self):
        target = self.dir / 'mine'
        target.mkdir()
        (target / 'notes.txt').write_text('keep me')
        code, out = self.fetch(target, GOOD)
        self.assertEqual(code, 2, out)
        self.assertEqual(sorted(p.name for p in target.iterdir()), ['notes.txt'])
        self.assertEqual((target / 'notes.txt').read_text(), 'keep me')
        self.assertFalse((self.dir / 'settings' / 'project_folder.txt').exists())

    def test_incomplete_zip_leaves_nothing(self):
        target = self.dir / 'new'
        code, out = self.fetch(target, {TOP + 'README.md': 'x', TOP + 'tools/recomp.py': ''})
        self.assertEqual(code, 1, out)
        self.assertIn('does not look complete', out[-1])
        self.assertFalse(target.exists())

    def test_entry_outside_the_folder_is_refused(self):
        target = self.dir / 'a' / 'new'
        files = dict(GOOD)
        files['../../evil.txt'] = 'x'
        code, out = self.fetch(target, files)
        self.assertEqual(code, 1, out)
        self.assertIn('outside its folder', out[-1])
        self.assertFalse(any(p.name == 'evil.txt' for p in self.dir.rglob('*')))
        self.assertFalse(target.exists())

    def test_existing_project_is_kept_as_it_is(self):
        target = self.dir / 'clone'
        (target / 'tools').mkdir(parents=True)
        (target / 'tools' / 'install_windowsmash.ps1').write_text('# mine')
        code, out = self.fetch(target, GOOD)
        self.assertEqual(code, 0, out)
        self.assertEqual((target / 'tools' / 'install_windowsmash.ps1').read_text(), '# mine')

    def test_shortcuts_point_into_the_folder_with_their_icons(self):
        target = self.dir / 'OpenSmash'
        self.assertEqual(self.fetch(target, GOOD)[0], 0)
        desk, start = self.dir / 'desk', self.dir / 'start'
        code, out = self.run_exe('--make-shortcuts', str(target), '--desktop', str(desk), '--start-menu', str(start))
        self.assertEqual(code, 0, out)
        self.assertEqual(len(out), 4, out)
        read = ('$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:LNK); '
                '"$($s.TargetPath)|$($s.Arguments)|$($s.WorkingDirectory)|$($s.IconLocation)"')
        for folder in (desk, start / 'OpenSmash'):
            got = {}
            for name in ('OpenSmash.lnk', 'Play SSBU.lnk'):
                lnk = folder / name
                self.assertTrue(lnk.is_file(), lnk)
                r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', read],
                                   capture_output=True, text=True, timeout=60, env=dict(os.environ, LNK=str(lnk)))
                got[name] = r.stdout.strip().split('|')
            self.assertEqual(got['OpenSmash.lnk'], [str(target / 'OpenSmash.exe'), '', str(target), str(target / 'OpenSmash.exe') + ',0'])
            self.assertEqual(got['Play SSBU.lnk'], [str(target / 'OpenSmash.exe'), '--play', str(target),
                                                    str(target / 'docs' / 'assets' / 'windowsmash.ico') + ',0'])

    def test_shortcuts_need_a_desktop_folder(self):
        code, out = self.run_exe('--make-shortcuts', str(self.dir))
        self.assertEqual(code, 1, out)


if __name__ == '__main__':
    unittest.main()
