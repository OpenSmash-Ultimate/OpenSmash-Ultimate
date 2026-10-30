<#
.SYNOPSIS
Sets WindowSmash up on this PC in one go: what "Install WindowSmash.bat" runs.

.DESCRIPTION
Steps, each skipped when it is already done, so running it again carries on where it stopped:
  1. update    this folder brought to the newest commit of GitHub's main branch, then the updated
               installer run in its place. A copy from GitHub's ZIP knows its commit (tools\
               source_version.txt, filled in by the download: export-subst); GitHub says whether
               main is ahead of it; if so the new ZIP's files replace the changed ones (with fresh
               file times, so the build redoes what changed), files the new version dropped are
               removed, and online_server.txt keeps a lobby code or address you set. A git checkout
               of main with no changes of its own is fast-forwarded with git pull. Anything else
               (another branch, a commit GitHub does not have, no internet) is left as it is.
  2. check     64-bit Windows 10 or 11, a processor with SSE4.2 (x86-64-v2; with AVX2 the game
               is built for AVX2, without it for older processors, expected to be slower: tools\
               isa.py; when Windows cannot be asked, the processor's name: SSBU_ISA=avx2 is set for
               the build when it says AVX2, unless SSBU_ISA chose for the last build, which build\
               isa.txt keeps, and a family without SSE4.2 stops it), the installed RAM, the free
               space on this drive (about 60 GB for the build). A failed check stops with a plain
               message.
  3. python    a Python 3.10 or newer with the packages (deps\python, then python on the PATH, then
               the py launcher); otherwise python.org's embeddable Python 3.14 into deps\python, with
               pip and requirements.txt. Every later Python call uses the one found.
  4. compiler  clang-cl, found and tried by tools\build_game.py --check; otherwise first the compiler
               without Visual Studio: tools\msvc_deps.py install puts LLVM (from GitHub) and
               Microsoft's C runtime and Windows SDK (from Microsoft's servers, once you accept
               Microsoft's license terms, which it shows) into deps\llvm and deps\msvc, about 570 MB
               downloaded, no administrator rights. If that download or the check after it fails
               (or with -BuildTools): the Visual Studio Build Tools with the C++ workload and the
               Clang component, through winget (a few GB), or the missing components added to a
               Visual Studio already there (asks for administrator rights); without winget, the
               manual step and its link. Microsoft's license declined: it stops (the Build Tools
               come under the same license).
  5. sdl3      deps\SDL3 from the newest SDL3-devel-<version>-VC.zip on GitHub, checked against the
               SHA-256 the release lists (controllers other than Xbox pads).
  6. codecs    nothing to fetch: the music and movie codecs are built from third_party\ during the
               game build (tools\codec_libs.py).
  7. files     a window asking where your game files, keys and firmware are, filled in from yuzu's
               or Ryujinx's folders when they have them, showing what was found, and your online
               name (online_name.txt), so the first start asks nothing. The firmware is optional,
               for your console's own fonts: left empty, the game uses the open fonts of
               third_party\fonts_open (no Chinese, Japanese or Korean text).
  8. extras    a desktop shortcut to OpenSmash.exe and, if ticked, a Windows Firewall rule for
               build\ssbu.exe (asks for administrator rights once).
  9. build     tools\build_game.py with those folders: the game's program, fonts, data, DLC,
               fighters, then the build itself (30 minutes to a few hours; minutes after an update).
 10. shaders   if ticked, compiled in this same window. The setup window asks low, medium, or high.
Everything is written to install.log next to this folder's launchers.

Why the embeddable Python and not `winget install Python.Python.3.14`: the embeddable zip goes
into deps\python and touches nothing else on the PC (no PATH, no file associations, no second
Python for other programs to find), it needs no installer run, and it is 11 MB. Its drawbacks do
not matter here: it has no tkinter (this wizard is WinForms) and no pip until get-pip.py adds it.
winget's package would be the choice for a Python the user wants to keep for other things.

.PARAMETER DryRun
Say what each step would do and do nothing: no downloads, no installs, no wizard, no build.
.PARAMETER Game
.PARAMETER Keys
.PARAMETER Firmware
The folders, instead of the wizard's auto-detection (the wizard still shows them unless -NoWizard).
The firmware is optional.
.PARAMETER NoFirmware
No firmware, over one found: the game uses the open fonts of third_party\fonts_open.
.PARAMETER OnlineName
The online name to write into online_name.txt.
.PARAMETER NoWizard
Ask in this window instead of opening the wizard. With -Game and -Keys, those answers are used and nothing is asked.
.PARAMETER Shortcut
.PARAMETER Firewall
.PARAMETER Shaders
With -NoWizard -Game -Keys: put the desktop shortcut on, add the firewall rule, and prepare shaders after the build. The setup window asks these when it opens.
.PARAMETER SkipBuild
Prepare the game files (program, fonts, data, DLC, fighters) but stop before the build itself.
.PARAMETER Jobs
Passed to the build: how many files to compile at a time (default: by the installed RAM).
.PARAMETER NoUpdate
Do not look for a newer WindowSmash (the updated installer is started with it).
.PARAMETER BuildTools
For the compiler, install the Visual Studio Build Tools rather than download it into deps\.
.PARAMETER Rebuild
Build the game even when nothing that goes into it changed (without it, an update that changed only
the mod manager or other tools skips the build: tools\skyline.py uptodate).
.PARAMETER AcceptLicense
The setup window's checkbox: Microsoft's license terms for the C runtime and Windows SDK are already
accepted, so the compiler download does not ask again.
#>
[CmdletBinding()]
param(
    [switch]$DryRun,
    [string]$Game,
    [string]$Keys,
    [string]$Firmware,
    [switch]$NoFirmware,
    [string]$OnlineName,
    [switch]$NoWizard,
    [switch]$Shortcut,
    [switch]$Firewall,
    [switch]$Shaders,
    [switch]$SkipBuild,
    [int]$Jobs,
    [switch]$NoUpdate,
    [switch]$BuildTools,
    [switch]$Rebuild,
    [switch]$AcceptLicense
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ScriptBound = $PSBoundParameters   # (this script's own arguments, to start the updated copy with)
$Root = Split-Path -Parent $PSScriptRoot
$Script:Python = $null            # the Python command: @(exe) or @('py', '-3')
$Script:WizardSnapshot = $null    # set to a .png path to have the wizard save a picture of itself and accept (tests)
$Script:Kernel32 = $null          # the type Test-CpuFeature declares IsProcessorFeaturePresent on (made once)

# ---- what to fetch, and from where -----------------------------------------------------------
$PythonMinor = '3.14'
$PythonFallbackVersion = '3.14.8'   # when python.org's listing cannot be read
$PythonFtp = 'https://www.python.org/ftp/python/'
$GetPipUrl = 'https://bootstrap.pypa.io/get-pip.py'
$SdlReleasesUrl = 'https://api.github.com/repos/libsdl-org/SDL/releases?per_page=30'
$SdlFallback = @{   # when GitHub's API cannot be read: a known release and its listed SHA-256
    Version = '3.4.18'
    Url = 'https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-VC.zip'
    Sha256 = '78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd'
}
$VsBuildToolsId = 'Microsoft.VisualStudio.BuildTools'   # Visual Studio 2026's; 2022's is Microsoft.VisualStudio.2022.BuildTools
$VsBuildToolsUrl = 'https://visualstudio.microsoft.com/visual-cpp-build-tools/'
$VsInstallerDir = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
$NeedDiskGb = 60
$MinRamGb = 8
$FirewallRuleName = 'WindowSmash (ssbu.exe)'
$UserAgent = 'WindowSmash-installer'
# Python code that exits 0 when tools\arc.py has a zstd: the zstandard package (any Python), or
# compression.zstd (3.14 and newer). zstandard is asked first: before 3.14, asking for
# compression.zstd raises an error instead of answering no (there is no compression package).
# (No double quotes in it: Windows PowerShell hands them to programs unescaped.)
$ZstdCheck = 'import sys, importlib.util as u; sys.exit(0 if u.find_spec(''zstandard'') or (sys.version_info >= (3, 14) and u.find_spec(''compression.zstd'')) else 1)'
# where updates come from: this repository's branch
$RepoOwner = 'OpenSmash-Ultimate'
$RepoName = 'OpenSmash-Ultimate'
$UpdateBranch = 'main'
$RepoApi = "https://api.github.com/repos/$RepoOwner/$RepoName"
$SourceVersionFile = 'tools\source_version.txt'   # "<commit> <date>": GitHub's ZIP download fills it in

# ---- pure functions: what the steps decide by (tested in tests/test_installer.py) --------------
function Read-SourceVersion {
    <# The commit a source_version.txt names, in lower case, or $null: a ZIP from GitHub has it filled
       in (git archive's export-subst), a git checkout has the $Format placeholder instead. #>
    param([string]$Text)
    if (-not $Text) { return $null }
    $m = [regex]::Match($Text, '(?<![0-9a-fA-F])([0-9a-fA-F]{40})(?![0-9a-fA-F])')
    if ($m.Success) { return $m.Groups[1].Value.ToLower() }
    return $null
}

function Get-UpdateDecision {
    <# What to do, from GitHub's comparison of this copy's commit (the base) with the branch (the head). #>
    param([string]$Status)
    switch ($Status) {
        'identical' { return 'current' }
        'ahead' { return 'update' }       # the branch has commits this copy lacks, and none the other way
        'behind' { return 'newer' }       # this copy has commits the branch lacks
        'diverged' { return 'diverged' }  # both: a copy of another branch
        default { return 'unknown' }
    }
}

function Get-RemovedPaths {
    <# The old version's files that the new one no longer has ('/' separators; names compared without
       case, as Windows does). A path that could reach outside the folder is never returned. #>
    param([string[]]$Old, [string[]]$New)
    $keep = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($p in $New) { [void]$keep.Add($p) }
    $out = @()
    foreach ($p in $Old) {
        if ($keep.Contains($p)) { continue }
        if ($p -match '(^|[/\\])\.\.([/\\]|$)' -or $p -match '^[/\\]' -or $p.Contains(':')) { continue }
        $out += $p
    }
    return , $out
}

function Get-LobbyLine {
    <# The line of an online_server.txt that Play SSBU.bat reads (the first that is not empty or a
       comment): @{Address; Code; Index}, or $null. #>
    param([string[]]$Lines)
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $t = $Lines[$i].Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        $parts = @($t -split '\s+')
        $code = $null
        if ($parts.Count -gt 1) { $code = $parts[1] }
        return @{ Address = $parts[0]; Code = $code; Index = $i }
    }
    return $null
}

function Merge-OnlineServer {
    <# online_server.txt after an update. Unchanged by you: the new version's. You only added a lobby
       code to the address the old version had: the new version's address with your code. Another
       address, or "off": yours, as it is. With no old version to compare against: yours. #>
    param([AllowNull()][string]$Local, [AllowNull()][string]$Old, [string]$New)
    if (-not $Local) { return $New }
    $l = ($Local -replace "`r`n", "`n")
    $n = ($New -replace "`r`n", "`n")
    if ($l.TrimEnd() -eq $n.TrimEnd()) { return $New }
    if (-not $Old) { return $Local }
    $o = ($Old -replace "`r`n", "`n")
    if ($l.TrimEnd() -eq $o.TrimEnd()) { return $New }
    $mine = Get-LobbyLine ($l -split "`n")
    $was = Get-LobbyLine ($o -split "`n")
    $newLines = @($n -split "`n")
    $now = Get-LobbyLine $newLines
    if ($mine -and $was -and $now -and $mine.Address -eq $was.Address -and $mine.Address -ne 'off') {
        if (-not $mine.Code) { return $New }
        $newLines[$now.Index] = "$($now.Address) $($mine.Code)"
        return ($newLines -join "`n")
    }
    return $Local
}

