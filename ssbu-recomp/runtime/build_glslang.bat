@echo off
rem Builds ssbu_glslang.dll (vk_glslang_shim.c + the glslang 16.6.0 release libraries) with MSVC. Run from the source folder
rem (scripts\setup.py does). GLSLANG_ROOT = the folder of the glslang release (include\ and lib\); default ..\reference\upstream\glslang-16.6.0,
rem or a deps\glslang beside the source (ssbu-recomp\deps, or the merged tree's deps) when scripts/install.py has put it there.
rem Without Visual Studio the same build is made with the toolchain this repo sets up (deps\llvm's clang-cl over deps\msvc, as
rem tools/cc.py builds the rest of the program), so no Visual Studio install is needed for the DLL alone.
rem See plan\phases\V1_vulkan.md section 4 for why this is a DLL and not linked into the exe.
setlocal
if "%GLSLANG_ROOT%"=="" (
  set GLSLANG_ROOT=..\reference\upstream\glslang-16.6.0
  if exist "%~dp0..\deps\glslang\lib\glslang.lib" set GLSLANG_ROOT=%~dp0..\deps\glslang
  if exist "%~dp0..\..\deps\glslang\lib\glslang.lib" set GLSLANG_ROOT=%~dp0..\..\deps\glslang
)
set GL=%GLSLANG_ROOT%
set VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VSDIR=
if exist "%VSW%" "%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\ssbu_vsdir.txt"
if exist "%TEMP%\ssbu_vsdir.txt" set /p VSDIR=<"%TEMP%\ssbu_vsdir.txt"
if not "%VSDIR%"=="" goto msvc

rem no Visual Studio / Build Tools: clang-cl (SSBU_CLANG if set, else the deps\llvm this repo sets up) with deps\msvc as the
rem system root, the same toolchain tools/cc.py builds the program with
set CC=%SSBU_CLANG%
if "%CC%"=="" set CC=%~dp0..\deps\llvm\bin\clang-cl.exe
if not exist "%CC%" if not defined SSBU_CLANG set CC=%~dp0..\..\deps\llvm\bin\clang-cl.exe
set SYSROOT=%SSBU_MSVC_ROOT%
if "%SYSROOT%"=="" set SYSROOT=%~dp0..\deps\msvc
if not exist "%SYSROOT%\VC\Tools\MSVC" if not defined SSBU_MSVC_ROOT set SYSROOT=%~dp0..\..\deps\msvc
if not exist "%CC%" ( echo build_glslang: no Visual Studio / Build Tools with the C++ x64 tools found, and no clang-cl at %CC% ^(set SSBU_CLANG, or run tools/msvc_deps.py^) & exit /b 1 )
set LLD=
for %%I in ("%CC%") do if exist "%%~dpIlld-link.exe" set LLD=-fuse-ld=lld
set CLROOT=
set LKROOT=
if exist "%SYSROOT%\VC\Tools\MSVC" set CLROOT=/winsysroot "%SYSROOT%"
if exist "%SYSROOT%\VC\Tools\MSVC" set LKROOT=/winsysroot:"%SYSROOT%"
if not exist obj_host mkdir obj_host
"%CC%" /nologo /MD /O2 /W3 /LD %LLD% %CLROOT% /I "%GL%\include" vk_glslang_shim.c /Fo:obj_host\vk_glslang_shim.obj /Fe:ssbu_glslang.dll /link %LKROOT% /LIBPATH:"%GL%\lib" glslang.lib glslang-default-resource-limits.lib SPIRV-Tools-opt.lib SPIRV-Tools.lib
exit /b %errorlevel%

:msvc
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist obj_host mkdir obj_host
cl /nologo /MD /O2 /W3 /LD /I "%GL%\include" vk_glslang_shim.c /Fo:obj_host\vk_glslang_shim.obj /Fe:ssbu_glslang.dll /link /LIBPATH:"%GL%\lib" glslang.lib glslang-default-resource-limits.lib SPIRV-Tools-opt.lib SPIRV-Tools.lib
exit /b %errorlevel%
