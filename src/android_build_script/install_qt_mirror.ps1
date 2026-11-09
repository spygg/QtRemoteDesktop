# ============================================================================
# Qt 6.11+ mirror installer (no aqtinstall)
#
# Why this exists: Qt 6.11 changed the online repository layout from
#   windows_x86/desktop/qt6_693/qt6_693/Updates.xml
# to
#   windows_x86/desktop/qt6_6111/qt6_6111_mingw/Updates.xml
#   windows_x86/desktop/qt6_6111/qt6_6111_mingw/<package>/<version><archive>
# aqtinstall 3.3.0 (newest release, and the default pin of
# jurplel/install-qt-action) still builds the old path, so it fails with
# "Failed to locate XML data for Qt version '6.11.1'". The upstream fix
# (aqtinstall PR #1000) is merged but unreleased. This script reads the group
# Updates.xml directly and downloads exactly the archives that package declares,
# verifying each against its published .sha1 sidecar.
#
# Extraction targets come from the package's own <Operation name="Extract">
# entries, so kits land where Qt's official installer puts them (6.11.1/mingw_64
# for the desktop MinGW kit, 6.11.1/android_arm64_v8a for the Android kit).
#
# Mobile kits moved out of windows_x86/android in Qt 6.8 and now live at
#   all_os/android/qt6_6111/qt6_6111_arm64_v8a/Updates.xml
# The old windows_x86/android tree is still served but stops at 6.7.x, so a URL
# built from it silently resolves to Qt 6.1.1 archives (qt6_611 = 6.1.1).
#
# ASCII ONLY: Windows PowerShell 5.1 decodes .ps1 as the ANSI codepage.
#
# Usage:
#   install_qt_mirror.ps1                                # host + android + websockets + make
#   install_qt_mirror.ps1 -Components Host,Make
#   install_qt_mirror.ps1 -OutDir C:\Qt -ArchiveFilter qtbase
#
# -ArchiveFilter only fetches archives whose name contains the substring. It
# exists to prove URL construction and extraction layout cheaply; a Qt pruned
# down to one archive is not a working Qt.
# ============================================================================
[CmdletBinding()]
# Components is a single comma-joined string on purpose: with "pwsh -File", a
# space separated array ("-Components Host Android") leaves bare words behind
# and PowerShell binds those to the next positional parameter -- $Version --
# which silently produces URLs like qt6_Android.
param(
    [string]$Version = '6.11.1',
    [string]$OutDir = '',
    [string]$Mirror = 'https://download.qt.io/online/qtsdkrepository',
    [string]$Components = 'Host,Android,WebSockets,Make',
    [string]$ArchiveFilter = '',
    [string]$SevenZip = '',
    [int]$Retries = 3
)

$ErrorActionPreference = 'Continue'
if ($Version -notmatch '^\d+\.\d+(\.\d+)?$') {
    Write-Host "[qt-mirror] ERROR: -Version does not look like a version: '$Version'" -ForegroundColor Red
    exit 1
}
$ComponentList = @($Components -split '[,\s]+' | Where-Object { $_ })
$valid = @('Host', 'Android', 'WebSockets', 'Make')
foreach ($c in $ComponentList) {
    if ($valid -notcontains $c) {
        Write-Host "[qt-mirror] ERROR: unknown -Components entry '$c' (valid: $($valid -join ', '))" -ForegroundColor Red
        exit 1
    }
}
if (-not $OutDir) { $OutDir = Join-Path $PWD 'qt-mirror' }
$DownloadDir = Join-Path $OutDir '_downloads'
New-Item -ItemType Directory -Force -Path $OutDir, $DownloadDir | Out-Null

function Fail([string]$Message) {
    Write-Host "[qt-mirror] ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Get-RemoteFile([string]$Url, [string]$Dest) {
    # curl handles the download.qt.io -> mirror redirect and range-less retries;
    # Invoke-WebRequest on PS 5.1 buffers the whole archive in memory.
    for ($i = 1; $i -le $Retries; $i++) {
        & curl.exe -sSL --fail --connect-timeout 30 --retry 2 -o $Dest $Url 2>&1 |
            ForEach-Object { Write-Host "  curl: $_" }
        if ($LASTEXITCODE -eq 0 -and (Test-Path $Dest)) { return $true }
        Write-Host "  attempt $i/$Retries failed for $Url (curl exit $LASTEXITCODE)" -ForegroundColor Yellow
        Remove-Item $Dest -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds (5 * $i)
    }
    return $false
}

