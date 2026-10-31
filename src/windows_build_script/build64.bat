@echo off
setlocal EnableExtensions
rem ============================================
rem  QtRemoteDesktop Windows 64-bit one-key build (bat)
rem
rem  Same project as build.bat, but built as a 64-bit binary and linked against
rem  FFmpeg 6.1.2 instead of 3.4.8. Why a separate script:
rem    - 64-bit: NVIDIA ships no 32-bit CUDA, so a 32-bit process can never use
rem      h264_nvenc (cuInit returns CUDA_ERROR_NO_DEVICE).
rem    - FFmpeg >= 4.1: current drivers no longer answer the NVENC API 8.0
rem      "preset GUID" interface that 3.4.8 uses, so avcodec_open2 fails with
rem      -40 on this hardware. 6.1.2 opens preset p1-p7 and encodes.
rem  build.bat / build_onekey_build.ps1 keep producing the 32-bit 3.4.8 binary
rem  for XP / Win7 hosts; nothing they use is modified by this script.
rem
rem  Output is kept fully separated so the two bitnesses never overwrite
rem  each other: build64\  build_output64\  bin64\QtRemoteDesktop.exe
rem
rem  Requires: Git Bash + 64-bit Qt MinGW kit + CMake + nasm (optional)
rem  Auto-detected, override with env vars when needed:
rem    set QT_PREFIX_64=E:\programes\Qt\Qt\5.15.2\mingw81_64
rem    set MINGW_BIN_64=E:\programes\Qt\Qt\Tools\mingw810_64\bin
rem    set GIT_BASH=E:\programes\Git\bin\bash.exe
rem    set FFMPEG_VERSION=6.1.2
rem  FFmpeg / nv-codec-headers sources are fetched automatically when missing
rem  (see src\remotedesk\thridparty\ffmpeg) and then built by CMake itself,
rem  including the NVENC auto-probe.
rem
rem  NOTE: Keep this file ASCII-only. cmd.exe parses .bat with the system
rem  ANSI codepage (GBK on zh-CN); any non-ASCII comment may be mis-decoded
rem  and executed as a bogus command.
rem ============================================

set "SCRIPT_DIR=%~dp0"

set "PS_EXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS_EXE%" set "PS_EXE=powershell"

echo ============================================
echo  QtRemoteDesktop Windows 64-bit build (x64)
echo  FFmpeg 6.1.2 + NVENC auto-probe
echo ============================================
echo.

"%PS_EXE%" -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build_onekey_build64.ps1"
set "EXIT_CODE=%ERRORLEVEL%"

echo.
if "%EXIT_CODE%"=="0" (
    echo [OK] Build succeeded!
    echo Output: bin64^QtRemoteDesktop.exe
) else (
    echo [FAILED] Build error, exit code %EXIT_CODE%
)

exit /b %EXIT_CODE%
