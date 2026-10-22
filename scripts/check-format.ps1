<#
.SYNOPSIS
  Check (or with -Fix, apply) clang-format on every tracked C++ file.

.DESCRIPTION
  Uses Visual Studio's bundled clang-format, so the result matches CI. Exits
  with 1 if any file is not formatted. Run with -Fix to reformat in place.

.EXAMPLE
  .\scripts\check-format.ps1
  .\scripts\check-format.ps1 -Fix
#>
[CmdletBinding()]
param([switch] $Fix)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'lib\dev-env.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
$clangFormat = Join-Path (Get-VsInstallPath) 'VC\Tools\Llvm\x64\bin\clang-format.exe'
if (-not (Test-Path $clangFormat)) {
    throw "clang-format not found at $clangFormat. Install Visual Studio's 'C++ Clang Compiler for Windows' component."
}
& $clangFormat --version | Out-Host

Push-Location $repoRoot
try {
    $files = @(git ls-files -- '*.cpp' '*.cc' '*.h' '*.hpp')
    if ($LASTEXITCODE -ne 0) { throw 'git ls-files failed.' }
    Write-Host "$($files.Count) files"
    if ($files.Count -eq 0) { exit 0 }

    if ($Fix) {
        & $clangFormat -i --style=file @files
    } else {
        & $clangFormat --dry-run --Werror --style=file @files
    }
    $code = $LASTEXITCODE
}
finally {
    Pop-Location
}

if ($code -ne 0) {
    Write-Host 'Formatting differences found. Run .\scripts\check-format.ps1 -Fix and commit the result.' -ForegroundColor Red
    exit 1
}
Write-Host 'Formatting OK.' -ForegroundColor Green
exit 0
