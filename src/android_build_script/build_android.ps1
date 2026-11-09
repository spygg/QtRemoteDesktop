# ============================================================================
# QtRemoteDesktop Android one-key build (Qt 6 + arm64-v8a)
#
# Produces a signed APK from src/ with:
#   Qt 6.x android_arm64_v8a kit, ANDROID_ABI=arm64-v8a,
#   USE_FFMPEG=ON (MediaCodec cross-build), USE_WEBRTC=OFF, BUILD_THIRDPARTY=OFF.
#   Deployment type defaults to Release (what CI publishes); pass -DeploymentType
#   Debug for a debuggable package.
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
    [ValidateSet('Release', 'Debug')]
    [string]$DeploymentType = 'Release',
    [switch]$Clean
)

# Native tools (cmake/qt-cmake) print warnings on stderr; with
# $ErrorActionPreference='Stop' PowerShell 5.1 turns any stderr line into a
# terminating NativeCommandError. Keep it 'Continue' and check $LASTEXITCODE.
$ErrorActionPreference = 'Continue'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

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
$QtCmake = Join-Path $QtAndroid 'bin\qt-cmake.bat'
if (-not (Test-Path $QtCmake)) { Stop-Build "Missing Qt kit: $QtCmake (expected android_arm64_v8a)" }
# QT_HOST_PATH must be a desktop kit: androiddeployqt/moc/rcc are host executables and
# live there, not in the android kit. The offline installer calls it mingw_64 while other
# Qt 6 layouts use win64_mingw, and a runner may only have the MSVC one - accept any kit
# dir that actually carries androiddeployqt.
$QtHost = $null
foreach ($name in @('mingw_64', 'win64_mingw', 'msvc2022_64')) {
    $cand = Join-Path $QtRoot $name
    if (Test-Path (Join-Path $cand 'bin\androiddeployqt.exe')) { $QtHost = $cand; break }
}
if (-not $QtHost) {
    $QtHost = Get-ChildItem $QtRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\androiddeployqt.exe') } |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $QtHost) {
    Stop-Build "No desktop Qt kit with androiddeployqt.exe found under $QtRoot (QT_HOST_PATH is required)."
}

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
# Per-run log name: the Gradle daemon spawned by the build step inherits the write
# handle that Tee-Object opens and keeps it for hours (idle timeout), so a fixed name
# is unopenable for the next run and every Tee in this run starts warning.
$LogFile = Join-Path $BuildDir ("android_build-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))

Write-Host "Qt kit     : $QtAndroid"
Write-Host "Qt host    : $QtHost"
Write-Host "Android SDK: $SdkRoot"
Write-Host "Android NDK: $NdkRoot"
Write-Host "Build dir  : $BuildDir"
Write-Host "Deploy type: $DeploymentType"
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
    "-DQT_ANDROID_DEPLOYMENT_TYPE=$DeploymentType",
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

# ------------------------------------------------ sign + verify + summary --
# 按「最新写出的 android-build*.apk」取包，不预设路径：上一轮签好的
# android-build-release.apk 会一直留在原地，先匹配预期路径就等于反复签同一个旧包。
$found = Get-ChildItem -Path $BuildDir -Filter 'android-build*.apk' -Recurse -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $found) {
    $found = Get-ChildItem -Path $BuildDir -Filter '*.apk' -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
if (-not $found) {
    Stop-Build "Build reported success but no APK was found under $BuildDir."
}
$ApkPath = $found.FullName

$buildToolsDir = Find-NewestDir (Join-Path $SdkRoot 'build-tools')
$apksigner = if ($buildToolsDir) { Join-Path $buildToolsDir 'apksigner.bat' } else { $null }
if (-not $apksigner -or -not (Test-Path $apksigner)) {
    Stop-Build "apksigner.bat not found under $SdkRoot\build-tools. Install a Build-Tools package via SDK Manager."
}

# Qt 6 的 androiddeployqt/CMake 流程不签 release 包（QT_ANDROID_KEYSTORE_* 那几个属性
# 只有 Qt5 + qmake 的 gradle 模板会读），交出来的是 android-build-release-unsigned.apk。
# 这里用仓库内置的 debug.keystore 显式签名，本机和 CI 才是同一个签名身份，老包才能
# 覆盖升级。zipalign 必须在 sign 之前：APK Signing Block 一写进去，再对齐就作废签名。
# Debug 包由 gradle 用「这台机器自动生成」的 debug keystore 签，只适合本机调试，不发布。
if ($DeploymentType -eq 'Release') {
    $keystore = Join-Path $PSScriptRoot 'debug.keystore'
    if (-not (Test-Path $keystore)) {
        Stop-Build "src/android_build_script/debug.keystore is missing; the release APK cannot be signed."
    }
    $zipalign = Join-Path $buildToolsDir 'zipalign.exe'
    if (-not (Test-Path $zipalign)) {
        Stop-Build "zipalign.exe not found in $buildToolsDir."
    }
    $aligned = "$ApkPath.aligned"
    $signed = Join-Path (Split-Path $ApkPath -Parent) ((Split-Path $ApkPath -Leaf) -replace '-unsigned', '')
    Write-Host "[android-build] zipalign + apksigner (debug.keystore)..."
    # 不能再 Tee 进 $LogFile：gradle daemon 继承了前面 Tee-Object 打开的写句柄并一直持有，
    # 追加会报 "being used by another process"。这两个工具只在失败时才有诊断价值。
    $alignOut = & $zipalign -f 4 $ApkPath $aligned 2>&1
    if ($LASTEXITCODE -ne 0) {
        $alignOut | ForEach-Object { Write-Host $_ }
        Stop-Build "zipalign failed (exit $LASTEXITCODE) on $ApkPath"
    }
    $signOut = & $apksigner sign --ks $keystore --ks-pass 'pass:android' --key-pass 'pass:android' `
        --ks-key-alias androiddebugkey --out $signed $aligned 2>&1
    Remove-Item $aligned -Force -ErrorAction SilentlyContinue
    # apksigner 会在输出旁边留一份增量签名缓存，只对本机重签有用，不该混进产物目录
    Remove-Item "$signed.idsig" -Force -ErrorAction SilentlyContinue
    if ($LASTEXITCODE -ne 0) {
        $signOut | ForEach-Object { Write-Host $_ }
        Stop-Build "apksigner sign failed (exit $LASTEXITCODE)."
    }
    if ($signed -ne $ApkPath) { Remove-Item $ApkPath -Force -ErrorAction SilentlyContinue }
    $ApkPath = $signed
}

# A release APK that silently ends up unsigned installs nowhere, so verify the
# signature here and fail the build - CI gates the release on this job.
$verifyOut = & $apksigner verify --print-certs --min-sdk-version 29 $ApkPath 2>&1
if ($LASTEXITCODE -ne 0) {
    $verifyOut | ForEach-Object { Write-Host $_ }
    Stop-Build "APK signature verification failed (exit $LASTEXITCODE): $ApkPath"
}
$verifyOut | Select-String -Pattern 'DN:|Verified using' | ForEach-Object { Write-Host " $($_.Line.Trim())" }

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
