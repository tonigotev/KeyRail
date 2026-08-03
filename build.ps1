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

.PARAMETER Release
    Also archive the installers into releases\v<version>\. Only pass this when you
    are actually publishing a version, so releases\ stays a short list of real
    releases instead of a pile of every build.

.OUTPUTS
    dist\ always holds exactly one build: the current one. A full build clears it
    first, so there is never an older exe sitting next to a newer one. Old versions
    live in releases\ and nowhere else.

.EXAMPLE
    .\build.ps1
    Rebuild the daemon and restart it.

.EXAMPLE
    .\build.ps1 -All
    Rebuild the daemon plus the UI and installers.

.EXAMPLE
    .\build.ps1 -All -Release
    Full build, and keep a copy of the installers under releases\v0.1.0\.
#>

[CmdletBinding()]
param(
    [switch]$All,
    [switch]$NoRestart,
    [switch]$Release,
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$cppDir = Join-Path $root "keyrail_daemon"
$cppBuildDir = Join-Path $cppDir "build"
$uiDir = Join-Path $root "keyrail_ui"
$distDir = Join-Path $root "dist"
$releasesDir = Join-Path $root "releases"

# The installer file names carry this, and releases\ is keyed on it, so read it
# from the one place that actually defines it rather than hardcoding a copy.
$version = (Get-Content (Join-Path $uiDir "src-tauri\tauri.conf.json") -Raw | ConvertFrom-Json).version

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

# A full build produces every artifact, so dist\ is emptied first and refilled.
# That is the whole point: "which exe is the latest" should never be a question
# you have to answer by comparing timestamps. A daemon-only build cannot refill
# the installer, so it leaves the rest of dist\ alone and warns about it below.
if ($All -and (Test-Path $distDir)) {
    Remove-Item (Join-Path $distDir "*") -Recurse -Force -ErrorAction SilentlyContinue
    Write-Note "cleared dist\ so it holds only this build"
}
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

    # The updater only installs a bundle it can verify against the public key in
    # tauri.conf.json, so the build has to be signed with the matching private
    # key. Without it the build still succeeds and still installs by hand - it
    # just produces no .sig, and every existing install rejects the update.
    # The key is deliberately password protected. Windows cannot hold an empty
    # environment variable - assigning "" deletes it - so a password-less key
    # leaves TAURI_SIGNING_PRIVATE_KEY_PASSWORD unset, and the bundler then stops
    # to prompt for it and hangs any unattended build. A real password is the
    # only value that survives being passed through the environment here.
    $signingKey = Join-Path $env:USERPROFILE ".tauri\keyrail.key"
    $signingPass = Join-Path $env:USERPROFILE ".tauri\keyrail.pass"
    if ((Test-Path $signingKey) -and (Test-Path $signingPass)) {
        $env:TAURI_SIGNING_PRIVATE_KEY = Get-Content $signingKey -Raw
        $env:TAURI_SIGNING_PRIVATE_KEY_PASSWORD = (Get-Content $signingPass -Raw).Trim()
        Write-Note "signing updater artifacts with $signingKey"
    } elseif (Test-Path $signingKey) {
        Write-Warning "found $signingKey but no $signingPass - the build would stop to prompt for the password"
    } else {
        Write-Warning "no signing key at $signingKey - this build cannot be published as an update"
    }

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

    # The UI looks for keyraild.exe next to itself first, so dist\ works as a
    # self-contained folder you can copy anywhere.
    foreach ($installer in @("bundle\nsis", "bundle\msi")) {
        $dir = Join-Path $uiRelease $installer
        if (-not (Test-Path $dir)) { continue }
        $installers = Get-ChildItem $dir -File -Recurse | Where-Object { $_.Extension -in ".exe", ".msi", ".sig" }
        foreach ($file in $installers) {
            $copied = Copy-ToDist $file.FullName
            if ($copied) { $built += $copied }
        }
    }

    # Says what dist\ actually is, so the answer to "which build is this" lives
    # next to the files instead of in a chat log.
    $commit = "unknown"
    try {
        $ErrorActionPreference = "Continue"
        $commit = (& git -C $root rev-parse --short HEAD 2>$null)
        $ErrorActionPreference = "Stop"
    } catch { }
    @(
        "KeyRail $version"
        "built $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
        "commit $commit"
        ""
        "Install with KeyRail_${version}_x64-setup.exe."
        "This folder is wiped and refilled by build.ps1 -All, so it is always the"
        "newest build. Older versions are kept under releases\ only."
    ) -join "`r`n" | Set-Content (Join-Path $distDir "BUILD.txt") -Encoding ascii

    if ($Release) {
        Write-Step "Archiving release v$version"
        $target = Join-Path $releasesDir "v$version"
        if (Test-Path $target) {
            Write-Warning "releases\v$version already exists and will be overwritten. Bump the version in tauri.conf.json before publishing a different build."
        }
        New-Item -ItemType Directory -Force $target | Out-Null
        $setupName = "KeyRail_${version}_x64-setup.exe"
        foreach ($name in @($setupName, "$setupName.sig", "KeyRail_${version}_x64_en-US.msi", "BUILD.txt")) {
            $source = Join-Path $distDir $name
            if (Test-Path $source) { Copy-Item $source -Destination $target -Force }
        }

        # latest.json is what the app actually reads. Writing it here keeps the
        # signature, version and download URL in step with the exe they describe,
        # which hand-editing reliably gets wrong.
        $sigPath = Join-Path $distDir "$setupName.sig"
        if (Test-Path $sigPath) {
            $manifest = [ordered]@{
                version   = $version
                notes     = "See the release notes on GitHub."
                pub_date  = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
                platforms = [ordered]@{
                    "windows-x86_64" = [ordered]@{
                        signature = (Get-Content $sigPath -Raw).Trim()
                        url       = "https://github.com/tonigotev/KeyRail/releases/download/v$version/$setupName"
                    }
                }
            }
            $manifest | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $target "latest.json") -Encoding ascii
            Write-Note "wrote latest.json"
        } else {
            Write-Warning "no .sig produced, so latest.json was skipped. Updates will not work for this build."
        }

        Write-Note "archived to releases\v$version"
        Write-Host ""
        Write-Note "to publish: create a GitHub release tagged v$version and upload"
        Write-Note "  $setupName  and  latest.json  from releases\v$version"
    }
}

