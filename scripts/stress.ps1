<#
.SYNOPSIS
  Long stress-test runs (TODO 4.5; Phase 4 exit gate: 10 minutes clean, three times in a row).

.DESCRIPTION
  Configures and builds -Preset (default msvc-asan: AddressSanitizer and the debug assertions),
  then runs StressTest.WindowsFileSystem -Runs times, -Seconds each, one after the other, on real
  files in %TEMP%. Stops at the first failing run, since the gate asks for runs in a row. Each
  run's output goes to out\logs\stress-<n>.log; the build's to out\logs\stress-build.log.
  Exits with 1 if a run failed. (Every ctest run also runs the stress test, for 5 or 10 seconds.)

.EXAMPLE
  .\scripts\stress.ps1
.EXAMPLE
  .\scripts\stress.ps1 -Preset msvc-release -Seconds 60 -Runs 1
.EXAMPLE
  .\scripts\stress.ps1 -Seed 12345 -Runs 1
#>
[CmdletBinding()]
param(
    [string] $Preset = 'msvc-asan',
    [int] $Seconds = 600,
    [int] $Runs = 3,
    # Fixes the random choices of every run (thread interleavings still vary).
    [string] $Seed,
    [string] $VcpkgRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'lib\dev-env.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $repoRoot 'out\logs'
$exe = Join-Path $repoRoot "out\build\$Preset\tests\forgedb_stress_tests.exe"

# Native tools run through cmd.exe so their stderr never turns into PowerShell errors.
function Invoke-Logged([string] $CommandLine, [string] $Log) {
    Write-Host "==> $CommandLine" -ForegroundColor Cyan
    cmd.exe /c "$CommandLine 2>&1" | Tee-Object -FilePath $Log | Out-Host
    return $LASTEXITCODE
}

$results = New-Object System.Collections.Generic.List[object]
$savedSeconds = $env:FORGEDB_STRESS_SECONDS
$savedSeed = $env:FORGEDB_STRESS_SEED
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    Enter-X64DevShell
    Use-VisualStudioTools
    $null = Resolve-VcpkgRoot $VcpkgRoot

    $buildLog = Join-Path $logDir 'stress-build.log'
    foreach ($step in @("cmake --preset $Preset", "cmake --build --preset $Preset --target forgedb_stress_tests")) {
        if ((Invoke-Logged $step $buildLog) -ne 0) {
            throw "Failed: $step (see out\logs\stress-build.log)"
        }
    }
    if (-not (Test-Path $exe)) { throw "Not built: $exe" }

    $env:FORGEDB_STRESS_SECONDS = "$Seconds"
    if ($Seed) { $env:FORGEDB_STRESS_SEED = $Seed } else { Remove-Item Env:FORGEDB_STRESS_SEED -ErrorAction SilentlyContinue }
    for ($run = 1; $run -le $Runs; $run++) {
        $log = Join-Path $logDir "stress-$run.log"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $code = Invoke-Logged "`"$exe`" --gtest_filter=StressTest.WindowsFileSystem" $log
        $summary = (Select-String -Path $log -Pattern '^\[ stress   \] (seed .*)$' |
                    Select-Object -First 1 | ForEach-Object { $_.Matches[0].Groups[1].Value })
        $results.Add([pscustomobject]@{
            Run     = $run
            Result  = $(if ($code -eq 0) { 'PASS' } else { "FAIL ($code)" })
            Minutes = [math]::Round($timer.Elapsed.TotalMinutes, 1)
            Summary = $summary
        })
        if ($code -ne 0) { break }
    }
}
finally {
    $env:FORGEDB_STRESS_SECONDS = $savedSeconds
    $env:FORGEDB_STRESS_SEED = $savedSeed
    Pop-Location
}

Write-Host ''
$results | Format-Table -AutoSize -Wrap | Out-String -Width 250 | Write-Host
$failed = @($results | Where-Object { $_.Result -ne 'PASS' })
if ($failed.Count -gt 0 -or $results.Count -lt $Runs) {
    Write-Host "Stress test failed: see out\logs\stress-$($results.Count).log" -ForegroundColor Red
    exit 1
}
Write-Host "All $Runs stress runs passed ($Seconds s each)." -ForegroundColor Green
exit 0
