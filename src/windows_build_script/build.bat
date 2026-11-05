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
rem  FFmpeg: this script is the Windows XP 32-bit compatible path and pins
rem  FFmpeg 3.4.8 (build_onekey_build.ps1 passes -DFFMPEG_VERSION=3.4.8).
rem  Win7 32-bit and every other target use 6.1.2: configure a separate build
rem  directory with -DFFMPEG_VERSION=6.1.2 on the same Qt 5.7 toolchain.
rem
rem  32-BIT BUILD RULES:
rem    R1  Build Win7 32-bit (FFmpeg 6.1.2) FIRST, XP 32-bit (3.4.8) SECOND.
rem    R2  Both 32-bit trees share <repo>\build_output_win32\lib as their
rem        static-lib output dir. libVideoEncoder.a contains FFmpeg-version
rem        specific objects, so an archive from the other tree is linked as-is
rem        ("undefined reference to avcodec_register_all" for the 6.1.2 link,
rem        or an exe with no FFmpeg at all for the 3.4.8 link).
rem    R3  Always delete <BUILD_OUTPUT_DIR>\lib\*.a before building. Both
rem        build_onekey_build.ps1 and build_onekey_build_32pair.ps1 do this
rem        automatically; only manual cmake runs must do it by hand.
rem    R4  Compare the printed size/SHA256 with the deployed exe.
rem  For both 32-bit packages in one command (order + purge + verification
rem  + SHA256 report are built in) use build_32pair.bat.
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
