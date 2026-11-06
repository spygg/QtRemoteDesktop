# Shared toolchain detection for the build scripts.
# Resolution order (deterministic):
#   1. Environment overrides: QT_BASE_DIR, GIT_BASH
#   2. Existing build CMakeCache.txt (source of truth for a working build)
#   3. Scan of common Qt roots, preferring the older/32-bit MinGW kit
# No repo paths are hardcoded here; the repo root is passed in by the caller.

function ConvertTo-UnixPath([string]$winPath) {
    $drive = $winPath.Substring(0, 1).ToLower()
    $rest  = $winPath.Substring(2).Replace('\', '/')
    return "/$drive$rest"
}

# Prompt the user for an existing path. Returns $null on empty input (abort).
# Loops until the path exists or the user bails out, so we never accept a typo.
function Read-ValidPath([string]$Prompt) {
    if ($Host.UI.IsInputRedirected) {
        Write-Warning "Input is redirected; cannot prompt interactively."
        return $null
    }
    while ($true) {
        $p = Read-Host $Prompt
        if (-not $p) { return $null }
        $p = $p.Trim().Trim('"').Trim("'")
        if (Test-Path $p) { return $p }
        Write-Warning "Path not found: $p"
    }
}

function Get-GitBash {
    if ($env:GIT_BASH -and (Test-Path $env:GIT_BASH)) { return $env:GIT_BASH }
    # Resolve from git.exe's own install location (avoids the WSL bash stub)
    $gitExe = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($gitExe) {
        $gitRoot = Split-Path (Split-Path $gitExe.Source -Parent) -Parent
        $candidate = Join-Path $gitRoot 'bin\bash.exe'
        if (Test-Path $candidate) { return $candidate }
    }
    foreach ($c in @('C:\Program Files\Git\bin\bash.exe',
                     'C:\Program Files (x86)\Git\bin\bash.exe',
                     'E:\programes\Git\bin\bash.exe')) {
        if (Test-Path $c) { return $c }
    }
    $cmd = Get-Command bash.exe -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source -match '\\Git\\' -and $cmd.Source -match 'bash\.exe$') { return $cmd.Source }
    $p = Read-ValidPath "Git Bash (bash.exe) not found. Enter full path to bash.exe"
    if ($p) { return $p }
    throw "Git Bash not found. Set env GIT_BASH to bash.exe"
}

function Get-MakeProgram {
    if ($env:MINGW_MAKE -and (Test-Path $env:MINGW_MAKE)) { return $env:MINGW_MAKE }
    foreach ($c in @('C:\Qt\Tools\mingw530_32\bin\mingw32-make.exe',
                     'E:\programes\Qt\QtLegacy\Tools\mingw530_32\bin\mingw32-make.exe')) {
        if (Test-Path $c) { return $c }
    }
    $p = Read-ValidPath "mingw32-make.exe not found. Enter full path"
    if ($p) { return $p }
    throw "mingw32-make.exe not found. Set env MINGW_MAKE to mingw32-make.exe"
}

# Read Qt prefix + mingw bin from an existing CMakeCache.txt if present.
function Get-ToolchainFromCache([string]$repoRoot) {
    $cache = Join-Path $repoRoot 'build\CMakeCache.txt'
    if (-not (Test-Path $cache)) { return $null }
    $prefix = $null
    $make   = $null
    foreach ($line in Get-Content $cache) {
        if ($line -match '^CMAKE_PREFIX_PATH:.*=(.+)$')  { $prefix = $Matches[1].Trim() }
        if ($line -match '^CMAKE_MAKE_PROGRAM:.*=(.+)$') { $make   = $Matches[1].Trim() }
    }
    if (-not $prefix -or -not $make) { return $null }
    if (-not (Test-Path "$prefix\bin\qmake.exe")) { return $null }
    $mingwBin = Split-Path $make -Parent
    if (-not (Test-Path (Join-Path $mingwBin 'mingw32-make.exe'))) { return $null }
    return @{ Prefix = $prefix; MakeProgram = $make; MingwBin = $mingwBin }
}