function Get-ConfigPath {
    <# Where the game reads a settings file (runtime/config_folder.h): config\ once the game has made it
       (config\version.ini), else the port's folder; online_server.txt is config\'s only once there
       is one there. (tools\launcher.ps1 has the same.) #>
    param([string]$Root, [string]$Name)
    $inFolder = Join-Path (Join-Path $Root 'config') $Name
    if (-not (Test-Path -LiteralPath (Join-Path (Join-Path $Root 'config') 'version.ini') -PathType Leaf) -or
        ($Name -eq 'online_server.txt' -and -not (Test-Path -LiteralPath $inFolder -PathType Leaf))) {
        return Join-Path $Root $Name
    }
    return $inFolder
}

function Join-CommandLine {
    <# One Windows command line of these arguments (Start-Process takes a string: Windows PowerShell
       does not quote the elements of an array), quoted by CommandLineToArgvW's rules. #>
    param([string[]]$Arguments)
    $quoted = foreach ($a in $Arguments) {
        if ($a -eq '') { '""' }
        elseif ($a -notmatch '[\s"]') { $a }
        else {
            $s = [regex]::Replace($a, '(\\*)"', { param($m) ($m.Groups[1].Value * 2) + '\"' })
            $s = [regex]::Replace($s, '(\\+)$', { param($m) $m.Groups[1].Value * 2 })
            '"' + $s + '"'
        }
    }
    return ($quoted -join ' ')
}

function Get-RestartArguments {
    <# The installer's own arguments again, for starting the updated copy, with -NoUpdate added. #>
    param([System.Collections.IDictionary]$Bound)
    $out = @()
    foreach ($k in @($Bound.Keys | Sort-Object)) {
        $v = $Bound[$k]
        if ($v -is [System.Management.Automation.SwitchParameter] -or $v -is [bool]) {
            if ([bool]$v) { $out += "-$k" }
        } else {
            $out += @("-$k", "$v")
        }
    }
    if ($out -notcontains '-NoUpdate') { $out += '-NoUpdate' }
    return , $out
}

function Get-PythonEmbedUrl {
    <# The newest 3.<minor>.x on python.org's listing (its "3.14.8/" links), or the fallback. #>
    param([string]$Listing, [string]$Minor, [string]$Fallback, [string]$Base = $PythonFtp)
    $versions = @()
    foreach ($m in [regex]::Matches($Listing, 'href="(\d+\.\d+\.\d+)/"')) {
        $v = $m.Groups[1].Value
        if ($v.StartsWith($Minor + '.')) { $versions += [version]$v }
    }
    $pick = $Fallback
    if ($versions.Count -gt 0) { $pick = ($versions | Sort-Object -Descending | Select-Object -First 1).ToString() }
    return @{ Version = $pick; Url = "$Base$pick/python-$pick-embed-amd64.zip" }
}

function Select-Sdl3Asset {
    <# The newest release (not a prerelease) with an SDL3-devel-3.x.y-VC.zip, as the GitHub API lists
       them: its version, the zip's URL and the SHA-256 the API gives for it (digest), if any. #>
    param($Releases)
    $best = $null
    foreach ($r in $Releases) {
        if ($r.prerelease -or $r.draft) { continue }
        foreach ($a in $r.assets) {
            $m = [regex]::Match($a.name, '^SDL3-devel-(3\.\d+\.\d+)-VC\.zip$')
            if (-not $m.Success) { continue }
            $v = [version]$m.Groups[1].Value
            if ($null -eq $best -or $v -gt $best.Version) {
                $sha = $null
                if ($a.PSObject.Properties['digest'] -and $a.digest -match '^sha256:([0-9a-fA-F]{64})$') { $sha = $Matches[1].ToLower() }
                $best = @{ Version = $v; Url = $a.browser_download_url; Sha256 = $sha; Name = $a.name }
            }
        }
    }
    return $best
}

function Get-VsBuildToolsArguments {
    <# winget's arguments for the Build Tools with the C++ workload and the Clang component, installed
       passively (a progress window, no questions) and waited for. #>
    param([string]$Id)
    $override = '--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --includeRecommended'
    return @('install', '--id', $Id, '--exact', '--source', 'winget', '--accept-package-agreements',
             '--accept-source-agreements', '--disable-interactivity', '--override', $override)
}

