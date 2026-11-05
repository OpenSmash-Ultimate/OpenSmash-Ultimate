# Client-only connection to an existing yuzu-compatible room. Windows PowerShell 5.1+.
[CmdletBinding()]
param(
    [string]$Server,
    [string]$Nickname,
    [ValidateSet('eden','yuzu')][string]$ProxyProfile,
    [Security.SecureString]$Password,
    [switch]$NoPrompt,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$gameExe = Join-Path $repoRoot 'build\ssbu.exe'
$configFile = Join-Path $repoRoot 'config\yuzu_client.json'
try {
    if (!(Test-Path -LiteralPath $gameExe)) { throw 'Build OpenSmash first (Build WindowSmash.bat).' }
    # Use the same clock settings for the feature check and the game process.
    $env:SSBU_RB='0'
    $env:SSBU_RUNAHEAD='0'
    # Old executables must not silently ignore the selected client backend.
    $feature = & $gameExe --yuzu-client-check 2>$null
    if ($LASTEXITCODE -ne 0 -or $feature -notcontains 'yuzu-room-client protocol=1') {
        throw 'This ssbu.exe has no yuzu client. Rebuild the game runtime first.'
    }
    $saved = $null
    if (Test-Path -LiteralPath $configFile) { $saved = Get-Content -LiteralPath $configFile -Raw | ConvertFrom-Json }
    if (!$Server -and $saved) { $Server = $saved.server }
    if (!$Nickname -and $saved) { $Nickname = $saved.nickname }
    if (!$ProxyProfile -and $saved -and $saved.proxy_profile) {
        $savedProfile = [string]$saved.proxy_profile
        if ($savedProfile -notin @('eden','yuzu')) { throw 'ProxyProfile must be eden or yuzu.' }
        $ProxyProfile = $savedProfile
    }
    if (!$ProxyProfile) { $ProxyProfile = 'eden' }
    if ($ProxyProfile -notin @('eden','yuzu')) { throw 'ProxyProfile must be eden or yuzu.' }
    $ProxyProfile = $ProxyProfile.ToLowerInvariant()
    if (!$Server -and !$NoPrompt) { $Server = Read-Host 'Existing yuzu room server (host:port)' }
    if (!$Nickname) { $Nickname = 'OpenSmash' + (Get-Random -Minimum 1000 -Maximum 10000) }
    if (!$Server -or $Server -notmatch '^[a-zA-Z0-9._-]+(?::([0-9]{1,5}))?$') { throw 'Enter a hostname or IPv4 address, optionally followed by :port.' }
    if ($Matches[1] -and ([int]$Matches[1] -lt 1 -or [int]$Matches[1] -gt 65535)) { throw 'Port must be 1 through 65535.' }
    if ($Server.Length -ge 256) { throw 'Server address is too long.' }
    if ($Nickname -notmatch '^[ a-zA-Z0-9._-]{4,20}$') { throw 'Nickname must be 4-20 ASCII letters, digits, spaces, dots, underscores or hyphens.' }
    # Prefer the repository's Python, including the installer's embedded runtime.
    $pythonExe = @('.venv\Scripts\python.exe','deps\python\python.exe') |
        ForEach-Object { Join-Path $repoRoot $_ } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (!$pythonExe) { $pythonExe = (Get-Command python -ErrorAction Stop).Source }
    foreach ($library in @('enet.dll','libzstd.dll')) {
        if (!(Test-Path -LiteralPath (Join-Path $repoRoot ('build\yuzu\' + $library)))) {
            & $pythonExe (Join-Path $repoRoot 'tools\yuzu_client_deps.py')
            if ($LASTEXITCODE -ne 0) { throw 'Client libraries could not be built.' }
            break
        }
    }
    # CheckOnly validates the complete pre-launch path, including plugin preparation.
    Push-Location $repoRoot
    try {
        & $pythonExe (Join-Path $repoRoot 'tools\skyline.py') build --tree $repoRoot
        if ($LASTEXITCODE -ne 0) { throw 'Plugin preparation failed.' }
    } finally { Pop-Location }
    if ($CheckOnly) { Write-Output ('yuzu client ready: ' + $Server + ' / ' + $Nickname + ' / UDP profile: ' + $ProxyProfile); exit 0 }
    if (!$Password -and !$NoPrompt) { $Password = Read-Host 'Room password (Enter if none; not saved)' -AsSecureString }
    $plainPassword = ''
    if ($Password) {
        $buffer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password)
        try { $plainPassword = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($buffer) }
        finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($buffer) }
    }
    if ([Text.Encoding]::UTF8.GetByteCount($plainPassword) -gt 128) { throw 'Room password is too long.' }
    New-Item -ItemType Directory -Path (Split-Path -Parent $configFile) -Force | Out-Null
    @{ server=$Server; nickname=$Nickname; proxy_profile=$ProxyProfile } | ConvertTo-Json | Set-Content -LiteralPath $configFile -Encoding UTF8
    # Keep enough metadata to diagnose mixed-client joining without per-frame HLE spam.
    if (!(Test-Path Env:SSBU_NET_LOG)) { $env:SSBU_NET_LOG='1' }
    $env:SSBU_NET_BACKEND='yuzu'
    $env:SSBU_YUZU_SERVER=$Server
    $env:SSBU_YUZU_PROXY_PROFILE=$ProxyProfile
    $env:SSBU_YUZU_PASSWORD=$plainPassword
    $env:SSBU_NET_NAME=$Nickname
    $env:SSBU_RB_NET=''
    $env:SSBU_NET_HOST=''
    $env:SSBU_NET_RELAY=''
    $env:SSBU_DATA_ROOT=''
    $env:SSBU_INPUT=''
    Push-Location $repoRoot
    try {
        $started = Start-Process -FilePath $gameExe -ArgumentList @('dump\modules','--status','30','--online') -WorkingDirectory $repoRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $repoRoot 'build\yuzu-client.stdout.log') -RedirectStandardError (Join-Path $repoRoot 'build\yuzu-client.log')
        Write-Output ('Started yuzu room client. Process: ' + $started.Id + '. Log: build\yuzu-client.log')
    } finally { Pop-Location }
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
