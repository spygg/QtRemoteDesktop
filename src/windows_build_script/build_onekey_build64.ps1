# QtRemoteDesktop - Windows 64-bit one-key build.
# Companion of build_onekey_build.ps1 (32-bit + FFmpeg 3.4.8), which stays as-is.
#
# Bitness and FFmpeg version are decided in src/CMakeLists.txt (32-bit -> 3.4.8,
# 64-bit -> 6.1.2) and this script passes -DFFMPEG_VERSION explicitly, so the
# script and CMake can never disagree. All FFmpeg work - openh264, the NVENC
# auto-probe, configure, make, install - is done by CMake (cmake/BuildFFmpeg.cmake);
# this script only prepares the sources and drives cmake.
#
# Everything lands in build64/ build_output64/ bin64/ so the existing 32-bit
# build/ bin/ trees are never touched.

$ErrorActionPreference = 'Stop'

Write-Host "=============================================="
Write-Host "  QtRemoteDesktop 64-bit Build (FFmpeg 6.1.2)"
Write-Host "=============================================="
Write-Host ""

# ---- Paths (forward slashes: handed to cmake / make as-is) ----
$scriptDir = $PSScriptRoot
$srcDir    = [IO.Path]::GetFullPath((Join-Path $scriptDir '..')).Replace('\', '/')
$repoRoot  = [IO.Path]::GetFullPath((Join-Path $srcDir '..')).Replace('\', '/')
$buildDir  = Join-Path $repoRoot 'build64'
$outDir    = Join-Path $repoRoot 'build_output64'
$binDir    = Join-Path $repoRoot 'bin64'

$ffmpegVersion = if ($env:FFMPEG_VERSION) { $env:FFMPEG_VERSION } else { '6.1.2' }
$nvHeadersTag  = 'n12.2.72.0'   # NVENC API 12.2 - FFmpeg >= 5 needs "ffnvcodec >= 12.1.14.0"

$ffmpegDir     = Join-Path $srcDir 'remotedesk/thridparty/ffmpeg'
$ffmpegSrc     = Join-Path $ffmpegDir "FFmpeg-n$ffmpegVersion"
$nvHeadersSrc  = Join-Path $ffmpegDir "nv-codec-headers-$nvHeadersTag"

# ---- Shared helpers (Get-GitBash); does not alter build.bat behaviour ----
. (Join-Path $scriptDir 'build_common.ps1')

# PE machine of an exe: 64 / 32 / 0 (not a PE we recognise).
# Used to prove a Qt kit is really 64-bit - the directory name alone is a claim,
# e.g. Qt's bare "msvc2019" kit is 32-bit.
function Get-ExeBitness([string]$Path) {
    if (-not (Test-Path $Path)) { return 0 }
    $fs = [IO.File]::OpenRead($Path)
    try {
        $br = New-Object IO.BinaryReader($fs)
        if ($br.ReadInt16() -ne 0x5A4D) { return 0 }
        $fs.Position = 0x3C
        $peOff = $br.ReadInt32()
        if ($peOff -le 0 -or $peOff -gt ($fs.Length - 6)) { return 0 }
        $fs.Position = $peOff
        if ($br.ReadUInt32() -ne 0x00004550) { return 0 }
        switch ($br.ReadUInt16()) {
            0x8664 { return 64 }
            0xAA64 { return 64 }
            0x14c  { return 32 }
            default { return 0 }
        }
    } catch { return 0 } finally { $fs.Dispose() }
}

# 64-bit Qt MinGW kit + the matching mingw *_64 toolchain.
# build_common.ps1's scanner deliberately prefers the legacy 32-bit kit (that is
# what build.bat needs), so the 64-bit walk lives here.
function Find-Toolchain64 {
    $qmakeOverride = $env:QT_PREFIX_64
    $mingwOverride = $env:MINGW_BIN_64
    if ($qmakeOverride -and $mingwOverride) {
        if ((Get-ExeBitness (Join-Path $qmakeOverride 'bin\qmake.exe')) -ne 64) {
            throw "QT_PREFIX_64 is not a 64-bit kit: $qmakeOverride"
        }
        return @{ Prefix = $qmakeOverride; MingwBin = $mingwOverride }
    }

    $roots = @($env:QT_ROOT, 'E:\programes\Qt', 'C:\Qt', "$env:ProgramFiles\Qt",
               "$env:ProgramFiles (x86)\Qt") | Where-Object { $_ -and (Test-Path $_) }
    if (-not $roots) { throw "no Qt installation root found (set QT_PREFIX_64 + MINGW_BIN_64)" }

    $candidates = @()
    foreach ($root in $roots) {
        # <root>/<version>/<kit> and <root>/<QtLegacy>/<version>/<kit>
        $bases = @($root) + (Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                              ForEach-Object { $_.FullName })
        foreach ($base in $bases) {
            $verDirs = Get-ChildItem $base -Directory -ErrorAction SilentlyContinue |
                       Where-Object { $_.Name -match '^\d+\.\d+' }
            foreach ($v in $verDirs) {
                $kits = Get-ChildItem $v.FullName -Directory -ErrorAction SilentlyContinue |
                        Where-Object {
                            $_.Name -match 'mingw' -and $_.Name -match '_64$' -and
                            (Get-ExeBitness (Join-Path $_.FullName 'bin\qmake.exe')) -eq 64
                        }
                foreach ($k in $kits) {
                    $family = if (Test-Path (Join-Path $k.FullName 'lib\cmake\Qt5\Qt5Config.cmake')) { 5 }
                              elseif (Test-Path (Join-Path $k.FullName 'lib\cmake\Qt6\Qt6Config.cmake')) { 6 }
                              else { continue }
                    # kit name mingw81_64 -> gcc 8.1 used to build that Qt
                    $tag = if ($k.Name -match 'mingw(\d+)_64$') { $Matches[1] } else { '' }
                    $candidates += @{ Prefix = $k.FullName; Qt = $family; Version = [version]$v.Name;
                                      Base = $base; KitTag = $tag }
                }
            }
        }
    }
    if (-not $candidates) { throw "no 64-bit Qt MinGW kit found (set QT_PREFIX_64 + MINGW_BIN_64)" }

    # The project's find_package(QT NAMES Qt5 Qt6) resolves Qt5 first, so a Qt5
    # kit is the closest match to the 32-bit build; newest version wins inside it.
    $ordered = $candidates | Sort-Object @{E = { $_.Qt }}, @{E = { $_.Version }; D = $true}
    foreach ($c in $ordered) {
        $toolsRoot = Join-Path $c.Base 'Tools'
        if (-not (Test-Path $toolsRoot)) { continue }
        $tools = Get-ChildItem $toolsRoot -Directory -ErrorAction SilentlyContinue |
                 Where-Object { $_.Name -match 'mingw.*_64$' -and
                                (Get-ExeBitness (Join-Path $_.FullName 'bin\g++.exe')) -eq 64 }
        # prefer the toolchain whose gcc version matches the kit name (8.1 <-> mingw81_64),
        # else newest 64-bit mingw available
        $sorted = @()
        if ($c.KitTag) {
            $sorted += @($tools | Where-Object { $_.Name -match ("^mingw{0}\d*_64$" -f $c.KitTag) })
        }
        $sorted += @($tools | Sort-Object { [int](($_.Name -replace '^\D*(\d+).*$', '$1')) } -Descending)
        foreach ($t in ($sorted | Where-Object { $_ } | Select-Object -Unique)) {
            $mingwBin = Join-Path $t.FullName 'bin'
            if (Test-Path (Join-Path $mingwBin 'mingw32-make.exe')) {
                return @{ Prefix = $c.Prefix; MingwBin = $mingwBin; Qt = $c.Qt; Version = $c.Version }
            }
        }
    }
    throw "found a 64-bit Qt kit but no matching 64-bit MinGW toolchain under its Tools folder"
}

# GitHub archive tags match our directory naming (FFmpeg-n6.1.2,
# nv-codec-headers-n12.2.72.0), so extraction needs no renaming step.
function Ensure-ExtractedSource([string]$destDir, [string]$url, [string]$label) {
    if (Test-Path $destDir) {
        Write-Host "[src] $label already present: $destDir"
        return $true
    }
    $tgz = Join-Path $env:TEMP ("qtrd_" + (Split-Path $destDir -Leaf) + ".tar.gz")
    Write-Host "[src] downloading $label -> $url"
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $url -OutFile $tgz -UseBasicParsing
    } catch {
        Write-Host "[src][WARN] download failed: $($_.Exception.Message)" -ForegroundColor Yellow
        return $false
    }
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (-not (Test-Path $tar)) { $tar = 'tar' }
    New-Item -ItemType Directory -Path $ffmpegDir -Force | Out-Null
    & $tar -xzf $tgz -C $ffmpegDir
    if ($LASTEXITCODE -ne 0) { throw "$label extraction failed (exit $LASTEXITCODE): $tgz" }
    Remove-Item -LiteralPath $tgz -Force -ErrorAction SilentlyContinue
    return (Test-Path $destDir)
}

# Stage the 64-bit Qt runtime next to the new exe.
# The already-deployed 32-bit bin/ tells us exactly which Qt modules the app
# links; we copy the same file names out of the 64-bit kit. MinGW's own runtime
# differs per bitness (dw2 vs seh) so it is taken from the toolchain by glob.
function Stage-QtRuntime([string]$qtPrefix, [string]$mingwBin, [string]$targetDir) {
    New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    $refDir = Join-Path $repoRoot 'bin'
    $copied = 0
    if (Test-Path (Join-Path $refDir 'Qt5Core.dll')) {
        $dlls = Get-ChildItem $refDir -Recurse -Filter '*.dll' -File -ErrorAction SilentlyContinue
        foreach ($d in $dlls) {
            $rel = $d.FullName.Substring($refDir.Length + 1).Replace('\', '/')
            if ($rel -match '^(libgcc_s_|libstdc\+\+|libwinpthread)') { continue }
            # Qt ships modules in bin/ and plugins in plugins/<group>/
            $src = if ($rel.Contains('/')) {
                Join-Path (Join-Path $qtPrefix 'plugins') $rel
            } else {
                Join-Path (Join-Path $qtPrefix 'bin') (Split-Path $rel -Leaf)
            }
            if (-not (Test-Path $src)) { continue }
            $dst = Join-Path $targetDir $rel
            New-Item -ItemType Directory -Path (Split-Path $dst -Parent) -Force | Out-Null
            Copy-Item -LiteralPath $src -Destination $dst -Force
            $copied++
        }
    }
    foreach ($pat in @('libgcc_s_*.dll', 'libstdc++-*.dll', 'libwinpthread-*.dll')) {
        Get-ChildItem $mingwBin -Filter $pat -File -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $targetDir $_.Name) -Force
            $copied++
        }
    }
    if ($copied -eq 0) {
        $wdq = Join-Path $qtPrefix 'bin\windeployqt.exe'
        $exe = Join-Path $targetDir 'QtRemoteDesktop.exe'
        if ((Test-Path $wdq) -and (Test-Path $exe)) {
            Write-Host "[runtime] no reference deployment found, running windeployqt"
            Invoke-StepChecked "windeployqt" {
                & $wdq --release --no-translations --compiler-runtime $exe 2>&1 | Format-ToolOutput
            }
        } else {
            Write-Host "[runtime][WARN] could not stage Qt runtime into $targetDir - copy the 64-bit DLLs manually" -ForegroundColor Yellow
        }
    } else {
        Write-Host "[runtime] staged $copied DLL(s) into $targetDir"
    }
}

