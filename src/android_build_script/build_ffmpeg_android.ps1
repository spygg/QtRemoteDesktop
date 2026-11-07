# Android arm64-v8a FFmpeg 交叉编译（供 QtRemoteDesktop Android 端 H.264 硬编使用）
#
# 目标产物：build_output/ffmpeg_install-6.1.2-android-arm64
#   - 只编静态库 libavcodec/libavutil/libswscale
#   - 启用 --enable-mediacodec --enable-jni --enable-encoder=h264_mediacodec
#   - 目录名带 -android-arm64，与桌面 MinGW 产物（ffmpeg_install-6.1.2）严格隔离
#
# 复现步骤：本脚本可单独运行，也会被 build_android.ps1 自动调用。
#   powershell -ExecutionPolicy Bypass -File src\android_build_script\build_ffmpeg_android.ps1
# 强制重编：加 -Force。默认已是最新时秒退。

param(
    [string]$NdkRoot = "",
    [string]$BuildDir = "",
    [string]$InstallDir = "",
    [int]$Jobs = 0,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$FFMPEG_VERSION = "6.1.2"
$ANDROID_API = 24

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$srcDir = Join-Path $repo "src\remotedesk\thridparty\ffmpeg\FFmpeg-n$FFMPEG_VERSION"
if (-not (Test-Path (Join-Path $srcDir "configure"))) {
    throw "找不到 FFmpeg 源码：$srcDir"
}
if (-not $BuildDir) { $BuildDir = Join-Path $repo "build_output\ffmpeg_build-$FFMPEG_VERSION-android-arm64" }
if (-not $InstallDir) { $InstallDir = Join-Path $repo "build_output\ffmpeg_install-$FFMPEG_VERSION-android-arm64" }
if ($Jobs -le 0) { $Jobs = [Math]::Max(2, [Environment]::ProcessorCount) }

# ---- 定位 NDK ----
if (-not $NdkRoot -and $env:ANDROID_NDK_ROOT) { $NdkRoot = $env:ANDROID_NDK_ROOT }
if (-not $NdkRoot) {
    $sdk = $env:ANDROID_SDK_ROOT
    if (-not $sdk) { $sdk = $env:ANDROID_HOME }
    if (-not $sdk) { $sdk = "E:\programes\Android\SDK" }
    $ndkParent = Join-Path $sdk "ndk"
    if (Test-Path $ndkParent) {
        $preferred = Join-Path $ndkParent "27.1.12297006"
        if (Test-Path $preferred) {
            $NdkRoot = $preferred
        } else {
            $NdkRoot = Get-ChildItem $ndkParent -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
        }
    }
}
if (-not $NdkRoot -or -not (Test-Path $NdkRoot)) {
    throw "找不到 Android NDK：请用 -NdkRoot 指定，或设置 ANDROID_NDK_ROOT / ANDROID_SDK_ROOT"
}
$hostTag = "windows-x86_64"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\$hostTag\bin"
$sysroot = Join-Path $NdkRoot "toolchains\llvm\prebuilt\$hostTag\sysroot"
if (-not (Test-Path (Join-Path $toolBin "clang.exe"))) {
    throw "NDK 工具链不完整：$toolBin\clang.exe 不存在"
}

# ---- 定位 sh（configure 需要 POSIX shell）与 GNU make ----
$sh = (Get-Command sh -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
if (-not $sh -or -not (Test-Path $sh)) {
    throw "找不到 sh（需要 Git for Windows 的 usr\bin\sh.exe）"
}
$make = ""
$makeCandidates = @(
    (Join-Path $repo "..\..\..\programes\Qt\Qt\Tools\mingw1310_64\bin\mingw32-make.exe"),
    "E:\programes\Qt\Qt\Tools\mingw1310_64\bin\mingw32-make.exe",
    "C:\Qt\Tools\mingw1310_64\bin\mingw32-make.exe"
)
foreach ($c in $makeCandidates) {
    $full = [System.IO.Path]::GetFullPath($c)
    if (Test-Path $full) { $make = $full; break }
}
if (-not $make) {
    $cmd = Get-Command mingw32-make -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { $make = $cmd.Source }
}
if (-not $make) {
    throw "找不到 GNU make（Windows 下用 Qt 自带的 mingw32-make）：请安装后在 PATH 中可见"
}

function ConvertTo-Posix([string]$p) {
    $q = $p -replace '\\', '/'
    if ($q -match '^([A-Za-z]):/(.*)$') {
        return "/" + $Matches[1].ToLower() + "/" + $Matches[2]
    }
    return $q
}
function ConvertTo-DriveForward([string]$p) {
    return ($p -replace '\\', '/')
}

$buildPosix = ConvertTo-Posix $BuildDir
$srcPosix = ConvertTo-Posix $srcDir
$installDrive = ConvertTo-DriveForward $InstallDir
$sysrootDrive = ConvertTo-DriveForward $sysroot

# ---- 幂等：产物齐全且摘要一致则跳过 ----
$sigFile = Join-Path $BuildDir ".qtrd_ffmpeg_android_sig"
# request-sync 补丁：mediacodecenc 识别 pict_type=I 后下发 AMediaCodec_setParameters，
# 让客户端 PLI 能直接产 IDR，不必销毁重建编码器。改了这个补丁必须让签名失效重编。
$wantSig = "$FFMPEG_VERSION|arm64-v8a|api$ANDROID_API|$NdkRoot|mediacodec+jni+h264_mediacodec|bsf:h264_metadata|pic|static|patch:request-sync"
if (-not $Force -and (Test-Path $sigFile) -and
    (Test-Path (Join-Path $InstallDir "lib\libavcodec.a")) -and
    (Test-Path (Join-Path $InstallDir "lib\libavutil.a")) -and
    (Test-Path (Join-Path $InstallDir "lib\libswscale.a"))) {
    $oldSig = (Get-Content $sigFile -Raw).Trim()
    if ($oldSig -eq $wantSig) {
        Write-Host "FFmpeg(Android): 已是最新，跳过（$InstallDir）" -ForegroundColor DarkGray
        exit 0
    }
}

Write-Host "FFmpeg(Android): 交叉编译 $FFMPEG_VERSION arm64-v8a (API $ANDROID_API)" -ForegroundColor Cyan
Write-Host "  NDK     : $NdkRoot"
Write-Host "  源码    : $srcDir"
Write-Host "  构建目录: $BuildDir"
Write-Host "  产物目录: $InstallDir"
Write-Host "  make -j : $Jobs"

if ($Force -or -not (Test-Path $BuildDir)) { Remove-Item $BuildDir -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $BuildDir, (Join-Path $InstallDir "lib") | Out-Null

# ---- configure ----
$cfgArgs = @(
    "--target-os=android",
    "--arch=aarch64",
    "--cpu=armv8-a",
    "--enable-cross-compile",
    "--sysroot=$sysrootDrive",
    "--cc=clang", "--cxx=clang++",
    "--ar=llvm-ar", "--ranlib=llvm-ranlib", "--nm=llvm-nm", "--strip=llvm-strip",
    "--disable-debug", "--disable-doc", "--disable-programs", "--disable-network",
    "--disable-avformat", "--disable-avfilter", "--disable-avdevice",
    "--disable-swresample", "--disable-postproc",
    "--disable-everything", "--enable-swscale",
    "--enable-jni", "--enable-mediacodec",
    "--enable-encoder=h264_mediacodec",
    # mediacodecenc 会把宽高对齐到 16（如 806x360 → 816x368），再通过 h264_metadata
    # 比特流过滤器写入裁剪参数。少了这个 BSF，avcodec_open2 直接返回
    # AVERROR_BSF_NOT_FOUND(-1179861752)，表现为"硬编打开失败"。
    "--enable-bsf=h264_metadata",
    "--enable-pic", "--enable-static", "--disable-shared",
    "--extra-cflags=--target=aarch64-linux-android$ANDROID_API --sysroot=$sysrootDrive -fPIC",
    "--extra-ldflags=--target=aarch64-linux-android$ANDROID_API --sysroot=$sysrootDrive",
    "--prefix=$installDrive"
)
function Quote-Sh([string]$s) { return "'" + ($s -replace "'", "'\''") + "'" }
$cfgLine = (Quote-Sh "$srcPosix/configure") + " " + (($cfgArgs | ForEach-Object { Quote-Sh $_ }) -join " ")
$shBin = Split-Path -Parent $sh

$env:PATH = "$toolBin;$shBin;$env:PATH"
$buildLog = Join-Path $BuildDir "configure.log"
# 把 configure 写成 .sh 再执行：避免 PowerShell 向 sh -c 传复杂引号时被二次拆分/改写
$cfgScript = Join-Path $BuildDir "qtrd_configure.sh"
Set-Content -Path $cfgScript -Encoding utf8 -Value @(
    "cd " + (Quote-Sh $buildPosix)
    ($cfgLine + " > " + (Quote-Sh (ConvertTo-Posix $buildLog)) + " 2>&1")
)
& $sh (ConvertTo-Posix $cfgScript)
if ($LASTEXITCODE -ne 0) {
    Write-Host "---- configure 失败，日志尾部 ----" -ForegroundColor Red
    Get-Content $buildLog -Tail 40
    throw "FFmpeg configure 失败（exit $LASTEXITCODE），完整日志：$buildLog"
}
Write-Host "FFmpeg(Android): configure 通过" -ForegroundColor Green

# ---- Windows 原生 make 不认 git-bash 写进生成文件的 POSIX 绝对路径，改写成盘符形式 ----
$roots = @($repo, $BuildDir, $InstallDir, $NdkRoot)
$rewrite = @()
foreach ($r in $roots) {
    $rp = ConvertTo-Posix $r
    if ($rp -ne $r) {
        $rewrite += @($rp, (ConvertTo-DriveForward $r))
    }
}
$genFiles = Get-ChildItem $BuildDir -Recurse -File | Where-Object {
    $_.Extension -in @(".mak", ".mk", ".sh") -or $_.Name -eq "Makefile"
}
foreach ($f in $genFiles) {
    $text = Get-Content $f.FullName -Raw
    for ($i = 0; $i -lt $rewrite.Count; $i += 2) {
        $text = $text.Replace($rewrite[$i], $rewrite[$i + 1])
    }
    Set-Content $f.FullName $text -NoNewline
}

# ---- make ----
Write-Host "FFmpeg(Android): 开始编译（-j$Jobs，首次约 5–15 分钟）" -ForegroundColor Cyan
$makeLog = Join-Path $BuildDir "make.log"
Push-Location $BuildDir
try {
    # 用 --jobs=N 而不是 -jN：PowerShell 会把 -jN 拆成两个原生参数，make 报 usage
    & $make "--jobs=$Jobs" 2>&1 | Tee-Object -FilePath $makeLog | Select-Object -Last 5
    if ($LASTEXITCODE -ne 0) {
        Write-Host "---- make 失败，日志尾部 ----" -ForegroundColor Red
        Get-Content $makeLog -Tail 40
        throw "FFmpeg make 失败（exit $LASTEXITCODE），完整日志：$makeLog"
    }
} finally {
    Pop-Location
}

# ---- 安装库 + 头文件（Windows 上 make install 会因路径重写装错位置，手动复制）----
Push-Location $BuildDir
try {
    & $make install-libs 2>&1 | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg install-libs 失败（exit $LASTEXITCODE）" }
} finally {
    Pop-Location
}
foreach ($lib in @("libavcodec", "libavutil", "libswscale")) {
    $incDir = Join-Path $InstallDir "include\$lib"
    New-Item -ItemType Directory -Force -Path $incDir | Out-Null
    Copy-Item (Join-Path $srcDir "$lib\*.h") $incDir -Force
}
Copy-Item (Join-Path $BuildDir "libavutil\avconfig.h"), (Join-Path $BuildDir "libavutil\ffversion.h") `
    (Join-Path $InstallDir "include\libavutil") -Force

# ---- 校验：库齐全 + mediacodec 编码器确实编进去了 ----
$avcodec = Join-Path $InstallDir "lib\libavcodec.a"
$avutil = Join-Path $InstallDir "lib\libavutil.a"
$swscale = Join-Path $InstallDir "lib\libswscale.a"
foreach ($p in @($avcodec, $avutil, $swscale)) {
    if (-not (Test-Path $p)) { throw "产物缺失：$p" }
}
$nm = Join-Path $toolBin "llvm-nm.exe"
$syms = & $nm -g $avcodec 2>$null
$h264MediaCodec = ($syms | Select-String "ff_h264_mediacodec_encoder" | Measure-Object).Count
$mediaCodecApi = ($syms | Select-String "AMediaCodec" | Measure-Object).Count
$h264MetadataBsf = ($syms | Select-String "ff_h264_metadata_bsf" | Measure-Object).Count
if ($h264MediaCodec -eq 0) {
    throw "libavcodec.a 中没有 ff_h264_mediacodec_encoder：configure 未真正启用 h264_mediacodec"
}
if ($h264MetadataBsf -eq 0) {
    throw "libavcodec.a 中没有 ff_h264_metadata_bsf：非 16 对齐分辨率无法编码（mediacodecenc 依赖它裁剪补边）"
}
# request-sync 补丁校验：补丁会把字面量 "request-sync" 编进 mediacodecenc。
# 缺了它客户端 PLI 只能退回"销毁重建编码器"（旧行为，能出帧但有 50~130ms 抖动）。
$llvmStrings = Join-Path $toolBin "llvm-strings.exe"
$requestSync = 0
if (Test-Path $llvmStrings) {
    $requestSync = (& $llvmStrings $avcodec 2>$null | Select-String "request-sync" | Measure-Object).Count
    if ($requestSync -eq 0) {
        throw "libavcodec.a 里没有 request-sync 字面量：mediacodecenc 的 request-sync 补丁未编入"
    }
}
Set-Content $sigFile ("{0}`n" -f $wantSig) -NoNewline

Write-Host "FFmpeg(Android): 构建完成" -ForegroundColor Green
Write-Host "  libavcodec.a : $((Get-Item $avcodec).Length) bytes"
Write-Host "  libavutil.a  : $((Get-Item $avutil).Length) bytes"
Write-Host "  libswscale.a : $((Get-Item $swscale).Length) bytes"
Write-Host "  h264_mediacodec 符号: $h264MediaCodec 个；h264_metadata BSF 符号: $h264MetadataBsf 个；AMediaCodec 符号: $mediaCodecApi 个；request-sync 补丁: $requestSync"
