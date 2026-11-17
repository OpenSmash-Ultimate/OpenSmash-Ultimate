@echo off
setlocal
cd /d "%~dp0"
set "launcher_python=python"
if exist "%~dp0.venv\Scripts\python.exe" set "launcher_python=%~dp0.venv\Scripts\python.exe"
if exist "%~dp0deps\venv\Scripts\python.exe" set "launcher_python=%~dp0deps\venv\Scripts\python.exe"
"%launcher_python%" "%~dp0tools\build_avalonia.py" --rid win-x64 %*
set "result=%ERRORLEVEL%"
echo.
pause
exit /b %result%
