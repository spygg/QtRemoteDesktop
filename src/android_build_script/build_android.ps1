# ============================================================================
# QtRemoteDesktop Android one-key build (Qt 6 + arm64-v8a)
#
# Produces a debug APK from src/ with:
#   Qt 6.x android_arm64_v8a kit, ANDROID_ABI=arm64-v8a,
#   USE_FFMPEG=OFF, USE_WEBRTC=OFF, BUILD_THIRDPARTY=OFF.
#
# Usage:
#   build_android.bat
#   build_android.bat -QtRoot "E:\programes\Qt\Qt\6.11.1" -BuildDir "G:\remotedesk\build_android"
#   build_android.bat -Clean          # wipe the build dir before configuring
#
# Environment overrides (all optional):
#   QT_ANDROID_ROOT   Qt install root that contains android_arm64_v8a + mingw_64
#   ANDROID_SDK_ROOT  Android SDK root
#   ANDROID_NDK_ROOT  Android NDK root
#
# Idempotent: an existing build directory is reconfigured and rebuilt
# incrementally; nothing is deleted unless -Clean is passed.
# ============================================================================
[CmdletBinding()]
param(
    [string]$QtRoot = $env:QT_ANDROID_ROOT,
    [string]$BuildDir = "",
    [string]$SdkRoot = $env:ANDROID_SDK_ROOT,
    [string]$NdkRoot = $env:ANDROID_NDK_ROOT,
    [switch]$Clean
)

# Native tools (cmake/qt-cmake) print warnings on stderr; with
# $ErrorActionPreference='Stop' PowerShell 5.1 turns any stderr line into a
# terminating NativeCommandError. Keep it 'Continue' and check $LASTEXITCODE.
$ErrorActionPreference = 'Continue'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$LogFile = if ($BuildDir) { Join-Path $BuildDir 'android_build.log' } else { Join-Path $env:TEMP 'rd_android_build.log' }

function Stop-Build([string]$Message) {
    Write-Host ""
    Write-Host "[android-build] ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Find-NewestDir([string]$Path, [string]$Filter = '*') {
    if (-not (Test-Path $Path)) { return $null }
    Get-ChildItem -Path $Path -Directory -Filter $Filter -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}

# ---------------------------------------------------------------- locate Qt --
if (-not $QtRoot) {
    $candidates = @()
    foreach ($base in @("$env:ProgramFiles\Qt", 'C:\Qt', 'E:\programes\Qt\Qt')) {
        if (Test-Path $base) {
            foreach ($d in Get-ChildItem -Path $base -Directory -ErrorAction SilentlyContinue) {
                if (Test-Path (Join-Path $d.FullName 'android_arm64_v8a\bin\qt-cmake.bat')) {
                    $candidates += $d.FullName
                }
            }
        }
    }
    if ($candidates.Count -gt 0) {
        $QtRoot = ($candidates | Sort-Object -Descending | Select-Object -First 1)
    }
}
if (-not $QtRoot) {
    Stop-Build "Qt for Android not found. Set QT_ANDROID_ROOT, e.g. -QtRoot E:\programes\Qt\Qt\6.11.1"
}
$QtAndroid = Join-Path $QtRoot 'android_arm64_v8a'
$QtHost = Join-Path $QtRoot 'mingw_64'
$QtCmake = Join-Path $QtAndroid 'bin\qt-cmake.bat'
if (-not (Test-Path $QtCmake)) { Stop-Build "Missing Qt kit: $QtCmake (expected android_arm64_v8a)" }
if (-not (Test-Path $QtHost)) { Stop-Build "Missing Qt host tools: $QtHost (expected mingw_64)" }

# --------------------------------------------------------------- locate SDK --
if (-not $SdkRoot) {
    foreach ($cand in @($env:ANDROID_HOME, 'E:\programes\Android\SDK', 'C:\Android\Sdk', "$env:LOCALAPPDATA\Android\Sdk")) {
        if ($cand -and (Test-Path (Join-Path $cand 'platform-tools'))) { $SdkRoot = $cand; break }
    }
}
if (-not $SdkRoot -or -not (Test-Path $SdkRoot)) {
    Stop-Build "Android SDK not found. Set ANDROID_SDK_ROOT or pass -SdkRoot."
}
if (-not $NdkRoot) {
    # Prefer the NDK version known to work with this project (r27), else newest.
    $preferred = Join-Path $SdkRoot 'ndk\27.1.12297006'
    $NdkRoot = if (Test-Path $preferred) { $preferred } else { Find-NewestDir (Join-Path $SdkRoot 'ndk') }
}
if (-not $NdkRoot -or -not (Test-Path $NdkRoot)) {
    Stop-Build "Android NDK not found under $SdkRoot\ndk. Set ANDROID_NDK_ROOT or pass -NdkRoot."
}

# Ninja is required by the Qt Android toolchain; use the one shipped with SDK CMake.
$sdkCmakeBin = Find-NewestDir (Join-Path $SdkRoot 'cmake') '3.*' | ForEach-Object { Join-Path $_ 'bin' }
if ($sdkCmakeBin -and (Test-Path (Join-Path $sdkCmakeBin 'ninja.exe'))) {
    $env:PATH = "$sdkCmakeBin;$env:PATH"
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    Stop-Build "ninja.exe not found (expected under $SdkRoot\cmake\<ver>\bin). Install CMake/Ninja via SDK Manager."
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Stop-Build "cmake.exe not found in PATH."
}
if (-not $env:JAVA_HOME -or -not (Test-Path $env:JAVA_HOME)) {
    Stop-Build "JAVA_HOME is not set (Gradle needs a JDK 17+)."
}

