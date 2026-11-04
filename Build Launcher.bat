@echo off
rem Builds OpenSmash.exe and puts it in the folder root (next to this game).
rem Close OpenSmash first. The exe is not stored in git; this rebuilds it.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_launcher.ps1" %*
set "result=%ERRORLEVEL%"
echo.
pause
exit /b %result%
