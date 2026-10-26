"""The installer's decisions (tools/install_windowsmash.ps1): its pure functions, run in Windows
PowerShell with made-up inputs. No downloads, no installs, no game files."""
from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
PS = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
SCRIPT = ROOT / 'tools/install_windowsmash.ps1'


def ps_text(script):
    """What PowerShell prints running `script` with the installer's functions loaded (its host output)."""
    command = f". '{SCRIPT}'; {script}"
    r = subprocess.run([str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command],
                       capture_output=True, text=True)
    return r.stdout + r.stderr


def ps(expression):
    """The JSON of a PowerShell expression evaluated with the installer's functions loaded."""
    command = f". '{SCRIPT}'; {expression} | ConvertTo-Json -Compress -Depth 6"
    r = subprocess.run([str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command],
                       capture_output=True, text=True)
    if r.returncode or not r.stdout.strip():
        raise AssertionError(r.stdout + r.stderr)
    return json.loads(r.stdout)


SDL_RELEASES = json.dumps([
    {"tag_name": "release-3.5.0", "prerelease": True, "draft": False,
     "assets": [{"name": "SDL3-devel-3.5.0-VC.zip", "browser_download_url": "https://x/pre.zip", "digest": "sha256:" + "a" * 64}]},
    {"tag_name": "release-3.4.20", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL3-devel-3.4.20-mingw.zip", "browser_download_url": "https://x/mingw.zip"}]},
    {"tag_name": "release-3.4.16", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL3-devel-3.4.16-VC.zip", "browser_download_url": "https://x/16.zip", "digest": "sha256:" + "b" * 64}]},
    {"tag_name": "release-3.4.18", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL3-3.4.18.tar.gz", "browser_download_url": "https://x/src.tar.gz"},
                {"name": "SDL3-devel-3.4.18-VC.zip", "browser_download_url": "https://x/18.zip", "digest": "sha256:" + "C" * 64}]},
    {"tag_name": "release-2.32.0", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL2-devel-2.32.0-VC.zip", "browser_download_url": "https://x/sdl2.zip"}]},
])


# Step-CheckPc's surroundings, made up: Windows 10 build 19045 with a Ryzen and 40 GB, a drive whose free space is not
# known, a .NET whose C# compiler is broken (Add-Type throws), and a $Root with no build folder (no build\isa.txt)
PC_MOCKS = ("function Add-Type { throw '(0) : Unable to find messages file ''cscui.dll''' }; "
            "function Get-PSDrive { $null }; "
            "function Get-CimInstance { [CmdletBinding()] param([string]$ClassName) switch ($ClassName) { "
            "'Win32_OperatingSystem' { [pscustomobject]@{ Caption = 'Microsoft Windows 10 Home'; Version = '10.0.19045'; "
            "BuildNumber = '19045'; OSArchitecture = '64-bit' } } "
            "'Win32_Processor' { [pscustomobject]@{ Name = 'AMD Ryzen 7 3700X 8-Core Processor' } } "
            "'Win32_ComputerSystem' { [pscustomobject]@{ TotalPhysicalMemory = 40GB } } } }; "
            "$Root = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString()); ")
PC_STEP = "try { Step-CheckPc } catch { 'STOPPED: ' + $_.Exception.Message }"


@unittest.skipUnless(PS.exists(), 'Windows PowerShell only')
class InstallerDecisionTests(unittest.TestCase):
    def test_python_embed_url_newest_of_the_minor(self):
        listing = 'href="3.14.6/" href="3.9.1/" href="3.14.8/" href="3.15.0/" href="3.14.10/"'
        got = ps(f"@{{ pick = (Get-PythonEmbedUrl '{listing}' '3.14' '3.14.0'); none = (Get-PythonEmbedUrl '' '3.14' '3.14.8') }}")
        self.assertEqual(got['pick']['Version'], '3.14.10')
        self.assertEqual(got['pick']['Url'], 'https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip')
        self.assertEqual(got['none']['Version'], '3.14.8')
        self.assertTrue(got['none']['Url'].endswith('/3.14.8/python-3.14.8-embed-amd64.zip'))

    def test_sdl3_asset_newest_release_with_a_vc_zip(self):
        releases = SDL_RELEASES.replace("'", "''")
        got = ps(f"$r = (Select-Sdl3Asset ('{releases}' | ConvertFrom-Json)); @{{ v = $r.Version.ToString(); url = $r.Url; sha = $r.Sha256; name = $r.Name }}")
        self.assertEqual(got['v'], '3.4.18')   # not the prerelease 3.5.0, not 3.4.20 without a VC zip
        self.assertEqual(got['url'], 'https://x/18.zip')
        self.assertEqual(got['sha'], 'c' * 64)
        self.assertEqual(got['name'], 'SDL3-devel-3.4.18-VC.zip')

    def test_sdl3_asset_without_digest_or_releases(self):
        releases = json.dumps([{"tag_name": "release-3.4.4", "prerelease": False, "draft": False,
                                "assets": [{"name": "SDL3-devel-3.4.4-VC.zip", "browser_download_url": "https://x/4.zip"}]}])
        got = ps(f"$r = (Select-Sdl3Asset ('{releases}' | ConvertFrom-Json)); @{{ v = $r.Version.ToString(); sha = $r.Sha256; none = (Select-Sdl3Asset @()) }}")
        self.assertEqual(got['v'], '3.4.4')
        self.assertIsNone(got['sha'])
        self.assertIsNone(got['none'])

    def test_visual_studio_arguments(self):
        got = ps("@{ install = (Get-VsBuildToolsArguments 'Microsoft.VisualStudio.BuildTools'); "
                 "bt = (Get-VsModifyArguments 'C:\\VS\\BuildTools' 'Microsoft.VisualStudio.Product.BuildTools'); "
                 "community = (Get-VsModifyArguments 'C:\\VS\\Community' 'Microsoft.VisualStudio.Product.Community') }")
        install = got['install']
        self.assertEqual(install[:3], ['install', '--id', 'Microsoft.VisualStudio.BuildTools'])
        self.assertIn('--accept-package-agreements', install)
        self.assertIn('--accept-source-agreements', install)
        override = install[install.index('--override') + 1]
        for part in ('--passive', '--wait', '--norestart', '--add Microsoft.VisualStudio.Workload.VCTools',
                     '--add Microsoft.VisualStudio.Component.VC.Llvm.Clang', '--includeRecommended'):
            self.assertIn(part, override)
        self.assertEqual(got['bt'][:3], ['modify', '--installPath', 'C:\\VS\\BuildTools'])
        self.assertIn('Microsoft.VisualStudio.Workload.VCTools', got['bt'])
        self.assertIn('Microsoft.VisualStudio.Workload.NativeDesktop', got['community'])
        self.assertIn('Microsoft.VisualStudio.Component.VC.Llvm.Clang', got['community'])
        self.assertIn('--passive', got['community'])
        self.assertNotIn('--wait', got['community'])   # (setup.exe does not take it)
        self.assertIn('--wait', override)              # (the bootstrapper winget runs does)

    def test_visual_studio_modify_command_line(self):
        # setup.exe gets one quoted command line (a list reached it unquoted: the path broke at its
        # spaces, exit code 87), no --wait, which only the bootstrapper takes, and administrator
        # rights from the start (without them it refuses --passive: exit code 5007)
        path = 'C:\\Program Files\\Microsoft Visual Studio\\2022\\Community'
        with tempfile.TemporaryDirectory(prefix='installer-test-') as tmp:
            Path(tmp, 'setup.exe').write_bytes(b'')
            script = (f"$VsInstallerDir = '{tmp}'; $global:n = 0; "
                      "function Install-PortableCompiler { 'failed' }; "   # (the download route first; it failed)
                      "function Test-Compiler { $global:n++; @{ Ok = ($global:n -gt 1); Output = 'clang-cl: ok' } }; "
                      f"function Get-VsInstances {{ @{{ Path = '{path}'; Product = 'Microsoft.VisualStudio.Product.Community' }} }}; "
                      "function Start-Process { param($FilePath, $ArgumentList, $Verb, [switch]$Wait, [switch]$PassThru) "
                      "  $global:seen = $ArgumentList; $global:verb = $Verb; "
                      "  if ($global:code -eq 'declined') { throw 'The operation was canceled by the user.' } "
                      "  [pscustomobject]@{ ExitCode = $global:code } }; ")
            out = ps_text(script + "$global:code = 0; Step-Compiler; 'IS STRING: ' + ($global:seen -is [string]); "
                          "'VERB: ' + $global:verb; 'LINE: ' + $global:seen")
            self.assertIn('IS STRING: True', out)
            self.assertIn('VERB: RunAs', out)
            line = out.split('LINE: ', 1)[1].strip()
            r = subprocess.run(f'"{sys.executable}" -c "import sys, json; print(json.dumps(sys.argv[1:]))" {line}',
                               capture_output=True, text=True)
            argv = json.loads(r.stdout)
            self.assertEqual(argv[:3], ['modify', '--installPath', path])
            self.assertIn('--passive', argv)
            self.assertNotIn('--wait', argv)
            out = ps_text(script + "$global:code = 8006; try { Step-Compiler } catch { 'STOPPED: ' + $_.Exception.Message }")
            self.assertIn('STOPPED: The Visual Studio Installer could not add the C++ tools: Visual Studio is running', out)
            self.assertIn('(code 8006)', out)
            out = ps_text(script + "$global:code = 'declined'; try { Step-Compiler } catch { 'STOPPED: ' + $_.Exception.Message }")
            self.assertIn('STOPPED: The Visual Studio Installer needs administrator rights', out)
            self.assertIn('click Yes when Windows asks', out)

    def test_visual_studio_exit_codes(self):
        got = ps("@{ ok = (Get-VsExitMessage 0); reboot = (Get-VsExitMessage 3010); running = (Get-VsExitMessage 8006); "
                 "bad = (Get-VsExitMessage 87); blocked = (Get-VsExitMessage 5007); other = (Get-VsExitMessage 1603) }")
        self.assertIsNone(got['ok'])
        self.assertIsNone(got['reboot'])
        self.assertIn('close it', got['running'])
        self.assertIn('options', got['bad'])
        self.assertIn('administrator rights', got['blocked'])
        self.assertEqual(got['other'], 'it stopped')

    def test_folder_candidates_and_resolution(self):
        got = ps("$c = Get-FolderCandidates 'C:\\Users\\me\\AppData\\Roaming' 'C:\\Users\\me'; "
                 "$have = @('C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\system', 'C:\\Users\\me\\.switch'); "
                 "@{ keys0 = $c.Keys[0]; firmware0 = $c.Firmware[0]; game0 = $c.Game[0]; "
                 "ryu = (Resolve-FirstExisting $c.Keys { param($p) $have -contains $p }); "
                 "none = (Resolve-FirstExisting $c.Game { param($p) $false }); "
                 "ryugame = ($c.Game -contains 'C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\bis\\user\\Contents\\registered'); "
                 "ryufw = ($c.Firmware -contains 'C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\bis\\system\\Contents\\registered'); "
                 "switch = ($c.Keys -contains 'C:\\Users\\me\\.switch') }")
        self.assertEqual(got['keys0'], 'C:\\Users\\me\\AppData\\Roaming\\yuzu\\keys')
        self.assertEqual(got['firmware0'], 'C:\\Users\\me\\AppData\\Roaming\\yuzu\\nand\\system\\Contents\\registered')
        self.assertEqual(got['game0'], 'C:\\Users\\me\\AppData\\Roaming\\yuzu\\nand\\user\\Contents\\registered')
        self.assertEqual(got['ryu'], 'C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\system')   # the first that exists
        self.assertIsNone(got['none'])
        self.assertTrue(got['ryugame'] and got['ryufw'] and got['switch'])

    def test_firmware_optional(self):
        # (only for the fonts: none is fine, a folder that is named must be a firmware; an emulator's is suggested only
        # when it has one)
        with tempfile.TemporaryDirectory() as tmp:
            fw, empty = os.path.join(tmp, 'fw'), os.path.join(tmp, 'empty')
            for d in (fw, empty):
                os.makedirs(d)
            open(os.path.join(fw, '0100.nca'), 'w').close()
            got = ps(f"@{{ none = (Test-FirmwareFolder ''); empty = (Test-FirmwareFolder '{empty}'); "
                     f"gone = (Test-FirmwareFolder '{tmp}\\missing'); fw = (Test-FirmwareFolder '{fw}') }}")
            self.assertTrue(got['none']['Ok'])
            self.assertIn('the open fonts of third_party\\fonts_open', got['none']['Text'])
            self.assertEqual([got['empty']['Ok'], got['gone']['Ok'], got['fw']['Ok']], [False, False, True])
            detect = ("function Get-SavedFolders { @{} }; "
                      "function Get-FolderCandidates { @{ Keys = @(); Firmware = $global:fws; Game = @() } }; ")
            got = ps(detect + f"$global:fws = @('{empty}', '{fw}'); $a = (Get-DetectedFolders).Firmware; "
                     f"$global:fws = @('{empty}'); @{{ a = $a; b = (Get-DetectedFolders).Firmware }}")
            self.assertEqual(got['a'], fw)
            self.assertIsNone(got['b'])

    def test_build_with_and_without_firmware(self):
        out = ps_text("$DryRun = $true; $Script:Python = @('python'); "
                      "Step-Build @{ Game = 'D:\\G'; Keys = 'D:\\K'; Firmware = '' } | Out-Null; "
                      "Step-Build @{ Game = 'D:\\G'; Keys = 'D:\\K'; Firmware = 'D:\\F' } | Out-Null")
        self.assertIn('--game D:\\G --keys D:\\K --no-firmware', out)
        self.assertIn('--game D:\\G --keys D:\\K --firmware D:\\F', out)

    def test_firmware_question_optional(self):
        # -NoWizard: the question says it is optional and what for; Enter on nothing, or none over the one found, is none
        mocks = ("$global:prompts = @(); "
                 "function Read-Host { param([string]$Prompt) $global:prompts += $Prompt; $a = $global:q[0]; $global:q.RemoveAt(0); $a }; "
                 "function Test-GameFolder { @{ Ok = $true; Text = 'the game' } }; function Test-KeysFolder { @{ Ok = $true; Text = 'keys' } }; ")
        for typed, found in (("''", '$null'), ("'none'", "'D:\\FW'")):
            out = ps_text(mocks + f"$global:q = [Collections.ArrayList]@('D:\\G', 'D:\\K', {typed}, 'Me', 'n', 'n', 'n'); "
                          f"$r = Read-Answers @{{ Game = $null; Keys = $null; Firmware = {found} }} 'P'; "
                          "'RESULT firmware=[' + $r.Firmware + '] prompt=[' + $global:prompts[2] + ']'")
            self.assertIn('RESULT firmware=[] prompt=[Folder with your firmware', out)
            self.assertIn('(optional; none: the open fonts) [none]]' if found == '$null' else '[D:\\FW]]', out)
            self.assertIn('The firmware is optional', out)

    def test_avx2_by_processor_name(self):
        cases = {
            'Intel(R) Core(TM) i5-4590 CPU @ 3.30GHz': True,
            'Intel(R) Core(TM) i7-12700K': True,
            'Intel(R) Core(TM) i7-3770 CPU @ 3.40GHz': False,
            'Intel(R) Core(TM) Ultra 7 155H': True,
            'AMD Ryzen 5 3600 6-Core Processor': True,
            'AMD EPYC 7302P': True,
            'Intel(R) Core(TM)2 Duo CPU E8400': False,
            'Intel(R) Celeron(R) N4000 CPU': None,
            'Intel(R) Xeon(R) Gold 6148': None,
        }
        names = '; '.join(f"'{n}' = (Test-Avx2ByName '{n}')" for n in cases)
        got = ps('@{ ' + names + ' }')
        for name, want in cases.items():
            self.assertEqual(got[name], want, name)

    SSE42_BY_NAME = {   # (below x86-64-v2: no SSE4.2, False; else not known by name, None)
        'Intel(R) Core(TM)2 Duo CPU E8400 @ 3.00GHz': False,
        'AMD Phenom(tm) II X4 965 Processor': False,
        'AMD Athlon(tm) II X2 250 Processor': False,
        'AMD Athlon(tm) 64 X2 Dual Core Processor 4200+': False,
        'Intel(R) Atom(TM) CPU N270 @ 1.60GHz': False,
        'Intel(R) Atom(TM) CPU D2700 @ 2.13GHz': False,
        'AMD A8-3850 APU with Radeon(tm) HD Graphics': False,
        'AMD A6-3420M APU with Radeon(tm) HD Graphics': False,
        'Intel(R) Atom(TM) CPU Z3735F @ 1.33GHz': None,   # (Silvermont: SSE4.2)
        'AMD A10-7850K Radeon R7, 12 Compute Cores 4C+8G': None,
        'AMD Athlon(tm) X4 860K Quad Core Processor': None,
        'Intel(R) Core(TM) i7 CPU 920 @ 2.67GHz': None,
        'Intel(R) Core(TM) i7-3770 CPU @ 3.40GHz': None,
        'AMD Ryzen 5 3600 6-Core Processor': None,
        'Intel(R) Celeron(R) N4000 CPU': None,
        # Core 2's Pentiums, Celerons and Xeons; Pentium 4 and D
        'Pentium(R) Dual-Core CPU E5700 @ 3.00GHz': False,
        'Pentium(R) Dual-Core CPU T4200 @ 2.00GHz': False,
        'Intel(R) Pentium(R) Dual CPU E2180 @ 2.00GHz': False,
        'Intel(R) Pentium(R) 4 CPU 3.00GHz': False,
        'Intel(R) Pentium(R) D CPU 3.40GHz': False,
        'Intel(R) Celeron(R) CPU E3300 @ 2.50GHz': False,
        'Celeron(R) Dual-Core CPU T3000 @ 1.80GHz': False,
        'Intel(R) Celeron(R) CPU 430 @ 1.80GHz': False,
        'Intel(R) Celeron(R) CPU 900 @ 2.20GHz': False,
        'Intel(R) Celeron(R) M CPU 520 @ 1.60GHz': False,
        'Intel(R) Xeon(R) CPU E5450 @ 3.00GHz': False,
        'Intel(R) Xeon(R) CPU X5460 @ 3.16GHz': False,
        'Intel(R) Xeon(R) CPU 5160 @ 3.00GHz': False,
        'Intel(R) Xeon(R) CPU X3360 @ 2.83GHz': False,
        'Intel(R) Pentium(R) CPU G620 @ 2.60GHz': None,       # (Sandy Bridge)
        'Intel(R) Pentium(R) CPU G6950 @ 2.80GHz': None,      # (Clarkdale)
        'Intel(R) Pentium(R) CPU P6200 @ 2.13GHz': None,      # (Arrandale)
        'Intel(R) Pentium(R) CPU N3700 @ 1.60GHz': None,      # (Airmont)
        'Intel(R) Pentium(R) Silver N5000 CPU @ 1.10GHz': None,
        'Intel(R) Pentium(R) Gold G5400 CPU @ 3.70GHz': None,
        'Intel(R) Celeron(R) CPU 847 @ 1.10GHz': None,        # (Sandy Bridge)
        'Intel(R) Celeron(R) CPU 787 @ 1.30GHz': None,        # (Sandy Bridge: 7xx left out, Penryn's too)
        'Intel(R) Celeron(R) CPU G530 @ 2.40GHz': None,
        'Intel(R) Celeron(R) CPU B820 @ 1.70GHz': None,
        'Intel(R) Celeron(R) CPU 1007U @ 1.50GHz': None,
        'Intel(R) Celeron(R) CPU 2957U @ 1.40GHz': None,
        'Intel(R) Celeron(R) CPU N2840 @ 2.16GHz': None,      # (Silvermont)
        'Intel(R) Celeron(R) CPU J1900 @ 1.99GHz': None,
        'Intel(R) Xeon(R) CPU X5650 @ 2.67GHz': None,         # (Westmere)
        'Intel(R) Xeon(R) CPU E5520 @ 2.27GHz': None,         # (Nehalem)
        'Intel(R) Xeon(R) CPU X3440 @ 2.53GHz': None,         # (Lynnfield)
        'Intel(R) Xeon(R) CPU W3520 @ 2.67GHz': None,         # (Bloomfield)
        'Intel(R) Xeon(R) CPU E5-2670 0 @ 2.60GHz': None,
        'Intel(R) Xeon(R) CPU E31230 @ 3.20GHz': None,        # (Sandy Bridge, as its first ones were named)
        'Intel(R) Xeon(R) CPU E3-1230 V2 @ 3.30GHz': None,
        # Atom 230 and 330 (Diamondville)
        'Intel(R) Atom(TM) CPU 230 @ 1.60GHz': False,
        'Intel(R) Atom(TM) CPU 330 @ 1.60GHz': False,
        'Intel(R) Atom(TM) CPU C2758 @ 2.40GHz': None,        # (Avoton)
        'Intel(R) Atom(TM) CPU E3845 @ 1.91GHz': None,        # (Bay Trail)
        'Intel(R) Atom(TM) x5-Z8350 CPU @ 1.44GHz': None,
        # K8 and K10: Athlon, Turion, Sempron
        'AMD Athlon(tm) X2 Dual Core Processor BE-2400': False,
        'AMD Athlon(tm) Dual Core Processor 5050e': False,
        'AMD Athlon(tm) X2 Dual-Core QL-60': False,
        'AMD Athlon(tm) Processor LE-1640': False,
        'AMD Athlon(tm) 7750 Dual-Core Processor': False,
        'AMD Turion(tm) 64 X2 Mobile Technology TL-60': False,
        'AMD Turion(tm) II Dual-Core Mobile M520': False,
        'AMD Turion(tm) II Neo K625 Dual-Core Processor': False,
        'AMD Sempron(tm) Processor 3100+': False,
        'AMD Sempron(tm) Processor LE-1250': False,
        'AMD Sempron(tm) 140 Processor': False,
        'AMD Sempron(tm) X2 190 Dual Core Processor': False,
        'AMD Athlon(tm) X2 340 Dual Core Processor': None,    # (Trinity)
        'AMD Athlon(tm) X4 750K Quad Core Processor': None,
        'AMD Athlon(tm) 5350 APU with Radeon(tm) R3': None,   # (Jaguar)
        'AMD Athlon(tm) Silver 3050U with Radeon Graphics': None,
        'AMD Athlon 3000G with Radeon Vega Graphics': None,
        'AMD Sempron(tm) 2650 APU with Radeon(tm) R3': None,  # (Jaguar)
        'AMD Sempron(tm) 3850 APU with Radeon(tm) R3': None,
        # Bobcat; Llano's E2-3200
        'AMD E-350 Processor': False,
        'AMD E-450 APU with Radeon(tm) HD Graphics': False,
        'AMD C-60 APU with Radeon(tm) HD Graphics': False,
        'AMD E1-1200 APU with Radeon(tm) HD Graphics': False,
        'AMD E2-1800 APU with Radeon(tm) HD Graphics': False,
        'AMD E2-3200 APU with Radeon(tm) HD Graphics': False,
        'AMD E2-3000 APU with Radeon(TM) HD Graphics': None,   # (Jaguar)
        'AMD E1-2500 APU with Radeon(TM) HD Graphics': None,
        'AMD E2-3800 APU with Radeon(TM) HD Graphics': None,
        'AMD E1-6010 APU with AMD Radeon R2 Graphics': None,   # (Puma)
        'AMD E2-9000e RADEON R2, 4 COMPUTE CORES 2C+2G': None,  # (Excavator)
        'AMD A4-5000 APU with Radeon(TM) HD Graphics': None,   # (Jaguar)
        'AMD R-464L APU with Radeon(tm) HD Graphics': None,    # (Trinity)
    }

    def test_sse42_by_processor_name(self):
        names = '; '.join(f"'{n}' = (Test-Sse42ByName '{n}')" for n in self.SSE42_BY_NAME)
        got = ps('@{ ' + names + ' }')
        for name, want in self.SSE42_BY_NAME.items():
            self.assertEqual(got[name], want, name)

    def test_pc_check_without_csharp_compiler(self):
        # a PC whose .NET C# compiler is broken: Add-Type -MemberDefinition stopped step 2 with
        # "Unable to find messages file 'cscui.dll'"; the AVX2 question must not need the compiler
        mocks, step = PC_MOCKS, PC_STEP
        got = ps(mocks + "@{ sse2 = (Test-CpuFeature 10); avx2 = (Test-CpuFeature 40) }")
        self.assertIs(got['sse2'], True)              # every x64 processor: Windows answered
        self.assertIn(got['avx2'], (True, False))
        out = ps_text(mocks + step)
        self.assertNotIn('cscui', out)
        self.assertIn('AMD Ryzen 7 3700X 8-Core Processor - has', out)
        isa = "; 'isa=' + $env:SSBU_ISA"   # what the build is told (the installer's environment is the build's)
        unknown = "$env:SSBU_ISA = $null; function Test-CpuFeature { $null }; "   # Windows cannot be asked: the name
        out = ps_text(mocks + unknown + step + isa)
        self.assertIn('AMD Ryzen 7 3700X 8-Core Processor - has AVX2', out)
        self.assertIn('isa=avx2', out)   # (and the build is told: it cannot ask either)
        self.assertNotIn('STOPPED', out)
        out = ps_text(mocks + unknown + "$env:SSBU_ISA = 'x86-64-v2'; " + step + isa)
        self.assertIn('isa=x86-64-v2', out)   # what the person set stays
        # SSE4.2 without AVX2: built for older processors (x86-64-v2), said, not stopped
        out = ps_text(mocks + "$env:SSBU_ISA = $null; function Test-CpuFeature { param([int]$Feature) $Feature -ne 40 }; "
                      + step + isa)
        self.assertIn('AMD Ryzen 7 3700X 8-Core Processor - has no AVX2: the game is built for older processors (x86-64-v2)', out)
        self.assertIn('isa=\n', out.replace('\r', '') + '\n')   # (the build asks Windows itself)
        self.assertNotIn('STOPPED', out)
        out = ps_text(mocks + "function Test-CpuFeature { $false }; " + step)   # below x86-64-v2: stopped
        self.assertIn('STOPPED: The processor (AMD Ryzen 7 3700X 8-Core Processor) has no SSE4.2', out)
        # Windows before 10 version 2004 (build 19041) cannot be asked: the name, which stops a family without SSE4.2
        older = mocks.replace('19045', '18363')
        for name, said in (('Intel(R) Core(TM)2 Duo CPU E8400', 'STOPPED: The processor (Intel(R) Core(TM)2 Duo CPU E8400) '
                                                                 'has no SSE4.2'),
                           ('Intel(R) Core(TM) i7-3770 CPU @ 3.40GHz', 'Intel(R) Core(TM) i7-3770 CPU @ 3.40GHz - has no AVX2: '
                                                                        'the game is built for older processors (x86-64-v2)')):
            with self.subTest(name):
                out = ps_text(older.replace('AMD Ryzen 7 3700X 8-Core Processor', name) + "$env:SSBU_ISA = $null; " + step + isa)
                self.assertIn(said, out)
                self.assertNotIn('isa=avx2', out)

    def test_pc_check_keeps_the_build_folder_s_choice(self):
        # What build\isa.txt says (tools/isa.py): a choice SSBU_ISA made for the last build ('told', what the game's
        # start-up message has a player do: SSBU_ISA=x86-64-v2) is not overridden by the name on a Windows that cannot be
        # asked (before build 19041); a folder an older version built (ssbu.exe, no isa.txt) stays avx2 when the name
        # does not say. What the installer sets is what the build is told (its environment is the build's).
        def check(name, files, windows='18363', features='$null', env='$null'):
            with tempfile.TemporaryDirectory(prefix='installer-test-') as tmp:
                for rel, text in files.items():
                    Path(tmp, 'build').mkdir(exist_ok=True)
                    Path(tmp, 'build', rel).write_text(text, newline='\n')
                mocks = PC_MOCKS.replace('AMD Ryzen 7 3700X 8-Core Processor', name).replace('19045', windows)
                out = ps_text(mocks + f"$Root = '{tmp}'; $env:SSBU_ISA = {env}; "
                              f"function Test-CpuFeature {{ param([int]$Feature) {features} }}; "
                              + PC_STEP + "; 'isa=' + $env:SSBU_ISA")
            return out.replace('\r', '') + '\n'
        avx2, unknown = 'Intel(R) Core(TM) i5-4590 CPU @ 3.30GHz', 'Intel(R) Xeon(R) CPU E5-2680 v3 @ 2.50GHz'
        told = 'x86-64-v2\ntold\n# the instruction set this folder\'s build is compiled for\n'
        out = check(avx2, {'isa.txt': told, 'ssbu.exe': 'MZ'})
        self.assertIn(f'{avx2} - has AVX2: the game stays built for x86-64-v2, as SSBU_ISA chose for the last build', out)
        self.assertIn('isa=\n', out)   # (not told avx2: the build keeps build\isa.txt's choice)
        self.assertNotIn('STOPPED', out)
        out = check(avx2, {'isa.txt': 'avx2\ntold\n'})   # (the installer's own SSBU_ISA=avx2, kept)
        self.assertIn('the game stays built for avx2, as SSBU_ISA chose for the last build', out)
        self.assertIn('isa=\n', out)
        out = check(avx2, {'isa.txt': 'x86-64-v2\nchosen\n'})   # (not told: the name says, as before)
        self.assertIn('isa=avx2\n', out)
        out = check(avx2, {'isa.txt': told}, env="'avx2'")   # (SSBU_ISA set: what the person set stays)
        self.assertIn('isa=avx2\n', out)
        self.assertNotIn('stays built for', out)
        # Windows asked (build 19041 and later): a told choice is kept on a processor with AVX2, and said
        out = check(avx2, {'isa.txt': told}, windows='19045', features='$true')
        self.assertIn(f'{avx2} - has AVX2: the game stays built for x86-64-v2, as SSBU_ISA chose', out)
        self.assertIn('isa=\n', out)
        out = check(avx2, {'isa.txt': told}, windows='19045', features='$Feature -ne 40')   # (one without: x86-64-v2 anyway)
        self.assertIn(f'{avx2} - has no AVX2: the game is built for older processors (x86-64-v2)', out)
        # a name that does not say: the folder an older version built stays avx2; with no build, x86-64-v2
        out = check(unknown, {'ssbu.exe': 'MZ'})
        self.assertIn(f'{unknown} - whether it has AVX2 is unknown: the game stays built for avx2, as it was (Windows cannot '
                      'be asked; if the game then says the processor lacks AVX2, build it with SSBU_ISA=x86-64-v2', out)
        self.assertIn('isa=\n', out)
        out = check(unknown, {'isa.txt': 'x86-64-v2\nchosen\n', 'ssbu.exe': 'MZ'})
        self.assertIn('the game stays built for x86-64-v2, as it was (Windows cannot be asked; SSBU_ISA=avx2 builds for AVX2', out)
        out = check(unknown, {})
        self.assertIn(f'{unknown} - whether it has AVX2 is unknown: the game is built for older processors (x86-64-v2)', out)
        self.assertIn('isa=\n', out)

    def test_source_version(self):
        sha = '9fdc8fd21b3fde86c3e58a1e634a7e76a87220c7'
        got = ps(f"@{{ zip = (Read-SourceVersion '{sha.upper()} 2026-10-19'); git = (Read-SourceVersion '$Format:%H %cs$'); "
                 f"none = (Read-SourceVersion ''); long = (Read-SourceVersion '{sha}0') }}")
        self.assertEqual(got['zip'], sha)
        self.assertIsNone(got['git'])     # a git checkout: the placeholder
        self.assertIsNone(got['none'])
        self.assertIsNone(got['long'])    # 41 hex digits are not a commit

    def test_source_version_filled_in_by_downloads(self):
        attributes = (ROOT / '.gitattributes').read_text()
        self.assertIn('tools/source_version.txt export-subst', attributes.splitlines())
        self.assertIn('$Format:%H', (ROOT / 'tools/source_version.txt').read_text())

    def test_update_decision(self):
        got = ps("@{ a = (Get-UpdateDecision 'ahead'); i = (Get-UpdateDecision 'identical'); b = (Get-UpdateDecision 'behind'); "
                 "d = (Get-UpdateDecision 'diverged'); x = (Get-UpdateDecision 'something') }")
        self.assertEqual(got, {'a': 'update', 'i': 'current', 'b': 'newer', 'd': 'diverged', 'x': 'unknown'})

    def test_removed_paths(self):
        got = ps("@{ r = (Get-RemovedPaths @('tools/a.py', 'tools/old.py', 'README.md', 'docs/Gone.md', '../evil.txt', 'x/../../y', 'C:/z') "
                 "@('tools/a.py', 'readme.md', 'tools/new.py')); none = (Get-RemovedPaths @('a') @('A')) }")
        self.assertEqual(got['r'], ['tools/old.py', 'docs/Gone.md'])   # case-only renames kept, escapes refused
        self.assertEqual(got['none'], [])

    def test_online_server_merge(self):
        head = '# The online lobby\n# host:port, then an optional lobby code\n'
        old, new = head + 'lobby.example:39420\n', head + 'lobby.example:39421\n'
        cases = {
            'untouched': (old, old, new, new),
            'code added': (head + 'lobby.example:39420 FRIENDS\n', old, new, head + 'lobby.example:39421 FRIENDS\n'),
            'code, CRLF': ((head + 'lobby.example:39420   FRIENDS\n').replace('\n', '\r\n'), old, new, head + 'lobby.example:39421 FRIENDS\n'),
            'own lobby': (head + 'my.server:5000\n', old, new, head + 'my.server:5000\n'),
            'off': (head + 'off\n', old, new, head + 'off\n'),
            'comment only': ('# mine\n' + head + 'lobby.example:39420\n', old, new, new),
            'old unknown': (head + 'lobby.example:39420 FRIENDS\n', None, new, head + 'lobby.example:39420 FRIENDS\n'),
            'no file': (None, old, new, new),
            'same as new': (new, old, new, new),
        }
        def lit(s):
            return '$null' if s is None else "'" + s.replace("'", "''") + "'"
        expr = '@{ ' + '; '.join(f"'{k}' = (Merge-OnlineServer {lit(l)} {lit(o)} {lit(n)})" for k, (l, o, n, _) in cases.items()) + ' }'
        got = ps(expr)
        for k, (_l, _o, _n, want) in cases.items():
            self.assertEqual(got[k].replace('\r\n', '\n'), want, k)

    def test_config_path(self):
        """online_name.txt where the game reads it (runtime/config_folder.h): config\\ once the game has made it."""
        with tempfile.TemporaryDirectory() as tmp:
            expr = (f"$r = '{tmp}'; @(foreach ($n in 'online_name.txt', 'online_server.txt') "
                    "{ (Get-ConfigPath $r $n).Substring($r.Length) })")
            self.assertEqual(ps(expr), ['\\online_name.txt', '\\online_server.txt'])
            os.makedirs(os.path.join(tmp, 'config'))
            Path(tmp, 'config', 'version.ini').write_text('version = 1\n')
            self.assertEqual(ps(expr), ['\\config\\online_name.txt', '\\online_server.txt'])
            Path(tmp, 'config', 'online_server.txt').write_text('off\n')
            self.assertEqual(ps(expr), ['\\config\\online_name.txt', '\\config\\online_server.txt'])

    def test_command_line_round_trip(self):
        args = ['plain', 'with space', 'quote"inside', 'trailing\\', 'back\\"quote', 'C:\\My Dump\\', '']
        lit = ', '.join("'" + a.replace("'", "''") + "'" for a in args)
        line = ps(f"@{{ line = (Join-CommandLine @({lit})) }}")['line']
        r = subprocess.run(f'"{sys.executable}" -c "import sys, json; print(json.dumps(sys.argv[1:]))" {line}',
                           capture_output=True, text=True)
        self.assertEqual(json.loads(r.stdout), args, line)

    def test_restart_arguments(self):
        got = ps("@{ a = (Get-RestartArguments @{ DryRun = [switch]$true; SkipBuild = [switch]$false; Game = 'D:\\My Dump'; Jobs = 4 }); "
                 "b = (Get-RestartArguments @{ NoUpdate = [switch]$true }); c = (Get-RestartArguments @{}) }")
        self.assertEqual(got['a'], ['-DryRun', '-Game', 'D:\\My Dump', '-Jobs', '4', '-NoUpdate'])
        self.assertEqual(got['b'], ['-NoUpdate'])
        self.assertEqual(got['c'], ['-NoUpdate'])

    def test_compiler_step_without_clang(self):
        # (the first run on a PC without the compiler: no Visual Studio, one edition, two editions, no winget;
        # the dry run says the download route comes first, with msvc_deps.py plan's lines, then the Build Tools)
        with tempfile.TemporaryDirectory(prefix='installer-test-') as tmp:
            Path(tmp, 'setup.exe').write_bytes(b'')
            mocks = (f"$DryRun = $true; $VsInstallerDir = '{tmp}'; "
                     "function Get-PythonOutput { param([string[]]$Arguments) $global:planned = ($Arguments -join ' '); "
                     "  @{ Output = ('LLVM 23.1.3 from GitHub: https://x/llvm.tar.zst' + [char]10 + "
                     "'  only after you accept Microsoft''s license terms: https://x/license'); Code = 0 } }; "
                     "function Test-Compiler { @{ Ok = $false; Output = 'ERROR: clang-cl.exe was not found' } }; ")
            winget = "function Get-Command { [pscustomobject]@{ Source = 'C:\\winget.exe' } }; "
            one = "@{ Path = 'C:\\VS\\BuildTools'; Product = 'Microsoft.VisualStudio.Product.BuildTools' }"
            two = one + "; @{ Path = 'C:\\VS\\Community'; Product = 'Microsoft.VisualStudio.Product.Community' }"
            cases = {
                'none': (winget + "function Get-VsInstances { return @() }; ", 'would run: winget install --id Microsoft.VisualStudio.BuildTools'),
                'one': (winget + f"function Get-VsInstances {{ {one} }}; ", 'Visual Studio in C:\\VS\\BuildTools: setup.exe modify'),
                'two': (winget + f"function Get-VsInstances {{ {two} }}; ", 'Visual Studio in C:\\VS\\BuildTools: setup.exe modify'),
                'no winget': ("function Get-Command { $null }; function Get-VsInstances { }; ", 'visual-cpp-build-tools'),
            }
            for name, (setup, want) in cases.items():
                out = ps_text(mocks + setup + "try { Step-Compiler } catch { 'STOPPED: ' + $_.Exception.Message }; 'PLAN: ' + $global:planned")
                self.assertIn(want, out, name)
                self.assertNotIn("property 'Count' cannot be found", out, name)
                self.assertIn('would run tools\\msvc_deps.py install', out, name)
                self.assertIn("only after you accept Microsoft's license terms: https://x/license", out, name)
                self.assertIn('msvc_deps.py plan', out, name)
                self.assertLess(out.index('msvc_deps.py install'), out.index(want), name)   # that route first
                if name != 'no winget':
                    self.assertNotIn('STOPPED', out, name)
            out = ps_text(mocks + cases['none'][0] + "$BuildTools = $true; Step-Compiler")   # -BuildTools: straight to them
            self.assertNotIn('msvc_deps', out)
            self.assertIn('would run: winget install --id Microsoft.VisualStudio.BuildTools', out)

    def test_compiler_step_download_route(self):
        # the download route's outcomes: it works; declined (stops: the Build Tools are under the same
        # license); it fails, or works but the check after it does not (removed): the Build Tools next
        with tempfile.TemporaryDirectory(prefix='installer-test-') as tmp:
            Path(tmp, 'setup.exe').write_bytes(b'')
            mocks = (f"$VsInstallerDir = '{tmp}'; $global:n = 0; $global:removed = $false; $global:vs = $false; "
                     "function Test-Compiler { $global:n++; if ($global:n -le $global:fails) { @{ Ok = $false; Output = 'ERROR: no compiler' } } "
                     "  else { @{ Ok = $true; Output = ('  clang-cl: C:\\x\\clang-cl.exe' + [char]10 + '  toolchain: deps\\llvm (LLVM 23.1.3) with deps\\msvc') } } }; "
                     "function Install-PortableCompiler { $global:how }; "
                     "function Remove-PortableCompiler { $global:removed = $true }; "
                     "function Get-VsInstances { @{ Path = 'C:\\VS\\BuildTools'; Product = 'Microsoft.VisualStudio.Product.BuildTools' } }; "
                     "function Start-Process { param($FilePath, $ArgumentList, $Verb, [switch]$Wait, [switch]$PassThru) "
                     "  $global:vs = $true; [pscustomobject]@{ ExitCode = 0 } }; ")
            report = "; 'REMOVED: ' + $global:removed; 'VS: ' + $global:vs; 'CHECKS: ' + $global:n"
            out = ps_text(mocks + "$global:how = 'done'; $global:fails = 1; Step-Compiler" + report)
            self.assertIn('toolchain: deps\\llvm (LLVM 23.1.3)', out)
            self.assertIn('VS: False', out)
            self.assertIn('CHECKS: 2', out)
            out = ps_text(mocks + "$global:how = 'declined'; $global:fails = 1; try { Step-Compiler } catch { 'STOPPED: ' + $_.Exception.Message }" + report)
            self.assertIn("STOPPED: Microsoft's license terms were not accepted", out)
            self.assertIn('VS: False', out)
            out = ps_text(mocks + "$global:how = 'failed'; $global:fails = 1; Step-Compiler" + report)
            self.assertIn('installing the Visual Studio Build Tools instead', out)
            self.assertIn('VS: True', out)
            self.assertIn('REMOVED: False', out)
            out = ps_text(mocks + "$global:how = 'done'; $global:fails = 2; Step-Compiler" + report)
            self.assertIn('the downloaded compiler did not pass the check', out)
            self.assertIn('REMOVED: True', out)
            self.assertIn('VS: True', out)
            self.assertIn('CHECKS: 3', out)

    def test_zstd_check_before_python_314(self):
        # Python 3.13 and older have no compression package: asking for compression.zstd raised
        # there, so a Python with zstandard installed was taken for one without it
        check = ps("@{ code = $ZstdCheck }")['code']
        self.assertNotIn('"', check)
        older = ("import sys, types, importlib.machinery; sys.modules['compression'] = None; "
                 "sys.version_info = (3, 13, 7, 'final', 0); ")
        fake_zstandard = ("m = types.ModuleType('zstandard'); m.__spec__ = importlib.machinery.ModuleSpec('zstandard', None); "
                          "sys.modules['zstandard'] = m; ")
        no_zstandard = "import sys; sys.modules['zstandard'] = None; "
        def run(prefix):
            return subprocess.run([sys.executable, '-c', prefix + f'exec({check!r})'], capture_output=True, text=True)
        r = run(older + fake_zstandard)
        self.assertEqual(r.returncode, 0, r.stderr)                 # 3.13 with zstandard: there
        r = run(older + no_zstandard)
        self.assertEqual((r.returncode, r.stderr), (1, ''))         # 3.13 without: a plain no, no error
        if sys.version_info >= (3, 14):
            r = run(no_zstandard)
            self.assertEqual(r.returncode, 0, r.stderr)             # 3.14: compression.zstd

    def test_small_decisions(self):
        got = ps("@{ fw = (Get-FirewallRuleCommand 'C:\\WindowSmash\\build\\ssbu.exe' 'WindowSmash (ssbu.exe)'); "
                 "v1 = (Test-PythonVersionText 'Python 3.14.6'); v2 = (Test-PythonVersionText '3.10.0'); "
                 "v3 = (Test-PythonVersionText '3.9.13'); v4 = (Test-PythonVersionText 'Python was not found'); "
                 "v5 = (Test-PythonVersionText '2.7.18'); "
                 "d1 = (Get-DiskNeedGb 60 25); d2 = (Get-DiskNeedGb 60 70) }")
        self.assertIn('New-NetFirewallRule', got['fw'])
        self.assertIn("-Program 'C:\\WindowSmash\\build\\ssbu.exe'", got['fw'])
        self.assertIn("-DisplayName 'WindowSmash (ssbu.exe)'", got['fw'])
        self.assertIn('-Direction Inbound', got['fw'])
        self.assertEqual([got['v1'], got['v2'], got['v3'], got['v4'], got['v5']], [True, True, False, False, False])
        self.assertEqual(got['d1'], 35)
        self.assertEqual(got['d2'], 0)


class InstallerPatternTests(unittest.TestCase):
    """Test-Sse42ByName's patterns, read from the installer and tried with Python's re where PowerShell is not there to
    run test_sse42_by_processor_name: the same names. Not case-sensitive, as PowerShell's -match; the constructs they
    use (\\b, \\s, \\d, classes, groups, alternation) mean the same to .NET's regex."""
    def test_sse42_by_processor_name(self):
        text = SCRIPT.read_text(encoding='utf-8')
        body = text[text.index('function Test-Sse42ByName'):]
        body = body[:body.index('\n}\n')]
        families = re.findall(r"'([^']*)'", body[body.index('$families = @('):body.index('foreach')])
        self.assertGreaterEqual(len(families), 14)
        self.assertIn('if ($Name -match $family) { return $false }', body)
        for name, want in InstallerDecisionTests.SSE42_BY_NAME.items():
            got = False if any(re.search(family, name, re.I) for family in families) else None
            self.assertEqual(got, want, name)


    def test_config_path_as_the_launcher(self):
        """The installer finds online_name.txt by the launcher's rule (tests/test_launcher_ini.py runs that one)."""
        def rule(text):
            body = text[text.index('function Get-ConfigPath'):]
            body = body[body.index('$inFolder ='):]
            return body[:body.index('\n}\n')].replace('\r', '')
        text = SCRIPT.read_text(encoding='utf-8')
        self.assertEqual(rule(text), rule((ROOT / 'tools/launcher.ps1').read_text(encoding='utf-8')))
        self.assertIn("$nameFile = Get-ConfigPath $Root 'online_name.txt'", text)
        self.assertNotRegex(text, r"Join-Path \$Root '(display\.ini|controllers\.ini|launcher\.ini|online_name\.txt|online_direct\.txt)'")


if __name__ == '__main__':
    unittest.main()
