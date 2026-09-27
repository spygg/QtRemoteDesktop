@echo off
setlocal EnableExtensions
rem ============================================
rem  QtRemoteDesktop Windows one-key build (bat)
rem  Requires: Git Bash + Qt (MinGW) + CMake
rem
rem  Dependencies / install hints:
rem    - Qt 5.x (MinGW kit, with QtWebSockets): https://www.qt.io/download
rem       or use Qt Online Installer, tick Qt 5.15.2 > MinGW 8.1.0 64-bit
rem    - CMake 3.15 or newer:  https://cmake.org/download/  (tick Add to PATH)
rem    - Git Bash (provides bash.exe): https://git-scm.com/
rem    - mingw32-make: bundled with Qt, at <Qt>\Tools\mingw*\bin\mingw32-make.exe
rem
rem  Paths are auto-detected (Qt under E:\programes\Qt / C:\Qt / C:\Program
rem  Files\Qt, Git Bash under E:\programes\Git / C:\Program Files\Git, or from
rem  an existing build\CMakeCache.txt). Override with env vars only if needed:
rem    set QT_BASE_DIR=E:\programes\Qt\QtLegacy\5.7\mingw53_32
rem    set GIT_BASH=E:\programes\Git\bin\bash.exe
rem    set MINGW_MAKE=E:\programes\Qt\QtLegacy\Tools\mingw530_32\bin\mingw32-make.exe
rem  If auto-detection still fails, the script prompts you to enter the paths.
rem
rem  Build is incremental: build/ and build_output/ are never deleted
rem  unless you clean them explicitly.
rem
rem  NOTE: Keep this file ASCII-only. cmd.exe parses .bat with the system
rem  ANSI codepage (GBK on zh-CN); any non-ASCII (e.g. Chinese) comments
rem  will be mis-decoded and may be executed as bogus commands.
rem ============================================

rem script directory
set "SCRIPT_DIR=%~dp0"

rem locate powershell
set "PS_EXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS_EXE%" set "PS_EXE=powershell"

echo ========================================
echo  QtRemoteDesktop Windows one-key build
echo ========================================
echo.

"%PS_EXE%" -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build_onekey_build.ps1"
set "EXIT_CODE=%ERRORLEVEL%"

echo.
if "%EXIT_CODE%"=="0" (
    echo [OK] Build succeeded!
    echo Output: bin\QtRemoteDesktop.exe
) else (
    echo [FAILED] Build error, exit code %EXIT_CODE%
)

exit /b %EXIT_CODE%
