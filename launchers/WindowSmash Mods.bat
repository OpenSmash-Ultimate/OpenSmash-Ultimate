@echo off
rem Opens WindowSmash Mods, the mod manager (tools\modmanager\server.py): GameBanana's Smash Ultimate
rem mods found, installed, turned on and off, and shared as modpacks. It opens in a window of its own
rem (Edge's app mode); closing that window ends it.
setlocal
cd /d "%~dp0.."
call tools\find_python.bat || (pause & exit /b 1)
set "PYW=%PY%"
if exist "deps\python\pythonw.exe" set "PYW=deps\python\pythonw.exe"
start "WindowSmash Mods" %PYW% tools\modmanager\server.py
