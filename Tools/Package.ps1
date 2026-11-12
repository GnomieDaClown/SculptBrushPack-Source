# Copyright (C) 2026 Joseph Wiginton.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sculpt Brush Pack. Makes the archive that is attached to a release: SculptBrushPack-<version>.zip, with the two plugin
# folders as git tracks them, the licence, the README and the instructions for coding agents, and, with -BinariesFrom,
# each plugin's prebuilt Win64 editor module, so that the plugins run without being compiled. The version is the
# VersionName of Plugins/SculptBrushPack/SculptBrushPack.uplugin.
#
#   pwsh -File Tools/Package.ps1 -BinariesFrom C:/Work/BuiltProject   # a release archive, with binaries
#   pwsh -File Tools/Package.ps1                                      # source only
#   pwsh -File Tools/Package.ps1 -OutDir C:/Temp
#
# -BinariesFrom names a project folder in which this same commit of the plugins was built in the project's Plugins folder
# (Tools/BrushPack.ps1 -Project does that). From each plugin's Binaries/Win64 there it takes the module's .dll and
# UnrealEditor.modules; debug symbols are left out. Binaries are never committed to the repository.
#
# It refuses to run while what it packages has uncommitted changes, so that an archive always matches a commit.

param(
    [string]$OutDir,
    [string]$BinariesFrom
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$packaged = @('Plugins', 'LICENSE', 'README.md', 'CLAUDE.md', 'AGENTS.md')

$descriptor = Get-Content -Raw (Join-Path $repoRoot 'Plugins/SculptBrushPack/SculptBrushPack.uplugin') | ConvertFrom-Json
$name = "SculptBrushPack-$($descriptor.VersionName)"
if (-not $OutDir) { $OutDir = Join-Path $repoRoot 'Build' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zipPath = Join-Path (Resolve-Path $OutDir).Path "$name.zip"

$files = @(& git -C $repoRoot ls-files -- $packaged)
if ($LASTEXITCODE -ne 0 -or $files.Count -eq 0) { throw 'git lists no files to package: run this in a clone of the repository.' }
$changed = @(& git -C $repoRoot status --porcelain -- $packaged)
if ($changed.Count -gt 0) { throw "What is packaged has uncommitted changes:`n$($changed -join "`n")" }
$commit = (& git -C $repoRoot rev-parse --short HEAD).Trim()

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path $zipPath) { Remove-Item $zipPath }
$binaryCount = 0
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in ($files | Sort-Object)) {
        # Entry names use forward slashes, under one top folder named after the archive.
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $repoRoot $file), "$name/$file", [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
    if ($BinariesFrom) {
        foreach ($plugin in @('SculptBrushPack', 'SculptBrushPackMorph')) {
            $binaries = Join-Path $BinariesFrom "Plugins/$plugin/Binaries/Win64"
            foreach ($binary in @("UnrealEditor-$plugin.dll", 'UnrealEditor.modules')) {
                $source = Join-Path $binaries $binary
                if (-not (Test-Path $source)) { throw "Missing ${source}: build the plugins in that project first." }
                [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $source, "$name/Plugins/$plugin/Binaries/Win64/$binary", [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
                $binaryCount++
            }
        }
    }
} finally {
    $zip.Dispose()
}

$hash = (Get-FileHash -Algorithm SHA256 $zipPath).Hash.ToLower()
Write-Host $zipPath
Write-Host "$($files.Count) files from commit $commit and $binaryCount prebuilt binaries, $((Get-Item $zipPath).Length) bytes, sha256 $hash"
