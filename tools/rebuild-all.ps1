param(
    [int]$LauncherPid = 0,
    [string]$Configuration = "Release",
    [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$GameBuild = Join-Path $Root "tools\build_game.py"
$LauncherBuild = Join-Path $PSScriptRoot "build_launcher.ps1"
$Exe = Join-Path $Root "OpenSmash.exe"

if (-not (Test-Path -LiteralPath $GameBuild)) { throw "Game build script not found: $GameBuild" }
if (-not (Test-Path -LiteralPath $LauncherBuild)) { throw "Launcher build script not found: $LauncherBuild" }
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw "dotnet was not found. Install the .NET 8 SDK before rebuilding the launcher."
}

$python = @(
    (Join-Path $Root "deps\venv\Scripts\python.exe"),
    (Join-Path $Root "deps\python\python.exe")
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $python) {
    foreach ($name in @("python3", "python")) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { $python = $command.Source; break }
    }
}
if (-not $python) { throw "Python 3.10 or newer was not found. Run the setup first." }

if (Get-Process -Name ssbu -ErrorAction SilentlyContinue) {
    throw "The game is still running. Close it before building again."
}

if ($LauncherPid -gt 0) {
    $launcher = Get-Process -Id $LauncherPid -ErrorAction SilentlyContinue
    if (-not $launcher -or $launcher.ProcessName -ne "OpenSmash") {
        throw "The launcher process to restart was not found."
    }
}
elseif (Get-Process -Name OpenSmash -ErrorAction SilentlyContinue) {
    throw "OpenSmash is still running. Close it before using this script."
}

Write-Output "__OPENSMASH_PHASE__:game"
$buildArgs = @("-u", $GameBuild)
if ($Jobs -gt 0) { $buildArgs += @("--jobs", $Jobs.ToString()) }
& $python @buildArgs
if ($LASTEXITCODE -ne 0) { throw "The game build failed ($LASTEXITCODE)." }

Write-Output "__OPENSMASH_PHASE__:launcher"
if ($LauncherPid -gt 0) {
    & $LauncherBuild -Configuration $Configuration -LauncherPid $LauncherPid
}
else {
    & $LauncherBuild -Configuration $Configuration
}
if (-not $?) { throw "The launcher build failed." }

if (-not (Test-Path -LiteralPath $Exe)) { throw "The launcher build finished but $Exe is missing." }
Write-Output "Build complete."
Start-Process -FilePath $Exe -WorkingDirectory $Root
