"""The files' names from before the rename (WindowSmash to OpenSmash) still work: older OpenSmash.exe, older
copies and desktop shortcuts start them by name. Each runs its new file, next to it, with the same arguments, and
exits with its exit code. Here: each old name with a made-up new file that says what it was given."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_linux_installer import find_bash  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BASH = find_bash()
TAKES_UPDATE_ONLY = r'\[switch\]\s*\$UpdateOnly\b'   # (LauncherForm.InstallerTakesUpdateOnly, v0.4)

FAKE_PS1 = r'''param([switch]$UpdateOnly, [string]$Game = 'none', [int]$Code = 0, [switch]$Throw, [switch]$NativeFail)
Write-Host "UpdateOnly=$UpdateOnly Game=[$Game] bound=$($PSBoundParameters.Keys -join ',')"
if ($NativeFail) { cmd.exe /c exit 5 }
if ($Throw) { throw 'stopped' }
if ($Code) { exit $Code }
'''


class OldNames(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix='old-names-'))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_the_old_installer_passes_the_older_launchers_checks(self):
        text = (ROOT / 'tools' / 'install_windowsmash.ps1').read_text(encoding='utf-8-sig')
        self.assertRegex(text, TAKES_UPDATE_ONLY)                  # (OpenSmash.exe v0.4: Update)
        self.assertIn('[switch]$PythonOnly', text)                  # (OpenSmash.Avalonia v0.4.1: Python)
        self.assertIn("$RepoName = 'OpenSmash-Ultimate'", text)     # (v0.4: an old copy's installer replaced)
        new = (ROOT / 'tools' / 'install_opensmash.ps1').read_text(encoding='utf-8-sig')
        for switch in ('UpdateOnly', 'BuildAfterUpdate', 'PythonOnly', 'LauncherOnly', 'NoWizard', 'NoUpdate'):
            self.assertRegex(new, r'\[switch\]\s*\$' + switch + r'\b')

    @unittest.skipUnless(os.name == 'nt', 'Windows PowerShell')
    def test_the_old_installer_runs_the_new_one(self):
        tools = self.dir / 'with space' / 'tools'
        tools.mkdir(parents=True)
        shutil.copy2(ROOT / 'tools' / 'install_windowsmash.ps1', tools)
        (tools / 'install_opensmash.ps1').write_text(FAKE_PS1)
        for args in (['-UpdateOnly', '-Game', r'C:\My Games\x y'], ['-Game', ''], ['-Game', 'a"b'], ['-Game', 'C:\\x y\\'],
                     ['-Code', '7'], ['-NativeFail'], ['-Throw']):
            got = []
            for script in ('install_opensmash.ps1', 'install_windowsmash.ps1'):
                r = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(tools / script),
                                    *args], capture_output=True, text=True, timeout=120, stdin=subprocess.DEVNULL)
                got.append((r.returncode, r.stdout.strip()))
            self.assertEqual(got[1], got[0], args)
            self.assertIn('bound=', got[1][1], args)

    @unittest.skipUnless(os.name == 'nt', 'Windows PowerShell')
    def test_the_old_installer_as_older_launchers_start_it(self):
        # (OpenSmash.exe v0.4's setup window: -Command "& '<root>/tools/install_windowsmash.ps1' -NoWizard -Game '...'",
        # no console window, stdin closed; its Update: -File with the same)
        tools = self.dir / "it's here" / 'tools'
        tools.mkdir(parents=True)
        shutil.copy2(ROOT / 'tools' / 'install_windowsmash.ps1', tools)
        (tools / 'install_opensmash.ps1').write_text(FAKE_PS1)
        quoted = lambda s: "'" + s.replace("'", "''") + "'"
        got = []
        for script in ('install_opensmash.ps1', 'install_windowsmash.ps1'):
            line = f"& {quoted(str(tools / script))} -UpdateOnly -Game {quoted('C:/My Games/x y')} -Code 3"
            r = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', line],
                               capture_output=True, text=True, timeout=120, stdin=subprocess.DEVNULL,
                               creationflags=subprocess.CREATE_NO_WINDOW)
            got.append((r.returncode, r.stdout.strip()))
        self.assertEqual(got[1], got[0])
        self.assertEqual(got[1][1], 'UpdateOnly=True Game=[C:/My Games/x y] bound=UpdateOnly,Game,Code')
        self.assertNotEqual(got[1][0], 0)   # (-Command: 1 for a script that ended with exit 3, both ways)

    @unittest.skipUnless(BASH, 'needs bash')
    def test_the_old_shell_setups_run_the_new_ones(self):
        for old, new in (('install-windowsmash.sh', 'install-opensmash.sh'),
                         ('Install WindowSmash.command', 'Install OpenSmash.command')):
            top = self.dir / old.replace(' ', '_')
            top.mkdir()
            shutil.copy2(ROOT / old, top / old)
            (top / new).write_bytes(b'#!/bin/bash\necho "args:$#:$1|$2"\nexit 4\n')
            r = subprocess.run([BASH, str(top / old), '--game', 'a b'], capture_output=True, text=True, timeout=60)
            self.assertEqual((r.returncode, r.stdout.strip()), (4, 'args:2:--game|a b'), old)
            self.assertTrue((ROOT / old).read_bytes().startswith(b'#!/'), old)
            self.assertNotIn(b'\r', (ROOT / old).read_bytes(), old)

    @unittest.skipUnless(os.name == 'nt', 'cmd.exe')
    def test_the_old_mods_launcher_runs_the_new_one(self):
        launchers = self.dir / 'launchers'
        launchers.mkdir()
        shutil.copy2(ROOT / 'launchers' / 'WindowSmash Mods.bat', launchers)
        (launchers / 'OpenSmash Mods.bat').write_text('@echo off\r\necho args:%*\r\nexit /b 3\r\n')
        r = subprocess.run(['cmd.exe', '/c', 'call', str(launchers / 'WindowSmash Mods.bat'), 'x'], capture_output=True,
                           text=True, timeout=60)
        self.assertEqual((r.returncode, r.stdout.strip()), (3, 'args:x'))

    def test_the_old_icon_is_still_there(self):
        # (shortcuts and applications-menu entries made before the rename point at it)
        self.assertEqual((ROOT / 'docs' / 'assets' / 'windowsmash.ico').read_bytes(),
                         (ROOT / 'docs' / 'assets' / 'opensmash.ico').read_bytes())


if __name__ == '__main__':
    unittest.main()
