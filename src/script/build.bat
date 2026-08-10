@echo off
setlocal EnableExtensions
rem ============================================
rem  QtRemoteDesktop Windows one-key build (bat)
rem  Requires: Git Bash + Qt (MinGW) + CMake
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
    echo Output: build_output\release\QtRemoteDesktop.exe
) else (
    echo [FAILED] Build error, exit code %EXIT_CODE%
)

exit /b %EXIT_CODE%