function Get-VsModifyArguments {
    <# The Visual Studio installer's arguments to add the C++ workload and Clang to an edition
       already installed (vswhere's installationPath and productId). #>
    param([string]$InstallPath, [string]$ProductId)
    $workload = 'Microsoft.VisualStudio.Workload.NativeDesktop'
    if ($ProductId -match 'BuildTools') { $workload = 'Microsoft.VisualStudio.Workload.VCTools' }
    # (no --wait: only the bootstrapper takes it, the installer's setup.exe does not; Start-Process -Wait waits)
    return @('modify', '--installPath', $InstallPath, '--add', $workload, '--add', 'Microsoft.VisualStudio.Component.VC.Llvm.Clang',
             '--includeRecommended', '--passive', '--norestart')
}

function Get-VsExitMessage {
    <# What a Visual Studio Installer exit code means for the person, or $null when it succeeded
       (Microsoft's list: "Use command-line parameters to install Visual Studio", Error codes). #>
    param([int]$Code)
    switch ($Code) {
        0 { return $null }
        3010 { return $null }   # done; Windows wants a restart before the tools are used
        1641 { return $null }   # done, and a restart was started
        740 { return 'it needs administrator rights, so allow it when Windows asks' }
        1001 { return 'the Visual Studio Installer is already open, so close it first' }
        1003 { return 'Visual Studio is in use, so close it first' }
        8006 { return 'Visual Studio is running, so close it first' }
        1602 { return 'it was cancelled' }
        5004 { return 'it was cancelled' }
        1618 { return 'another installation is running, so wait for it to finish' }
        5007 { return "it was blocked: it needs administrator rights, or this PC doesn't meet its requirements" }
        87 { return 'it did not accept its options' }
        -1073720687 { return "it couldn't connect to the internet" }
        default { return 'it stopped' }
    }
}

function Get-FolderCandidates {
    <# Where the emulators keep keys, firmware and installed games, most likely first. #>
    param([string]$AppData, [string]$UserProfile)
    $keys = @(); $firmware = @(); $game = @()
    foreach ($name in @('yuzu', 'eden', 'sudachi', 'citron', 'suyu')) {   # yuzu and the forks with its layout
        $keys += "$AppData\$name\keys"
        $firmware += "$AppData\$name\nand\system\Contents\registered"
        $game += "$AppData\$name\nand\user\Contents\registered"
        $game += "$AppData\$name\sdmc\Nintendo\Contents\registered"
    }
    $keys += "$AppData\Ryujinx\system"
    $firmware += "$AppData\Ryujinx\bis\system\Contents\registered"
    $game += "$AppData\Ryujinx\bis\user\Contents\registered"
    $game += "$AppData\Ryujinx\sdcard\Nintendo\Contents\registered"
    $keys += "$UserProfile\.switch"
    return @{ Keys = $keys; Firmware = $firmware; Game = $game }
}

function Resolve-FirstExisting {
    <# The first of the paths for which the test says yes, or $null. #>
    param([string[]]$Paths, [scriptblock]$Exists)
    foreach ($p in $Paths) {
        if (& $Exists $p) { return $p }
    }
    return $null
}

function Test-Avx2ByName {
    <# Whether a processor has AVX2, from its name, when Windows cannot be asked (before 20H1):
       $true, $false, or $null when the name does not say. #>
    param([string]$Name)
    if ($Name -match 'Ryzen|EPYC|Threadripper') { return $true }
    if ($Name -match 'Core\(TM\)\s+Ultra|Core\s+Ultra') { return $true }
    $m = [regex]::Match($Name, 'Core\(TM\)\s+i[3579]-(\d{4,5})')
    if ($m.Success) { return ([int]$m.Groups[1].Value -ge 4000) }   # 4th generation (Haswell) and newer
    $m = [regex]::Match($Name, '\bi[3579]-(\d{4,5})')
    if ($m.Success) { return ([int]$m.Groups[1].Value -ge 4000) }
    if ($Name -match 'Core\(TM\)2|Core\(TM\)\s+i[3579]\s+\d{3}\b|Phenom|Athlon\s+(64|II|X[24])\b|Atom|A[468]-\d{4}|A10-\d{4}') { return $false }
    return $null
}

function Test-Sse42ByName {
    <# Whether a processor has SSE4.2 (x86-64-v2, what the game needs at the least), from its name, when
       Windows cannot be asked (before 20H1): $false for the families known to lack it, else $null. Each
       pattern is a family no later processor shares a name with (a wrong $false stops the installer):
         Intel  Core 2, and the Pentium Dual-Core, Pentium Dual, Pentium 4 and D of its time; its Celerons
                (E1xxx, E3xxx, T1xxx, T3xxx, and 2xx to 6xx and 9xx: not 8xx, Sandy Bridge's, nor 7xx,
                where Penryn's and Sandy Bridge's meet) and Xeons (3000 to 3300, 5100 to 5400: not 5500
                and up, Nehalem's); the Atoms before Silvermont (N, D and Z with three digits or 2 and
                three, 230, 330)
         AMD    Phenom; Athlon 64 and II, and the other K8 and K10 Athlons (X2 Dual Core, Dual Core,
                LE-1xxx, 7x50 Dual-Core); Turion; the K8 and K10 Semprons (NNNN+, LE-1xxx, 1xx, X2 1xx:
                not Jaguar's, four digits and APU); Bobcat (C-xx, E-xxx, E1-1xxx, E2-1xxx); Llano
                (A4, A6 and A8-3xxx, E2-3200: not Jaguar's E2-3000 and E1-2xxx)
       (-match: not case-sensitive.) #>
    param([string]$Name)
    $families = @(
        'Core\(TM\)2', 'Pentium\(R\)\s+(Dual(-Core)?|4|D)\s+CPU\b',
        'Celeron\(R\)\s+(Dual-Core\s+|M\s+)?CPU\s+([ET][13]\d{3}|[2-69]\d{2})\b', 'Xeon\(R\)\s+CPU\s+[EXL]?(3[0-3]|5[1-4])\d{2}\b',
        'Atom\(TM\)\s+CPU\s+([NDZ]2?\d{3}|[23]30)\b',
        'Phenom', 'Athlon(\(tm\))?\s+(64|II)\b',
        'Athlon\(tm\)\s+((X2\s+)?Dual[-\s]Core\s+(Processor|QL-)|Processor\s+LE-1\d{3}\b|7[4-8]50\s+Dual-Core)',
        'Turion', 'Sempron\(tm\)\s+(Processor\s+(LE-1\d{3}\b|\d{4}\+)|X2\s+1\d{2}\b|1\d{2}\s+Processor)',
        '\bAMD\s+[CE]-\d{2,3}\b', '\bE[12]-1\d{3}\b', '\bA[468]-3\d{3}', '\bE2-3200\b')
    foreach ($family in $families) {
        if ($Name -match $family) { return $false }
    }
    return $null
}

function Get-BuildIsa {
    <# What a build folder (build\) says it was built for (tools\isa.py): @{ Target; Told }, Told when
       SSBU_ISA chose it, which the build keeps (isa.txt: the target, then 'told' or 'chosen'). Without an
       isa.txt, a folder an older version linked ssbu.exe in is avx2's, chosen (every older version built
       for avx2; isa.py keeps it when the processor cannot be asked); else $null. #>
    param([string]$Folder)
    $file = Join-Path $Folder 'isa.txt'
    if (Test-Path -LiteralPath $file -PathType Leaf) {
        $lines = @(Get-Content -LiteralPath $file -ErrorAction SilentlyContinue)
        if ($lines.Count -ge 1 -and @('avx2', 'x86-64-v2') -ccontains ([string]$lines[0]).Trim()) {
            return @{ Target = ([string]$lines[0]).Trim(); Told = ($lines.Count -ge 2 -and [string]$lines[1] -ceq 'told') }
        }
    }
    if (Test-Path -LiteralPath (Join-Path $Folder 'ssbu.exe') -PathType Leaf) { return @{ Target = 'avx2'; Told = $false } }
    return $null
}

function Test-CpuFeature {
    <# Windows' IsProcessorFeaturePresent(feature): $true, $false, or $null when it cannot be asked.
       The call is declared with Reflection.Emit, not Add-Type -MemberDefinition: Add-Type runs the
       .NET Framework C# compiler (csc.exe), which fails on some PCs ("Unable to find messages file
       'cscui.dll'", e.g. while a .NET update waits for a restart). #>
    param([int]$Feature)
    try {
        if (-not $Script:Kernel32) {
            $name = New-Object Reflection.AssemblyName 'WindowSmashSetup.Native'
            $assembly = [AppDomain]::CurrentDomain.DefineDynamicAssembly($name, [Reflection.Emit.AssemblyBuilderAccess]::Run)
            $type = $assembly.DefineDynamicModule('WindowSmashSetup.Native').DefineType('WindowSmashSetup.Kernel32', 'Public, Class')
            $method = $type.DefinePInvokeMethod('IsProcessorFeaturePresent', 'kernel32.dll',
                [Reflection.MethodAttributes]'Public, Static, PinvokeImpl', [Reflection.CallingConventions]::Standard,
                [bool], [Type[]]@([int]), [Runtime.InteropServices.CallingConvention]::Winapi, [Runtime.InteropServices.CharSet]::Auto)
            $method.SetImplementationFlags($method.GetMethodImplementationFlags() -bor [Reflection.MethodImplAttributes]::PreserveSig)
            $Script:Kernel32 = $type.CreateType()
        }
        return [bool]$Script:Kernel32::IsProcessorFeaturePresent($Feature)
    } catch {
        return $null
    }
}

function Get-FirewallRuleCommand {
    <# The PowerShell that adds the inbound allow rule for the game (run with administrator rights). #>
    param([string]$Exe, [string]$Name)
    return "New-NetFirewallRule -DisplayName '$Name' -Direction Inbound -Program '$Exe' -Action Allow -Profile Any | Out-Null"
}

function Test-PythonVersionText {
    <# Whether "3.14.6"-like text names a Python WindowSmash can use (3.10 or newer, 3.x). #>
    param([string]$Text)
    $m = [regex]::Match($Text, '(\d+)\.(\d+)')
    if (-not $m.Success) { return $false }
    $v = [version]("{0}.{1}" -f $m.Groups[1].Value, $m.Groups[2].Value)
    return ($v -ge [version]'3.10' -and $v.Major -eq 3)
}

function Get-DiskNeedGb {
    <# How much free space the build still needs: the full amount, less what dump\ and build\ hold already. #>
    param([double]$NeedGb, [double]$AlreadyGb)
    return [math]::Max(0, $NeedGb - $AlreadyGb)
}

# ---- helpers ---------------------------------------------------------------------------------------
function Write-Step { param([string]$Text) Write-Host ''; Write-Host $Text -ForegroundColor Cyan }
function Write-Info { param([string]$Text) Write-Host "  $Text" }
function Write-Warn { param([string]$Text) Write-Host "  $Text" -ForegroundColor Yellow }
function Write-Would { param([string]$Text) Write-Host "  (dry run) would $Text" -ForegroundColor DarkGray }

function Invoke-Download {
    param([string]$Url, [string]$Dest, [string]$What)
    Write-Info "downloading $What"
    Write-Info "  from $Url"
    $dir = Split-Path -Parent $Dest
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    $partial = "$Dest.partial"
    $old = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'   # (the progress bar makes Windows PowerShell's downloads very slow)
    try {
        Invoke-WebRequest -Uri $Url -OutFile $partial -UseBasicParsing -Headers @{ 'User-Agent' = $UserAgent }
    } finally {
        $ProgressPreference = $old
    }
    Move-Item -LiteralPath $partial -Destination $Dest -Force
    Write-Info ("  {0:N1} MB" -f ((Get-Item -LiteralPath $Dest).Length / 1MB))
}

function Get-Json {
    param([string]$Url)
    return Invoke-RestMethod -Uri $Url -UseBasicParsing -Headers @{ 'User-Agent' = $UserAgent; 'Accept' = 'application/vnd.github+json' }
}

function Get-Text {
    param([string]$Url)
    return (Invoke-WebRequest -Uri $Url -UseBasicParsing -Headers @{ 'User-Agent' = $UserAgent }).Content
}

function Test-FileSha256 {
    param([string]$File, [string]$Expected)
    $actual = (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLower()
    return ($actual -eq $Expected.ToLower())
}

function Invoke-Python {
    <# Runs the chosen Python with these arguments, output to this window; returns its exit code. #>
    param([string[]]$Arguments)
    $exe = $Script:Python[0]
    $pre = @()
    if ($Script:Python.Count -gt 1) { $pre = @($Script:Python[1..($Script:Python.Count - 1)]) }
    & $exe @pre @Arguments | Out-Host   # (to the window, not into this function's result)
    return $LASTEXITCODE
}

function Get-PythonOutput {
    <# Runs the chosen Python and returns what it printed (errors included), and the exit code. #>
    param([string[]]$Arguments)
    $exe = $Script:Python[0]
    $pre = @()
    if ($Script:Python.Count -gt 1) { $pre = @($Script:Python[1..($Script:Python.Count - 1)]) }
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $exe @pre @Arguments 2>&1 | ForEach-Object { "$_" }
        return @{ Output = ($out -join "`n"); Code = $LASTEXITCODE }
    } finally {
        $ErrorActionPreference = $old
    }
}

function Test-PythonCommand {
    <# The version of a Python command (@(exe, args...)), or $null when it does not run. #>
    param([string[]]$Command)
    try {
        $old = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $exe = $Command[0]
        $pre = @()
        if ($Command.Count -gt 1) { $pre = @($Command[1..($Command.Count - 1)]) }
        # (no double quotes in what is passed to a program: Windows PowerShell hands them over unescaped)
        $out = & $exe @pre '-c' 'import sys; print(sys.version)' 2>&1 | ForEach-Object { "$_" }
        $ErrorActionPreference = $old
        if ($LASTEXITCODE -ne 0) { return $null }
        $text = ($out -join ' ').Trim().Split(' ')[0]   # "3.14.8" of "3.14.8 (tags/v3.14.8:..., Sep 30 2026) [MSC ...]"
        if (Test-PythonVersionText $text) { return $text }
        return $null
    } catch {
        return $null
    }
}

# ---- step 1: the newest WindowSmash -----------------------------------------------------------------
function Invoke-Http {
    <# A GET that answers with its status instead of throwing (a 404 is an answer here, and a thrown
       one would land in install.log as an error): @{Code; Body; Error}. #>
    param([string]$Url, [string]$Accept = '*/*')
    $client = $null
    try {
        Add-Type -AssemblyName System.Net.Http
        $client = New-Object System.Net.Http.HttpClient
        $client.Timeout = [TimeSpan]::FromSeconds(60)
        [void]$client.DefaultRequestHeaders.TryAddWithoutValidation('User-Agent', $UserAgent)
        [void]$client.DefaultRequestHeaders.TryAddWithoutValidation('Accept', $Accept)
        $resp = $client.GetAsync($Url).GetAwaiter().GetResult()
        return @{ Code = [int]$resp.StatusCode; Body = $resp.Content.ReadAsStringAsync().GetAwaiter().GetResult(); Error = $null }
    } catch {
        $e = $_.Exception
        while ($e.InnerException) { $e = $e.InnerException }
        return @{ Code = 0; Body = $null; Error = $e.Message }
    } finally {
        if ($client) { $client.Dispose() }
    }
}

function Invoke-Api {
    <# GitHub's API: @{Ok; Data} or @{Ok = $false; NotFound; Error}. #>
    param([string]$Url)
    $r = Invoke-Http $Url 'application/vnd.github+json'
    if ($r.Code -ge 200 -and $r.Code -lt 300) {
        try { return @{ Ok = $true; Data = ($r.Body | ConvertFrom-Json) } } catch { return @{ Ok = $false; NotFound = $false; Error = 'an answer that is not JSON' } }
    }
    $why = $r.Error
    if (-not $why) { $why = "HTTP $($r.Code)" }
    return @{ Ok = $false; NotFound = ($r.Code -eq 404); Error = $why }
}

function Get-LocalCommit {
    $file = Join-Path $Root $SourceVersionFile
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    return Read-SourceVersion ([IO.File]::ReadAllText($file))
}

function Update-GitCheckout {
    <# A git checkout of the branch, with no changes of its own: git pull --ff-only. #>
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { Write-Warn 'this folder is a git checkout, but git is not installed: not updating'; return $false }
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $branch = (& git -C $Root rev-parse --abbrev-ref HEAD 2>$null | Out-String).Trim()
        $head = (& git -C $Root rev-parse HEAD 2>$null | Out-String).Trim()
        if ($branch -ne $UpdateBranch) { Write-Info "this is a git checkout of the branch $($branch), not $($UpdateBranch): not updating (update it with git)"; return $false }
        $dirty = (& git -C $Root status --porcelain --untracked-files=no 2>$null | Out-String).Trim()
        if ($dirty) { Write-Info 'this git checkout has changes of its own: not updating (update it with git)'; return $false }
        $remote = (((& git -C $Root ls-remote origin "refs/heads/$UpdateBranch" 2>$null | Out-String).Trim()) -split '\s+')[0]
        if (-not $remote) { Write-Warn 'could not reach the git remote: going on with this version'; return $false }
        if ($remote -eq $head) { Write-Info "up to date ($($head.Substring(0, 7)), git checkout)"; return $false }
        if ($DryRun) { Write-Would "run git pull --ff-only ($($head.Substring(0, 7)) to $($remote.Substring(0, 7)))"; return $false }
        & git -C $Root pull --ff-only origin $UpdateBranch | Out-Host
        $after = (& git -C $Root rev-parse HEAD 2>$null | Out-String).Trim()
        if ($after -eq $head) { Write-Warn 'git pull changed nothing (see above): going on with this version'; return $false }
        Write-Info "updated with git: $($head.Substring(0, 7)) to $($after.Substring(0, 7))"
        return $true
    } finally {
        $ErrorActionPreference = $old
    }
}

