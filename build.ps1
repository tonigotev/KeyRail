<#
.SYNOPSIS
    Builds KeyRail and drops the runnable exes into dist\.

.DESCRIPTION
    By default this builds only the C++ daemon (keyraild.exe), which takes a few
    seconds. Pass -All to also build the Tauri settings UI, which takes several
    minutes on a cold Rust build.

    A running keyraild.exe holds a lock on its own file, so the script stops the
    daemon through its control pipe before building and starts the new build
    again afterwards.

.PARAMETER All
    Also build the Tauri UI and its installers.

.PARAMETER NoRestart
    Leave the daemon stopped after building instead of starting the new one.

.PARAMETER Configuration
    CMake configuration to build. Release by default.

.EXAMPLE
    .\build.ps1
    Rebuild the daemon and restart it.

.EXAMPLE
    .\build.ps1 -All
    Rebuild the daemon plus the UI and installers.
#>

[CmdletBinding()]
param(
    [switch]$All,
    [switch]$NoRestart,
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$cppDir = Join-Path $root "keyrail_daemon"
$cppBuildDir = Join-Path $cppDir "build"
$uiDir = Join-Path $root "keyrail_ui"
$distDir = Join-Path $root "dist"

# Windows PowerShell turns any stderr output from a native exe into a terminating
# error while ErrorActionPreference is Stop. cmake, npm and tauri all write
# ordinary progress to stderr, so native calls run with it relaxed and are judged
# by their exit code instead.
function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$What
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $Action
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)" }
}

function Write-Step([string]$message) {
    Write-Host ""
    Write-Host "==> $message" -ForegroundColor Cyan
}

function Write-Note([string]$message) {
    Write-Host "    $message" -ForegroundColor DarkGray
}

function Resolve-CMake {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }

    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    throw "cmake not found. Install Visual Studio 2022 with the C++ workload, or put cmake on PATH."
}

# Ask the daemon to quit through its own control pipe. Returns $true when a
# daemon was running, so the script knows whether to start one again later.
function Stop-Daemon {
    $running = @(Get-Process keyraild -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) { return $false }

    try {
        $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "keyrail-control", [System.IO.Pipes.PipeDirection]::InOut)
        $pipe.Connect(3000)
        $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
        $bytes = [System.Text.Encoding]::UTF8.GetBytes('{"command":"quit"}')
        $pipe.Write($bytes, 0, $bytes.Length)
        $pipe.Flush()
        $pipe.Dispose()
        Write-Note "asked the running daemon to quit"
    } catch {
        Write-Note "control pipe did not answer, falling back to Stop-Process"
    }

    foreach ($process in $running) {
        if (-not $process.WaitForExit(5000)) {
            Write-Note "daemon $($process.Id) ignored the quit request, stopping it"
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit(3000) | Out-Null
        }
    }

    return $true
}

# The settings UI has no control channel, so rebuilding it means closing it.
# Only -All rebuilds the UI, so this never fires on a plain daemon build.
function Stop-Ui {
    $running = @(Get-Process keyrail_ui -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) { return $false }
    Write-Note "closing the settings UI so its exe can be replaced"
    foreach ($process in $running) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit(4000) | Out-Null
    }
    return $true
}

