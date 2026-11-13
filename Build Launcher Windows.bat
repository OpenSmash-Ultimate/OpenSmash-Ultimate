@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_launcher.ps1" %*
set "result=%ERRORLEVEL%"
echo.
pause
exit /b %result%
