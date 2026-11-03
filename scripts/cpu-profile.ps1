<#
.SYNOPSIS
  A CPU profile of one forgedb_bench workload on ForgeDB, for TODO 5.3.

.DESCRIPTION
  1. Builds forgedb_bench with -Preset (default msvc-release; Release builds carry
     debug symbols, so the profile names ForgeDB's functions).
  2. Starts forgedb_bench --engine=forgedb with -Workload, -Num keys and one run, and
     attaches Visual Studio's command-line profiler (VSDiagnostics.exe, CPU sampling)
     to it right away. Workloads other than the fills first fill the database with
     fillrandom (not measured by forgedb_bench, but in the profile), so a readrandom
     profile shows the random writes and the random reads both, under Put and Get.
  3. Writes out\profile\<workload>-<date-time>.diagsession (open it in Visual Studio:
     File > Open > File) and the benchmark's output next to it (.log).

  No administrator rights needed. docs/benchmarks.md has the machine setup; the
  same applies here (power plan, Defender exclusion of -DbFolder).

.EXAMPLE
  .\scripts\cpu-profile.ps1
.EXAMPLE
  .\scripts\cpu-profile.ps1 -Workload seekrandom -Num 500000
#>
[CmdletBinding()]
param(
    [ValidateSet('fillseq', 'fillrandom', 'overwrite', 'readrandom', 'readmissing', 'readseq',
                 'seekrandom', 'mixed')]
    [string] $Workload = 'readrandom',
    [long] $Num = 1000000,
    [string] $Preset = 'msvc-release',
    # Sampling rate: Visual Studio documents Low as 100 and High as 4,000 samples per
    # second; Base lies between.
    [ValidateSet('Low', 'Base', 'High')]
    [string] $Rate = 'Base',
    # Where the database goes (default out\bench-db, as bench.ps1).
    [string] $DbFolder,
    [switch] $SkipBuild,
    [string] $VcpkgRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'lib\dev-env.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $DbFolder) { $DbFolder = Join-Path $repoRoot 'out\bench-db' }
$DbFolder = [IO.Path]::GetFullPath($DbFolder)
$outDir = Join-Path $repoRoot 'out\profile'
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$session = Join-Path $outDir "$Workload-$stamp.diagsession"
$benchLog = Join-Path $outDir "$Workload-$stamp.log"
$exe = Join-Path $repoRoot "out\build\$Preset\tools\forgedb_bench.exe"

function Invoke-Logged([string] $CommandLine, [string] $Log, [switch] $Append) {
    Write-Host "==> $CommandLine" -ForegroundColor Cyan
    cmd.exe /c "$CommandLine 2>&1" | Tee-Object -FilePath $Log -Append:$Append | Out-Host
    return $LASTEXITCODE
}

# Runs VSDiagnostics.exe; its stderr is shown as text, not raised as an error.
function Invoke-Collector([string] $Collector, [string[]] $Arguments) {
    Write-Host "==> VSDiagnostics.exe $($Arguments -join ' ')" -ForegroundColor Cyan
    $ErrorActionPreference = 'Continue'  # this function's scope only
    & $Collector @Arguments 2>&1 | ForEach-Object { "$_" } | Out-Host
    return $LASTEXITCODE
}

Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    New-Item -ItemType Directory -Force -Path $DbFolder | Out-Null
    Enter-X64DevShell
    Use-VisualStudioTools
    $null = Resolve-VcpkgRoot $VcpkgRoot

    if (-not $SkipBuild) {
        $buildLog = Join-Path $outDir "$Workload-$stamp-build.log"
        foreach ($step in @("cmake --preset $Preset", "cmake --build --preset $Preset --target forgedb_bench")) {
            if ((Invoke-Logged $step $buildLog -Append) -ne 0) {
                throw "Failed: $step (see $buildLog)"
            }
        }
    }
    if (-not (Test-Path $exe)) { throw "Not built: $exe" }
    if (-not (Test-Path ([IO.Path]::ChangeExtension($exe, '.pdb')))) {
        Write-Warning "No forgedb_bench.pdb next to the exe: the profile will show addresses, not function names. Rebuild."
    }

    $collectorDir = Join-Path (Get-VsInstallPath) 'Team Tools\DiagnosticsHub\Collector'
    $collector = Join-Path $collectorDir 'VSDiagnostics.exe'
    $config = Join-Path $collectorDir "AgentConfigs\CPUUsage$Rate.json"
    foreach ($path in $collector, $config) {
        if (-not (Test-Path $path)) {
            throw ("Not found: $path. The profiler comes with Visual Studio's 'Desktop development with C++' " +
                   'workload; check it in the Visual Studio Installer.')
        }
    }

    # One fresh database for the run (forgedb_bench removes it afterwards).
    $arguments = @('--engine=forgedb', "--workloads=$Workload", "--num=$Num", '--runs=1',
                   "`"--db=$DbFolder`"")
    Write-Host "==> $exe $($arguments -join ' ')" -ForegroundColor Cyan
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -PassThru `
                   -RedirectStandardOutput $benchLog -RedirectStandardError "$benchLog.err"
    $null = $process.Handle  # keeps the exit code readable after the process ends

    $id = Get-Random -Minimum 1 -Maximum 256
    $code = Invoke-Collector $collector @('start', "$id", "/attach:$($process.Id)", "/loadConfig:$config")
    if ($code -ne 0) {
        $process.Kill()
        throw "VSDiagnostics.exe could not attach (exit code $code)"
    }
    $process.WaitForExit()
    $code = Invoke-Collector $collector @('stop', "$id", "/output:$session")
    if ($code -ne 0) { throw "VSDiagnostics.exe could not save the session (exit code $code)" }

    Get-Content $benchLog | Out-Host
    if ($process.ExitCode -ne 0) {
        Get-Content "$benchLog.err" -ErrorAction SilentlyContinue | Out-Host
        throw "forgedb_bench failed (exit code $($process.ExitCode))"
    }
    Remove-Item "$benchLog.err" -ErrorAction SilentlyContinue
}
finally {
    Pop-Location
}

Write-Host ''
Write-Host "Profile: $session" -ForegroundColor Green
Write-Host 'Open it in Visual Studio (File > Open > File), then the CPU Usage tool''s Flame Graph and Functions views.'
exit 0