function Copy-ToDist([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    $leaf = Split-Path $path -Leaf
    try {
        Copy-Item $path -Destination $distDir -Force -ErrorAction Stop
    } catch {
        # Usually means the app is open. The real build output is still valid,
        # so this only costs the dist\ copy.
        Write-Warning "could not copy $leaf into dist (in use?). Close it and rerun to refresh dist\$leaf"
        return $null
    }
    return (Join-Path $distDir $leaf)
}

$built = @()

# --- daemon -----------------------------------------------------------------

$cmake = Resolve-CMake
Write-Step "Building the daemon ($Configuration)"
Write-Note "cmake: $cmake"

$wasRunning = Stop-Daemon

if (-not (Test-Path (Join-Path $cppBuildDir "CMakeCache.txt"))) {
    Write-Note "configuring build directory"
    Invoke-Native -What "cmake configure" -Action { & $cmake -S $cppDir -B $cppBuildDir | Out-Null }
}

Invoke-Native -What "daemon build" -Action { & $cmake --build $cppBuildDir --config $Configuration }

if (-not (Test-Path $distDir)) { New-Item -ItemType Directory -Path $distDir | Out-Null }

$daemonExe = Join-Path $cppBuildDir "$Configuration\keyraild.exe"
$copied = Copy-ToDist $daemonExe
if ($copied) { $built += $copied }
$copied = Copy-ToDist (Join-Path $cppBuildDir "$Configuration\media_list.exe")
if ($copied) { $built += $copied }

# --- UI ---------------------------------------------------------------------

if ($All) {
    Write-Step "Building the settings UI"

    if (-not (Get-Command npm -ErrorAction SilentlyContinue)) { throw "npm not found. Install Node.js." }
    if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) { throw "cargo not found. Install Rust via rustup." }

    $uiWasRunning = Stop-Ui

    # The bundler picks the daemon up from src-tauri\binaries, so stage the
    # freshly built copies there first. Without this the installer ships a UI
    # that cannot find its daemon, which only works in the repo because the UI
    # falls back to walking up to the build tree.
    $stageDir = Join-Path $uiDir "src-tauri\binaries"
    New-Item -ItemType Directory -Force $stageDir | Out-Null
    foreach ($name in @("keyraild.exe", "media_list.exe")) {
        $source = Join-Path $cppBuildDir "$Configuration\$name"
        if (-not (Test-Path $source)) { throw "$name is missing; the daemon build must run before the UI build" }
        Copy-Item $source -Destination $stageDir -Force
    }
    Write-Note "staged the daemon for bundling"

    Push-Location $uiDir
    try {
        if (-not (Test-Path (Join-Path $uiDir "node_modules"))) {
            Write-Note "installing npm dependencies"
            Invoke-Native -What "npm install" -Action { & npm install }
        }

        Write-Note "this takes a few minutes on a cold Rust build"
        Invoke-Native -What "UI build" -Action { & npm run tauri -- build }
    } finally {
        Pop-Location
    }

    $uiRelease = Join-Path $uiDir "src-tauri\target\release"
    $uiExe = Join-Path $uiRelease "keyrail_ui.exe"
    $copied = Copy-ToDist $uiExe
    if ($copied) { $built += $copied }
    if ($uiWasRunning) {
        Write-Note "reopening the settings UI"
        Start-Process -FilePath $uiExe
    }

    # The UI looks for keyraild.exe next to itself first, so dist\ works as a
    # self-contained folder you can copy anywhere.
    foreach ($installer in @("bundle\nsis", "bundle\msi")) {
        $dir = Join-Path $uiRelease $installer
        if (-not (Test-Path $dir)) { continue }
        $installers = Get-ChildItem $dir -File -Recurse | Where-Object { $_.Extension -in ".exe", ".msi" }
        foreach ($file in $installers) {
            $copied = Copy-ToDist $file.FullName
            if ($copied) { $built += $copied }
        }
    }
}

# --- restart ----------------------------------------------------------------

if ($wasRunning -and -not $NoRestart) {
    Write-Step "Restarting the daemon"
    Start-Process -FilePath $daemonExe
    Start-Sleep -Milliseconds 800
    if (Get-Process keyraild -ErrorAction SilentlyContinue) {
        Write-Note "keyraild is running again"
    } else {
        Write-Warning "keyraild did not stay running. Start it from the UI to see why."
    }
} elseif ($wasRunning) {
    Write-Note "daemon left stopped (-NoRestart)"
}

Write-Step "Done"
foreach ($path in $built) {
    Write-Host "    $path" -ForegroundColor Green
}
if (-not $All) {
    Write-Host ""
    Write-Note "run .\build.ps1 -All to also build the settings UI and installers"
}