function Update-FromZip {
    <# Replaces this folder's WindowSmash files with those of commit $New (GitHub's ZIP of it).
       Only files whose content changed are written; they get the current time, so the build redoes
       what they feed. Files of commit $Old that $New dropped are removed. Nothing else is touched:
       dump\, build\, save\, deps\, settings and the like are not in the ZIP. #>
    param([string]$Old, [string]$New, [string]$NewDate)
    $work = Join-Path $Root 'deps\update'
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    $zip = Join-Path $work "WindowSmash-$New.zip"
    Invoke-Download "https://codeload.github.com/$RepoOwner/$RepoName/zip/$New" $zip "WindowSmash $($New.Substring(0, 7))"
    $extract = Join-Path $work 'extract'
    Expand-Archive -LiteralPath $zip -DestinationPath $extract
    $top = @(Get-ChildItem -LiteralPath $extract -Directory)
    if ($top.Count -ne 1 -or -not (Test-Path -LiteralPath (Join-Path $top[0].FullName 'tools\recomp.py'))) {
        throw 'The downloaded WindowSmash does not look complete; nothing was changed. Run this again.'
    }
    $src = $top[0].FullName
    $newFiles = @(Get-ChildItem -LiteralPath $src -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($src.Length + 1).Replace('\', '/') })
    $removed = @()
    $tree = Invoke-Api "$RepoApi/git/trees/$($Old)?recursive=1"
    if ($tree.Ok -and -not $tree.Data.truncated) {
        $oldFiles = @($tree.Data.tree | Where-Object { $_.type -eq 'blob' } | ForEach-Object { $_.path })
        $removed = Get-RemovedPaths $oldFiles $newFiles
    } else {
        Write-Warn "the old version's file list could not be read: files the new version dropped stay"
    }
    $oldLobby = $null
    $was = Invoke-Http "https://raw.githubusercontent.com/$RepoOwner/$RepoName/$Old/online_server.txt"
    if ($was.Code -eq 200) { $oldLobby = [string]$was.Body }

    $changed = 0
    $now = Get-Date
    foreach ($rel in $newFiles) {
        $from = Join-Path $src $rel
        $to = Join-Path $Root $rel
        try {
            if ($rel -ieq 'online_server.txt' -and (Test-Path -LiteralPath $to)) {
                $mine = [IO.File]::ReadAllText($to)
                $theirs = [IO.File]::ReadAllText($from)
                $merged = Merge-OnlineServer $mine $oldLobby $theirs
                if ($merged -ne $theirs) { Write-Info 'online_server.txt: kept your lobby settings' }
                if ($merged -ne $mine) { [IO.File]::WriteAllText($to, $merged); $changed++ }
                continue
            }
            if ((Test-Path -LiteralPath $to -PathType Leaf) -and (Get-FileHash -LiteralPath $to).Hash -eq (Get-FileHash -LiteralPath $from).Hash) { continue }
            $dir = Split-Path -Parent $to
            if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
            Copy-Item -LiteralPath $from -Destination $to -Force
            (Get-Item -LiteralPath $to).LastWriteTime = $now
            $changed++
        } catch {
            throw "Could not replace $rel ($($_.Exception.Message)). Close the game and any window using this folder, then run this again."
        }
    }
    $gone = 0
    foreach ($rel in $removed) {
        $p = Join-Path $Root $rel
        if (Test-Path -LiteralPath $p -PathType Leaf) { Remove-Item -LiteralPath $p -Force; $gone++ }
    }
    # last: until this is written, a stopped update is simply done again next time
    [IO.File]::WriteAllText((Join-Path $Root $SourceVersionFile), "$New $NewDate`n")
    Remove-Item -LiteralPath $work -Recurse -Force
    Write-Info "updated: $changed file(s) replaced, $gone removed; your game files, settings, save and builds are as they were"
    return $true
}

function Step-Update {
    <# Brings this folder to the newest commit of GitHub's branch. $true when files changed. #>
    Write-Step '[1/10] Get the newest WindowSmash'
    if ($NoUpdate) { Write-Info 'not checking (-NoUpdate)'; return $false }
    if (Test-Path -LiteralPath (Join-Path $Root '.git')) { return (Update-GitCheckout) }
    $local = Get-LocalCommit
    if (-not $local) {
        Write-Warn "this folder does not say which version it is ($SourceVersionFile names no commit): not updating."
        Write-Warn "To get the newest, download the ZIP again from https://github.com/$RepoOwner/$RepoName"
        return $false
    }
    $short = $local.Substring(0, 7)
    $head = Invoke-Api "$RepoApi/commits/$UpdateBranch"
    if (-not $head.Ok) { Write-Warn "could not ask GitHub for a newer version ($($head.Error)); going on with this one"; return $false }
    $headSha = [string]$head.Data.sha
    $headDate = ([string]$head.Data.commit.committer.date).Substring(0, 10)
    if ($headSha -eq $local) { Write-Info "up to date ($short, $headDate)"; return $false }
    $cmp = Invoke-Api "$RepoApi/compare/$($local)...$($headSha)"
    if (-not $cmp.Ok) {
        if ($cmp.NotFound) { Write-Info "this copy ($short) is not on GitHub (made on this PC, or from a branch since deleted): not updating" }
        else { Write-Warn "could not ask GitHub for a newer version ($($cmp.Error)); going on with this one" }
        return $false
    }
    switch (Get-UpdateDecision $cmp.Data.status) {
        'current' { Write-Info "up to date ($short)"; return $false }
        'newer' { Write-Info "this copy ($short) is newer than GitHub's $($UpdateBranch): not updating"; return $false }
        'diverged' { Write-Info "this copy ($short) comes from another branch than $($UpdateBranch): not updating"; return $false }
        'update' { }
        default { Write-Warn "GitHub's answer ($($cmp.Data.status)) was not understood: not updating"; return $false }
    }
    $n = [int]$cmp.Data.ahead_by
    $plural = ''
    if ($n -ne 1) { $plural = 's' }
    Write-Info "a newer WindowSmash is on GitHub: $n new commit$plural ($short to $($headSha.Substring(0, 7)), $headDate)"
    $titles = @($cmp.Data.commits | ForEach-Object { ([string]$_.commit.message -split "`n")[0] } | Where-Object { $_ -notmatch '^Merge (pull request|branch) ' })
    [array]::Reverse($titles)
    foreach ($t in ($titles | Select-Object -First 8)) { Write-Info "  - $t" }
    if ($titles.Count -gt 8) { Write-Info "  ... and $($titles.Count - 8) more" }
    if ($DryRun) { Write-Would "download it and replace this folder's WindowSmash files (your game files, settings, save and builds stay)"; return $false }
    return (Update-FromZip $local $headSha $headDate)
}

# ---- step 2: the PC ---------------------------------------------------------------------------------
function Step-CheckPc {
    Write-Step '[2/10] Check this PC'
    $os = Get-CimInstance Win32_OperatingSystem
    $build = [int]$os.BuildNumber
    Write-Info "$($os.Caption) ($($os.Version), $($os.OSArchitecture))"
    if ($os.OSArchitecture -notmatch '64') { throw 'WindowSmash needs 64-bit Windows; this Windows is not 64-bit.' }
    if ([version]$os.Version -lt [version]'10.0') { throw 'WindowSmash needs Windows 10 or 11.' }

    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $name = ($cpu.Name -replace '\s+', ' ').Trim()
    $sse42 = $null
    $avx2 = $null
    if ($build -ge 19041) {   # Windows 10 version 2004 and later can be asked about SSE4.2 and AVX2 (features 38 and 40)
        $sse42 = Test-CpuFeature 38
        $avx2 = Test-CpuFeature 40
    }
    if ($null -eq $sse42) { $sse42 = Test-Sse42ByName $name }
    if ($sse42 -eq $false) {
        throw "The processor ($name) has no SSE4.2, which the game needs at the least (x86-64-v2: Intel processors since 2008, AMD since 2011): WindowSmash cannot run on it."
    }
    $byName = $null -eq $avx2
    if ($byName) { $avx2 = Test-Avx2ByName $name }
    # What the build chooses (tools\isa.py) when SSBU_ISA is not set: what SSBU_ISA chose for the last build (build\isa.txt
    # says 'told') on a processor with AVX2 or one it cannot ask; else by Windows' answer. Where Windows cannot be asked
    # (before build 19041), nor can the build: avx2 when the name says AVX2 (the build is told so, and build\isa.txt keeps
    # it), else what the last build was for (an older version's: avx2), else x86-64-v2.
    $last = $null
    if (-not $env:SSBU_ISA) { $last = Get-BuildIsa (Join-Path $Root 'build') }
    $has = if ($avx2 -eq $true) { 'has AVX2' } elseif ($avx2 -eq $false) { 'has no AVX2' } else { 'whether it has AVX2 is unknown' }
    if ($last -and $last.Told -and ($byName -or $avx2 -eq $true)) {
        Write-Info "$name - $($has): the game stays built for $($last.Target), as SSBU_ISA chose for the last build (build\isa.txt; SSBU_ISA=auto chooses by the processor)"
    } elseif ($avx2 -eq $true) {
        Write-Info "$name - has AVX2"
        if ($byName -and -not $env:SSBU_ISA) {
            $env:SSBU_ISA = 'avx2'
            Write-Info 'the game is built for AVX2 (SSBU_ISA=avx2): Windows cannot be asked, the name says'
        }
    } elseif ($byName -and $last) {
        $other = if ($last.Target -eq 'avx2') { 'if the game then says the processor lacks AVX2, build it with SSBU_ISA=x86-64-v2' } else { 'SSBU_ISA=avx2 builds for AVX2' }
        Write-Info "$name - $($has): the game stays built for $($last.Target), as it was (Windows cannot be asked; $($other): docs\BUILDING.md)"
    } elseif ($avx2 -eq $false) { Write-Info "$name - has no AVX2: the game is built for older processors (x86-64-v2), expected to be slower" }
    else { Write-Warn "$name - whether it has AVX2 is unknown: the game is built for older processors (x86-64-v2), expected to be slower (with AVX2: docs\BUILDING.md, SSBU_ISA)" }

    $ramGb = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
    if ($ramGb -lt $MinRamGb) { throw "This PC has $ramGb GB of RAM; WindowSmash needs at least $MinRamGb GB (16 GB is comfortable)." }
    if ($ramGb -lt 16) { Write-Warn "$ramGb GB of RAM: enough, but the build will be slow (16 GB is comfortable)" } else { Write-Info "$ramGb GB of RAM" }

    $drive = [IO.Path]::GetPathRoot($Root)
    $psDrive = $null
    if ($drive -match '^[A-Za-z]:') { $psDrive = Get-PSDrive -Name $drive.Substring(0, 1) -ErrorAction SilentlyContinue }
    if ($psDrive -and $null -ne $psDrive.Free) {
        $free = $psDrive.Free / 1GB
        $already = 0
        foreach ($d in @('dump', 'build')) {
            $p = Join-Path $Root $d
            if (Test-Path -LiteralPath $p) { $already += [double](Get-ChildItem -LiteralPath $p -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1GB }
        }
        $need = Get-DiskNeedGb $NeedDiskGb $already
        Write-Info ("{0:N0} GB free on {1}, {2:N0} GB still needed for the build" -f $free, $drive, $need)
        if ($free -lt $need) {
            throw ("Only {0:N0} GB are free on {1}; the build needs about {2:N0} GB more. Free some space, or move WindowSmash to a drive with room." -f $free, $drive, $need)
        }
    } else {
        Write-Warn "the free space on $drive could not be read; the build needs about $NeedDiskGb GB"
    }
    $gpu = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) | Select-Object -First 1
    if ($gpu) { Write-Info "graphics: $gpu (needs DirectX 11 and an up-to-date driver)" }
}

# ---- step 3: Python -----------------------------------------------------------------------------------
function Find-Python {
    <# The first Python 3.10+ that runs: deps\python, python on the PATH, the py launcher. #>
    $own = Join-Path $Root 'deps\python\python.exe'
    $candidates = @(, @($own)) + @(, @('python')) + @(, @('py', '-3'))
    foreach ($c in $candidates) {
        if ($c[0] -eq $own -and -not (Test-Path -LiteralPath $own)) { continue }
        $v = Test-PythonCommand $c
        if ($v) { return @{ Command = $c; Version = $v } }
    }
    return $null
}

function Test-PythonPackages {
    <# Whether the chosen Python has what the tools import (pycryptodomex, and a zstd). #>
    $r = Get-PythonOutput @('-c', 'import Cryptodome')
    if ($r.Code -ne 0) { return $false }
    $r = Get-PythonOutput @('-c', $ZstdCheck)
    return ($r.Code -eq 0)
}

function Step-Python {
    Write-Step '[3/10] Python'
    $found = Find-Python
    if ($found) {
        $Script:Python = $found.Command
        Write-Info "Python $($found.Version): $($found.Command -join ' ')"
    } else {
        $target = Join-Path $Root 'deps\python'
        $listing = ''
        try { $listing = Get-Text $PythonFtp } catch { Write-Warn "python.org's listing could not be read ($($_.Exception.Message)); taking $PythonFallbackVersion" }
        $pick = Get-PythonEmbedUrl $listing $PythonMinor $PythonFallbackVersion
        if ($DryRun) {
            Write-Would "download Python $($pick.Version) (embeddable, $($pick.Url)) into deps\python, add pip, and install requirements.txt there"
            return
        }
        Write-Info "no Python 3.10 or newer found: setting up Python $($pick.Version) in deps\python (touches nothing else on the PC)"
        $zip = Join-Path $Root "deps\python-$($pick.Version)-embed-amd64.zip"
        if (-not (Test-Path -LiteralPath $zip)) { Invoke-Download $pick.Url $zip "Python $($pick.Version)" }
        if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
        Expand-Archive -LiteralPath $zip -DestinationPath $target
        Remove-Item -LiteralPath $zip -Force
        # the embeddable Python ignores site-packages until its ._pth file says "import site"
        foreach ($pth in Get-ChildItem -LiteralPath $target -Filter 'python3*._pth') {
            $text = [IO.File]::ReadAllText($pth.FullName)
            [IO.File]::WriteAllText($pth.FullName, $text.Replace('#import site', 'import site'))
        }
        $Script:Python = @((Join-Path $target 'python.exe'))
        $v = $null
        foreach ($try in 1..5) {   # (a just-extracted program can be busy for a moment: the antivirus looks at it first)
            $v = Test-PythonCommand $Script:Python
            if ($v) { break }
            Start-Sleep -Seconds 2
        }
        if (-not $v) { throw "deps\python\python.exe does not run after extracting $($pick.Url)." }
        Write-Info "Python $v in deps\python"
    }
    # pip, for the embeddable Python (the others have it)
    $pip = Get-PythonOutput @('-m', 'pip', '--version')
    if ($pip.Code -ne 0) {
        if ($DryRun) { Write-Would 'add pip with get-pip.py' } else {
            $getpip = Join-Path $Root 'deps\get-pip.py'
            Invoke-Download $GetPipUrl $getpip 'get-pip.py (adds pip)'
            if ((Invoke-Python @($getpip, '--no-warn-script-location')) -ne 0) { throw 'get-pip.py failed (see above).' }
            Remove-Item -LiteralPath $getpip -Force
        }
    }
    if (Test-PythonPackages) {
        Write-Info 'packages: installed'
    } elseif ($DryRun) {
        Write-Would 'run pip install -r requirements.txt'
    } else {
        Write-Info 'installing the packages (requirements.txt)'
        if ((Invoke-Python @('-m', 'pip', 'install', '-r', (Join-Path $Root 'requirements.txt'))) -ne 0) { throw 'pip could not install the packages (see above).' }
        if (-not (Test-PythonPackages)) { throw 'The packages are still missing after pip install.' }
    }
    # py7zr: the Mods app opens .7z mods with it when 7-Zip isn't installed (Windows' tar can't).
    # Optional: without it, only .7z mods need 7-Zip
    if (-not $DryRun -and (Get-PythonOutput @('-c', 'import py7zr')).Code -ne 0) {
        Write-Info 'adding py7zr (opens .7z mods)'
        if ((Invoke-Python @('-m', 'pip', 'install', '--quiet', 'py7zr')) -ne 0) { Write-Info 'py7zr could not be added: .7z mods need 7-Zip (7-zip.org)' }
    }
}

# ---- step 4: the compiler ---------------------------------------------------------------------------
function Test-Compiler {
    $r = Get-PythonOutput @((Join-Path $Root 'tools\build_game.py'), '--check')
    return @{ Ok = ($r.Code -eq 0); Output = $r.Output }
}

function Get-VsInstances {
    <# Visual Studio editions installed here, from vswhere: @{Path; Product}. #>
    $vswhere = Join-Path $VsInstallerDir 'vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return @() }
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $json = & $vswhere -all -products '*' -format json 2>$null | Out-String
    $ErrorActionPreference = $old
    if (-not $json.Trim()) { return @() }
    $out = @()
    foreach ($i in ($json | ConvertFrom-Json)) { $out += @{ Path = $i.installationPath; Product = $i.productId } }
    return $out
}

function Invoke-VsModify {
    <# Runs the Visual Studio Installer's modify command with administrator rights and waits for it;
       stops with what its exit code means when it fails. Its arguments go as one quoted command
       line: given a list, Start-Process joins it unquoted and the installation path breaks at its
       spaces (code 87). The rights are asked for here (-Verb RunAs): setup.exe runs with those of
       whoever starts it, and with --passive it will not ask for them itself ("Commands with
       --quiet or --passive should be run elevated from the beginning", code 5007). #>
    param([string]$Setup, [string[]]$Arguments)
    try {
        $p = Start-Process -FilePath $Setup -ArgumentList (Join-CommandLine $Arguments) -Verb RunAs -Wait -PassThru
    } catch {
        throw ("The Visual Studio Installer needs administrator rights to add the C++ tools, and it did not get them " +
               "($($_.Exception.Message)). Run this again and click Yes when Windows asks.")
    }
    $why = Get-VsExitMessage $p.ExitCode
    if ($why) {
        throw ("The Visual Studio Installer could not add the C++ tools: $why (code $($p.ExitCode)). Run this again; if it keeps " +
               "failing, open Visual Studio Installer, click Modify, tick 'Desktop development with C++' and 'C++ Clang tools for Windows', " +
               "install, then run this again.")
    }
}

function Install-PortableCompiler {
    <# tools\msvc_deps.py install, in this window: it shows Microsoft's license terms and asks before it
       downloads anything. 'done', 'declined' or 'failed' (it said why). #>
    $py = @((Join-Path $Root 'tools\msvc_deps.py'), 'install')
    if ($AcceptLicense) { $py += '--accept-license' }
    $code = Invoke-Python $py
    if ($code -eq 0) { return 'done' }
    if ($code -eq 2) { return 'declined' }
    return 'failed'
}

function Remove-PortableCompiler {
    <# tools\msvc_deps.py remove: deps\llvm and deps\msvc, so that cc.py looks for Visual Studio again. #>
    [void](Invoke-Python @((Join-Path $Root 'tools\msvc_deps.py'), 'remove'))
}

function Step-PortableCompiler {
    <# The compiler without Visual Studio (deps\llvm, deps\msvc). $true when it works now; $false to go
       on to the Build Tools; stops when Microsoft's license terms are declined. #>
    if ($DryRun) {
        Write-Would 'run tools\msvc_deps.py install, which shows Microsoft''s license terms and asks you to accept them, then downloads (no administrator rights):'
        $plan = Get-PythonOutput @((Join-Path $Root 'tools\msvc_deps.py'), 'plan')
        foreach ($line in ($plan.Output -split "`n")) { if ($line.Trim()) { Write-Info "  $($line.Trim())" } }
        if ($plan.Code -ne 0) { Write-Warn '  (Microsoft''s list could not be read just now: the download would fail, and the Build Tools come next)' }
        Write-Info '  declined: it stops there. If the download or the check after it fails, the Visual Studio Build Tools instead:'
        return $false
    }
    Write-Info 'downloading a compiler that needs no Visual Studio: LLVM from GitHub, and Microsoft''s C runtime and Windows SDK'
    Write-Info 'from Microsoft''s servers, into deps\llvm and deps\msvc (about 570 MB, no administrator rights)'
    $how = Install-PortableCompiler
    if ($how -eq 'declined') {
        throw ("Microsoft's license terms were not accepted, so the C runtime and the Windows SDK, which the game is built with, " +
               "cannot be downloaded. The Visual Studio Build Tools come under the same terms. Run OpenSmash.exe again to " +
               "accept them, or install Visual Studio yourself (setup guide step 4).")
    }
    if ($how -eq 'done') {
        $check = Test-Compiler
        if ($check.Ok) {
            foreach ($line in ($check.Output -split "`n")) { if ($line -match 'clang-cl|toolchain') { Write-Info $line.Trim() } }
            return $true
        }
        Write-Warn 'the downloaded compiler did not pass the check:'
        Write-Info ($check.Output -split "`n" | Where-Object { $_ -match 'ERROR' } | Select-Object -First 1)
        Remove-PortableCompiler
    }
    Write-Warn 'installing the Visual Studio Build Tools instead'
    return $false
}

function Step-Compiler {
    Write-Step '[4/10] The compiler'
    $check = Test-Compiler
    if ($check.Ok) {
        foreach ($line in ($check.Output -split "`n")) { if ($line -match 'clang-cl|Python|toolchain') { Write-Info $line.Trim() } }
        return
    }
    Write-Info ($check.Output -split "`n" | Where-Object { $_ -match 'ERROR' } | Select-Object -First 1)
    if (-not $BuildTools -and (Step-PortableCompiler)) { return }
    # @(): a function's list comes back as nothing when empty and as the one item when alone
    $instances = @(Get-VsInstances)
    $setup = Join-Path $VsInstallerDir 'setup.exe'
    if ($instances.Count -gt 0 -and (Test-Path -LiteralPath $setup)) {
        $inst = $instances[0]
        $modify = Get-VsModifyArguments $inst.Path $inst.Product
        if ($DryRun) { Write-Would "add the C++ workload and Clang to the Visual Studio in $($inst.Path): setup.exe $($modify -join ' ')"; return }
        Write-Info "adding the C++ workload and Clang to the Visual Studio in $($inst.Path)"
        Write-Info 'Windows now asks for administrator rights; then a progress window opens (this downloads up to a few GB)'
        Invoke-VsModify $setup $modify
    } else {
        $winget = Get-Command winget -ErrorAction SilentlyContinue
        if (-not $winget) {
            throw ("winget is not available, so the Build Tools cannot be installed from here. Download them from $VsBuildToolsUrl, " +
                   "tick 'Desktop development with C++' and, under Installation details, 'C++ Clang tools for Windows', install, then run this again.")
        }
        $install = Get-VsBuildToolsArguments $VsBuildToolsId
        if ($DryRun) { Write-Would "run: winget $($install -join ' ')"; return }
        Write-Info 'installing the Visual Studio Build Tools with the C++ workload and Clang through winget'
        Write-Info 'this downloads a few GB and takes a while; a progress window opens, and this window waits for it'
        & $winget.Source @install
        $code = $LASTEXITCODE
        if ($code -eq -1978335189) {   # already installed, but without what we need: add it
            $instances = @(Get-VsInstances)
            if ($instances.Count -eq 0 -or -not (Test-Path -LiteralPath $setup)) { throw "winget says the Build Tools are installed, but they could not be found. Open Visual Studio Installer, click Modify, tick 'Desktop development with C++' and 'C++ Clang tools for Windows', then run this again." }
            $inst = $instances[0]
            Invoke-VsModify $setup (Get-VsModifyArguments $inst.Path $inst.Product)
        } elseif ($code -notin @(0, 3010)) {
            throw "winget stopped with code $code. Install the Build Tools by hand from $VsBuildToolsUrl (tick 'Desktop development with C++' and 'C++ Clang tools for Windows'), then run this again."
        }
    }
    $check = Test-Compiler
    if (-not $check.Ok) {
        throw ("The compiler still does not work after the install:`n" + $check.Output +
               "`nA restart may be needed. After it, run OpenSmash.exe again.")
    }
    foreach ($line in ($check.Output -split "`n")) { if ($line -match 'clang-cl|toolchain') { Write-Info $line.Trim() } }
}

# ---- step 5: SDL3 ----------------------------------------------------------------------------------------
function Step-Sdl3 {
    Write-Step '[5/10] SDL3 (controllers other than Xbox pads)'
    $dir = Join-Path $Root 'deps\SDL3'
    if ((Test-Path -LiteralPath (Join-Path $dir 'include\SDL3\SDL_gamepad.h')) -and (Test-Path -LiteralPath (Join-Path $dir 'lib\x64\SDL3.dll'))) {
        $v = ''
        $h = Join-Path $dir 'include\SDL3\SDL_version.h'
        if (Test-Path -LiteralPath $h) {
            $t = [IO.File]::ReadAllText($h)
            $ma = [regex]::Match($t, '#define SDL_MAJOR_VERSION\s+(\d+)'); $mi = [regex]::Match($t, '#define SDL_MINOR_VERSION\s+(\d+)'); $mc = [regex]::Match($t, '#define SDL_MICRO_VERSION\s+(\d+)')
            if ($ma.Success -and $mi.Success -and $mc.Success) { $v = " $($ma.Groups[1].Value).$($mi.Groups[1].Value).$($mc.Groups[1].Value)" }
        }
        Write-Info "deps\SDL3: already there$v"
        return
    }
    $asset = $null
    try { $asset = Select-Sdl3Asset (Get-Json $SdlReleasesUrl) } catch { Write-Warn "GitHub's release list could not be read ($($_.Exception.Message))" }
    if (-not $asset) { $asset = @{ Version = [version]$SdlFallback.Version; Url = $SdlFallback.Url; Sha256 = $SdlFallback.Sha256; Name = (Split-Path -Leaf $SdlFallback.Url) } }
    if ($DryRun) { Write-Would "download SDL3 $($asset.Version) ($($asset.Url)) into deps\SDL3, checking its SHA-256 ($($asset.Sha256))"; return }
    $zip = Join-Path $Root "deps\$($asset.Name)"
    if (-not (Test-Path -LiteralPath $zip)) { Invoke-Download $asset.Url $zip "SDL3 $($asset.Version)" }
    if ($asset.Sha256) {
        if (-not (Test-FileSha256 $zip $asset.Sha256)) { Remove-Item -LiteralPath $zip -Force; throw "The downloaded $($asset.Name) does not match the SHA-256 the release lists. Run this again to download it afresh." }
        Write-Info 'SHA-256 matches the release'
    } else {
        Write-Warn 'the release lists no SHA-256 for it; taking the download as it is'
    }
    $extract = Join-Path $Root 'deps\SDL3-extract'
    if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
    Expand-Archive -LiteralPath $zip -DestinationPath $extract
    $inner = Get-ChildItem -LiteralPath $extract -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'include\SDL3\SDL_gamepad.h') } | Select-Object -First 1
    if (-not $inner) { throw "$($asset.Name) does not hold include\SDL3 as expected." }
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    Move-Item -LiteralPath $inner.FullName -Destination $dir
    Remove-Item -LiteralPath $extract -Recurse -Force
    Remove-Item -LiteralPath $zip -Force
    Write-Info "deps\SDL3: SDL3 $($asset.Version)"
}

# ---- step 6: codecs ---------------------------------------------------------------------------------------
function Step-Codecs {
    Write-Step '[6/10] Music and movie codecs'
    $opus = Join-Path $Root 'third_party\opus\src\opus_decoder.c'
    $vpx = Join-Path $Root 'third_party\libvpx\vp9\vp9_dx_iface.c'
    if ((Test-Path -LiteralPath $opus) -and (Test-Path -LiteralPath $vpx)) {
        Write-Info 'libopus and libvpx are in third_party and are built during the game build: nothing to download'
    } else {
        Write-Warn 'third_party\opus or third_party\libvpx is missing: the game will build without music or movies (download WindowSmash again to get them)'
    }
}

# ---- step 7: the game files ------------------------------------------------------------------------------
function Get-SavedFolders {
    <# Last time's answers (dump\build_folders.txt, written by build_game.py). #>
    $saved = @{}
    $f = Join-Path $Root 'dump\build_folders.txt'
    if (Test-Path -LiteralPath $f) {
        foreach ($line in [IO.File]::ReadAllLines($f)) {
            $i = $line.IndexOf('=')
            if ($i -gt 0) { $saved[$line.Substring(0, $i)] = $line.Substring($i + 1) }
        }
    }
    return $saved
}

function Get-DetectedFolders {
    <# The folders to suggest: last time's, else the emulators', else nothing. #>
    $saved = Get-SavedFolders
    $c = Get-FolderCandidates $env:APPDATA $env:USERPROFILE
    $exists = { param($p) Test-Path -LiteralPath $p -PathType Container }
    $hasNca = { param($p) (Test-Path -LiteralPath $p -PathType Container) -and @(Get-ChildItem -LiteralPath $p -Filter '*.nca' -ErrorAction SilentlyContinue).Count -gt 0 }
    $keys = $null; $firmware = $null; $game = $null
    if ($saved.ContainsKey('keys')) { $keys = $saved['keys'] } else { $keys = Resolve-FirstExisting ($c.Keys | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'prod.keys') }) $exists }
    # (an emulator's firmware folder only when it has one: the firmware is optional)
    if ($saved.ContainsKey('firmware')) { $firmware = $saved['firmware'] } else { $firmware = Resolve-FirstExisting $c.Firmware $hasNca }
    if ($saved.ContainsKey('game')) { $game = $saved['game'] } else { $game = Resolve-FirstExisting $c.Game $exists }
    return @{ Game = $game; Keys = $keys; Firmware = $firmware }
}