if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'build_android' }
$LogFile = Join-Path $BuildDir 'android_build.log'
$ApkRel = 'remotedesk\android-build\build\outputs\apk\debug\android-build-debug.apk'
$ApkPath = Join-Path $BuildDir $ApkRel

Write-Host "Qt kit     : $QtAndroid"
Write-Host "Qt host    : $QtHost"
Write-Host "Android SDK: $SdkRoot"
Write-Host "Android NDK: $NdkRoot"
Write-Host "Build dir  : $BuildDir"
Write-Host ""

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "[android-build] Removing $BuildDir (-Clean)"
    Remove-Item -Path $BuildDir -Recurse -Force
}
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
Remove-Item -Path $LogFile -Force -ErrorAction SilentlyContinue

# ------------------------------------------------------ FFmpeg (arm64-v8a) --
# Cross-compiled once into build_output\ffmpeg_install-<ver>-android-arm64
# (strictly separate from the desktop MinGW build). The script is idempotent.
$FfmpegScript = Join-Path $PSScriptRoot 'build_ffmpeg_android.ps1'
if (-not (Test-Path $FfmpegScript)) {
    Stop-Build "build_ffmpeg_android.ps1 not found next to build_android.ps1"
}
Write-Host "[android-build] FFmpeg (arm64-v8a, MediaCodec) ..."
& pwsh -NoProfile -ExecutionPolicy Bypass -File $FfmpegScript -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) {
    Stop-Build "FFmpeg Android cross-build failed (exit $LASTEXITCODE)."
}

# ------------------------------------------------------------- configure ------
$configureArgs = @(
    '-S', (Join-Path $RepoRoot 'src'),
    '-B', $BuildDir,
    '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DQT_HOST_PATH=$QtHost",
    '-DQT_ANDROID_DEPLOYMENT_TYPE=Debug',
    '-DQT_ANDROID_ABIS=arm64-v8a',
    '-DANDROID_ABI=arm64-v8a',
    "-DANDROID_SDK_ROOT=$SdkRoot",
    "-DANDROID_NDK_ROOT=$NdkRoot",
    '-DUSE_FFMPEG=ON',
    '-DUSE_WEBRTC=OFF',
    '-DBUILD_THIRDPARTY=OFF'
)

Write-Host "[android-build] Configure..."
& $QtCmake @configureArgs 2>&1 | Tee-Object -FilePath $LogFile -Append
if ($LASTEXITCODE -ne 0) {
    Get-Content $LogFile -Tail 200 | Select-String -Pattern 'CMake Error|error:|FAILED:' -SimpleMatch:$false |
        Select-Object -First 1 | ForEach-Object { Stop-Build $_.Line.Trim() }
    Stop-Build "CMake configure failed (exit $LASTEXITCODE). Full log: $LogFile"
}

# --------------------------------------------------------------- build -------
Write-Host "[android-build] Build (target apk)..."
& cmake --build $BuildDir --target apk 2>&1 | Tee-Object -FilePath $LogFile -Append
if ($LASTEXITCODE -ne 0) {
    $firstError = Get-Content $LogFile -Tail 300 |
        Select-String -Pattern '^(FAILED:|ninja: error|CMake Error)|error: ' |
        Select-Object -First 1
    if ($firstError) { Stop-Build $firstError.Line.Trim() }
    Stop-Build "Gradle/CMake build failed (exit $LASTEXITCODE). Full log: $LogFile"
}

# ------------------------------------------------------------- summary -------
if (-not (Test-Path $ApkPath)) {
    $found = Get-ChildItem -Path $BuildDir -Filter '*.apk' -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($found) { $ApkPath = $found.FullName }
}
if (-not (Test-Path $ApkPath)) {
    Stop-Build "Build reported success but no APK was found under $BuildDir."
}

$size = (Get-Item $ApkPath).Length
# Compute SHA256 via .NET: Get-FileHash relies on module autoloading and is not
# available in some stripped PowerShell 5.1 installations.
$sha = ''
try {
    $shaAlg = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($ApkPath)
    $bytes = $shaAlg.ComputeHash($fs)
    $fs.Dispose()
    $shaAlg.Dispose()
    $sha = (($bytes | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
} catch {
    $sha = 'unavailable (' + $_.Exception.Message + ')'
}
$sizeMb = [math]::Round($size / 1MB, 2)

Write-Host ""
Write-Host "============================================================" -ForegroundColor Green
Write-Host " Android build succeeded" -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Green
Write-Host " APK     : $ApkPath"
Write-Host " Size    : $size bytes ($sizeMb MB)"
Write-Host " SHA256  : $sha"
Write-Host ""
Write-Host " Install : adb install -r `"$ApkPath`""
Write-Host " Launch  : adb shell am start -n com.radiometer.remotedesk/.MainActivity"
Write-Host ""
Write-Host " Build log: $LogFile"
exit 0
