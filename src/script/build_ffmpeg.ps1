# FFmpeg Build Script
# All project paths are derived relative to this script's location.
$ErrorActionPreference = 'Stop'

# ---- Derived project paths (no hardcoded repo path) ----
$scriptDir = $PSScriptRoot
$srcDir    = [IO.Path]::GetFullPath((Join-Path $scriptDir '..'))
$repoRoot  = [IO.Path]::GetFullPath((Join-Path $srcDir '..'))
$buildDir  = Join-Path $repoRoot 'build'
$outDir    = Join-Path $repoRoot 'build_output'
$ffmpegSrc = Join-Path $srcDir 'remotedesk\thridparty\ffmpeg\FFmpeg-n3.4.8'
$ffmpegBuild   = Join-Path $buildDir 'ffmpeg_build'
$ffmpegInstall = Join-Path $outDir 'ffmpeg_install'
$openh264Lib   = Join-Path $outDir 'openh264_install\lib'
$openh264Inc   = Join-Path $outDir 'openh264_install\include\wels'

# ---- Toolchain detection (shared; override via env: QT_BASE_DIR / GIT_BASH / MINGW_MAKE) ----
. (Join-Path $scriptDir 'build_common.ps1')

$toolchain = Get-Toolchain $repoRoot
$gitBash   = Get-GitBash
$mingwBin  = $toolchain.MingwBin

Write-Host "清理构建目录..."
if (Test-Path $ffmpegBuild) { Remove-Item -LiteralPath $ffmpegBuild -Recurse -Force }
New-Item -ItemType Directory -Path $ffmpegBuild -Force | Out-Null
New-Item -ItemType Directory -Path "$ffmpegInstall/lib" -Force | Out-Null

Write-Host "配置 FFmpeg..."
$unixBuild = ConvertTo-UnixPath $ffmpegBuild
$unixSrc   = ConvertTo-UnixPath $ffmpegSrc
$unixInst  = ConvertTo-UnixPath $ffmpegInstall
$msysPath  = ConvertTo-UnixPath $mingwBin
$cflags    = "-I" + $openh264Inc.Replace('\', '/')
$ldflags   = "-L" + $openh264Lib.Replace('\', '/')

& $gitBash -lc "cd $unixBuild && PATH=${msysPath}:`$PATH && $unixSrc/configure --cc=gcc --ld=gcc --target-os=mingw32 --arch=x86 --disable-debug --disable-doc --disable-programs --disable-network --disable-avformat --disable-avfilter --disable-avdevice --disable-swresample --disable-postproc --disable-avresample --disable-everything --enable-swscale --enable-libopenh264 --enable-encoder=libopenh264 --enable-decoder=h264 --enable-decoder=mjpeg --enable-decoder=mpeg4 --disable-x86asm --enable-static --disable-shared --extra-cflags=$cflags --extra-ldflags=$ldflags --extra-libs=-lopenh264 --prefix=$unixInst"

Write-Host "Configure result: $LASTEXITCODE"
if (Test-Path "$ffmpegBuild/ffbuild/config.mak") { Write-Host "OK ffbuild/config.mak created" } else { Write-Host "FAILED config.mak not found" }
