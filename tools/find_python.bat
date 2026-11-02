@echo off
rem Finds the Python that runs WindowSmash's tools, for the launchers: sets PY to it. First the
rem one Install WindowSmash.bat set up (deps\python), then python on the PATH, then the py
rem launcher - the first that is 3.10 or newer. Run from the port's folder; exits with 1, after
rem saying what to do, if there is none.
set "PY="
if exist "deps\python\python.exe" (
    set "PY=deps\python\python.exe"
    exit /b 0
)
python -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1 && set "PY=python"
if defined PY exit /b 0
py -3 -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1 && set "PY=py -3"
if defined PY exit /b 0
echo Python 3.10 or newer was not found. Double-click "OpenSmash.exe" to set everything
echo up, or install Python as the setup guide says ^(docs\BUILDING.md^), with "Add python.exe to
echo PATH" ticked, then run this again.
exit /b 1
