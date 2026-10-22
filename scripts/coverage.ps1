<#
.SYNOPSIS
  Measure line coverage of src/ and include/ with OpenCppCoverage and enforce a minimum.

.DESCRIPTION
  Runs the already-built test suite of one preset (default msvc-debug) under
  OpenCppCoverage, following ctest's child processes. Only files under src/
  and include/ count; tests, vcpkg packages and generated files don't.
  Writes out\coverage\<preset>\coverage.xml (Cobertura) and an HTML report in
  out\coverage\<preset>\html\index.html, prints per-file line coverage, and
  exits with 1 if the tests fail or line coverage is below -MinLinePercent.

  Install OpenCppCoverage first:  choco install opencppcoverage
  (or the installer from https://github.com/OpenCppCoverage/OpenCppCoverage/releases).

.EXAMPLE
  .\scripts\build-all.ps1 -Presets msvc-debug
  .\scripts\coverage.ps1 -MinLinePercent 85
#>
[CmdletBinding()]
param(
    [string] $Preset = 'msvc-debug',
    [double] $MinLinePercent = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'lib\dev-env.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "out\build\$Preset"
$outDir = Join-Path $repoRoot "out\coverage\$Preset"

if (-not (Test-Path (Join-Path $buildDir 'CTestTestfile.cmake'))) {
    throw "No test build in $buildDir. Build it first: .\scripts\build-all.ps1 -Presets $Preset"
}

$occ = Get-Command OpenCppCoverage.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($occ) {
    $occ = $occ.Source
} else {
    $occ = Join-Path $env:ProgramFiles 'OpenCppCoverage\OpenCppCoverage.exe'
    if (-not (Test-Path $occ)) { throw 'OpenCppCoverage not found. Install it: choco install opencppcoverage' }
}

Enter-X64DevShell
Use-VisualStudioTools
$ctest = (Get-Command ctest.exe).Source

if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$xmlPath = Join-Path $outDir 'coverage.xml'

$occArgs = @(
    '--sources', (Join-Path $repoRoot 'src'),
    '--sources', (Join-Path $repoRoot 'include'),
    '--cover_children',
    '--export_type', "cobertura:$xmlPath",
    '--export_type', "html:$(Join-Path $outDir 'html')",
    '--', $ctest, '--test-dir', $buildDir, '--output-on-failure', '--no-tests=error'
)
Write-Host "==> $occ $($occArgs -join ' ')" -ForegroundColor Cyan
& $occ @occArgs | Out-Host
$testExit = $LASTEXITCODE

if (-not (Test-Path $xmlPath)) { throw "OpenCppCoverage did not write $xmlPath (exit code $testExit)." }

# Cobertura: <coverage line-rate="0.93"> with <class filename=... line-rate=...> per file.
[xml] $report = Get-Content -Raw -LiteralPath $xmlPath
$inv = [Globalization.CultureInfo]::InvariantCulture
$total = [double]::Parse($report.coverage.'line-rate', $inv) * 100

$rows = foreach ($class in $report.SelectNodes('//class')) {
    [pscustomobject]@{
        File = $class.filename
        Lines = @($class.SelectNodes('lines/line')).Count
        Covered = @($class.SelectNodes('lines/line[@hits!="0"]')).Count
        Percent = [math]::Round([double]::Parse($class.'line-rate', $inv) * 100, 1)
    }
}
$rows | Sort-Object File | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
Write-Host ('Line coverage: {0:N1}%  (minimum {1:N1}%)' -f $total, $MinLinePercent)
Write-Host "HTML report:   $(Join-Path $outDir 'html\index.html')"

if ($testExit -ne 0) {
    Write-Host "Tests failed under coverage (exit code $testExit)." -ForegroundColor Red
    exit 1
}
if ($total -lt $MinLinePercent) {
    Write-Host ('Coverage {0:N1}% is below the {1:N1}% minimum.' -f $total, $MinLinePercent) -ForegroundColor Red
    exit 1
}
exit 0