# Native build tools write warnings to stderr. With $ErrorActionPreference='Stop'
# PowerShell turns any redirected stderr line into a terminating error, so a mere
# "_WIN32_WINNT redefined" warning would abort a build that is going fine.
# Run the tool with 'Continue' and judge success by its exit code only.
# 2>&1 keeps stderr in step with stdout in the log; redirected error records are
# printed as their message, otherwise they show up as "RemoteException" noise.
function Format-ToolOutput {
    process {
        if ($_ -is [Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
    }
}

function Invoke-StepChecked([string]$What, [scriptblock]$Body) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $code = 0
    try {
        & $Body
        if ($null -ne $LASTEXITCODE) { $code = $LASTEXITCODE }
    } finally { $ErrorActionPreference = $prev }
    if ($code -ne 0) { throw "$What failed (exit $code)" }
}

# ---- 1. Toolchain ----
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    foreach ($c in @('E:\programes\Qt\Qt\Tools\CMake_64\bin\cmake.exe',
                     'C:\Qt\Tools\CMake_64\bin\cmake.exe')) {
        if (Test-Path $c) { $cmake = @{ Source = $c }; break }
    }
}
if (-not $cmake) { throw "cmake not found on PATH (or under a Qt Tools folder)" }
$cmakeExe = $cmake.Source

