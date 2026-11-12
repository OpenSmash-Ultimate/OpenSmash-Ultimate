# Builds OpenSmash.exe (the launcher) and copies it to the folder root.
# The root exe is a build product. *.exe is gitignored, so GitHub shows the
# project (OpenSmash.Launcher) and this script, not the 72 MB file.
# Close OpenSmash first. Windows will not replace an exe that is still running.
param(
    [string]$Configuration = "Release",
    [int]$LauncherPid = 0
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $Root "OpenSmash.Launcher\OpenSmash.Launcher.csproj"
$Out = Join-Path $Root "OpenSmash.Launcher\bin\publish"
$Exe = Join-Path $Root "OpenSmash.exe"
$StagedExe = Join-Path $Root "OpenSmash.new.exe"

if (-not (Test-Path $Project)) { throw "Launcher project not found: $Project" }
$dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
if (-not $dotnet) { throw "dotnet was not found. Install the .NET 8 SDK, then run this again." }

if ($LauncherPid -gt 0) {
    $launcher = Get-Process -Id $LauncherPid -ErrorAction SilentlyContinue
    if (-not $launcher -or $launcher.ProcessName -ne "OpenSmash") {
        throw "The launcher process to restart was not found."
    }
}
elseif (Get-Process -Name OpenSmash -ErrorAction SilentlyContinue) {
    throw "OpenSmash is still running. Quit it (the window and the game), then run this again."
}

Write-Output "Publishing the launcher..."
& dotnet publish $Project -c $Configuration -o $Out
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)." }

$built = Join-Path $Out "OpenSmash.exe"
if (-not (Test-Path $built)) { throw "Publish finished but $built is missing." }
Copy-Item -LiteralPath $built -Destination $StagedExe -Force

if ($LauncherPid -gt 0) {
    Write-Output "__OPENSMASH_RESTART__"
    while (Get-Process -Id $LauncherPid -ErrorAction SilentlyContinue) {
        Start-Sleep -Milliseconds 200
    }
}

Move-Item -LiteralPath $StagedExe -Destination $Exe -Force
Write-Output "Wrote $Exe"
