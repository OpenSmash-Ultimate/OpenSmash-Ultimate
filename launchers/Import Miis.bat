@echo off
rem Imports your own Miis into miis\, where the game reads them (tools\miis.py; docs\PLAYING.md):
rem double-click it to import the Miis of every emulator found on this PC (Ryujinx, yuzu, Eden,
rem Citron, ...) and paste Mii Studio links, or drop Mii files (.charinfo, .nfsd, .ffsd, a
rem MiiDatabase.dat, ...) on it. Restart the game afterwards.
setlocal
cd /d "%~dp0.."
call tools\find_python.bat || (
    echo.
    pause
    exit /b 1
)
%PY% tools\miis.py import --ask %*
set "result=%ERRORLEVEL%"
echo.
pause
exit /b %result%
