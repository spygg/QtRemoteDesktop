# QtRemoteDesktop - Windows 32-bit PAIRED build (one command, two packages).
#
#   build_win7_612 + bin_win7_612  ->  FFmpeg 6.1.2   (Win7 / 8 / 10 / 11, 32-bit)
#   build_xp_348  + bin_xp_348    ->  FFmpeg 3.4.8   (Windows XP, 32-bit)
#
# =============================================================================
# BUILD RULES (do not reorder, do not skip; a broken archive silently ships a
# package without FFmpeg - that is exactly what happened on 2026-11-04:
# bin_xp_348\QtRemoteDesktop.exe was 7,394,065 bytes instead of ~10.2 MB because
# it linked the 6.1.2 libVideoEncoder.a produced by the Win7 tree).
#
#   R1  Order: Win7 (FFmpeg 6.1.2) FIRST, XP (FFmpeg 3.4.8) SECOND.
#       Re-run this script (it always purges first) instead of building the two
#       trees by hand in the other order.
#
#   R2  Both 32-bit trees share <repo>/build_output_win32/lib as their archive
#       output directory (CMAKE_ARCHIVE_OUTPUT_DIRECTORY, src/CMakeLists.txt).
#       libVideoEncoder.a embeds FFmpeg-version-specific objects, so an archive
#       left behind by the other tree is linked as-is:
#         - XP archive into the 6.1.2 link -> "undefined reference to
#           `avcodec_register_all'";
#         - 6.1.2 archive into the XP link -> exe builds fine but contains no
#           FFmpeg / openh264 code at all (no H.264, no video support).
#
#   R3  Therefore EVERY target purges <repo>/build_output_win32/lib/*.a right
#       before its configure+build step (Reset-SharedArchives below). Purging
#       all archives - not just libVideoEncoder.a - keeps the invariant simple;
#       they are only re-linked from objects that already exist, so the cost is
#       re-archive + relink, not a full recompile. FFmpeg/openh264 static libs
#       live in build_output_win32/ffmpeg_install* / openh264_install and are
#       NOT touched.
#
#   R4  Every package is verified right after its build (Assert-FFmpegLink):
#       the exe must contain a symbol that only ITS FFmpeg version has
#       (6.1.2: av_channel_layout_default, 3.4.8: avcodec_register_all), must
#       NOT contain the other version's marker, and must contain the openh264
#       encoder (WelsCreateSVCEncoder). This is the machine check for R2.
#
#   R5  The run ends with a report containing path + size + SHA256 of both
#       exes; compare those values with what was deployed on the target machine.
#
# The same rules are documented in build_onekey_build.ps1 (XP-only path),
# build.bat / build_32pair.bat and README.md.
# =============================================================================
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File build_onekey_build_32pair.ps1
#   ... -File build_onekey_build_32pair.ps1 -Target win7      # 6.1.2 only
#   ... -File build_onekey_build_32pair.ps1 -Target xp        # 3.4.8 only
#   ... -File build_onekey_build_32pair.ps1 -Jobs 8
#
# FFmpeg versions are passed to CMake explicitly (-DFFMPEG_VERSION=...), so the
# script and src/CMakeLists.txt can never disagree. FFmpeg itself is prebuilt
# (build_output_win32/ffmpeg_install, ffmpeg_install-6.1.2); if a version's
# install dir is missing, CMake builds it from the bundled source (BUILD_FFMPEG).