$tc       = Find-Toolchain64
$gitBash  = Get-GitBash
$qtPrefix = $tc.Prefix
$mingwBin = $tc.MingwBin
$gcc      = Join-Path $mingwBin 'gcc.exe'
$gxx      = Join-Path $mingwBin 'g++.exe'
$makeExe  = Join-Path $mingwBin 'mingw32-make.exe'

Write-Host "cmake    : $cmakeExe"
Write-Host "Qt kit : $qtPrefix (Qt$($tc.Qt), $($tc.Version), 64-bit)"
Write-Host "MinGW  : $mingwBin"
Write-Host "Git    : $(Split-Path (Split-Path $gitBash -Parent) -Parent)"
Write-Host ""

# sh / nasm / perl live next to Git; mingw first so gcc wins over any other
# compiler on PATH. BuildFFmpeg.cmake finds sh and nasm through this PATH.
$gitRoot = Split-Path (Split-Path $gitBash -Parent) -Parent
$env:PATH = "$mingwBin;$(Join-Path $gitRoot 'bin');$(Join-Path $gitRoot 'usr\bin');$env:PATH"

# ---- 2. FFmpeg + nv-codec-headers sources ----
# CMake builds FFmpeg itself, but it can only do so if the (gitignored) source
# tree is in place. Fetch it here so a fresh clone still produces NVENC.
$ffmpegOk = (Test-Path (Join-Path $ffmpegSrc 'configure'))
if (-not $ffmpegOk) {
    $ffmpegOk = Ensure-ExtractedSource $ffmpegSrc `
        "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n$ffmpegVersion.tar.gz" `
        "FFmpeg $ffmpegVersion source"
}
if (-not $ffmpegOk) {
    throw "FFmpeg $ffmpegVersion source is required for a 64-bit build. Extract it to $ffmpegSrc manually."
}

