@echo off
rem Called by the Play scripts before starting the root runtime.
setlocal
cd /d "%~dp0.."
set "plugin_python=python"
if exist ".venv\Scripts\python.exe" set "plugin_python=.venv\Scripts\python.exe"
if exist "deps\python\python.exe" set "plugin_python=deps\python\python.exe"
"%plugin_python%" tools\os_mods.py prepare
exit /b %errorlevel%
