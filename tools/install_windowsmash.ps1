<#
.SYNOPSIS
The setup's old name: it runs tools\install_opensmash.ps1 with the same arguments and exits with its exit code.

.DESCRIPTION
Kept because older OpenSmash.exe (v0.3 and v0.4) and older copies start the setup by this name, for example
-UpdateOnly, -BuildAfterUpdate, -PythonOnly or -LauncherOnly. Those launchers read this file before they run it:
it must take [switch]$UpdateOnly and [switch]$PythonOnly and be for $RepoName = 'OpenSmash-Ultimate', and the
installer it starts takes every one of those switches. A copy that got this file alone (one updated by such a
launcher) gets install_opensmash.ps1 from GitHub first.
#>
$RepoOwner = 'OpenSmash-Ultimate'
$RepoName = 'OpenSmash-Ultimate'
$target = Join-Path $PSScriptRoot 'install_opensmash.ps1'

if (-not (Test-Path -LiteralPath $target)) {
    $ref = if ("$env:OPENSMASH_UPDATE_COMMIT" -match '^[0-9a-fA-F]{40}$') { $env:OPENSMASH_UPDATE_COMMIT } else { 'main' }
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri "https://raw.githubusercontent.com/$RepoOwner/$RepoName/$ref/tools/install_opensmash.ps1" -OutFile "$target.new"
        Move-Item -LiteralPath "$target.new" -Destination $target -Force
    } catch {
        Write-Host "tools\install_opensmash.ps1 is missing and could not be downloaded ($($_.Exception.Message)). Download OpenSmash again."
        exit 1
    }
}

# A process of its own, as when it is started by name: the same exit code in every case (an exit N, an error, the
# end of the script), its output on this one's, and an empty argument kept.
function ConvertTo-Argument([string]$a) {
    if ($a -ne '' -and $a -notmatch '[\s"]') { return $a }
    '"' + (($a -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"'
}
$line = (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $target) + @($args) | ForEach-Object { ConvertTo-Argument "$_" }) -join ' '
$p = Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $line -NoNewWindow -PassThru
$null = $p.Handle   # (keeps the exit code readable after the process ends)
$p.WaitForExit()
exit $p.ExitCode
