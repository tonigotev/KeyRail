<#
.SYNOPSIS
    Builds the store-ready zip of the browser media bridge extension.

.DESCRIPTION
    Stages only the files the extension actually loads, then zips them into
    dist\. The same zip uploads to both the Chrome Web Store and Microsoft Edge
    Add-ons.

    Deliberately excluded:
      content.js, adapters\  - not referenced by the manifest and never injected
                               by background.js. Shipping unused code that reads
                               page content invites review questions for no gain.
      README.md              - describes the Load unpacked flow, which stops
                               being how anyone installs this once published.

.PARAMETER Version
    Overrides the version written into the packaged manifest. The store rejects
    re-uploads that do not increase it.
#>

[CmdletBinding()]
param(
    [string]$Version
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$source = Join-Path $root "integrations\browser\mediaTargetBridge"
$dist = Join-Path $root "dist"
$stage = Join-Path $dist "extension"

# Every file the extension actually uses at runtime.
$include = @(
    "manifest.json",
    "background.js",
    "ms-capture.js",
    "popup.html",
    "popup.js",
    "icons\icon-16.png",
    "icons\icon-32.png",
    "icons\icon-48.png",
    "icons\icon-128.png"
)

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
New-Item -ItemType Directory -Force $dist | Out-Null

foreach ($relative in $include) {
    $from = Join-Path $source $relative
    if (-not (Test-Path $from)) { throw "missing required file: $relative" }
    $to = Join-Path $stage $relative
    New-Item -ItemType Directory -Force (Split-Path $to -Parent) | Out-Null
    Copy-Item $from -Destination $to -Force
}

$manifestPath = Join-Path $stage "manifest.json"
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json

if ($Version) {
    $manifest.version = $Version
    $manifest | ConvertTo-Json -Depth 10 | Set-Content $manifestPath -Encoding utf8
}

$zip = Join-Path $dist "hotkey-to-command-media-bridge-$($manifest.version).zip"
if (Test-Path $zip) { Remove-Item $zip -Force }

# Built entry by entry rather than with Compress-Archive, which writes Windows
# backslashes into the entry names. The ZIP spec requires forward slashes and
# the Chrome Web Store uploader rejects archives that use backslashes, so the
# separator is normalised explicitly here.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem $stage -Recurse -File | Sort-Object FullName) {
        $entryName = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $file.FullName, $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $archive.Dispose()
}

Write-Host ""
Write-Host "packaged version $($manifest.version)" -ForegroundColor Cyan
Get-ChildItem $stage -Recurse -File | ForEach-Object {
    "    " + $_.FullName.Substring($stage.Length + 1)
}
Write-Host ""
Write-Host "upload this file:" -ForegroundColor Green
Write-Host "    $zip"
Write-Host "    $([math]::Round((Get-Item $zip).Length / 1KB)) KB"
