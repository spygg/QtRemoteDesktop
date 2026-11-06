# One-Key Build Script for QtRemoteDesktop
# Usage: Run this script in PowerShell as Administrator
# All project paths are derived relative to this script's location.
#
# This is the Windows XP 32-bit compatible build path and the only build that
# pins FFmpeg 3.4.8 (passed to CMake explicitly below). Win7 32-bit and every
# other target use FFmpeg 6.1.2: reuse the same Qt 5.7 mingw53_32 toolchain but
# point a separate build directory at it with -DFFMPEG_VERSION=6.1.2.
#
# =============================================================================
# 32-bit BUILD RULES (see also build_onekey_build_32pair.ps1 / build.bat / README)
#
#   R1  Order: Win7 32-bit (FFmpeg 6.1.2) FIRST, XP 32-bit (FFmpeg 3.4.8) SECOND.
#       For the pair in one command use build_onekey_build_32pair.ps1,
#       which enforces this order and prints both hashes.
#
#   R2  Both 32-bit trees write their static libs into the same shared archive
#       directory: <BUILD_OUTPUT_DIR>/lib (CMAKE_ARCHIVE_OUTPUT_DIRECTORY).
#       libVideoEncoder.a embeds FFmpeg-version-specific objects, so an archive
#       from the other tree gets linked as-is: the 6.1.2 link then fails with
#       "undefined reference to `avcodec_register_all'", and the 3.4.8 link
#       silently produces an exe with no FFmpeg at all (7,394,065 bytes instead
#       of ~10.2 MB on 2026-11-04).
#
#   R3  This script therefore deletes <outDir>/lib/*.a before configuring.
#       Purging every archive (not just libVideoEncoder.a) keeps the invariant
#       simple and costs only re-archive + relink. FFmpeg/openh264 static libs
#       live in <outDir>/ffmpeg_install* / openh264_install and are not touched.
#
#   R4  The final exe's size and SHA256 are printed at the end so the package
#       can be compared with what is actually deployed on the target machine.
#
#   NOTE: this script's own build/ + build_output/ tree is private, but if you
#   also maintain the Win7 32-bit tree against the same BUILD_OUTPUT_DIR, the
#   purge in R3 is what keeps the two from contaminating each other. Prefer
#   build_onekey_build_32pair.ps1 in that case.
# =============================================================================

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
$ffmpegVersion   = '3.4.8'   # XP-compatible path: the only build pinned to 3.4.8
$ffmpegSrc = Join-Path $ffmpegDir "FFmpeg-n$ffmpegVersion"
$ffmpegTar = Join-Path $env:TEMP "ffmpeg-$ffmpegVersion.tar.bz2"
$ffmpegLocalTar = Join-Path $ffmpegDir "ffmpeg-$ffmpegVersion.tar.bz2"
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

# ---- Package fingerprint helpers (R4) ----
# Get-FileHash does not exist on PowerShell 2.0 (default on Windows 7 / XP),
# so hash with .NET directly. Upper-case hex, same format as certutil.
function Get-FileSha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($Path)
        try { $hash = $sha.ComputeHash($fs) } finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
    return (($hash | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
}

function Format-Size([long]$Bytes) {
    return "{0:N0} bytes ({1:N2} MB)" -f $Bytes, ($Bytes / 1MB)
}

# 共享 build_output 里的 openh264 产物可能是别的构建树（64 位）写下的：
# 本脚本第 2 步原先只判断文件"存在"就跳过重建，于是链接期出现
#   undefined reference to `WelsCreateSVCEncoder'
# （64 位 pe-x86-64 归档无法被 32 位 ld 使用，2026-11-06 实测）。这里用
# 目标工具链自带的 objdump 校验归档格式，格式不符就重建。
function Test-MingwArchive32([string]$Archive, [string]$MingwBin) {
    if (-not (Test-Path -LiteralPath $Archive)) { return $false }
    $objdump = Join-Path $MingwBin 'objdump.exe'
    if (-not (Test-Path -LiteralPath $objdump)) { return $true }   # 无法校验时保持旧行为
    # 归档里混入异架构成员时 objdump 会把错误写到 stderr；这里放宽 EAP，
    # 只按输出内容判定，避免诊断过程本身抛异常。
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $out = & $objdump -f $Archive 2>&1 | Out-String }
    finally { $ErrorActionPreference = $prev }
    return ($out -match 'file format pe-i386')
}

