# Shared helpers for ForgeDB's PowerShell scripts. Dot-source it:
#   . (Join-Path $PSScriptRoot 'lib\dev-env.ps1')
# Works in Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII-only:
# Windows PowerShell 5.1 reads BOM-less files in the system code page.

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

# Puts Visual Studio's clang tools, CMake and Ninja at the FRONT of PATH for
# this process. Other toolchains on PATH (for example Strawberry Perl, which
# ships its own cmake.exe, ninja.exe, gcc and ld.exe) can otherwise shadow them.
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

# Picks the vcpkg checkout to use and exports it as VCPKG_ROOT.
# Order: explicit -Override, the user-level VCPKG_ROOT, this session's
# VCPKG_ROOT, C:\vcpkg. The user-level variable beats the session one because
# the developer shell may point VCPKG_ROOT at Visual Studio's bundled vcpkg.
function Resolve-VcpkgRoot([string] $Override) {
    $candidates = @(
        $Override,
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