function Test-Sha1([string]$Archive, [string]$Url) {
    # The mirrors publish <archive>.sha1 next to the archive. If it cannot be
    # fetched that is an error, not a reason to skip verification.
    $shaPath = "$Archive.sha1"
    if (-not (Get-RemoteFile "$Url.sha1" $shaPath)) {
        Fail "cannot fetch checksum: $Url.sha1"
    }
    $expect = ((Get-Content $shaPath -Raw) -split '\s+')[0].Trim().ToLower()
    $actual = (Get-FileHash -Algorithm SHA1 -Path $Archive).Hash.ToLower()
    if ($expect -ne $actual) {
        Fail "SHA1 mismatch for $Archive (expected $expect, got $actual)"
    }
    Write-Host "  sha1 ok: $actual" -ForegroundColor DarkGray
    Remove-Item $shaPath -Force -ErrorAction SilentlyContinue
}

function Find-SevenZip([string]$OverridePath) {
    if ($OverridePath) {
        if (Test-Path $OverridePath) { return (Resolve-Path $OverridePath).Path }
        Fail "-SevenZip does not point at a file: $OverridePath"
    }
    foreach ($c in @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe")) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    foreach ($n in @('7z', '7za')) {
        $cmd = Get-Command $n -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cmd) { return $cmd.Source }
    }
    Fail "no 7z/7za found (Qt ships .7z archives). Pass -SevenZip <path to 7z.exe>."
}

