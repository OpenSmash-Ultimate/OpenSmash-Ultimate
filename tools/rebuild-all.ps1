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
# (no .NET SDK: the game is still rebuilt; the launcher is then the latest release's, as after a failed rebuild)
$HaveDotnet = [bool](Get-Command dotnet -ErrorAction SilentlyContinue)

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
# --no-input: nobody can answer a question here (the launcher reads this script's output, it has no window to type
# in): a folder build_game.py still needs and dump\build_folders.txt does not name stops it with a message saying so,
# instead of waiting forever at "Press Enter to keep the folder in [brackets]" (Discord, 2026-11-18).
$buildArgs = @("-u", $GameBuild, "--no-input")
if ($Jobs -gt 0) { $buildArgs += @("--jobs", $Jobs.ToString()) }
& $python @buildArgs
if ($LASTEXITCODE -ne 0) { throw "The game build failed ($LASTEXITCODE)." }

function Invoke-ReleaseLauncher {
    <# The latest release's OpenSmash.exe, checked against its published SHA-256 (install_windowsmash.ps1 -LauncherOnly):
       $true when OpenSmash.exe is the release's now. A running exe is renamed, not overwritten, so this works with the
       launcher still open. #>
    $installer = Join-Path $PSScriptRoot "install_windowsmash.ps1"
    if (-not (Test-Path -LiteralPath $installer)) { return $false }
    $p = Start-Process -FilePath "powershell.exe" -WindowStyle Hidden -Wait -PassThru -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$installer`"", "-LauncherOnly")
    return ($p.ExitCode -eq 0)
}

Write-Output "__OPENSMASH_PHASE__:launcher"
$launcherProblem = $null
if ($HaveDotnet) {
    try {
        if ($LauncherPid -gt 0) {
            & $LauncherBuild -Configuration $Configuration -LauncherPid $LauncherPid
        }
        else {
            & $LauncherBuild -Configuration $Configuration
        }
        if (-not $?) { $launcherProblem = "The launcher build failed." }
    }
    catch { $launcherProblem = $_.Exception.Message }
}
else { $launcherProblem = "dotnet was not found (the .NET 8 SDK builds the launcher)." }
if ($launcherProblem) {
    Write-Output "The launcher was not rebuilt: $launcherProblem Using the latest release's OpenSmash.exe instead."
    if (-not (Invoke-ReleaseLauncher)) {
        # (a git checkout, a copy following main, or no download: the game is built all the same; the open launcher stays)
        Write-Output "The latest release's OpenSmash.exe could not be put here either: the game is built, the launcher is the one you had."
        if ($LauncherPid -gt 0) { Write-Output "Build complete."; exit 0 }
    }
    elseif ($LauncherPid -gt 0) {
        Write-Output "__OPENSMASH_RESTART__"
        while (Get-Process -Id $LauncherPid -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 200 }
    }
}

if (-not (Test-Path -LiteralPath $Exe)) { throw "The launcher build finished but $Exe is missing." }
Write-Output "Build complete."
Start-Process -FilePath $Exe -WorkingDirectory $Root