# --- refresh the installed copy ----------------------------------------------

# Running a build tree copy alongside an installed copy is how you end up with
# two daemons fighting over the browser bridge port, two WebView2 profiles, and
# a UI that reports state belonging to the other one. If KeyRail is installed,
# the freshly built installer is applied over it so there is exactly one app on
# the machine and it is always the current build.
$installedDir = Join-Path $env:LOCALAPPDATA "KeyRail"
$installedUi = Join-Path $installedDir "keyrail_ui.exe"

if ($All -and (Test-Path $installedUi)) {
    Write-Step "Updating the installed copy"
    $setup = Get-ChildItem $distDir -Filter "KeyRail_*_x64-setup.exe" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($setup) {
        Invoke-Native -What "installer" -Action { & $setup.FullName /S | Out-Null }
        Start-Sleep -Seconds 2
        Write-Note "installed app updated from $($setup.Name)"
    } else {
        Write-Warning "no installer found in dist; the installed copy is now older than this build"
    }
}

# --- restart ----------------------------------------------------------------

if ($wasRunning -and -not $NoRestart) {
    Write-Step "Restarting"
    # Prefer the installed daemon so the running process matches the installed
    # app rather than the build tree.
    $daemonToRun = if (Test-Path (Join-Path $installedDir "keyraild.exe")) {
        Join-Path $installedDir "keyraild.exe"
    } else {
        $daemonExe
    }
    Start-Process -FilePath $daemonToRun
    Start-Sleep -Milliseconds 800
    if (Get-Process keyraild -ErrorAction SilentlyContinue) {
        Write-Note "keyraild is running again"
    } else {
        Write-Warning "keyraild did not stay running. Start it from the UI to see why."
    }
}

if ($All -and $uiWasRunning -and (Test-Path $installedUi)) {
    Write-Note "reopening the installed settings UI"
    Start-Process -FilePath $installedUi
} elseif ($wasRunning -and $NoRestart) {
    Write-Note "daemon left stopped (-NoRestart)"
}

Write-Step "Done"
foreach ($path in $built) {
    Write-Host "    $path" -ForegroundColor Green
}

if (-not $All) {
    # dist\ was not cleared, so any installer in it predates the daemon just
    # built. Saying so beats letting someone ship it by mistake.
    $staleInstaller = Get-ChildItem $distDir -Filter "KeyRail_*-setup.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -lt (Get-Item $daemonExe).LastWriteTime }
    if ($staleInstaller) {
        Write-Host ""
        Write-Warning "dist\$($staleInstaller[0].Name) is older than this daemon build. Run .\build.ps1 -All to refresh it."
    }
    Write-Host ""
    Write-Note "run .\build.ps1 -All to also build the settings UI and installers"
}