function Get-PackageMeta([string]$GroupUrl, [string]$PackageName) {
    $xmlPath = Join-Path $DownloadDir (($PackageName -replace '[^A-Za-z0-9._-]', '_') + '.Updates.xml')
    if (-not (Get-RemoteFile "$GroupUrl/Updates.xml" $xmlPath)) {
        Fail "cannot fetch $GroupUrl/Updates.xml"
    }
    $xml = Get-Content $xmlPath -Raw -Encoding UTF8
    $block = $null
    foreach ($m in [regex]::Matches($xml, '<PackageUpdate>(.*?)</PackageUpdate>', 'Singleline')) {
        $nm = [regex]::Match($m.Groups[1].Value, '<Name>([^<]+)</Name>')
        if ($nm.Success -and $nm.Groups[1].Value -eq $PackageName) { $block = $m.Groups[1].Value; break }
    }
    if (-not $block) { Fail "package $PackageName not in $GroupUrl (Qt version or repo layout changed?)" }
    $ver = [regex]::Match($block, '<Version>([^<]+)</Version>').Groups[1].Value
    $archives = [regex]::Match($block, '<DownloadableArchives>([^<]+)</DownloadableArchives>').Groups[1].Value
    if (-not $ver -or -not $archives) { Fail "incomplete metadata for $PackageName (Version / DownloadableArchives)" }
    $targets = @{}
    foreach ($op in [regex]::Matches($block, '<Operation name="Extract">(.*?)</Operation>', 'Singleline')) {
        $args = @([regex]::Matches($op.Groups[1].Value, '<Argument>([^<]*)</Argument>') |
            ForEach-Object { $_.Groups[1].Value })
        if ($args.Count -ge 2) { $targets[$args[1]] = $args[0] }
    }
    return [pscustomobject]@{
        Version  = $ver
        Archives = @($archives -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        Targets  = $targets
        GroupUrl = $GroupUrl
        Package  = $PackageName
    }
}

function Install-Package($Meta, [string]$DefaultTargetDir) {
    $urlBase = "$($Meta.GroupUrl)/$($Meta.Package)"
    $total = $Meta.Archives.Count
    $n = 0
    $bytes = 0
    foreach ($a in $Meta.Archives) {
        $n++
        if ($ArchiveFilter -and ($a -notlike "*$ArchiveFilter*")) {
            Write-Host "  [$n/$total] skipped (no -ArchiveFilter match): $a" -ForegroundColor DarkGray
            continue
        }
        $target = $DefaultTargetDir
        if ($Meta.Targets.ContainsKey($a)) {
            # @TargetDir@ is the installer root, i.e. our -OutDir
            $target = $Meta.Targets[$a] -replace '@TargetDir@', $OutDir
        }
        # The new layout glues Version and archive name into one file name.
        $url = "$urlBase/$($Meta.Version)$a"
        $dest = Join-Path $DownloadDir $a
        Write-Host "  [$n/$total] $a"
        if (-not (Get-RemoteFile $url $dest)) { Fail "download failed: $url" }
        $bytes += (Get-Item $dest).Length
        Test-Sha1 $dest $url
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        & $script:SevenZip x "-o$target" -y $dest | Select-Object -Last 3
        if ($LASTEXITCODE -ne 0) { Fail "7z failed (exit $LASTEXITCODE) extracting $a into $target" }
        Remove-Item $dest -Force -ErrorAction SilentlyContinue
    }
    Write-Host ("  done: {0} archive(s) listed, {1:N1} MB downloaded" -f $n, ($bytes / 1MB)) -ForegroundColor Green
}

$script:SevenZip = Find-SevenZip $SevenZip
Write-Host "[qt-mirror] 7z           : $($script:SevenZip)"
Write-Host "[qt-mirror] mirror       : $Mirror"
Write-Host "[qt-mirror] install root : $OutDir"
Write-Host "[qt-mirror] components   : $($ComponentList -join ', ')"
Write-Host ""

$noDots = $Version -replace '\.', ''
$hostArch = 'win64_mingw'
$hostVariant = '_' + ($hostArch -replace '^win64_', '')

foreach ($c in $ComponentList) {
    switch ($c) {
        'Host' {
            Write-Host "[qt-mirror] desktop MinGW kit (QT_HOST_PATH)" -ForegroundColor Cyan
            $group = "$Mirror/windows_x86/desktop/qt6_$noDots/qt6_$noDots$hostVariant"
            $meta = Get-PackageMeta $group "qt.qt6.$noDots.$hostArch"
            Install-Package $meta (Join-Path (Join-Path $OutDir $Version) 'mingw_64')
            $marker = Get-ChildItem -Path (Join-Path $OutDir $Version) -Recurse -Filter 'androiddeployqt.exe' -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if (-not $marker) { Fail "no androiddeployqt.exe under $OutDir\$Version after Host install" }
            Write-Host "[qt-mirror] androiddeployqt: $($marker.FullName)" -ForegroundColor Green
        }
        'Android' {
            Write-Host "[qt-mirror] Android arm64-v8a kit" -ForegroundColor Cyan
            $group = "$Mirror/all_os/android/qt6_$noDots/qt6_${noDots}_arm64_v8a"
            $meta = Get-PackageMeta $group "qt.qt6.$noDots.android_arm64_v8a"
            Install-Package $meta (Join-Path (Join-Path $OutDir $Version) 'android_arm64_v8a')
            $qc = Join-Path $OutDir "$Version\android_arm64_v8a\bin\qt-cmake.bat"
            if (-not (Test-Path $qc)) { Fail "no qt-cmake.bat after Android install, expected $qc" }
            Write-Host "[qt-mirror] qt-cmake: $qc" -ForegroundColor Green
        }
        'WebSockets' {
            # The project links Qt::WebSockets, and that module is NOT in the base
            # android package's archives (qtbase/qtsvg/qtdeclarative/qtdoc/qttools/
            # qttranslations). Without it androiddeployqt's cmake configure dies on
            # find_package(WebSockets). DBus is not needed: every DBus find_package
            # in src/ is guarded by "NOT ANDROID".
            Write-Host "[qt-mirror] Android Qt WebSockets addon" -ForegroundColor Cyan
            $group = "$Mirror/all_os/android/qt6_$noDots/qt6_${noDots}_arm64_v8a"
            $meta = Get-PackageMeta $group "qt.qt6.$noDots.addons.qtwebsockets.android_arm64_v8a"
            Install-Package $meta (Join-Path (Join-Path $OutDir $Version) 'android_arm64_v8a')
            $ws = Join-Path $OutDir "$Version\android_arm64_v8a\lib\cmake\Qt6WebSockets\Qt6WebSocketsConfig.cmake"
            if (-not (Test-Path $ws)) { Fail "no Qt6WebSocketsConfig.cmake after WebSockets install, expected $ws" }
            Write-Host "[qt-mirror] Qt6WebSockets: $ws" -ForegroundColor Green
        }
        'Make' {
            # FFmpeg's cross build needs GNU make; neither the NDK nor Git provides it.
            Write-Host "[qt-mirror] MinGW toolchain (mingw32-make)" -ForegroundColor Cyan
            $group = "$Mirror/windows_x86/desktop/tools_mingw1310"
            $meta = Get-PackageMeta $group 'qt.tools.win64_mingw1310'
            Install-Package $meta (Join-Path (Join-Path $OutDir 'Tools') 'mingw1310_64')
            $mk = Get-ChildItem -Path (Join-Path $OutDir 'Tools') -Recurse -Filter 'mingw32-make.exe' -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if (-not $mk) { Fail "no mingw32-make.exe under $OutDir\Tools after Make install" }
            Write-Host "[qt-mirror] GNU make: $($mk.FullName)" -ForegroundColor Green
        }
    }
    Write-Host ""
}

Write-Host "[qt-mirror] all requested components installed under $OutDir" -ForegroundColor Green
exit 0
