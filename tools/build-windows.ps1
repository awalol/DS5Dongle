<#
.SYNOPSIS
    One-command builder for the Pico2W DualSense 5 Bridge firmware on Windows 11
    (no WSL required).

.DESCRIPTION
    Installs every prerequisite (winget where possible, portable downloads as a
    fallback), fetches the pinned Raspberry Pi Pico SDK + TinyUSB, initialises
    this repo's submodules, then configures and builds the firmware with CMake +
    Ninja. The resulting ds5-bridge.uf2 is copied next to this script and onto
    your Desktop.

    The script is idempotent: re-running it skips anything already installed or
    downloaded.

.PARAMETER Variant
    standard (default) - normal firmware.
    debug              - adds -DENABLE_SERIAL=ON -DENABLE_VERBOSE=ON.
    wake               - adds -DENABLE_WAKE_HID=ON (Wake-on-PS build).

.PARAMETER Clean
    Delete the variant's build directory before configuring.

.PARAMETER Repo
    When run standalone (the script is not inside a checkout), the project
    git URL to clone. Defaults to the upstream project. Override to build a
    fork.

.PARAMETER Ref
    Branch, tag or commit to build when cloned standalone. Empty = the
    repo's default branch.

.EXAMPLE
    # Standalone: download just this file anywhere and run it - it clones
    # the project under %USERPROFILE%\.ds5-build and builds it.
    powershell -ExecutionPolicy Bypass -File .\build-windows.ps1

.EXAMPLE
    # From inside a cloned repo:
    powershell -ExecutionPolicy Bypass -File tools\build-windows.ps1 -Variant wake

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\build-windows.ps1 -Repo https://github.com/youruser/DS5Dongle.git -Ref master
#>