function Test-KeysFolder {
    param([string]$Folder)
    if (-not $Folder -or -not (Test-Path -LiteralPath $Folder -PathType Container)) { return @{ Ok = $false; Text = 'Choose the folder with prod.keys and title.keys.' } }
    $prod = Test-Path -LiteralPath (Join-Path $Folder 'prod.keys')
    $title = Test-Path -LiteralPath (Join-Path $Folder 'title.keys')
    if (-not $prod) { return @{ Ok = $false; Text = 'There is no prod.keys here.' } }
    if (-not $title) { return @{ Ok = $true; Text = 'prod.keys found; no title.keys (needed for games bought from the eShop).' } }
    return @{ Ok = $true; Text = 'prod.keys and title.keys found.' }
}

function Test-FirmwareFolder {
    <# The firmware is optional (only for the fonts): none is fine, and the game uses the open fonts. #>
    param([string]$Folder)
    if (-not $Folder) { return @{ Ok = $true; Text = "None: the game uses the open fonts of third_party\fonts_open, without Chinese, Japanese or Korean text. Your firmware gives your console's own fonts." } }
    if (-not (Test-Path -LiteralPath $Folder -PathType Container)) { return @{ Ok = $false; Text = 'Choose the folder with your firmware (a few hundred .nca files), or leave this empty.' } }
    $n = @(Get-ChildItem -LiteralPath $Folder -Filter '*.nca' -ErrorAction SilentlyContinue).Count
    if ($n -eq 0) { return @{ Ok = $false; Text = 'There are no .nca files here.' } }
    return @{ Ok = $true; Text = "$n .nca files (the game's fonts are taken out of them)." }
}

