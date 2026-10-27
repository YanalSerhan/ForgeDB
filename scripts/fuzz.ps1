<#
.SYNOPSIS
  Long fuzzing runs (TODO 2.7; Phase 2 exit gate: 1 hour per target, no crashes, no ASan reports).

.DESCRIPTION
  Configures and builds the clangcl-fuzz preset, writes the seed corpus, then runs each fuzz
  target for -Seconds, one after the other. Inputs the fuzzer finds interesting are kept in
  out\fuzz\<target>-work and reused by later runs; crashing inputs go to out\fuzz\artifacts\.
  Each target's output goes to out\logs\fuzz-<target>.log. Exits with 1 if any target failed.
  (CI's smoke run is `ctest --preset clangcl-fuzz`, 60 seconds per target, via build-all.ps1.)

.EXAMPLE
  .\scripts\fuzz.ps1
.EXAMPLE
  .\scripts\fuzz.ps1 -Seconds 600 -Targets log_reader
#>
[CmdletBinding()]
param(
    [int] $Seconds = 3600,
    [string[]] $Targets = @('log_reader', 'write_batch'),
    [string] $VcpkgRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'lib\dev-env.ps1')

$Targets = @($Targets | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$repoRoot = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $repoRoot 'out\logs'
$corpus = Join-Path $repoRoot 'out\fuzz'
$bin = Join-Path $repoRoot 'out\build\clangcl-fuzz\fuzz'

# Native tools run through cmd.exe so their stderr never turns into PowerShell errors.
function Invoke-Logged([string] $CommandLine, [string] $Log) {
    Write-Host "==> $CommandLine" -ForegroundColor Cyan
    cmd.exe /c "$CommandLine 2>&1" | Tee-Object -FilePath $Log | Out-Host
    return $LASTEXITCODE
}

$results = New-Object System.Collections.Generic.List[object]
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    Enter-X64DevShell
    Use-VisualStudioTools
    $null = Resolve-VcpkgRoot $VcpkgRoot

    foreach ($step in @('cmake --preset clangcl-fuzz', 'cmake --build --preset clangcl-fuzz')) {
        if ((Invoke-Logged $step (Join-Path $logDir 'fuzz-build.log')) -ne 0) {
            throw "Failed: $step (see out\logs\fuzz-build.log)"
        }
    }
    $seeds = Join-Path $bin 'forgedb_fuzz_seeds.exe'
    if ((Invoke-Logged "`"$seeds`" `"$corpus`"" (Join-Path $logDir 'fuzz-seeds.log')) -ne 0) {
        throw 'Writing the seed corpus failed (see out\logs\fuzz-seeds.log)'
    }

    foreach ($target in $Targets) {
        $exe = Join-Path $bin "forgedb_fuzz_$target.exe"
        if (-not (Test-Path $exe)) { throw "No fuzz target named '$target' ($exe)" }
        $log = Join-Path $logDir "fuzz-$target.log"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        # The prefix ends in '/', not '\': a backslash right before a closing quote
        # escapes the quote, and the rest of the command line becomes one argument.
        $command = "`"$exe`" -max_total_time=$Seconds -print_final_stats=1 " +
                   "`"-artifact_prefix=$corpus\artifacts/`" `"$corpus\$target-work`" `"$corpus\$target`""
        $code = Invoke-Logged $command $log
        $runs = (Select-String -Path $log -Pattern 'stat::number_of_executed_units:\s+(\d+)' |
                 Select-Object -Last 1 | ForEach-Object { $_.Matches[0].Groups[1].Value })
        $results.Add([pscustomobject]@{
            Target  = $target
            Result  = $(if ($code -eq 0) { 'PASS' } else { "FAIL ($code)" })
            Inputs  = $runs
            Seconds = [int]$timer.Elapsed.TotalSeconds
        })
    }
}
finally {
    Pop-Location
}

Write-Host ''
$results | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
$failed = @($results | Where-Object { $_.Result -ne 'PASS' })
if ($failed.Count -gt 0) {
    Write-Host "Crashing inputs (if any): $corpus\artifacts" -ForegroundColor Red
    exit 1
}
Write-Host 'All fuzz targets ran without a crash.' -ForegroundColor Green
exit 0
