<#
.SYNOPSIS
  Configure, build and test ForgeDB with every CMake preset (or only the ones you name).

.DESCRIPTION
  - Sets up the x64 Visual Studio developer environment itself if needed.
  - Runs: cmake --preset P, cmake --build --preset P, ctest --preset P
    (the test step runs only for presets that have a test preset: clangcl-tidy,
    msvc-analyze and clangcl-fuzz are build-only).
  - Keeps going after a failure unless -StopOnFailure is given.
  - Writes every step's full output to out\logs\<preset>-<step>.log.
  - Prints a summary table and exits with 1 if anything failed.
  CI runs this same script, one preset per job.

.EXAMPLE
  .\scripts\build-all.ps1

.EXAMPLE
  .\scripts\build-all.ps1 -Presets msvc-debug,clangcl-asan

.EXAMPLE
  .\scripts\build-all.ps1 -Clean -StopOnFailure
#>
[CmdletBinding()]
param(
    [string[]] $Presets = @('msvc-debug', 'msvc-release', 'msvc-asan', 'clangcl-debug', 'clangcl-asan',
                           'clangcl-tidy', 'msvc-analyze', 'clangcl-fuzz'),
    [switch] $Clean,
    [switch] $StopOnFailure,
    # Configure and build only (used by the CI coverage job, which runs the tests itself).
    [switch] $SkipTests,
    # Use this vcpkg checkout instead of searching for one (used by CI).
    [string] $VcpkgRoot
)

Set-StrictMode -Version Latest
# Stop on PowerShell errors. Native tools (cmake, ctest) are judged by exit code;
# they run through cmd.exe so their stderr never turns into PowerShell errors.
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'lib\dev-env.ps1')

# "powershell -File build-all.ps1 -Presets a,b" passes "a,b" as ONE string,
# so split on commas ourselves.
$Presets = @($Presets | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$repoRoot = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $repoRoot 'out\logs'

function Get-TestPresetNames {
    $lines = cmd.exe /c 'ctest --list-presets 2>nul'
    $names = @()
    foreach ($line in $lines) {
        if ($line -match '^\s+"([^"]+)"') { $names += $Matches[1] }
    }
    return $names
}

# Runs one command, streams its output to the console and a log file, and
# returns the exit code.
function Invoke-Step([string] $Preset, [string] $Step, [string] $CommandLine) {
    $log = Join-Path $logDir "$Preset-$Step.log"
    Write-Host ''
    Write-Host "==> [$Preset] $Step : $CommandLine" -ForegroundColor Cyan
    cmd.exe /c "$CommandLine 2>&1" | Tee-Object -FilePath $log | Out-Host
    return $LASTEXITCODE
}

$results = New-Object System.Collections.Generic.List[object]

Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    Enter-X64DevShell
    Use-VisualStudioTools
    $resolvedVcpkg = Resolve-VcpkgRoot $VcpkgRoot
    Write-Host "Using VCPKG_ROOT = $resolvedVcpkg"
    $testPresets = Get-TestPresetNames

    foreach ($preset in $Presets) {
        $row = [ordered]@{ Preset = $preset; Configure = '-'; Build = '-'; Test = '-'; Seconds = 0 }
        $timer = [Diagnostics.Stopwatch]::StartNew()

        if ($Clean) {
            $buildDir = Join-Path $repoRoot "out\build\$preset"
            if (Test-Path $buildDir) {
                Write-Host "Removing $buildDir"
                Remove-Item -Recurse -Force $buildDir
            }
        }

        $steps = @(
            @{ Name = 'Configure'; Command = "cmake --preset $preset" },
            @{ Name = 'Build';     Command = "cmake --build --preset $preset" },
            @{ Name = 'Test';      Command = "ctest --preset $preset" }
        )
        $ok = $true
        foreach ($step in $steps) {
            if ($step.Name -eq 'Test' -and ($SkipTests -or ($testPresets -notcontains $preset))) {
                $row.Test = 'n/a'
                continue
            }
            if (-not $ok) {
                $row[$step.Name] = 'skipped'
                continue
            }
            $code = Invoke-Step $preset $step.Name $step.Command
            if ($code -eq 0) {
                $row[$step.Name] = 'PASS'
            } else {
                $row[$step.Name] = "FAIL ($code)"
                $ok = $false
            }
        }

        $row.Seconds = [int]$timer.Elapsed.TotalSeconds
        $results.Add([pscustomobject]$row)
        if (-not $ok -and $StopOnFailure) {
            Write-Host "Stopping after failure in $preset (-StopOnFailure)." -ForegroundColor Yellow
            break
        }
    }
}
finally {
    Pop-Location
}

Write-Host ''
# Explicit width: when output is redirected (CI, a log file) there is no console
# width and Format-Table would otherwise print nothing.
$results | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
Write-Host "Full logs: $logDir"

$failed = @($results | Where-Object {
    $_.Configure -like 'FAIL*' -or $_.Build -like 'FAIL*' -or $_.Test -like 'FAIL*'
})
if ($failed.Count -gt 0) {
    Write-Host "$($failed.Count) preset(s) failed: $(($failed | ForEach-Object { $_.Preset }) -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host 'All presets passed.' -ForegroundColor Green
exit 0
