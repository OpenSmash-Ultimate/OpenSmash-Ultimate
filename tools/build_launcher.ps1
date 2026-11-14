# Windows adapter shared by Build Launcher Windows.bat and the game installer.
param([ValidateSet('Release','Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$launcherRoot = Split-Path -Parent $PSScriptRoot
$embeddedPython = Join-Path $launcherRoot 'deps/python/python.exe'
$builder = Join-Path $PSScriptRoot 'build_launcher.py'
if (Test-Path -LiteralPath $embeddedPython) {
    & $embeddedPython $builder --rid win-x64 --configuration $Configuration
} elseif (Get-Command py -ErrorAction SilentlyContinue) {
    & py -3 $builder --rid win-x64 --configuration $Configuration
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    & python $builder --rid win-x64 --configuration $Configuration
} else {
    throw 'Python 3 was not found. Install Python 3 and the .NET 8 SDK, then run this again.'
}
if ($LASTEXITCODE -ne 0) { throw "OpenSmash build failed ($LASTEXITCODE)." }