# ---- Shared-archive purge (R2/R3) ----
# Both 32-bit trees put their static libs in <BUILD_OUTPUT_DIR>/lib and
# libVideoEncoder.a embeds FFmpeg-version-specific objects, so an archive left
# by a tree built against the other FFmpeg version is linked as-is. Dropping
# every archive before configure keeps the two trees from contaminating each
# other; the libs are re-archived from objects that already exist.
# Retry: Defender / a lingering linker can hold a handle for a moment
# ("Unable to rename ... Permission denied" while ar re-creates the archive).
function Reset-SharedArchives([string]$LibDir) {
    if (-not (Test-Path $LibDir)) {
        New-Item -ItemType Directory -Path $LibDir -Force | Out-Null
        return
    }
    $removed = 0
    for ($attempt = 1; $attempt -le 6; $attempt++) {
        $left = @(Get-ChildItem -LiteralPath $LibDir -Filter '*.a' -File -ErrorAction SilentlyContinue)
        if (-not $left) { break }
        foreach ($f in $left) {
            Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue
            if (-not (Test-Path -LiteralPath $f.FullName)) { $removed++ }
        }
        if (-not (Get-ChildItem -LiteralPath $LibDir -Filter '*.a' -File -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 400
    }
    $left = @(Get-ChildItem -LiteralPath $LibDir -Filter '*.a' -File -ErrorAction SilentlyContinue)
    if ($left) {
        throw ("could not purge shared archives in {0}: {1} still locked" -f
               $LibDir, (($left | Select-Object -ExpandProperty Name) -join ', '))
    }
    Write-Host "[purge] removed $removed shared archive(s) from $LibDir (R2/R3)"
}

# ---- Stray-header cleanup ------------------------------------------------
# src/remotedesk/ must never hold loose .h files. Older runs (and a failed
# 'make install', see note in stage 3) dropped FFmpeg's libavutil headers
# there. Remove them so the source tree stays clean.
$remotedeskDir = Join-Path $srcDir 'remotedesk'
function Remove-StrayHeaders([string]$dir) {
    $stray = Get-ChildItem -LiteralPath $dir -Filter '*.h' -File -ErrorAction SilentlyContinue
    if ($stray) {
        Write-Host "[cleanup] removing $($stray.Count) stray header(s) from $dir"
        $stray | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
    }
}
Remove-StrayHeaders $remotedeskDir

# Prepend mingw to PATH for openh264 make / gcc resolution
$env:PATH = "$mingwBin;$env:PATH"

# ---- 1. Prepare FFmpeg source ----
if (-not (Test-Path "$ffmpegSrc\configure")) {
    # Prefer the local tar bundle over re-downloading
    if (-not (Test-Path $ffmpegLocalTar) -and -not (Test-Path $ffmpegTar)) {
        Write-Host "[1/5] Downloading FFmpeg $ffmpegVersion..."
        Invoke-WebRequest -Uri "https://ffmpeg.org/releases/ffmpeg-$ffmpegVersion.tar.bz2" -OutFile $ffmpegTar -UseBasicParsing
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
$openh264Archive = "$openh264Install\lib\libopenh264.a"
if (-not (Test-MingwArchive32 $openh264Archive $mingwBin)) {
    if (Test-Path -LiteralPath $openh264Archive) {
        Write-Host "[2/5] existing openh264 archive is not 32-bit MinGW - rebuilding" -ForegroundColor Yellow
        if (Test-Path $openh264Install) { Remove-Item -LiteralPath $openh264Install -Recurse -Force }
    }
    if (Test-Path $openh264Build) { Remove-Item -LiteralPath $openh264Build -Recurse -Force }
    New-Item -ItemType Directory -Path $openh264Build -Force | Out-Null
    New-Item -ItemType Directory -Path "$openh264Install\lib" -Force | Out-Null
    New-Item -ItemType Directory -Path "$openh264Install\include\wels" -Force | Out-Null

    Copy-Item -Path "$openh264Src\*" -Destination $openh264Build -Recurse -Force
    # openh264 的 Makefile 在 install 阶段调用 sh 工具链（install/sed/sort/awk），
    # 这些只在 Git for Windows 的 usr\bin 里；不补 PATH 会报 "install: command not found"。
    $gitUsrBin = Join-Path (Split-Path (Split-Path $gitBash -Parent) -Parent) 'usr\bin'
    Push-Location $openh264Build
    $prevPath = $env:PATH
    try {
        $env:PATH = "$env:PATH;$gitUsrBin"
        # 不调用 Makefile 的 install 目标：它依赖 sh 的 install/sed，在 mingw make
        # 派生的 sh PATH 里找不到（2026-11-06 实测）。all 目标产出 libopenh264.a，
        # 这里直接按 install 的布局手工落盘。
        & make OS=mingw_nt ARCH=x86 CC=gcc AR=ar all -j4
        if ($LASTEXITCODE -ne 0) { throw "openh264 make failed" }
        Copy-Item -LiteralPath (Join-Path $openh264Build 'libopenh264.a') -Destination $openh264Archive -Force
        Copy-Item -Path (Join-Path $openh264Build 'codec\api\wels\*.h') -Destination "$openh264Install\include\wels" -Force
    } finally {
        $env:PATH = $prevPath
        Pop-Location
    }
}
if (-not (Test-MingwArchive32 $openh264Archive $mingwBin)) {
    throw "openh264 archive is not a 32-bit MinGW archive: $openh264Archive"
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

    # ---- pkg-config support for FFmpeg configure ----
    # FFmpeg 3.4.8 (this XP-compatible build) detects libopenh264 via pkg-config.
    # Two layers make the detection use THIS repo's openh264 no matter where the
    # repo lives:
    #   1. QTRD_OPENH264_INC/LIB env vars (repo-relative, derived from $scriptDir)
    #      consumed by the global pkg-config shim in Git\usr\bin.
    #   2. A repo-local pkg-config shim placed first on PATH (used when the
    #      filesystem honors the exec bit; harmless otherwise).
    $openh264Fwd = $openh264Install.Replace('\', '/')
    $pkgCfgDir = Join-Path $ffmpegBuild 'pkgconfig_tools'
    New-Item -ItemType Directory -Path $pkgCfgDir -Force | Out-Null
    $shim = @'
#!/bin/sh
# Auto-generated by build_onekey_build.ps1 - paths derived relative to this repo.
OPENH264_INC="__QTRD_INC__"
OPENH264_LIB="__QTRD_LIB__"
case "$1" in
  --exists)  exit 0 ;;
  --cflags)  echo "-I$OPENH264_INC" ;;
  --libs)    echo "-L$OPENH264_LIB -lopenh264" ;;
  --version) echo "2.6.0" ;;
  *) echo "Package openh264 was not found in the pkg-config search path." >&2; exit 1 ;;
esac
'@
    $shim = $shim.Replace('__QTRD_INC__', $openh264Fwd + '/include').Replace('__QTRD_LIB__', $openh264Fwd + '/lib')
    $shimFile = Join-Path $pkgCfgDir 'pkg-config'
    [System.IO.File]::WriteAllText($shimFile, $shim, (New-Object System.Text.UTF8Encoding($false)))
    $unixShimDir = ConvertTo-UnixPath $pkgCfgDir
    & $gitBash -lc "chmod +x $unixShimDir/pkg-config 2>/dev/null; true"

    # ---- x86asm 自动判定（与 build_linux.sh 对齐）----
    # --disable-x86asm 会把 libavcodec 的 nasm 汇编（h264/mpeg4/mjpeg 解码等
    # .asm 实现）全部关掉，退化为纯 C（此前无条件硬编码，h264 软解偏慢）。
    # 这里检测 nasm/yasm：有则启用 x86asm，无则退纯 C 并提示。
    # 注意：inline asm（swscale 的 MMX 转换）不受此开关影响，保持启用。
    $ffX86AsmFlag = '--disable-x86asm'
    $asmCheck = (& $gitBash -lc 'command -v nasm >/dev/null 2>&1 && echo nasm || { command -v yasm >/dev/null 2>&1 && echo yasm || echo none; }').Trim()
    if ($asmCheck -eq 'none') {
        Write-Host "[ffmpeg][warn] 未找到 nasm/yasm，x86asm 将退化为纯 C（h264 软解等变慢）" -ForegroundColor Yellow
    } else {
        $ffX86AsmFlag = ''
        Write-Host "[ffmpeg] 检测到汇编器 $asmCheck，启用 x86asm 汇编优化"
    }

    Write-Host "Configuring & building FFmpeg via git-bash (this takes a while)..."
    & $gitBash -lc "cd $unixBuild && QTRD_OPENH264_INC=${openh264Fwd}/include QTRD_OPENH264_LIB=${openh264Fwd}/lib PATH=${unixShimDir}:${msysPath}:`$PATH && $unixSrc/configure --cc=gcc --ld=gcc --target-os=mingw32 --arch=x86 --disable-debug --disable-doc --disable-programs --disable-network --disable-avformat --disable-avfilter --disable-avdevice --disable-swresample --disable-postproc --disable-avresample --disable-everything --enable-swscale --enable-libopenh264 --enable-encoder=libopenh264 --enable-decoder=h264 --enable-decoder=mjpeg --enable-decoder=mpeg4 $ffX86AsmFlag --enable-static --disable-shared --extra-cflags=$cflags --extra-ldflags=$ldflags --extra-libs=-lopenh264 --prefix=$unixInst"
    $cfgExit = $LASTEXITCODE
    if ($cfgExit -ne 0 -or -not (Test-Path "$ffmpegBuild\ffbuild\config.mak")) {
        throw "FFmpeg configure failed (exit $cfgExit). Check the output above."
    }
    Write-Host "Configure OK (ffbuild/config.mak present)"

    # mingw make needs Windows-style paths in generated Makefiles
    Get-ChildItem $ffmpegBuild -Recurse -Include '*.mak','*.mk','Makefile','*.sh' | ForEach-Object {
        (Get-Content $_.FullName -Raw) -replace "$unixRepo/", "$winRepo/" | Set-Content $_.FullName -Encoding UTF8
    }

    # Install the STATIC LIBS only - never 'make install'.
    # 'make install' also runs install-libavutil-headers, whose rule builds one
    # huge command line: install -m 644 <70+ headers> "<INCINSTDIR>". Under
    # git-bash/MSYS the drive-letter path gets mangled and the headers land in
    # src/remotedesk/ (source tree) instead of $ffmpegInstall - reproducing a
    # set of stray FFmpeg headers at the top of the source tree on every clean
    # rebuild. Headers are copied explicitly right below instead.
    & $gitBash -lc "cd $unixBuild && PATH=${msysPath}:`$PATH && make -j4 && (make install-libs || true)"
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
    # Safety net: drop any header the build may still have written into the
    # source tree (e.g. when FFmpeg is rebuilt by an older script version).
    Remove-StrayHeaders $remotedeskDir
}

