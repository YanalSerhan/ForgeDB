# ForgeDB

[![CI](https://github.com/YanalSerhan/ForgeDB/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/YanalSerhan/ForgeDB/actions/workflows/ci.yml)
<!-- Placeholders until Phase 1 (coverage gate) and Phase 2 (fuzz smoke job). -->
![coverage](https://img.shields.io/badge/coverage-pending-lightgrey)
![fuzz](https://img.shields.io/badge/fuzz-pending-lightgrey)

ForgeDB is a crash-safe, embedded key-value storage engine based on a log-structured merge tree
(LSM tree), written in C++20 for Windows 10/11 x64 on NTFS. Writes go to a checksummed write-ahead
log with group commit, live in a lock-free-read skip list memtable, and are flushed to immutable
sorted table files that are compacted in the background; a MANIFEST tracks every live file so the
database recovers to a consistent state after a crash. The core library has no third-party
dependencies. **Status: Phase 0 (foundations), not usable yet.** See [TODO.md](TODO.md) for the plan.

## Building on Windows

### Prerequisites (one time)

1. **Visual Studio 2022 (17.5 or newer) or later**, with the *Desktop development with C++*
   workload and these individual components:
   - *C++ CMake tools for Windows* (provides CMake 3.25+ and Ninja)
   - *C++ Clang Compiler for Windows* (provides `clang-cl`; needed for the `clangcl-*` presets)
2. **vcpkg** (dependencies for tests and benchmarks only), in a normal PowerShell:

   ```powershell
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   [Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\vcpkg", "User")
   ```

### Every session: open an x64 developer shell

The default *Developer PowerShell for VS* targets **x86**. ForgeDB is x64-only, so start the
shell for x64 explicitly:

```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$env:VCPKG_ROOT = "C:\vcpkg"   # the developer shell may point this at Visual Studio's own vcpkg
```

### Configure, build, test

```powershell
cd C:\path\to\ForgeDB
cmake --preset msvc-debug        # first run also builds GoogleTest through vcpkg (a few minutes)
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

| Preset          | Compiler | Build type     | Notes                                   |
|-----------------|----------|----------------|-----------------------------------------|
| `msvc-debug`    | MSVC     | Debug          | assertions on                           |
| `msvc-release`  | MSVC     | Release        | also builds microbenchmarks             |
| `msvc-asan`     | MSVC     | RelWithDebInfo | AddressSanitizer, assertions on         |
| `clangcl-debug` | clang-cl | Debug          | assertions on                           |
| `clangcl-asan`  | clang-cl | RelWithDebInfo | AddressSanitizer, assertions on         |
| `clangcl-fuzz`  | clang-cl | RelWithDebInfo | libFuzzer + ASan; library and fuzz targets only |

Build output goes to `out/build/<preset>/`.

To configure, build and test **every** preset in one go (it sets up the x64 developer environment
itself, prints a summary table and writes logs to `out\logs\`):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-all.ps1
powershell -ExecutionPolicy Bypass -File scripts\build-all.ps1 -Presets msvc-debug,clangcl-asan -Clean
```

Other scripts (same environment setup, usable locally and in CI):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\check-format.ps1        # add -Fix to reformat
powershell -ExecutionPolicy Bypass -File scripts\coverage.ps1            # after building msvc-debug;
                                                                         # needs: choco install opencppcoverage
```

## Continuous integration

GitHub Actions on `windows-2025-vs2026` (`.github/workflows/ci.yml`): one job per preset running
`scripts\build-all.ps1`, a coverage job (OpenCppCoverage, report uploaded as an artifact), and a
clang-format check. vcpkg packages are cached between runs. ASan builds include a canary test
that overflows a heap buffer on purpose and passes only if ASan reports it.

**Troubleshooting.** Other toolchains on `PATH` can shadow Visual Studio's tools; Strawberry Perl,
for example, ships its own `cmake.exe`, `ninja.exe`, `gcc` and `ld.exe`. `build-all.ps1` puts
Visual Studio's CMake, Ninja and clang-cl first. In a manual shell, check with
`where.exe cmake ninja clang-cl`, and delete `out\build\<preset>` after fixing `PATH`, because
CMake caches the tool paths it found.
