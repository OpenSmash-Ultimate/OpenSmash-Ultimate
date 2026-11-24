<#
.SYNOPSIS
RecompLauncher's setup step (recomplauncher.json "prepare"): the whole install, unattended.

.DESCRIPTION
RecompLauncher (https://recomplauncher.com) runs this without a window after the player picked their copy of the game
and their Switch keys. It runs tools\install_opensmash.ps1 -NoWizard with those folders, never asks anything, and
turns its steps ("[N/10] ...") and the build's percentages into the lines RecompLauncher reads:
    progress <0..1> <text>
Exit 0 when the game is built. A second run with nothing changed is quick (install_opensmash.ps1 skips the build
when nothing that goes into the game changed) and ends with "progress 1 Already built".

.PARAMETER Game
The folder with the game's files (the game, its 13.0.5 update, DLC), RecompLauncher's {input:game}.
.PARAMETER ProdKeys
.PARAMETER TitleKeys
The keys RecompLauncher keeps (shared switch.prod-keys / switch.title-keys): files, or a folder holding both.
.PARAMETER AcceptLicense
The player ticked Microsoft's license terms for the C runtime and Windows SDK in RecompLauncher (the compiler download
needs them; RecompLauncher never ticks it for the player).
.PARAMETER DryRun
Say what would run, run nothing (tests).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$ProdKeys,
    [string]$TitleKeys,
    [switch]$AcceptLicense,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

function Say([double]$Fraction, [string]$Text) {
    $f = [Math]::Min(1.0, [Math]::Max(0.0, $Fraction))
    [Console]::Out.WriteLine(('progress {0} {1}' -f $f.ToString('0.###', [Globalization.CultureInfo]::InvariantCulture), $Text))
    [Console]::Out.Flush()
}

# install_opensmash.ps1 wants one folder with prod.keys and title.keys: RecompLauncher may give two files.
$keys = $ProdKeys
if (Test-Path -LiteralPath $ProdKeys -PathType Leaf) {
    $keys = Join-Path $Root 'dump\recomplauncher_keys'
    New-Item -ItemType Directory -Force -Path $keys | Out-Null
    Copy-Item -LiteralPath $ProdKeys -Destination (Join-Path $keys 'prod.keys') -Force
    if ($TitleKeys) {
        $title = if (Test-Path -LiteralPath $TitleKeys -PathType Leaf) { $TitleKeys } else { Join-Path $TitleKeys 'title.keys' }
        if (Test-Path -LiteralPath $title) { Copy-Item -LiteralPath $title -Destination (Join-Path $keys 'title.keys') -Force }
    }
}

$install = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Root 'tools\install_opensmash.ps1'),
             '-NoWizard', '-NoUpdate', '-Game', $Game, '-Keys', $keys, '-NoFirmware')
if ($AcceptLicense) { $install += '-AcceptLicense' }
if ($DryRun) { $install += '-DryRun' }

# Each install step's share of the whole (the build, step 9, is most of the time).
$start = @{ 1 = 0.0; 2 = 0.01; 3 = 0.02; 4 = 0.05; 5 = 0.12; 6 = 0.13; 7 = 0.14; 8 = 0.25; 9 = 0.26; 10 = 0.99 }
$step = 0; $built = $false; $nothing = $false
Say 0 'Starting the OpenSmash setup'
$env:PYTHONUNBUFFERED = '1'
$ErrorActionPreference = 'Continue'   # (the install's stderr lines must not stop this script: PowerShell 5.1 wraps them as errors)
& powershell.exe @install 2>&1 | ForEach-Object {
    $line = [string]$_
    if ($line -match '^\[(\d+)/10\]\s*(.+)$') {
        $step = [int]$Matches[1]
        Say $start[$step] $Matches[2].Trim()
    } elseif ($step -eq 9 -and $line -match '(\d{1,3})%') {
        $pct = [int]$Matches[1]
        if ($pct -le 100) { Say (0.26 + 0.72 * $pct / 100) ('Building the game: ' + $line.Trim()) }
    }
    if ($line -match 'nothing that goes into the game changed') { $nothing = $true }
    # (everything else goes to the launcher's log, unchanged)
    [Console]::Error.WriteLine($line)
}
$code = $LASTEXITCODE
if ($code -ne 0) { exit $code }
Say 1 $(if ($nothing) { 'Already built' } else { 'Done' })
exit 0
