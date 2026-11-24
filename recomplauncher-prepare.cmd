@echo off
rem RecompLauncher's setup step (recomplauncher.json "prepare"): tools\recomplauncher_prepare.ps1, unattended.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\recomplauncher_prepare.ps1" %*
exit /b %ERRORLEVEL%
