@echo off
rem Removes WindowSmash Mods and puts the game back as it was: the game reads its own files again,
rem the modded data is deleted, the mod manager is removed (files it replaced put back); it asks
rem whether to delete your downloaded mods too. (tools\modmanager\install_mods.py --uninstall)
rem It runs from a copy in %TEMP%, as it removes this file.
setlocal
if not "%~1"=="--from-temp" (
    copy /y "%~f0" "%TEMP%\windowsmash-mods-uninstall.bat" >nul
    "%TEMP%\windowsmash-mods-uninstall.bat" --from-temp "%~dp0.."
    exit /b
)
cd /d "%~2"
call tools\find_python.bat || (pause & exit /b 1)
%PY% tools\modmanager\install_mods.py --target "%CD%" --uninstall
echo.
pause