$nvOk = Test-Path (Join-Path $nvHeadersSrc 'include/ffnvcodec/nvEncodeAPI.h')
if (-not $nvOk) {
    $nvOk = Ensure-ExtractedSource $nvHeadersSrc `
        "https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/$nvHeadersTag.tar.gz" `
        "nv-codec-headers"
}
if (-not $nvOk) {
    Write-Host "[WARN] nv-codec-headers missing - CMake will build FFmpeg without NVENC (software encode only)" -ForegroundColor Yellow
}
Write-Host ""

# ---- 3. Configure ----
Write-Host "[1/3] Configuring CMake (build64 / bin64 / build_output64)..."
Invoke-StepChecked "CMake configure" {
    & $cmakeExe -G "MinGW Makefiles" -S $srcDir -B $buildDir `
        "-DCMAKE_BUILD_TYPE=Release" `
        "-DCMAKE_C_COMPILER=$($gcc.Replace('\', '/'))" `
        "-DCMAKE_CXX_COMPILER=$($gxx.Replace('\', '/'))" `
        "-DCMAKE_MAKE_PROGRAM=$($makeExe.Replace('\', '/'))" `
        "-DCMAKE_PREFIX_PATH=$($qtPrefix.Replace('\', '/'))" `
        "-DBUILD_THIRDPARTY=ON" `
        "-DBUILD_OUTPUT_DIR=$outDir" `
        "-DBIN_OUTPUT_DIR=$binDir" `
        "-DFFMPEG_VERSION=$ffmpegVersion" 2>&1 | Format-ToolOutput
}
Write-Host ""

# ---- 4. Build (openh264_ep + ffmpeg_ep run before the app links) ----
$jobs = [Math]::Max(1, [int]$env:NUMBER_OF_PROCESSORS - 1)
Write-Host "[2/3] Building (this compiles FFmpeg $ffmpegVersion + NVENC on first run, expect tens of minutes)..."
Invoke-StepChecked "CMake build" {
    & $cmakeExe --build $buildDir -j $jobs 2>&1 | Format-ToolOutput
}
Write-Host ""

# ---- 5. 64-bit runtime next to the exe ----
Write-Host "[3/3] Staging Qt runtime for bin64..."
Stage-QtRuntime $qtPrefix $mingwBin $binDir

Write-Host ""
Write-Host "=========================================="
Write-Host "  Build Complete (64-bit)"
Write-Host "=========================================="
Write-Host ""
Write-Host "Output : $binDir\QtRemoteDesktop.exe"
Write-Host "FFmpeg : $outDir/ffmpeg_install-$ffmpegVersion"
Write-Host "Check  : the log under bin64\logs\ should show 'Using HW encoder: h264_nvenc'"
Write-Host "         (32-bit build.bat output in bin\ is untouched)"
