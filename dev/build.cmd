@echo off
rem dev\build.cmd - the development build from a plain command prompt or VS Code: Ninja (parallel) in
rem build-dev\, the game files not built in (the program finds them in _references\dos).
rem Usage: dev\build.cmd [Release|Debug|RelWithDebInfo]   (default: RelWithDebInfo)
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=RelWithDebInfo"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "delims=" %%i in ('call "%VSWHERE%" -latest -products * -property installationPath') do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio / Build Tools not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0.."
set "OUT=build-dev\%CONFIG%"
rem Configure only the first time: afterwards the build configures again by itself when needed
if not exist "%OUT%\build.ninja" (
    cmake -S . -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DSYN_EMBED_GAME=OFF || exit /b 1
)
cmake --build "%OUT%" || exit /b 1
