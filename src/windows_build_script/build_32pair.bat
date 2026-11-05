@echo off
setlocal EnableExtensions
rem ============================================
rem  QtRemoteDesktop Windows 32-bit paired build (bat)
rem  ONE COMMAND -> TWO PACKAGES:
rem    bin_win7_612\QtRemoteDesktop.exe   FFmpeg 6.1.2  (Win7 / 8 / 10 / 11 32-bit)
rem    bin_xp_348\QtRemoteDesktop.exe     FFmpeg 3.4.8  (Windows XP)
rem
rem  BUILD RULES enforced by build_onekey_build_32pair.ps1:
rem    R1  Win7 (6.1.2) is built FIRST, XP (3.4.8) SECOND.
rem    R2  Both trees share <repo>\build_output_win32\lib for static libs and
rem        libVideoEncoder.a holds FFmpeg-version specific objects; linking the
rem        other tree's archive either breaks the link (undefined reference to
rem        avcodec_register_all) or ships an exe with no FFmpeg at all.
rem    R3  Every target purges <repo>\build_output_win32\lib\*.a before its
rem        configure+build step.
rem    R4  Each exe is checked for its own FFmpeg marker + openh264 before the
rem        script reports success, and the run ends with size + SHA256 of both
rem        exes - compare those with what is deployed on the target machine.
rem
rem  Usage:
rem    build_32pair.bat                 rem both packages, Win7 first
rem    build_32pair.bat win7            rem FFmpeg 6.1.2 package only
rem    build_32pair.bat xp              rem FFmpeg 3.4.8 package only
rem
rem  NOTE: Keep this file ASCII-only (cmd.exe reads .bat with the ANSI
rem  codepage; non-ASCII comments can be mis-decoded and executed).
rem ============================================

setlocal EnableExtensions
set "SCRIPT_DIR=%~dp0"

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=both"

set "PS_EXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS_EXE%" set "PS_EXE=powershell"

echo ========================================
echo  QtRemoteDesktop 32-bit paired build
echo  order: Win7 (FFmpeg 6.1.2) -^> XP (FFmpeg 3.4.8)
echo ========================================
echo.

"%PS_EXE%" -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build_onekey_build_32pair.ps1" -Target "%TARGET%"
set "EXIT_CODE=%ERRORLEVEL%"

echo.
if "%EXIT_CODE%"=="0" (
    echo [OK] Build succeeded - check the SHA256 report above.
) else (
    echo [FAILED] Build error, exit code %EXIT_CODE%
)

exit /b %EXIT_CODE%