[CmdletBinding()]
param(
    [ValidateSet('both', 'win7', 'xp')]
    [string]$Target = 'both',
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'

$scriptDir = $PSScriptRoot
$srcDir    = [IO.Path]::GetFullPath((Join-Path $scriptDir '..'))
$repoRoot  = [IO.Path]::GetFullPath((Join-Path $srcDir '..'))
$sharedOut = Join-Path $repoRoot 'build_output_win32'   # BUILD_OUTPUT_DIR (shared by both trees)
$sharedLib = Join-Path $sharedOut 'lib'                 # CMAKE_ARCHIVE_OUTPUT_DIRECTORY

if ($Jobs -le 0) {
    $Jobs = [Math]::Max(1, [int]$env:NUMBER_OF_PROCESSORS - 1)
}

# Win7 first, XP second (R1). MustHave/MustNotHave are the per-version FFmpeg
# markers used by Assert-FFmpegLink (R4).
$allTargets = @(
    @{ Name = 'win7'; Label = 'Win7 32-bit (FFmpeg 6.1.2)'; FfmpegVersion = '6.1.2'
       BuildDir = 'build_win7_612'; BinDir = 'bin_win7_612'
       MustHave = 'av_channel_layout_default'; MustNotHave = 'avcodec_register_all' },
    @{ Name = 'xp';   Label = 'Windows XP 32-bit (FFmpeg 3.4.8)'; FfmpegVersion = '3.4.8'
       BuildDir = 'build_xp_348'; BinDir = 'bin_xp_348'
       MustHave = 'avcodec_register_all'; MustNotHave = 'av_channel_layout_default' }
)
$targets = if ($Target -eq 'both') { $allTargets } else { $allTargets | Where-Object { $_.Name -eq $Target } }

Write-Host "=============================================="
Write-Host "  QtRemoteDesktop 32-bit Paired Build"
Write-Host "  order: Win7 (6.1.2) -> XP (3.4.8)"
Write-Host "=============================================="
Write-Host ""

. (Join-Path $scriptDir 'build_common.ps1')

# Native tools write warnings to stderr; with $ErrorActionPreference='Stop'
# PowerShell turns a redirected stderr line into a terminating error, so a mere
# warning would abort a build that is going fine. Judge success by exit code.
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

# Build step with a narrow, logged retry for the archive race we keep hitting
# on this host: while -jN gcc/ar jobs write <BUILD_OUTPUT_DIR>/lib/*.a, an
# antivirus/indexer briefly holds the freshly created archive and `ar` fails
# with
#   ar.exe: unable to rename '...\libXXX.a'; reason: No such file or directory
#   ranlib: Malformed archive
# (seen 2026-11-04 20:4x and again during the paired run). Only that fingerprint
# is retried, and every retry is printed. Any other build error stops the run
# immediately - nothing is retried "to make it pass".
function Invoke-BuildChecked([string]$What, [scriptblock]$Body, [int]$MaxAttempts = 3, [scriptblock]$OnRetry = $null) {
    $flaky = 'unable to rename .*\.a|Malformed archive|archive has no index|\.a: Permission denied'
    for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $lines = @()
        $code = 0
        try {
            $lines = @(& $Body 2>&1 | ForEach-Object {
                if ($_ -is [Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
            })
            if ($null -ne $LASTEXITCODE) { $code = $LASTEXITCODE }
        } finally { $ErrorActionPreference = $prev }
        foreach ($l in $lines) { Write-Host $l }
        if ($code -eq 0) { return }
        $text = ($lines | Out-String)
        if ($attempt -lt $MaxAttempts -and $text -match $flaky) {
            Write-Host ("[retry] {0}: archive race detected (ar/ranlib rename), " -f $What) -ForegroundColor Yellow -NoNewline
            Write-Host ("retrying {0}/{1}..." -f ($attempt + 1), $MaxAttempts) -ForegroundColor Yellow
            Start-Sleep -Seconds 3
            # A retry must start from a clean archive set: the failed attempt may
            # have left an archive that is half-written or held for a moment, and
            # the next make would keep failing on the same file.
            if ($null -ne $OnRetry) { & $OnRetry }
            continue
        }
        throw "$What failed (exit $code)"
    }
}

# PE machine of an exe: 64 / 32 / 0 (not a PE we recognise). Proves the Qt kit
# and the MinGW toolchain really are 32-bit - the directory name is only a claim.
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

# SHA256 without Get-FileHash (missing on PowerShell 2.0, still the default on
# Windows 7 / XP). Upper-case hex, same format as certutil output.
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

# R3: drop every project archive in the shared lib dir. A stale
# libVideoEncoder.a from the other tree is what produced the 7,394,065-byte XP
# exe (no FFmpeg). Retry a few times: Defender / a lingering linker can hold a
# handle for a moment ("Unable to rename ... Permission denied").
function Reset-SharedArchives([string]$LibDir) {
    if (-not (Test-Path $LibDir)) {
        New-Item -ItemType Directory -Path $LibDir -Force | Out-Null
        Write-Host "[purge] $LibDir did not exist - created empty"
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

# R4: prove the package linked the FFmpeg build that belongs to this target.
function Assert-FFmpegLink([string]$Exe, [string]$NmExe, [string]$Label,
                           [string]$MustHave, [string]$MustNotHave) {
    if (-not (Test-Path -LiteralPath $Exe)) { throw "${Label}: package was not produced: $Exe" }
    # nm may print a warning on stderr; with ErrorActionPreference='Stop' a
    # redirected stderr line would abort the verification, so relax it here.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $raw = & $NmExe $Exe 2>$null
    } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0 -or -not $raw) { throw "${Label}: nm failed on $Exe" }

    $syms = @{}
    foreach ($line in $raw) {
        $cols = $line -split '\s+'
        if ($cols.Count -ge 2) { $syms[$cols[-1]] = $true }
    }
    $hasMust    = $syms.ContainsKey("_$MustHave")    -or $syms.ContainsKey($MustHave)
    $hasMustNot = $syms.ContainsKey("_$MustNotHave") -or $syms.ContainsKey($MustNotHave)
    $hasOpenh264 = $syms.ContainsKey('_WelsCreateSVCEncoder') -or $syms.ContainsKey('WelsCreateSVCEncoder')

    if (-not $hasMust) {
        throw ("{0}: FFmpeg marker '{1}' missing - this package has no usable FFmpeg " -f $Label, $MustHave) +
              "(stale archive from the other tree, or FFmpeg not linked at all)"
    }
    if ($hasMustNot) {
        throw ("{0}: found '{1}' - stale archive left over from the other tree was linked " -f $Label, $MustNotHave) +
              "(the shared build_output_win32/lib purge did not take effect)"
    }
    if (-not $hasOpenh264) { throw "${Label}: openh264 encoder not linked (WelsCreateSVCEncoder missing)" }

    Write-Host ("[verify] {0}: symbol '{1}' present, '{2}' absent, openh264 linked (symbols={3})" -f
                $Label, $MustHave, $MustNotHave, $syms.Count)
}

# ---- Toolchain ----
$toolchain = Get-Toolchain $repoRoot
$qtPrefix  = $toolchain.Prefix
$mingwBin  = $toolchain.MingwBin
$gcc       = Join-Path $mingwBin 'gcc.exe'
$gxx       = Join-Path $mingwBin 'g++.exe'
$makeExe   = Join-Path $mingwBin 'mingw32-make.exe'
$nmExe     = Join-Path $mingwBin 'nm.exe'

$cmakeExe = (Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -First 1)
if ($cmakeExe) { $cmakeExe = $cmakeExe.Source }
else {
    foreach ($c in @('E:\programes\Qt\Qt\Tools\CMake_64\bin\cmake.exe',
                     'C:\Qt\Tools\CMake_64\bin\cmake.exe',
                     'C:\Program Files\CMake\bin\cmake.exe')) {
        if (Test-Path $c) { $cmakeExe = $c; break }
    }
}
if (-not $cmakeExe) { throw "cmake not found on PATH (or under a Qt Tools folder)" }

if ((Get-ExeBitness $gxx) -ne 32) { throw "MinGW g++.exe is not 32-bit: $gxx" }
if ((Get-ExeBitness (Join-Path $qtPrefix 'bin\qmake.exe')) -ne 32) { throw "Qt kit is not 32-bit: $qtPrefix" }

Write-Host "cmake    : $cmakeExe"
Write-Host "Qt kit   : $qtPrefix (32-bit)"
Write-Host "MinGW    : $mingwBin"
Write-Host "shared   : $sharedOut (lib = $sharedLib)"
Write-Host "jobs     : $Jobs"
Write-Host ""

# sh / nasm / perl live next to Git; mingw first so gcc wins over any other
# compiler on PATH (needed when CMake has to build FFmpeg itself).
$gitBash = Get-GitBash
$gitRoot = Split-Path (Split-Path $gitBash -Parent) -Parent
$env:PATH = "$mingwBin;$(Join-Path $gitRoot 'bin');$(Join-Path $gitRoot 'usr\bin');$env:PATH"

# FFmpeg install dir per version (src/CMakeLists.txt: 3.4.8 -> ffmpeg_install,
# everything else -> ffmpeg_install-<version>).
$result = @()

foreach ($t in $targets) {
    $buildDir  = Join-Path $repoRoot $t.BuildDir
    $binDir    = Join-Path $repoRoot $t.BinDir
    $ffmpegDir = if ([version]$t.FfmpegVersion -lt [version]'5.0') {
        Join-Path $sharedOut 'ffmpeg_install'
    } else {
        Join-Path $sharedOut "ffmpeg_install-$($t.FfmpegVersion)"
    }
    $exe = Join-Path $binDir 'QtRemoteDesktop.exe'

    Write-Host ""
    Write-Host "----------------------------------------------------------------"
    Write-Host "  $($t.Label)"
    Write-Host "  tree: $buildDir   bin: $binDir"
    Write-Host "----------------------------------------------------------------"

    if (-not (Test-Path (Join-Path $ffmpegDir 'lib/libavcodec.a'))) {
        Write-Host "[WARN] FFmpeg $($t.FfmpegVersion) prebuilt not found at $ffmpegDir" -ForegroundColor Yellow
        Write-Host "       CMake will build it from the bundled source (slow, first run only)" -ForegroundColor Yellow
    }

    # R2/R3: the other tree's archives must not survive into this link.
    Reset-SharedArchives $sharedLib

    Write-Host "[1/3] Configuring CMake (-DFFMPEG_VERSION=$($t.FfmpegVersion))..."
    Invoke-StepChecked "CMake configure ($($t.Name))" {
        & $cmakeExe -G "MinGW Makefiles" -S $srcDir -B $buildDir `
            "-DCMAKE_BUILD_TYPE=Release" `
            "-DCMAKE_C_COMPILER=$($gcc.Replace('\', '/'))" `
            "-DCMAKE_CXX_COMPILER=$($gxx.Replace('\', '/'))" `
            "-DCMAKE_MAKE_PROGRAM=$($makeExe.Replace('\', '/'))" `
            "-DCMAKE_PREFIX_PATH=$($qtPrefix.Replace('\', '/'))" `
            "-DBUILD_THIRDPARTY=ON" `
            "-DBUILD_OUTPUT_DIR=$sharedOut" `
            "-DBIN_OUTPUT_DIR=$binDir" `
            "-DFFMPEG_VERSION=$($t.FfmpegVersion)" 2>&1 | Format-ToolOutput
    }

    Write-Host "[2/3] Building..."
    Invoke-BuildChecked "CMake build ($($t.Name))" {
        & $cmakeExe --build $buildDir -j $Jobs 2>&1
    } 3 { Reset-SharedArchives $sharedLib }

    Write-Host "[3/3] Verify package..."
    Assert-FFmpegLink $exe $nmExe $t.Label $t.MustHave $t.MustNotHave

    $result += [pscustomobject]@{
        Target = $t.Name
        Label  = $t.Label
        FFmpeg = $t.FfmpegVersion
        Exe    = $exe
        Bytes  = (Get-Item -LiteralPath $exe).Length
        Sha256 = Get-FileSha256 $exe
    }
}

Write-Host ""
Write-Host "=============================================="
Write-Host "  32-bit Paired Build Complete (R1 order kept)"
Write-Host "=============================================="
Write-Host ""
foreach ($r in $result) {
    Write-Host ("{0}  (FFmpeg {1})" -f $r.Label, $r.FFmpeg)
    Write-Host ("  path  : {0}" -f $r.Exe)
    Write-Host ("  size  : {0}" -f (Format-Size $r.Bytes))
    Write-Host ("  sha256: {0}" -f $r.Sha256)
    Write-Host ""
}
Write-Host "Compare the SHA256 above with the exe deployed on the target machine."
Write-Host "Deploy order does not matter; BUILD order does (Win7 6.1.2 first, XP 3.4.8 second)."

