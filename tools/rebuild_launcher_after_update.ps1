param(
    [Parameter(Mandatory = $true)][int]$WaitForProcessId,
    [Parameter(Mandatory = $true)][string]$Repo,
    [Parameter(Mandatory = $true)][string]$LauncherPath,
    [Parameter(Mandatory = $true)][string]$PendingMarker
)

$ErrorActionPreference = 'Stop'
$Repo = [IO.Path]::GetFullPath($Repo)
$LauncherPath = [IO.Path]::GetFullPath($LauncherPath)
$PendingMarker = [IO.Path]::GetFullPath($PendingMarker)

while (Get-Process -Id $WaitForProcessId -ErrorAction SilentlyContinue) {
    Start-Sleep -Milliseconds 250
}

$buildScript = Join-Path $Repo 'tools\build_launcher.ps1'
try {
    if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) {
        throw 'tools\build_launcher.ps1 is missing.'
    }
    & $buildScript
    if (-not (Test-Path -LiteralPath $LauncherPath -PathType Leaf)) {
        throw 'The launcher build finished without producing OpenSmash.exe.'
    }
    if (Test-Path -LiteralPath $PendingMarker) { Remove-Item -LiteralPath $PendingMarker -Force }
    $started = Start-Process -FilePath $LauncherPath -WorkingDirectory $Repo -PassThru
    if (-not $started) { throw 'Windows did not start the rebuilt launcher.' }
}
catch {
    $reason = $_.Exception.Message
    [IO.File]::WriteAllText($PendingMarker, [DateTime]::UtcNow.ToString('O'))
    Add-Type -AssemblyName System.Windows.Forms
    [void][Windows.Forms.MessageBox]::Show(
        "OpenSmash could not rebuild the launcher.`n`n$reason`n`nThe current launcher will be reopened. Retry from its next startup prompt.",
        'OpenSmash update',
        [Windows.Forms.MessageBoxButtons]::OK,
        [Windows.Forms.MessageBoxIcon]::Error)
    if (Test-Path -LiteralPath $LauncherPath -PathType Leaf) {
        Start-Process -FilePath $LauncherPath -WorkingDirectory $Repo | Out-Null
    }
    exit 1
}