# Scan common Qt roots; prefer older (legacy) 32-bit MinGW kits.
function Get-ToolchainByScan {
    $roots = @()
    if ($env:QT_BASE_DIR) { $roots += $env:QT_BASE_DIR }
    $roots += 'E:\programes\Qt', 'C:\Qt', 'C:\Program Files\Qt'
    # Order roots so a "QtLegacy" subdir is preferred over a newer "Qt" one.
    $qtBase = $roots | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $qtBase) { Write-Host "[toolchain] no Qt root found at common locations"; return $null }

    $legacy = Get-ChildItem $qtBase -Directory -Name -ErrorAction SilentlyContinue | Where-Object { $_ -match 'Legacy|legacy' }
    $baseDirs = @()
    foreach ($d in $legacy) { $baseDirs += Join-Path $qtBase $d }
    $baseDirs += (Get-ChildItem $qtBase -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch 'Legacy|legacy' } | ForEach-Object { $_.FullName })

    foreach ($base in $baseDirs) {
        $versions = Get-ChildItem $base -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.\d+' } | Sort-Object Name
        foreach ($v in $versions) {
            $kit = Get-ChildItem $v.FullName -Directory -ErrorAction SilentlyContinue |
                Where-Object {
                    $_.Name -match 'mingw' -and
                    (Test-Path (Join-Path $_.FullName 'bin\qmake.exe')) -and
                    (Test-Path (Join-Path $_.FullName 'lib\cmake\Qt5\Qt5Config.cmake'))
                } | Sort-Object Name | Select-Object -First 1
            if (-not $kit) { continue }
            $prefix = $kit.FullName
            # Mingw kit sits next to the version dir: <base>\Tools\mingw*\bin
            $tools = Get-ChildItem (Join-Path $base 'Tools') -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match 'mingw' } | Sort-Object Name | Select-Object -First 1
            if (-not $tools) { continue }
            $mingwBin = Join-Path $tools.FullName 'bin'
            if (-not (Test-Path (Join-Path $mingwBin 'mingw32-make.exe'))) { continue }
            $make = Join-Path $mingwBin 'mingw32-make.exe'
            return @{ Prefix = $prefix; MakeProgram = $make; MingwBin = $mingwBin }
        }
    }
    Write-Host "[toolchain] no Qt MinGW kit found under $qtBase"; return $null
}

function Get-Toolchain([string]$repoRoot) {
    $cacheResult = Get-ToolchainFromCache $repoRoot
    if ($cacheResult) {
        Write-Host "[toolchain] reused from CMakeCache.txt"
        return $cacheResult
    }
    $scanResult = Get-ToolchainByScan
    if ($scanResult) {
        Write-Host "[toolchain] auto-detected from Qt installation"
        return $scanResult
    }

    # Auto-detection failed -> ask the user, then validate before continuing.
    Write-Host "[toolchain] auto-detection failed; please enter paths manually" -ForegroundColor Yellow
    $prefix = Read-ValidPath "Enter Qt kit prefix (folder containing bin\qmake.exe)"
    if (-not $prefix) { throw "Qt MinGW kit not found. Set env QT_BASE_DIR to the Qt kit folder." }
    if (-not (Test-Path (Join-Path $prefix 'bin\qmake.exe'))) {
        throw "No qmake.exe found under '$prefix'."
    }
    $mingwBin = Read-ValidPath "Enter MinGW bin folder (containing mingw32-make.exe)"
    if (-not $mingwBin) { throw "MinGW bin folder not provided." }
    if (-not (Test-Path (Join-Path $mingwBin 'mingw32-make.exe'))) {
        throw "No mingw32-make.exe found under '$mingwBin'."
    }
    $make = Join-Path $mingwBin 'mingw32-make.exe'
    return @{ Prefix = $prefix; MakeProgram = $make; MingwBin = $mingwBin }
}

# Windows 上外部扫描器/索引器会短暂占用刚写出的静态归档，ar 因此报
#   ar.exe: unable to rename '...\libXXX.a'; reason: Permission denied
# 紧接着 ranlib 读到半写归档报 "Malformed archive"（2026-11-06 实测：
# build.bat 第一次运行正是这两条，第二次才走到链接）。
# 只对这两类指纹做有限次重试；其他构建错误照常立刻失败，不做"重试到通过"。
function Invoke-NativeBuildWithArchiveRetry {
    param(
        [string]$What,
        [scriptblock]$Body,
        [scriptblock]$OnRetry = $null,
        [int]$MaxAttempts = 3
    )
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
            Write-Host ("[retry] {0}: archive race detected (ar/ranlib rename), retrying {1}/{2}..." -f
                       $What, ($attempt + 1), $MaxAttempts) -ForegroundColor Yellow
            Start-Sleep -Seconds 3
            if ($null -ne $OnRetry) { & $OnRetry }
            continue
        }
        throw "$What failed (exit $code)"
    }
}