function Test-GameFolder {
    <# What tools\gamefiles.py finds in the folder, with the keys. #>
    param([string]$Folder, [string]$KeysFolder)
    if (-not $Folder -or -not (Test-Path -LiteralPath $Folder -PathType Container)) { return @{ Ok = $false; Text = 'Choose the folder with your game files.' } }
    if (-not $KeysFolder -or -not (Test-Path -LiteralPath (Join-Path $KeysFolder 'prod.keys'))) { return @{ Ok = $false; Text = 'The keys folder is needed to read the game files.' } }
    $r = Get-PythonOutput @((Join-Path $Root 'tools\gamefiles.py'), $Folder, '--keys', $KeysFolder, '--json')
    try {
        $scan = $r.Output | ConvertFrom-Json
    } catch {
        return @{ Ok = $false; Text = ('Could not read the folder: ' + ($r.Output -split "`n" | Select-Object -Last 1)) }
    }
    $lines = @($scan.summary)
    if (Test-Path -LiteralPath (Join-Path $Folder 'romfs.bin')) { $lines += 'romfs.bin found (the game data, used as it is).' }
    if ($scan.program) { return @{ Ok = $true; Text = ($lines -join ' ') } }
    return @{ Ok = $false; Text = ((@("Problem: $($scan.problem)") + $lines) -join ' ') }   # (the problem first: two lines show)
}

