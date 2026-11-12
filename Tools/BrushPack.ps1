# Copyright (C) 2026 Joseph Wiginton.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sculpt Brush Pack. Builds the two plugins and runs their automation tests in a headless editor, on a stock Unreal
# Engine 5.8.
#
#   pwsh -File Tools/BrushPack.ps1                                   # build, then test, in this repository's host project
#   pwsh -File Tools/BrushPack.ps1 -EngineRoot D:/UE_5.8             # name the engine folder (the one that holds Engine/)
#   pwsh -File Tools/BrushPack.ps1 -Step Test -Filter SculptBrushPack.Brushes
#   pwsh -File Tools/BrushPack.ps1 -Project C:/Work/MyGame/MyGame.uproject   # a project that has the plugins in its Plugins folder
#
# The engine folder is taken from -EngineRoot, else from the environment variable UE_ROOT, else from the Epic Games
# launcher's list of installed engines (its entry UE_5.8).
#
# Close the Unreal editor first: UnrealBuildTool refuses to build while an editor with Live Coding is running.
#
# -Project must be a project with C++ targets: the editor target built is <project name>Editor. Without -Project the host
# project in Hosts/BrushPack58 is used, which finds the plugins in this repository's Plugins folder.
#
# Logs go under Build/Logs of this repository (ignored by git). The exit code is 0 only when every step that ran
# succeeded: the build without errors, and at least one test run with none failing.

param(
    [string]$EngineRoot,
    [string]$Project,
    [ValidateSet('Build', 'Test', 'All')][string]$Step = 'All',
    [string]$Filter = 'SculptBrushPack',
    [int]$TimeoutMinutes = 20
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $repoRoot 'Build/Logs'

function Resolve-EngineRoot {
    param([string]$Given)
    if ($Given) { return $Given }
    if ($env:UE_ROOT) { return $env:UE_ROOT }
    $launcherList = Join-Path $env:ProgramData 'Epic/UnrealEngineLauncher/LauncherInstalled.dat'
    if (Test-Path $launcherList) {
        $entry = (Get-Content -Raw $launcherList | ConvertFrom-Json).InstallationList | Where-Object { $_.AppName -eq 'UE_5.8' } | Select-Object -First 1
        if ($entry) { return $entry.InstallLocation }
    }
    throw 'Unreal Engine 5.8 was not found. Pass -EngineRoot <the folder that holds Engine/>, or set the environment variable UE_ROOT.'
}

$EngineRoot = Resolve-EngineRoot $EngineRoot
$versionFile = Join-Path $EngineRoot 'Engine/Build/Build.version'
if (-not (Test-Path $versionFile)) { throw "No Unreal Engine at '$EngineRoot': $versionFile is missing." }
$version = Get-Content -Raw $versionFile | ConvertFrom-Json
$engineName = "$($version.MajorVersion).$($version.MinorVersion).$($version.PatchVersion)"
if ($version.MajorVersion -ne 5 -or $version.MinorVersion -ne 8) {
    throw "The engine at '$EngineRoot' is $engineName. The plugins are written for Unreal Engine 5.8 and stop the build on any other version."
}

if (-not $Project) { $Project = Join-Path $repoRoot 'Hosts/BrushPack58/BrushPackHost58.uproject' }
if (-not (Test-Path $Project)) { throw "No project at '$Project'." }
$Project = (Resolve-Path $Project).Path
$editorTarget = [IO.Path]::GetFileNameWithoutExtension($Project) + 'Editor'

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
Write-Host "Engine: $engineName (changelist $($version.Changelist)) at $EngineRoot"
Write-Host "Project: $Project"

if ($Step -in @('Build', 'All')) {
    $stamp = Get-Date -Format 'yyyy-MM-dd_HHmmss'
    $log = Join-Path $logDir "build_$stamp.log"
    $started = Get-Date
    & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/Build.bat') $editorTarget Win64 Development "-Project=$Project" -WaitMutex -NoHotReload *>&1 | Tee-Object -FilePath $log | Out-Null
    $exit = $LASTEXITCODE
    $elapsed = [math]::Round(((Get-Date) - $started).TotalSeconds)

    $lines = @(Get-Content $log)
    $errors = @($lines | Where-Object { $_ -match ': (fatal )?error [A-Z]+\d+|error :|Unhandled exception|Result: Failed' })
    $warnings = @($lines | Where-Object { $_ -match ': warning [A-Z]+\d+' })
    Write-Host "Build ($editorTarget Win64 Development): exit $exit, $($errors.Count) errors, $($warnings.Count) warnings, $elapsed s"
    Write-Host "Log: $log"
    $errors | Select-Object -First 40 | ForEach-Object { Write-Host $_ }
    $warnings | Select-Object -First 20 | ForEach-Object { Write-Host $_ }
    if ($lines -match 'Unable to build while Live Coding is active') {
        Write-Host 'The Unreal editor is open. Have it closed by whoever is using it (do not end its process), then run this again.'
    }
    if ($exit -ne 0) { exit $exit }
}

if ($Step -in @('Test', 'All')) {
    $stamp = Get-Date -Format 'yyyy-MM-dd_HHmmss'
    $log = Join-Path $logDir "tests_$stamp.log"
    $reportDir = Join-Path $logDir "report_$stamp"
    $editorCmd = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
    $editorArgs = @(
        $Project,
        "-ExecCmds=Automation RunTests $Filter; Quit",
        '-TestExit=Automation Test Queue Empty',
        "-ReportExportPath=$reportDir",
        '-unattended', '-nopause', '-nosplash', '-nullrhi', '-nosound',
        "-abslog=$log", '-stdout', '-FullStdOutLogOutput'
    )
    $started = Get-Date
    $proc = Start-Process -FilePath $editorCmd -ArgumentList ($editorArgs | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) `
        -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $logDir "tests_$stamp.stdout.txt")
    if (-not $proc.WaitForExit($TimeoutMinutes * 60 * 1000)) {
        Stop-Process -Id $proc.Id -Force
        $exit = 'timeout'
    } else {
        $exit = $proc.ExitCode
    }
    $elapsed = [math]::Round(((Get-Date) - $started).TotalSeconds)

    $tests = @()
    $index = Join-Path $reportDir 'index.json'
    if (Test-Path $index) {
        $tests = @((Get-Content -Raw $index | ConvertFrom-Json).tests)
    }
    $passed = @($tests | Where-Object { $_.state -eq 'Success' }).Count
    $failed = $tests.Count - $passed
    Write-Host "Tests ($Filter): $passed passed, $failed failed, of $($tests.Count); editor exit $exit; $elapsed s"
    foreach ($test in $tests) {
        Write-Host ("  [{0}] {1}" -f $test.state, $test.fullTestPath)
        if ($test.state -ne 'Success') {
            $test.entries | Select-Object -First 12 | ForEach-Object { Write-Host "      $($_.event.type): $($_.event.message)" }
        }
    }
    Write-Host "Log: $log"
    if ($tests.Count -eq 0) { Write-Host 'No test ran. Read the log: the plugins may not have loaded.' }
    if ($tests.Count -eq 0 -or $failed -gt 0) { exit 1 }
}
exit 0