# ---- 4. Configure CMake ----
# R2/R3: drop the shared archives first. A stale libVideoEncoder.a from a tree
# built against the other FFmpeg version would be linked as-is, which either
# fails the link (avcodec_register_all undefined) or silently ships an exe
# without FFmpeg (no H.264 / no video support).
Reset-SharedArchives (Join-Path $outDir 'lib')

Write-Host "[4/5] Configuring CMake..."
& cmake -G "MinGW Makefiles" -S $srcDir -B $buildDir `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_PREFIX_PATH="$qtPrefix" `
    -DBUILD_THIRDPARTY=ON `
    "-DFFMPEG_VERSION=$ffmpegVersion"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

# ---- 5. Build project ----
Write-Host "[5/5] Building project..."
Invoke-NativeBuildWithArchiveRetry "CMake build" {
    & cmake --build $buildDir -j4 --config Release 2>&1
} {
    # 重试前清掉可能半写的归档，再让 make 重新归档/链接
    Reset-SharedArchives (Join-Path $outDir 'lib')
}

Write-Host ""
Write-Host "========================================"
Write-Host "  Build Complete!"
Write-Host "========================================"
Write-Host ""
$exePath = Join-Path $repoRoot 'bin\QtRemoteDesktop.exe'
if (-not (Test-Path -LiteralPath $exePath)) { throw "package missing after build: $exePath" }
Write-Host "Output : $exePath"
Write-Host "FFmpeg : $ffmpegVersion"
Write-Host ("Size   : {0}" -f (Format-Size (Get-Item -LiteralPath $exePath).Length))
Write-Host ("SHA256 : {0}" -f (Get-FileSha256 $exePath))
Write-Host ""
Write-Host "Compare the SHA256 above with the exe deployed on the target machine."
Write-Host "For both 32-bit packages in one command (Win7 6.1.2 first, then XP 3.4.8):"
Write-Host "  build_onekey_build_32pair.ps1"
