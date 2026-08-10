# One-Key Build Script for QtRemoteDesktop
# Usage: Run this script in PowerShell as Administrator
# All project paths are derived relative to this script's location.

$ErrorActionPreference = 'Stop'

Write-Host "========================================"
Write-Host "  QtRemoteDesktop One-Key Build Script"
Write-Host "========================================"
Write-Host ""

# ---- Derived project paths (no hardcoded repo path) ----
$scriptDir = $PSScriptRoot
$srcDir    = [IO.Path]::GetFullPath((Join-Path $scriptDir '..'))
$repoRoot  = [IO.Path]::GetFullPath((Join-Path $srcDir '..'))
$buildDir  = Join-Path $repoRoot 'build'
$outDir    = Join-Path $repoRoot 'build_output'
$ffmpegDir = Join-Path $srcDir 'remotedesk\thridparty\ffmpeg'
$ffmpegSrc = Join-Path $ffmpegDir 'FFmpeg-n3.4.8'
$ffmpegTar = Join-Path $env:TEMP 'ffmpeg-3.4.8.tar.bz2'
$ffmpegLocalTar = Join-Path $ffmpegDir 'ffmpeg-3.4.8.tar.bz2'
$openh264Src     = Join-Path $srcDir 'remotedesk\thridparty\third_party_src\openh264'
$openh264Build   = Join-Path $buildDir 'openh264_build'
$openh264Install = Join-Path $outDir 'openh264_install'
$ffmpegBuild     = Join-Path $buildDir 'ffmpeg_build'
$ffmpegInstall   = Join-Path $outDir 'ffmpeg_install'

# ---- Toolchain detection (shared; override via env: QT_BASE_DIR / GIT_BASH / MINGW_MAKE) ----
. (Join-Path $scriptDir 'build_common.ps1')

$toolchain = Get-Toolchain $repoRoot
$gitBash   = Get-GitBash
$qtPrefix  = $toolchain.Prefix
$mingwBin  = $toolchain.MingwBin
Write-Host "Git Bash : $gitBash"
Write-Host "Qt prefix: $qtPrefix"
Write-Host "MinGW bin: $mingwBin"
Write-Host ""

# Prepend mingw to PATH for openh264 make / gcc resolution
$env:PATH = "$mingwBin;$env:PATH"

# ---- 1. Prepare FFmpeg source ----
if (-not (Test-Path "$ffmpegSrc\configure")) {
    # Prefer the local tar bundle over re-downloading
    if (-not (Test-Path $ffmpegLocalTar) -and -not (Test-Path $ffmpegTar)) {
        Write-Host "[1/5] Downloading FFmpeg 3.4.8..."
        Invoke-WebRequest -Uri "https://ffmpeg.org/releases/ffmpeg-3.4.8.tar.bz2" -OutFile $ffmpegTar -UseBasicParsing
    }
    $tarToUse = if (Test-Path $ffmpegLocalTar) { $ffmpegLocalTar } else { $ffmpegTar }
    Write-Host "Extracting $tarToUse..."
    if (Test-Path $ffmpegSrc) { Remove-Item -LiteralPath $ffmpegSrc -Recurse -Force }
    tar -xjf $tarToUse -C $ffmpegDir
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg source extraction failed (corrupt tar?)" }
    $extracted = Get-ChildItem -LiteralPath $ffmpegDir -Directory -Filter 'ffmpeg-*'
    if ($extracted) {
        Move-Item -LiteralPath $extracted.FullName -Destination $ffmpegSrc -Force
    }
    if (-not (Test-Path "$ffmpegSrc\configure")) { throw "FFmpeg source not complete after extraction" }
}

