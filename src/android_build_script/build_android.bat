@echo off
setlocal EnableExtensions
rem ==========================================================
rem  QtRemoteDesktop Android one-key build (Qt 6, arm64-v8a)
rem
rem  Wraps build_android.ps1. One command produces a debug APK:
rem      src\android_build_script\build_android.bat
rem
rem  Options are forwarded to the PowerShell script:
rem      build_android.bat -QtRoot "E:\programes\Qt\Qt\6.11.1"
rem      build_android.bat -BuildDir "G:\remotedesk\build_android"
rem      build_android.bat -Clean
rem
rem  NOTE: Keep this file ASCII-only. cmd.exe parses .bat with the
rem  system ANSI codepage (GBK on zh-CN); non-ASCII comments can be
rem  mis-decoded and executed as bogus commands.
rem ==========================================================

set "SCRIPT_DIR=%~dp0"
set "PS_EXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS_EXE%" set "PS_EXE=powershell"

echo ==========================================================
echo  QtRemoteDesktop Android build (Qt 6 / arm64-v8a)
echo  FFmpeg=OFF  WebRTC=OFF  ThirdParty=OFF
echo ==========================================================
echo.

"%PS_EXE%" -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build_android.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"

echo.
if "%EXIT_CODE%"=="0" (
    echo [OK] Android build succeeded.
) else (
    echo [FAILED] Android build failed, exit code %EXIT_CODE%.
)

exit /b %EXIT_CODE%