[CmdletBinding()]
param(
    [ValidateSet('standard', 'debug', 'wake')]
    [string]$Variant = 'standard',
    [switch]$Clean,
    # Project to build when this script is run standalone (not from inside a
    # checkout). Override to build a fork.
    [string]$Repo = 'https://github.com/awalol/DS5Dongle.git',
    # Branch/tag/SHA to build when cloned standalone. Empty = default branch.
    [string]$Ref = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Bump on every change so a stale download is obvious in the banner.
$SCRIPT_REV   = '2026-05-16.7'

# --- Pinned versions: keep in sync with .github/workflows/build-firmware.yml ---
$PICO_SDK_REF = '2.2.0'
$TINYUSB_REF  = '0.20.0'
$ARM_VER      = '14.2.rel1'
$ARM_ZIP      = "arm-gnu-toolchain-$ARM_VER-mingw-w64-x86_64-arm-none-eabi.zip"
$ARM_URL      = "https://developer.arm.com/-/media/Files/downloads/gnu/$ARM_VER/binrel/$ARM_ZIP"
# Portable native host compiler (WinLibs MinGW-w64 UCRT) for pioasm/picotool.
$MINGW_URL    = 'https://github.com/brechtsanders/winlibs_mingw/releases/download/14.2.0posix-19.1.1-12.0.0-ucrt-r2/winlibs-x86_64-posix-seh-gcc-14.2.0-mingw-w64ucrt-12.0.0-r2.zip'

$ToolsHome = Join-Path $env:USERPROFILE '.ds5-build'
$SdkPath   = Join-Path $ToolsHome 'pico-sdk'
$ArmRoot   = Join-Path $ToolsHome 'arm-gnu-toolchain'
$ClonePath = Join-Path $ToolsHome 'DS5Dongle'
# $RepoRoot is resolved at runtime (Resolve-RepoRoot) - either an existing
# checkout this script sits in, or a fresh clone under $ToolsHome.
$RepoRoot  = $null
$GitExit   = 0     # last git exit code, set by Invoke-GitQuiet
$PythonExe = $null # real Python 3 interpreter, set by Resolve-Python

function Info  ($m) { Write-Host "[ds5] $m"            -ForegroundColor Cyan }
function Ok    ($m) { Write-Host "[ds5] $m"            -ForegroundColor Green }
function Warn  ($m) { Write-Host "[ds5] WARNING: $m"   -ForegroundColor Yellow }
function Die   ($m) { Write-Host "[ds5] ERROR: $m"     -ForegroundColor Red; exit 1 }

function Have ($cmd) { [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

function Add-SessionPath ($dir) {
    if ($dir -and (Test-Path $dir) -and ($env:Path -notlike "*$dir*")) {
        $env:Path = "$dir;$env:Path"
    }
}

# --- Discover already-installed tools that aren't on PATH (common with winget) -
function Add-CommonToolPaths {
    $candidates = @(
        "$env:ProgramFiles\CMake\bin",
        "$env:ProgramFiles\Git\cmd",
        "${env:ProgramFiles(x86)}\Git\cmd"
    )
    foreach ($c in $candidates) { Add-SessionPath $c }
    # winget often installs Ninja/Python under WinGet Links or per-user dirs.
    $wingetLinks = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links'
    Add-SessionPath $wingetLinks
}

# --- Step 0: ensure winget exists, bootstrap if missing, else portable mode ----
function Initialize-PackageManager {
    if (Have winget) { Ok 'winget present.'; return $true }

    Warn 'winget (App Installer) not found. Attempting automatic bootstrap...'
    try {
        $tmp = Join-Path $env:TEMP 'ds5-winget'
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null

        $deps = @(
            @{ name = 'VCLibs';
               url  = 'https://aka.ms/Microsoft.VCLibs.x64.14.00.Desktop.appx' },
            @{ name = 'AppInstaller';
               url  = 'https://aka.ms/getwinget' }   # latest DesktopAppInstaller bundle
        )
        foreach ($d in $deps) {
            $dest = Join-Path $tmp ($d.name + [IO.Path]::GetExtension($d.url))
            if (-not $dest.EndsWith('.appx') -and -not $dest.EndsWith('.msixbundle')) {
                $dest = Join-Path $tmp ($d.name + '.msixbundle')
            }
            Info "Downloading $($d.name)..."
            Invoke-WebRequest -Uri $d.url -OutFile $dest -UseBasicParsing
            Add-AppxPackage -Path $dest -ErrorAction Stop
        }
        # Refresh PATH for the App Execution Alias.
        Add-SessionPath (Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps')
        if (Have winget) { Ok 'winget bootstrapped successfully.'; return $true }
    }
    catch {
        Warn "winget bootstrap failed: $($_.Exception.Message)"
    }

    Warn 'Proceeding in PORTABLE mode (no winget) - tools downloaded locally.'
    return $false
}

# --- winget install with portable fallback per tool --------------------------
function Ensure-Tool {
    param(
        [string]$Command,
        [string]$WingetId,
        [bool]$WingetAvailable,
        [scriptblock]$PortableInstall
    )
    if (Have $Command) { Ok "$Command already available."; return }

    if ($WingetAvailable) {
        Info "Installing $Command via winget ($WingetId)..."
        winget install --id $WingetId --exact --silent --accept-source-agreements `
            --accept-package-agreements --disable-interactivity | Out-Host
        Add-CommonToolPaths
        if (Have $Command) { Ok "$Command installed."; return }
        Warn "$Command still not on PATH after winget; trying portable."
    }

    if ($PortableInstall) {
        Info "Installing $Command (portable)..."
        & $PortableInstall
        if (Have $Command) { Ok "$Command installed (portable)."; return }
    }
    Die "Could not install '$Command'. Install it manually and re-run."
}

function Install-PortableArchiveTool {
    param([string]$Name, [string]$Url, [string]$BinSubdir)
    $base = Join-Path $ToolsHome $Name
    $zip  = Join-Path $env:TEMP "$Name.zip"
    if (-not (Test-Path $base)) {
        Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
        New-Item -ItemType Directory -Force -Path $base | Out-Null
        Expand-Archive -Path $zip -DestinationPath $base -Force
        Remove-Item $zip -Force
    }
    $bin = if ($BinSubdir) { Join-Path $base $BinSubdir } else { $base }
    # Some archives nest a single top-level folder.
    if (-not (Test-Path $bin)) {
        $inner = Get-ChildItem $base -Directory | Select-Object -First 1
        if ($inner) { $bin = Join-Path $inner.FullName $BinSubdir }
    }
    Add-SessionPath $bin
}

# --- ARM GNU toolchain (always portable: winget package is stale ~10.x) ------
function Ensure-ArmToolchain {
    $armBin = Get-ChildItem -Path $ArmRoot -Recurse -Filter 'arm-none-eabi-gcc.exe' `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($armBin) {
        Add-SessionPath (Split-Path $armBin.FullName)
        Ok "ARM toolchain present: $($armBin.Directory)"
        return
    }
    Info "Downloading ARM GNU toolchain $ARM_VER (~500 MB, one-time)..."
    New-Item -ItemType Directory -Force -Path $ArmRoot | Out-Null
    $zip = Join-Path $env:TEMP $ARM_ZIP
    Invoke-WebRequest -Uri $ARM_URL -OutFile $zip -UseBasicParsing
    Info 'Extracting ARM toolchain...'
    Expand-Archive -Path $zip -DestinationPath $ArmRoot -Force
    Remove-Item $zip -Force
    $armBin = Get-ChildItem -Path $ArmRoot -Recurse -Filter 'arm-none-eabi-gcc.exe' |
        Select-Object -First 1
    if (-not $armBin) { Die 'ARM toolchain extraction failed.' }
    Add-SessionPath (Split-Path $armBin.FullName)
    Ok "ARM toolchain ready: $($armBin.Directory)"
}

# --- Host C/C++ compiler (for pico-sdk host tools: pioasm, picotool) ---------
# These run on the PC, not the RP2350, so they need a NATIVE compiler - the
# ARM cross-compiler cannot build them. Use MSVC if present, else a portable
# MinGW-w64.
function Ensure-HostCompiler {
    if (Have 'cl') { Ok 'Host compiler: MSVC (cl) on PATH'; return }
    # g++ on PATH that is NOT the arm-none-eabi cross-compiler
    $g = Get-Command 'g++' -ErrorAction SilentlyContinue
    if ($g -and $g.Source -notlike '*arm-none-eabi*') {
        Ok "Host compiler: $($g.Source)"
        $env:CC = 'gcc'; $env:CXX = 'g++'
        return
    }
    $mwRoot = Join-Path $ToolsHome 'mingw64'
    $gpp = Get-ChildItem -Path $mwRoot -Recurse -Filter 'g++.exe' `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $gpp) {
        Info 'Downloading portable MinGW-w64 host compiler (~120 MB, one-time)...'
        New-Item -ItemType Directory -Force -Path $mwRoot | Out-Null
        $zip = Join-Path $env:TEMP 'winlibs-mingw.zip'
        Invoke-WebRequest -UseBasicParsing -OutFile $zip -Uri $MINGW_URL
        Info 'Extracting MinGW-w64...'
        Expand-Archive -Path $zip -DestinationPath $mwRoot -Force
        Remove-Item $zip -Force
        $gpp = Get-ChildItem -Path $mwRoot -Recurse -Filter 'g++.exe' |
            Select-Object -First 1
    }
    if (-not $gpp) { Die 'Failed to provide a host C++ compiler.' }
    $binDir = Split-Path $gpp.FullName
    Add-SessionPath $binDir
    # Force host sub-builds (pioasm/picotool ExternalProjects) to use MinGW.
    $env:CC  = (Join-Path $binDir 'gcc.exe')
    $env:CXX = (Join-Path $binDir 'g++.exe')
    Ok "Host compiler: $binDir"
}

# --- Pico SDK 2.2.0 + TinyUSB 0.20.0 (mirrors build-firmware.yml) -------------
function Ensure-PicoSdk {
    if (-not (Test-Path (Join-Path $SdkPath 'pico_sdk_init.cmake'))) {
        Info "Cloning Pico SDK $PICO_SDK_REF..."
        git clone --depth 1 --branch $PICO_SDK_REF `
            https://github.com/raspberrypi/pico-sdk.git $SdkPath
        git -C $SdkPath submodule update --init --recursive
    } else {
        Ok 'Pico SDK already present.'
    }
    $tinyusb = Join-Path $SdkPath 'lib\tinyusb'
    $onTag = (git -C $tinyusb describe --tags --exact-match 2>$null)
    if ($onTag -ne $TINYUSB_REF) {
        Info "Switching TinyUSB to $TINYUSB_REF..."
        if (-not (git -C $tinyusb rev-parse -q --verify "refs/tags/$TINYUSB_REF" 2>$null)) {
            git -C $tinyusb fetch --depth 1 origin "refs/tags/${TINYUSB_REF}:refs/tags/$TINYUSB_REF"
        }
        git -C $tinyusb checkout --detach $TINYUSB_REF
    } else {
        Ok "TinyUSB already at $TINYUSB_REF."
    }
}

# --- Locate the project: existing checkout, or clone under $ToolsHome --------
function Test-Ds5Checkout ($dir) {
    if (-not $dir) { return $false }
    $cml = Join-Path $dir 'CMakeLists.txt'
    return (Test-Path $cml) -and (Select-String -Path $cml -Pattern 'ds5-bridge' -Quiet)
}

# Runs git so NOTHING reaches the pipeline: every stream is written to the
# host instead. Two PowerShell hazards handled here:
#  1. Uncaptured native stdout inside a function becomes its return value.
#  2. With $ErrorActionPreference='Stop', git writing to stderr (it uses it
#     for normal progress, e.g. "Already on 'master'") raises a terminating
#     NativeCommandError. So we relax it locally and judge by exit code.
# Sets $script:GitExit to git's real exit code; emits nothing.
function Invoke-GitQuiet {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & git @args 2>&1 | ForEach-Object { Write-Host $_ }
        $script:GitExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
}

# Sets $script:RepoRoot (does NOT return it) so stray git output can never
# contaminate the value, regardless of which stream git writes to.
function Resolve-RepoRoot {
    $script:RepoRoot = $null
    # Run from inside a checkout? (script at <repo>/tools/ or at <repo>/)
    foreach ($cand in @((Split-Path -Parent $PSScriptRoot), $PSScriptRoot)) {
        if (Test-Ds5Checkout $cand) {
            Ok "Using existing checkout: $cand"
            $script:RepoRoot = $cand
            return
        }
    }
    # Standalone: clone (or refresh) the project under $ToolsHome.
    if (Test-Path (Join-Path $ClonePath '.git')) {
        Info "Refreshing existing clone: $ClonePath"
        Invoke-GitQuiet -C $ClonePath remote set-url origin $Repo
        Invoke-GitQuiet -C $ClonePath fetch --tags --prune origin
        if ($Ref) {
            $target = $Ref
        } else {
            $head = (& git -C $ClonePath symbolic-ref --quiet --short refs/remotes/origin/HEAD 2>$null)
            $target = if ($head) { ($head | Select-Object -Last 1).ToString().Split('/')[-1] } else { 'master' }
        }
        Invoke-GitQuiet -C $ClonePath checkout --force $target
        Invoke-GitQuiet -C $ClonePath submodule update --init --recursive
        Invoke-GitQuiet -C $ClonePath pull --ff-only
    } else {
        Info "Cloning $Repo into $ClonePath ..."
        if ($Ref) {
            Invoke-GitQuiet clone --recurse-submodules --branch $Ref $Repo $ClonePath
        } else {
            Invoke-GitQuiet clone --recurse-submodules $Repo $ClonePath
        }
        if (-not (Test-Path (Join-Path $ClonePath '.git'))) { Die "Failed to clone $Repo" }
    }
    if (-not (Test-Ds5Checkout $ClonePath)) { Die "Clone at $ClonePath is not a DS5Dongle project." }
    $script:RepoRoot = $ClonePath
}

# --- Real Python 3 (CMake's FindPython3 needs one) --------------------------
# Clean Windows / Windows Sandbox ships a Microsoft Store "python.exe" alias
# under \WindowsApps that is NOT an interpreter - Get-Command finds it but
# CMake rejects it. Resolve a genuine interpreter and pass it to CMake.
function Test-RealPython ($exe) {
    if (-not $exe) { return $false }
    if ("$exe" -like '*\WindowsApps\*') { return $false }   # Store alias stub
    try {
        $v = (& $exe --version 2>&1 | Out-String)
        return ($v -match 'Python\s+3\.')
    } catch { return $false }
}

function Resolve-Python {
    if (Have 'py') {
        try {
            $p = (& py -3 -c 'import sys;print(sys.executable)' 2>$null | Select-Object -Last 1)
            if (Test-RealPython $p) { $script:PythonExe = "$p"; Ok "Python: $p"; return }
        } catch {}
    }
    foreach ($name in @('python', 'python3')) {
        $c = Get-Command $name -ErrorAction SilentlyContinue
        if ($c -and (Test-RealPython $c.Source)) {
            $script:PythonExe = $c.Source; Ok "Python: $($c.Source)"; return
        }
    }
    if ($useWinget) {
        Info 'Installing Python 3.12 via winget...'
        winget install --id Python.Python.3.12 --exact --silent --accept-source-agreements `
            --accept-package-agreements --disable-interactivity | Out-Host
        Add-CommonToolPaths
    }
    $globs = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python3*\python.exe'),
        (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\Python.Python.3.12_*\python.exe'),
        (Join-Path $env:ProgramFiles 'Python3*\python.exe'),
        'C:\Python3*\python.exe'
    )
    foreach ($g in $globs) {
        $hit = Get-ChildItem -Path $g -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit -and (Test-RealPython $hit.FullName)) {
            $script:PythonExe = $hit.FullName
            Add-SessionPath (Split-Path $hit.FullName)
            Ok "Python: $($hit.FullName)"; return
        }
    }
    # Last resort: portable embeddable build (enough for the SDK's scripts).
    Info 'Installing portable Python (embeddable)...'
    $pyDir = Join-Path $ToolsHome 'python'
    if (-not (Test-Path (Join-Path $pyDir 'python.exe'))) {
        $zip = Join-Path $env:TEMP 'python-embed.zip'
        Invoke-WebRequest -UseBasicParsing -OutFile $zip `
            -Uri 'https://www.python.org/ftp/python/3.12.7/python-3.12.7-embed-amd64.zip'
        New-Item -ItemType Directory -Force -Path $pyDir | Out-Null
        Expand-Archive -Path $zip -DestinationPath $pyDir -Force
        Remove-Item $zip -Force
    }
    $pyExe = Join-Path $pyDir 'python.exe'
    if (Test-RealPython $pyExe) {
        $script:PythonExe = $pyExe; Add-SessionPath $pyDir; Ok "Python: $pyExe"; return
    }
    Die 'Could not provide a working Python 3 interpreter.'
}

# ---------------------------------------------------------------------------- #
#  Main                                                                        #
# ---------------------------------------------------------------------------- #
Info "DS5Dongle Windows builder (rev $SCRIPT_REV) - variant: $Variant"
New-Item -ItemType Directory -Force -Path $ToolsHome | Out-Null
Add-CommonToolPaths

# CMakeLists.txt includes ~/.pico-sdk/cmake/pico-vscode.cmake if present, which
# can override the SDK/toolchain versions. Warn so the user knows why a stale
# Pico VS Code install might change the build.
$picoVscode = Join-Path $env:USERPROFILE '.pico-sdk\cmake\pico-vscode.cmake'
if (Test-Path $picoVscode) {
    Warn "Found $picoVscode - the Pico VS Code extension may override SDK/toolchain"
    Warn 'versions. If the build misbehaves, this script still passes the pinned'
    Warn "PICO_SDK_PATH ($SdkPath) explicitly, which normally wins."
}

$useWinget = Initialize-PackageManager

Ensure-Tool -Command 'git'    -WingetId 'Git.Git'           -WingetAvailable $useWinget -PortableInstall {
    Install-PortableArchiveTool -Name 'git' `
        -Url 'https://github.com/git-for-windows/git/releases/download/v2.47.1.windows.1/MinGit-2.47.1-64-bit.zip' `
        -BinSubdir 'cmd'
}
Ensure-Tool -Command 'cmake'  -WingetId 'Kitware.CMake'      -WingetAvailable $useWinget -PortableInstall {
    Install-PortableArchiveTool -Name 'cmake' `
        -Url 'https://github.com/Kitware/CMake/releases/download/v3.31.3/cmake-3.31.3-windows-x86_64.zip' `
        -BinSubdir 'cmake-3.31.3-windows-x86_64\bin'
}
Ensure-Tool -Command 'ninja'  -WingetId 'Ninja-build.Ninja'  -WingetAvailable $useWinget -PortableInstall {
    Install-PortableArchiveTool -Name 'ninja' `
        -Url 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' ''
}
Resolve-Python   # sets $script:PythonExe (handles the Store alias stub)

Ensure-ArmToolchain
Ensure-HostCompiler
Ensure-PicoSdk

# --- Locate / fetch the project source --------------------------------------
Resolve-RepoRoot   # sets $script:RepoRoot
if (-not $RepoRoot -or -not (Test-Ds5Checkout $RepoRoot)) {
    Die "Could not locate the DS5Dongle source (resolved: '$RepoRoot')."
}
Ok "Project source: $RepoRoot"

# --- Project submodules (lib/WDL, lib/opus per .gitmodules) -------------------
Info 'Initialising project submodules (WDL, opus)...'
Invoke-GitQuiet -C $RepoRoot submodule update --init --recursive
if ($GitExit -ne 0) { Die 'Submodule init failed (lib/WDL, lib/opus).' }

# --- Configure + build -------------------------------------------------------
$buildDir = Join-Path $RepoRoot "build\$Variant"
if ($Clean -and (Test-Path $buildDir)) {
    Info "Cleaning $buildDir..."
    Remove-Item -Recurse -Force $buildDir
}

$cmakeArgs = @(
    '-S', $RepoRoot, '-B', $buildDir, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DPICO_SDK_PATH=$SdkPath",
    "-DPython3_EXECUTABLE=$($PythonExe -replace '\\','/')"
)
switch ($Variant) {
    'debug' { $cmakeArgs += @('-DENABLE_SERIAL=ON', '-DENABLE_VERBOSE=ON') }
    'wake'  { $cmakeArgs += @('-DENABLE_WAKE_HID=ON') }
}

Info "Configuring: cmake $($cmakeArgs -join ' ')"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Die 'CMake configure failed.' }

Info 'Building ds5-bridge...'
& cmake --build $buildDir --target ds5-bridge
if ($LASTEXITCODE -ne 0) { Die 'Build failed.' }

# --- Collect output ----------------------------------------------------------
$uf2 = Join-Path $buildDir 'ds5-bridge.uf2'
if (-not (Test-Path $uf2)) { Die "Expected $uf2 was not produced." }

$outName = if ($Variant -eq 'standard') { 'ds5-bridge.uf2' } else { "ds5-bridge-$Variant.uf2" }
$nextToScript = Join-Path $PSScriptRoot $outName
Copy-Item $uf2 $nextToScript -Force
$desktop = [Environment]::GetFolderPath('Desktop')
$onDesktop = Join-Path $desktop $outName
Copy-Item $uf2 $onDesktop -Force

Write-Host ''
Ok  "Build succeeded! Firmware ($Variant):"
Ok  "  $nextToScript"
Ok  "  $onDesktop"
Write-Host ''
Info 'Flash it:'
Info '  1. Hold BOOTSEL on the Pico2W and plug it in via USB.'
Info '  2. It mounts as a USB drive (RP2350).'
Info "  3. Drag $outName onto that drive. The Pico reboots into the firmware."
