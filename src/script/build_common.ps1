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
    throw "Git Bash not found. Set env GIT_BASH to bash.exe"
}

function Get-MakeProgram {
    if ($env:MINGW_MAKE -and (Test-Path $env:MINGW_MAKE)) { return $env:MINGW_MAKE }
    foreach ($c in @('C:\Qt\Tools\mingw530_32\bin\mingw32-make.exe',
                     'E:\programes\Qt\QtLegacy\Tools\mingw530_32\bin\mingw32-make.exe')) {
        if (Test-Path $c) { return $c }
    }
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
    if (-not $qtBase) { throw "Qt install not found. Set env QT_BASE_DIR to the Qt root" }

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
    throw "No Qt MinGW kit found under $qtBase. Set env QT_BASE_DIR or MINGW_MAKE"
}

function Get-Toolchain([string]$repoRoot) {
    $cacheResult = Get-ToolchainFromCache $repoRoot
    if ($cacheResult) {
        Write-Host "[toolchain] reused from CMakeCache.txt"
        return $cacheResult
    }
    $scanResult = Get-ToolchainByScan
    Write-Host "[toolchain] auto-detected from Qt installation"
    return $scanResult
}
