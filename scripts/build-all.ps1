<#
.SYNOPSIS
  Configure, build and test ForgeDB with every CMake preset (or only the ones you name).

.DESCRIPTION
  - Sets up the x64 Visual Studio developer environment itself if needed.
  - Runs: cmake --preset P, cmake --build --preset P, ctest --preset P
    (the test step runs only for presets that have a test preset; clangcl-fuzz has none).
  - Keeps going after a failure unless -StopOnFailure is given.
  - Writes every step's full output to out\logs\<preset>-<step>.log.
  - Prints a summary table and exits with 1 if anything failed.

.EXAMPLE
  .\scripts\build-all.ps1

.EXAMPLE
  .\scripts\build-all.ps1 -Presets msvc-debug,clangcl-asan

.EXAMPLE
  .\scripts\build-all.ps1 -Clean -StopOnFailure
#>
[CmdletBinding()]
param(
    [string[]] $Presets = @('msvc-debug', 'msvc-release', 'msvc-asan', 'clangcl-debug', 'clangcl-asan', 'clangcl-fuzz'),
    [switch] $Clean,
    [switch] $StopOnFailure
)

Set-StrictMode -Version Latest
# Stop on PowerShell errors. Native tools (cmake, ctest) are judged by exit code;
# they run through cmd.exe so their stderr never turns into PowerShell errors.
$ErrorActionPreference = 'Stop'

# "powershell -File build-all.ps1 -Presets a,b" passes "a,b" as ONE string,
# so split on commas ourselves.
$Presets = @($Presets | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$repoRoot = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $repoRoot 'out\logs'

function Get-VsInstallPath {
    if ($env:VSINSTALLDIR -and (Test-Path -LiteralPath $env:VSINSTALLDIR)) { return $env:VSINSTALLDIR.TrimEnd('\') }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found. Is Visual Studio installed?' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio installation with the C++ x64 build tools was found.' }
    return $vs
}

function Enter-X64DevShell {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        Write-Host 'Already in an x64 developer environment.'
        return
    }
    if ($env:VSCMD_ARG_TGT_ARCH) {
        throw ("This shell is a developer shell for '$($env:VSCMD_ARG_TGT_ARCH)', not x64. " +
               'Open a new plain PowerShell window and run the script again; it sets up x64 itself.')
    }
    $vs = Get-VsInstallPath
    Write-Host "Entering x64 developer environment from: $vs"
    & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Host
}

# Puts Visual Studio's CMake, Ninja and clang-cl at the FRONT of PATH for this
# process. Other toolchains on PATH (for example Strawberry Perl, which ships
# its own cmake.exe, ninja.exe, gcc and ld.exe) can otherwise shadow them.
function Use-VisualStudioTools {
    $vs = Get-VsInstallPath
    $preferred = @(
        (Join-Path $vs 'VC\Tools\Llvm\x64\bin'),
        (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'),
        (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja')
    ) | Where-Object { Test-Path -LiteralPath $_ }
    $sep = [IO.Path]::PathSeparator
    $rest = $env:PATH -split [regex]::Escape($sep) | Where-Object { $_ -and ($preferred -notcontains $_.TrimEnd('\')) }
    $env:PATH = (@($preferred) + @($rest)) -join $sep

    Write-Host 'Tools used:'
    foreach ($tool in 'cmake', 'ninja', 'cl', 'clang-cl') {
        $cmd = Get-Command "$tool.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
        $where = 'NOT FOUND'
        if ($cmd) { $where = $cmd.Source }
        Write-Host ('  {0,-9} {1}' -f $tool, $where)
    }
    if (-not (Get-Command clang-cl.exe -ErrorAction SilentlyContinue)) {
        Write-Warning ("clang-cl not found, so the clangcl-* presets will fail. Install it with Visual Studio " +
                       "Installer > Modify > Individual components > 'C++ Clang Compiler for Windows'.")
    }
}

function Resolve-VcpkgRoot {
    # The user-level variable comes first: the developer shell may have pointed
    # VCPKG_ROOT at Visual Studio's bundled vcpkg for this session.
    $candidates = @(
        [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User'),
        $env:VCPKG_ROOT,
        'C:\vcpkg'
    )
    foreach ($candidate in $candidates) {
        # [IO.Path]::Combine, not Join-Path: Join-Path throws for a drive that doesn't exist.
        if ($candidate -and (Test-Path -LiteralPath ([IO.Path]::Combine($candidate, 'scripts', 'buildsystems', 'vcpkg.cmake')))) {
            $env:VCPKG_ROOT = $candidate
            return $candidate
        }
    }
    throw 'vcpkg not found. Clone it to C:\vcpkg, run bootstrap-vcpkg.bat and set VCPKG_ROOT (see README).'
}

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
    $vcpkgRoot = Resolve-VcpkgRoot
    Write-Host "Using VCPKG_ROOT = $vcpkgRoot"
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
            if ($step.Name -eq 'Test' -and ($testPresets -notcontains $preset)) {
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