# ---- 2. Build openh264 ----
Write-Host "[2/5] Building openh264..."
if (-not (Test-Path "$openh264Install\lib\libopenh264.a")) {
    if (Test-Path $openh264Build) { Remove-Item -LiteralPath $openh264Build -Recurse -Force }
    New-Item -ItemType Directory -Path $openh264Build -Force | Out-Null
    New-Item -ItemType Directory -Path "$openh264Install\lib" -Force | Out-Null
    New-Item -ItemType Directory -Path "$openh264Install\include\wels" -Force | Out-Null

    Copy-Item -Path "$openh264Src\*" -Destination $openh264Build -Recurse -Force
    Push-Location $openh264Build
    try {
        # make 的 install 在 Windows 下会吃掉反斜杠，PREFIX 需用正斜杠
        $prefixFwd = $openh264Install.Replace('\', '/')
        & make OS=mingw_nt ARCH=x86 CC=gcc AR=ar "PREFIX=$prefixFwd" DESTDIR='' all install -j4
        if ($LASTEXITCODE -ne 0) { throw "openh264 make failed" }
    } finally {
        Pop-Location
    }
}

# ---- 3. Build FFmpeg ----
Write-Host "[3/5] Building FFmpeg..."
$openh264Inc = "$openh264Install\include\wels"
$openh264Lib = "$openh264Install\lib"

if (-not (Test-Path "$ffmpegInstall\lib\libavcodec.a")) {
    if (Test-Path $ffmpegBuild) { Remove-Item -LiteralPath $ffmpegBuild -Recurse -Force }
    New-Item -ItemType Directory -Path $ffmpegBuild -Force | Out-Null
    New-Item -ItemType Directory -Path "$ffmpegInstall\lib" -Force | Out-Null

    $unixBuild = ConvertTo-UnixPath $ffmpegBuild
    $unixSrc   = ConvertTo-UnixPath $ffmpegSrc
    $unixInst  = ConvertTo-UnixPath $ffmpegInstall
    $unixRepo  = ConvertTo-UnixPath $repoRoot
    $winRepo   = $repoRoot.Replace('\', '/')
    $msysPath  = ConvertTo-UnixPath $mingwBin
    $cflags    = "-I" + $openh264Inc.Replace('\', '/')
    $ldflags   = "-L" + $openh264Lib.Replace('\', '/')

    Write-Host "Configuring & building FFmpeg via git-bash (this takes a while)..."
    & $gitBash -lc "cd $unixBuild && PATH=${msysPath}:`$PATH && $unixSrc/configure --cc=gcc --ld=gcc --target-os=mingw32 --arch=x86 --disable-debug --disable-doc --disable-programs --disable-network --disable-avformat --disable-avfilter --disable-avdevice --disable-swresample --disable-postproc --disable-avresample --disable-everything --enable-swscale --enable-libopenh264 --enable-encoder=libopenh264 --enable-decoder=h264 --enable-decoder=mjpeg --enable-decoder=mpeg4 --disable-x86asm --enable-static --disable-shared --extra-cflags=$cflags --extra-ldflags=$ldflags --extra-libs=-lopenh264 --prefix=$unixInst"
    $cfgExit = $LASTEXITCODE
    if ($cfgExit -ne 0 -or -not (Test-Path "$ffmpegBuild\ffbuild\config.mak")) {
        throw "FFmpeg configure failed (exit $cfgExit). Check the output above."
    }
    Write-Host "Configure OK (ffbuild/config.mak present)"

    # mingw make needs Windows-style paths in generated Makefiles
    Get-ChildItem $ffmpegBuild -Recurse -Include '*.mak','*.mk','Makefile','*.sh' | ForEach-Object {
        (Get-Content $_.FullName -Raw) -replace "$unixRepo/", "$winRepo/" | Set-Content $_.FullName -Encoding UTF8
    }

    & $gitBash -lc "cd $unixBuild && PATH=${msysPath}:`$PATH && make -j4 && (make install || true)"
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg make failed" }

    # make install sometimes fails on header paths (mingw make + git-bash install
    # path mangling); the static libs are installed fine, so copy headers explicitly.
    foreach ($lib in @('libavutil','libavcodec','libswscale')) {
        New-Item -ItemType Directory -Path "$ffmpegInstall\include\$lib" -Force | Out-Null
        Copy-Item -Path "$ffmpegSrc\$lib\*.h" -Destination "$ffmpegInstall\include\$lib\" -Force -ErrorAction SilentlyContinue
    }
    Copy-Item -LiteralPath "$ffmpegBuild\libavutil\avconfig.h" -Destination "$ffmpegInstall\include\libavutil\" -Force -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath "$ffmpegBuild\libavutil\ffversion.h" -Destination "$ffmpegInstall\include\libavutil\" -Force -ErrorAction SilentlyContinue
    foreach ($h in @('libavcodec\avcodec.h','libavutil\avutil.h','libswscale\swscale.h')) {
        if (-not (Test-Path "$ffmpegInstall\include\$h")) { throw "FFmpeg header missing: $h" }
    }
    foreach ($l in @('libavcodec.a','libavutil.a','libswscale.a')) {
        if (-not (Test-Path "$ffmpegInstall\lib\$l")) { throw "FFmpeg library missing: $l" }
    }
}

# ---- 4. Configure CMake ----
Write-Host "[4/5] Configuring CMake..."
& cmake -G "MinGW Makefiles" -S $srcDir -B $buildDir `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_PREFIX_PATH="$qtPrefix" `
    -DBUILD_THIRDPARTY=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

# ---- 5. Build project ----
Write-Host "[5/5] Building project..."
& cmake --build $buildDir -j4 --config Release
if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

Write-Host ""
Write-Host "========================================"
Write-Host "  Build Complete!"
Write-Host "========================================"
Write-Host ""
Write-Host "Output: $outDir\release\QtRemoteDesktop.exe"
