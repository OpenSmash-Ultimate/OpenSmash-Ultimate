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

function Invoke-ReleaseLauncherFallback {
    <# The release's OpenSmash.exe through the installer (-LauncherOnly): $true when OpenSmash.exe is the release's now. #>
    param([string]$Repo)
    $installer = Join-Path $Repo 'tools\install_windowsmash.ps1'
    if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) { return $false }
    if (-not (Select-String -LiteralPath $installer -Pattern 'LauncherOnly' -SimpleMatch -Quiet)) { return $false }
    try {
        $p = Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden -Wait -PassThru -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$installer`"", '-LauncherOnly')
        return ($p.ExitCode -eq 0)
    } catch { return $false }
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
    # The rebuild failed ("dotnet publish failed", a broken or missing .NET SDK): the latest release's OpenSmash.exe,
    # checked against its published SHA-256, as on a PC without the SDK (install_windowsmash.ps1 -LauncherOnly,
    # Update-LauncherExe). A git checkout, a copy following main, or no download: the error below, as before.
    if (Invoke-ReleaseLauncherFallback -Repo $Repo) {
        if (Test-Path -LiteralPath $PendingMarker) { Remove-Item -LiteralPath $PendingMarker -Force }
        if (Test-Path -LiteralPath $LauncherPath -PathType Leaf) {
            Start-Process -FilePath $LauncherPath -WorkingDirectory $Repo | Out-Null
        }
        exit 0
    }
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