function Show-Wizard {
    <# The setup window: three folders with what was found in each, the online name, the options.
       Returns @{Game; Keys; Firmware; OnlineName; Shaders; Shortcut; Firewall}, or $null if cancelled. #>
    param([hashtable]$Detected, [string]$DefaultName)
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [Windows.Forms.Application]::EnableVisualStyles()

    $form = New-Object Windows.Forms.Form
    $form.Text = 'WindowSmash setup'
    $form.AutoScaleMode = [Windows.Forms.AutoScaleMode]::Dpi
    $form.AutoScaleDimensions = New-Object Drawing.SizeF(96, 96)
    $form.ClientSize = New-Object Drawing.Size(760, 560)
    $form.FormBorderStyle = [Windows.Forms.FormBorderStyle]::FixedDialog
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.StartPosition = [Windows.Forms.FormStartPosition]::CenterScreen
    $form.Font = New-Object Drawing.Font('Segoe UI', 9.75)
    $ico = Join-Path $Root 'docs\assets\windowsmash.ico'
    if (Test-Path -LiteralPath $ico) { $form.Icon = New-Object Drawing.Icon($ico) }

    $intro = New-Object Windows.Forms.Label
    $intro.Text = 'Where are your game files? Folders found on this PC are filled in; change any of them with Browse. Each line below a folder says what was found in it.'
    $intro.Location = New-Object Drawing.Point(16, 12)
    $intro.Size = New-Object Drawing.Size(728, 40)
    $form.Controls.Add($intro)

    $rows = @{}
    $y = 58
    foreach ($spec in @(
        @{ Key = 'Game'; Title = 'Game files'; Hint = "yuzu's or Ryujinx's game folder, or yours: the game (.xci/.nsp/.nca), its 13.0.5 update, DLC" },
        @{ Key = 'Keys'; Title = 'Keys'; Hint = "The folder with prod.keys and title.keys" },
        @{ Key = 'Firmware'; Title = 'Firmware'; Hint = "Optional: its .nca files give the console's own fonts (with Chinese, Japanese and Korean text)" }
    )) {
        $title = New-Object Windows.Forms.Label
        $title.Text = "$($spec.Title):  $($spec.Hint)"
        $title.Location = New-Object Drawing.Point(16, $y)
        $title.Size = New-Object Drawing.Size(728, 20)
        $title.Font = New-Object Drawing.Font('Segoe UI', 9.75, [Drawing.FontStyle]::Bold)
        $form.Controls.Add($title)
        $box = New-Object Windows.Forms.TextBox
        $box.Location = New-Object Drawing.Point(16, ($y + 24))
        $box.Size = New-Object Drawing.Size(620, 26)
        $box.Text = [string]$Detected[$spec.Key]
        $form.Controls.Add($box)
        $browse = New-Object Windows.Forms.Button
        $browse.Text = 'Browse...'
        $browse.Location = New-Object Drawing.Point(644, ($y + 22))
        $browse.Size = New-Object Drawing.Size(100, 29)
        $form.Controls.Add($browse)
        $status = New-Object Windows.Forms.Label
        $status.Location = New-Object Drawing.Point(16, ($y + 54))
        $status.Size = New-Object Drawing.Size(728, 40)
        $status.ForeColor = [Drawing.Color]::DimGray
        $form.Controls.Add($status)
        $rows[$spec.Key] = @{ Box = $box; Status = $status; Browse = $browse }
        $y += 100
    }

    $nameLabel = New-Object Windows.Forms.Label
    $nameLabel.Text = 'Your online name (what other players see; kept in online_name.txt):'
    $nameLabel.Location = New-Object Drawing.Point(16, $y)
    $nameLabel.Size = New-Object Drawing.Size(500, 20)
    $form.Controls.Add($nameLabel)
    $nameBox = New-Object Windows.Forms.TextBox
    $nameBox.Location = New-Object Drawing.Point(520, ($y - 2))
    $nameBox.Size = New-Object Drawing.Size(224, 26)
    $nameBox.Text = $DefaultName
    $nameBox.MaxLength = 32
    $form.Controls.Add($nameBox)
    $y += 36

    $checks = @{}
    foreach ($spec in @(
        @{ Key = 'Shortcut'; Text = 'Make a desktop shortcut'; On = $true },
        @{ Key = 'Firewall'; Text = 'Allow the game through Windows Firewall now (asks for administrator rights once; needed for playing online)'; On = $true },
        @{ Key = 'Shaders'; Text = 'After the build, prepare the shaders in this window (you pick low, medium, or high; stops the first-time stutter of effects)'; On = $true }
    )) {
        $cb = New-Object Windows.Forms.CheckBox
        $cb.Text = $spec.Text
        $cb.Checked = $spec.On
        $cb.Location = New-Object Drawing.Point(16, $y)
        $cb.Size = New-Object Drawing.Size(728, 24)
        $form.Controls.Add($cb)
        $checks[$spec.Key] = $cb
        $y += 28
    }

    $note = New-Object Windows.Forms.Label
    $note.Text = 'Then the game is built, which takes 30 minutes to a few hours. If it stops, run OpenSmash.exe again: it carries on where it stopped.'
    $note.Location = New-Object Drawing.Point(16, ($y + 4))
    $note.Size = New-Object Drawing.Size(728, 40)
    $form.Controls.Add($note)

    $check = New-Object Windows.Forms.Button
    $check.Text = 'Check files'
    $check.Location = New-Object Drawing.Point(16, 516)
    $check.Size = New-Object Drawing.Size(120, 32)
    $form.Controls.Add($check)
    $ok = New-Object Windows.Forms.Button
    $ok.Text = 'Set up and build'
    $ok.Location = New-Object Drawing.Point(488, 516)
    $ok.Size = New-Object Drawing.Size(150, 32)
    $form.Controls.Add($ok)
    $cancel = New-Object Windows.Forms.Button
    $cancel.Text = 'Cancel'
    $cancel.Location = New-Object Drawing.Point(644, 516)
    $cancel.Size = New-Object Drawing.Size(100, 32)
    $cancel.DialogResult = [Windows.Forms.DialogResult]::Cancel
    $form.Controls.Add($cancel)
    $form.CancelButton = $cancel

    $results = @{}
    $refresh = {
        foreach ($k in @('Keys', 'Firmware', 'Game')) { $rows[$k].Status.Text = 'Checking...' }
        $form.Refresh()
        $results['Keys'] = Test-KeysFolder $rows['Keys'].Box.Text.Trim().Trim('"')
        $results['Firmware'] = Test-FirmwareFolder $rows['Firmware'].Box.Text.Trim().Trim('"')
        $results['Game'] = Test-GameFolder $rows['Game'].Box.Text.Trim().Trim('"') $rows['Keys'].Box.Text.Trim().Trim('"')
        foreach ($k in @('Keys', 'Firmware', 'Game')) {
            $rows[$k].Status.Text = $results[$k].Text
            if ($results[$k].Ok) { $rows[$k].Status.ForeColor = [Drawing.Color]::DarkGreen } else { $rows[$k].Status.ForeColor = [Drawing.Color]::Firebrick }
        }
    }
    foreach ($k in @('Game', 'Keys', 'Firmware')) {
        $row = $rows[$k]
        $row.Browse.Add_Click({
            $dlg = New-Object Windows.Forms.FolderBrowserDialog
            $dlg.Description = "Choose the folder"
            if ($row.Box.Text -and (Test-Path -LiteralPath $row.Box.Text)) { $dlg.SelectedPath = $row.Box.Text }
            if ($dlg.ShowDialog($form) -eq [Windows.Forms.DialogResult]::OK) { $row.Box.Text = $dlg.SelectedPath; & $refresh }
        }.GetNewClosure())
    }
    $check.Add_Click({ & $refresh })
    $ok.Add_Click({
        & $refresh
        $bad = @()
        foreach ($k in @('Game', 'Keys', 'Firmware')) { if (-not $results[$k].Ok) { $bad += $k } }
        if ($bad.Count -gt 0) {
            [Windows.Forms.MessageBox]::Show($form, "Please fix: $($bad -join ', ') (see the line under each folder).", 'WindowSmash setup', 'OK', 'Warning') | Out-Null
            return
        }
        if (-not $nameBox.Text.Trim()) { $nameBox.Text = $DefaultName }
        $form.DialogResult = [Windows.Forms.DialogResult]::OK
        $form.Close()
    })
    $form.Add_Shown({ $form.Activate(); & $refresh })
    if ($Script:WizardSnapshot) {   # (for checking the window without a person: save a picture of it, then accept it)
        $timer = New-Object Windows.Forms.Timer
        $timer.Interval = 1500
        $timer.Add_Tick({
            $timer.Stop()
            $bmp = New-Object Drawing.Bitmap($form.Width, $form.Height)
            $form.DrawToBitmap($bmp, (New-Object Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
            $bmp.Save($Script:WizardSnapshot, [Drawing.Imaging.ImageFormat]::Png)
            $form.DialogResult = [Windows.Forms.DialogResult]::OK
            $form.Close()
        })
        $timer.Start()
    }

    if ($form.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { return $null }
    return @{
        Game = $rows['Game'].Box.Text.Trim().Trim('"'); Keys = $rows['Keys'].Box.Text.Trim().Trim('"'); Firmware = $rows['Firmware'].Box.Text.Trim().Trim('"')
        OnlineName = $nameBox.Text.Trim(); Shortcut = $checks['Shortcut'].Checked; Firewall = $checks['Firewall'].Checked; Shaders = $checks['Shaders'].Checked
    }
}

function Read-Answers {
    <# The same questions in this window, for -NoWizard. #>
    param([hashtable]$Detected, [string]$DefaultName)
    $answers = @{}
    foreach ($spec in @(@{ Key = 'Game'; Q = 'Folder with your game files' }, @{ Key = 'Keys'; Q = 'Folder with prod.keys and title.keys' }, @{ Key = 'Firmware'; Q = "Folder with your firmware's .nca files (optional; none: the open fonts)" })) {
        $default = [string]$Detected[$spec.Key]
        if ($spec.Key -eq 'Firmware') {
            Write-Host "The firmware is optional: the game takes your console's own fonts from it, with Chinese, Japanese and Korean text. Without it the game uses the open fonts of third_party\fonts_open: Latin text in Open Sans, the button icons from yuzu's icon font, no Chinese, Japanese or Korean text. Fights are the same."
        }
        while ($true) {
            $prompt = $spec.Q
            if ($default) { $prompt += " [$default]" } elseif ($spec.Key -eq 'Firmware') { $prompt += ' [none]' }
            $a = (Read-Host $prompt).Trim().Trim('"')
            if (-not $a) { $a = $default }
            if ($spec.Key -eq 'Firmware' -and $a -eq 'none') { $a = '' }
            $r = switch ($spec.Key) { 'Keys' { Test-KeysFolder $a } 'Firmware' { Test-FirmwareFolder $a } default { Test-GameFolder $a $answers['Keys'] } }
            Write-Host "  $($r.Text)"
            if ($r.Ok) { $answers[$spec.Key] = $a; break }
        }
    }
    $n = (Read-Host "Your online name [$DefaultName]").Trim()
    if (-not $n) { $n = $DefaultName }
    $answers['OnlineName'] = $n
    $answers['Shortcut'] = ((Read-Host 'Put a shortcut on the desktop? [Y/n]').Trim() -notmatch '^n')
    $answers['Firewall'] = ((Read-Host 'Allow the game through Windows Firewall (asks for administrator rights)? [Y/n]').Trim() -notmatch '^n')
    $answers['Shaders'] = ((Read-Host 'Prepare the shaders after the build, in this window? [Y/n]').Trim() -notmatch '^n')
    return $answers
}

function Step-GameFiles {
    Write-Step '[7/10] Your game files'
    $detected = Get-DetectedFolders
    if ($Game) { $detected.Game = $Game }
    if ($Keys) { $detected.Keys = $Keys }
    if ($Firmware) { $detected.Firmware = $Firmware }
    if ($NoFirmware) { $detected.Firmware = $null }
    foreach ($k in @('Game', 'Keys', 'Firmware')) {
        if ($detected[$k]) { Write-Info "$($k.ToLower()): $($detected[$k])" } elseif ($k -eq 'Firmware') { Write-Info 'firmware: none (optional: without it the game uses the open fonts of third_party\fonts_open)' } else { Write-Info "$($k.ToLower()): not found on this PC (to be chosen)" }
    }
    $nameFile = Get-ConfigPath $Root 'online_name.txt'
    $default = $OnlineName
    if (-not $default -and (Test-Path -LiteralPath $nameFile)) { $default = ([IO.File]::ReadAllText($nameFile)).Trim() }
    if (-not $default) { $default = 'Player' + (Get-Random -Minimum 1000 -Maximum 9999) }
    # The OpenSmash window already asked. -NoWizard with -Game and -Keys uses those answers and does not open this window or ask in the console.
    if ($NoWizard -and $ScriptBound.ContainsKey('Game') -and $ScriptBound.ContainsKey('Keys')) {
        $fw = if ($NoFirmware -or -not $Firmware) { '' } else { $Firmware }
        foreach ($pair in @(
            @{ Name = 'keys'; Result = (Test-KeysFolder $Keys) },
            @{ Name = 'firmware'; Result = (Test-FirmwareFolder $fw) },
            @{ Name = 'game'; Result = (Test-GameFolder $Game $Keys) }
        )) {
            Write-Info "$($pair.Name): $($pair.Result.Text)"
            if (-not $pair.Result.Ok -and -not $DryRun) { throw $pair.Result.Text }
        }
        if (-not $DryRun -and $default -and -not (Test-Path -LiteralPath $nameFile)) { [IO.File]::WriteAllText($nameFile, $default + "`r`n") }
        Write-Info "online name: $default"
        return @{ Game = $Game; Keys = $Keys; Firmware = $fw; OnlineName = $default; Shortcut = [bool]$Shortcut; Firewall = [bool]$Firewall; Shaders = [bool]$Shaders }
    }
    if ($DryRun) {
        if ($detected.Game -and $detected.Keys) { Write-Info ('game folder check: ' + (Test-GameFolder $detected.Game $detected.Keys).Text) }
        Write-Would "open the setup window with these folders and the online name '$default', then build with the answers"
        return @{ Game = $detected.Game; Keys = $detected.Keys; Firmware = $detected.Firmware; OnlineName = $default; Shortcut = $true; Firewall = $false; Shaders = $false }
    }
    $answers = $null
    if (-not $NoWizard) {
        try { $answers = Show-Wizard $detected $default } catch { Write-Warn "the setup window could not open ($($_.Exception.Message)); asking here instead"; $NoWizard = $true }
        if (-not $NoWizard -and -not $answers) { throw 'Cancelled. Run OpenSmash.exe again when you are ready.' }
    }
    if ($NoWizard) { $answers = Read-Answers $detected $default }
    if (($answers.OnlineName -and $answers.OnlineName -ne $default) -or -not (Test-Path -LiteralPath $nameFile)) {
        [IO.File]::WriteAllText($nameFile, $answers.OnlineName + "`r`n")
    }
    Write-Info "online name: $($answers.OnlineName)"
    return $answers
}

# ---- step 8: shortcut and firewall -----------------------------------------------------------------------
function Get-DesktopFolder {
    return [Environment]::GetFolderPath('Desktop')
}

function Test-FirewallRule {
    <# Whether the rule is there for this folder's ssbu.exe (another WindowSmash folder has its own). #>
    param([string]$Exe)
    foreach ($r in @(Get-NetFirewallRule -DisplayName $FirewallRuleName -ErrorAction SilentlyContinue)) {
        $f = $r | Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue
        if ($f -and $f.Program -ieq $Exe) { return $true }
    }
    return $false
}

function Step-Extras {
    param([hashtable]$Answers)
    Write-Step '[8/10] Shortcut and firewall'
    $lnk = Join-Path (Get-DesktopFolder) 'OpenSmash.lnk'
    $launcher = Join-Path $Root 'OpenSmash.exe'
    if (-not $Answers.Shortcut) {
        Write-Info 'shortcut: not wanted'
    } elseif (Test-Path -LiteralPath $lnk) {
        Write-Info "shortcut: already on the desktop"
    } elseif ($DryRun) {
        Write-Would "put $lnk on the desktop, pointing at OpenSmash.exe"
    } else {
        $ws = New-Object -ComObject WScript.Shell
        $s = $ws.CreateShortcut($lnk)
        $s.TargetPath = $launcher
        $s.WorkingDirectory = $Root
        $s.IconLocation = $launcher + ',0'
        $s.Description = 'OpenSmash'
        $s.Save()
        Write-Info "shortcut: $lnk"
    }
    $exe = Join-Path $Root 'build\ssbu.exe'
    if (-not $Answers.Firewall) {
        Write-Info 'firewall rule: not wanted (Windows will ask the first time the game goes online)'
    } elseif (Test-FirewallRule $exe) {
        Write-Info "firewall rule: already there ($FirewallRuleName)"
    } elseif ($DryRun) {
        Write-Would "add the firewall rule '$FirewallRuleName' for $exe, with administrator rights"
    } else {
        $cmd = Get-FirewallRuleCommand $exe $FirewallRuleName
        Write-Info 'adding the firewall rule: Windows now asks for administrator rights'
        try {
            $p = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $cmd)
            if ($p.ExitCode -eq 0) { Write-Info "firewall rule: $FirewallRuleName" } else { Write-Warn "the firewall rule could not be added (code $($p.ExitCode)); Windows will ask the first time the game goes online" }
        } catch {
            Write-Warn 'no administrator rights given: Windows will ask the first time the game goes online'
        }
    }
}

# ---- step 9: the build ---------------------------------------------------------------------------------------
function Step-Build {
    param([hashtable]$Answers)
    Write-Step '[9/10] Build the game'
    $build = @((Join-Path $Root 'tools\build_game.py'), '--game', $Answers.Game, '--keys', $Answers.Keys)
    if ($Answers.Firmware) { $build += @('--firmware', $Answers.Firmware) } else { $build += '--no-firmware' }   # (none: the open fonts)
    if ($Jobs) { $build += @('--jobs', "$Jobs") }
    if ($SkipBuild) { $build += '--prepare-only' }
    if ($DryRun) { Write-Would ('run: ' + ($Script:Python -join ' ') + ' ' + ($build -join ' ')); return $false }
    # (an update of the mod manager or other tools only: the game is as it was, no build - its own check)
    if (-not $Rebuild -and -not $SkipBuild -and (Test-Path (Join-Path $Root 'build\ssbu.exe')) -and
        (Invoke-Python @((Join-Path $Root 'tools\skyline.py'), 'uptodate')) -eq 0) {
        Write-Info 'nothing that goes into the game changed: no build (run OpenSmash.exe again to build it anyway)'
        return $true
    }
    if ($SkipBuild) { Write-Info 'the program, fonts, data, DLC and fighters; not the build itself (-SkipBuild)' }
    else { Write-Info 'the program, fonts, data, DLC and fighters, then the build: 30 minutes to a few hours' }
    if ((Invoke-Python $build) -ne 0) { throw 'The build stopped (the message above says why). Fix that, then run OpenSmash.exe again: it carries on where it stopped.' }
    if ($SkipBuild) { return $false }
    # the community's Skyline plugins that mods need (ARCropolis, Smashline 2, One Slot Effects, nro_hook),
    # from their own releases (tools\std_plugins.py checks each), then built into the game with the mods' own
    Write-Info 'the standard Skyline plugins: fetched from their releases and built in (tools\std_plugins.py, tools\skyline.py)'
    Write-Warn 'building the plugins into the game can take a while: do not start the game until this window says Done'
    if ((Invoke-Python @((Join-Path $Root 'tools\std_plugins.py'), 'fetch')) -ne 0) {
        Write-Warn 'the plugins could not be fetched (see above): mods that need them will not work until this runs again'
    } else {
        $sky = @((Join-Path $Root 'tools\skyline.py'), 'build')
        if ($Jobs) { $sky += @('--jobs', "$Jobs") }
        if ((Invoke-Python $sky) -ne 0) { Write-Warn 'building the plugins in stopped (see above); the game itself is built' }
    }
    return $true
}

# ---- step 10: shaders --------------------------------------------------------------------------------------------
function Step-Shaders {
    param([hashtable]$Answers, [bool]$Built)
    Write-Step '[10/10] Shaders'
    if (-not $Answers.Shaders) { Write-Info 'not wanted (launchers\Prepare Shaders.bat does it any time)'; return }
    if ($DryRun -or -not $Built) { Write-Would 'ask low, medium, or high, then compile the shaders in this window'; return }
    Write-Host ''
    Write-Host 'How hard should this PC work on the shaders? low keeps it usable, high finishes sooner.'
    Write-Host 'SHADER_PRIORITY?'
    $choice = ''
    try { $choice = [Console]::In.ReadLine() } catch { $choice = '' }
    if ($null -eq $choice) { $choice = '' }
    $choice = $choice.Trim().ToLower()
    if ($choice -notin @('low', 'medium', 'high')) { $choice = 'low' }
    Write-Info "shader priority: $choice"
    $py = @((Join-Path $Root 'tools\shader_aot.py'), '--priority', $choice)
    if ((Invoke-Python $py) -ne 0) {
        Write-Warn 'the shader compile stopped (see above). What finished is kept; launchers\Prepare Shaders.bat runs the rest.'
    }
}

function Main {
    $log = Join-Path $Root 'install.log'
    try { Start-Transcript -Path $log -Append | Out-Null } catch { }
    try {
        Write-Host 'WindowSmash setup' -ForegroundColor Cyan
        Write-Host "  $Root"
        if ($DryRun) { Write-Host '  dry run: saying what each step would do, doing nothing' -ForegroundColor DarkGray }
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        if (Step-Update) {
            # the rest is the new version's to do: start its installer in this window, as this one was
            Write-Host ''
            Write-Host 'Starting the updated installer' -ForegroundColor Cyan
            try { Stop-Transcript | Out-Null } catch { }
            $line = Join-CommandLine (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Root 'tools\install_windowsmash.ps1')) + (Get-RestartArguments $ScriptBound))
            $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $line -NoNewWindow -Wait -PassThru
            return $p.ExitCode
        }
        Step-CheckPc
        Step-Python
        if (-not $Script:Python) { Write-Host ''; Write-Host '(dry run) the later steps need Python; stopping here' -ForegroundColor DarkGray; return 0 }
        Step-Compiler
        Step-Sdl3
        Step-Codecs
        $answers = Step-GameFiles
        Step-Extras $answers
        $built = Step-Build $answers
        Step-Shaders $answers $built
        Write-Host ''
        if ($built) {
            Write-Host 'Done. Start the game with OpenSmash.exe, or the shortcut on the desktop.' -ForegroundColor Green
        } else {
            Write-Host 'Setup done.' -ForegroundColor Green
        }
        return 0
    } catch {
        Write-Host ''
        Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
        if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray }
        Write-Host "(everything above is also in $log)"
        return 1
    } finally {
        try { Stop-Transcript | Out-Null } catch { }
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    exit (Main)
}
