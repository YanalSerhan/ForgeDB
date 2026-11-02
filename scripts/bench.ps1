<#
.SYNOPSIS
  The benchmark runs for docs/benchmarks.md (TODO 5.2): ForgeDB, RocksDB, LevelDB and SQLite.

.DESCRIPTION
  1. Builds forgedb_bench with -Preset (default msvc-bench: Release, all four engines).
  2. Checks the machine and warns (it never stops and never changes a setting):
     the power plan is not High performance or Ultimate Performance; the laptop is on
     battery; Defender's real-time scanning is on and the database folder (-DbFolder) is not
     excluded, or the exclusion cannot be read (that needs an administrator).
     docs/benchmarks.md says how to set each one.
  3. Records the machine in machine.json: CPU, RAM, the disk holding -DbFolder, Windows
     version and build, power plan, AC or battery, Defender, compiler and flags, the
     vcpkg versions of the other engines, the commit.
  4. Runs forgedb_bench -Runs times per engine and workload, each run on a fresh
     database, in two modes:
       unsynced: every workload, -Num keys (default 1,000,000)
       synced:   the write workloads only (-SyncWorkloads), -SyncNum keys (default
                 50,000): a synced write waits for the disk, and reads do not
                 depend on the mode
     Results: unsynced.json and synced.json, with the console output in
     unsynced.log and synced.log.
  5. If Python with matplotlib is there, runs bench\plot.py on the results folder:
     charts (PNG) and results.md, with every number as median [min-max].

  Everything goes to -Out (default out\bench\<date-time>). Expect one to two hours with
  the defaults, most of it SQLite's one-transaction-per-write fills; -Runs, -Num,
  -Engines and -Workloads make a shorter run.

.EXAMPLE
  .\scripts\bench.ps1
.EXAMPLE
  .\scripts\bench.ps1 -Runs 1 -Num 100000 -Modes unsynced
.EXAMPLE
  .\scripts\bench.ps1 -Engines forgedb,rocksdb -Workloads fillrandom,readrandom
#>
[CmdletBinding()]
param(
    [string] $Preset = 'msvc-bench',
    [string[]] $Engines = @('forgedb', 'rocksdb', 'leveldb', 'sqlite'),
    [string[]] $Workloads = @('fillseq', 'fillrandom', 'overwrite', 'readrandom', 'readmissing',
                              'readseq', 'seekrandom', 'mixed'),
    [string[]] $SyncWorkloads = @('fillseq', 'fillrandom', 'overwrite', 'mixed'),
    [ValidateSet('both', 'unsynced', 'synced')]
    [string] $Modes = 'both',
    [long] $Num = 1000000,
    [long] $SyncNum = 50000,
    [int] $Runs = 5,
    # Where the databases go: exclude this folder from Defender's real-time scanning.
    # (Not -Db: [CmdletBinding()] gives -Debug the alias -db.)
    [string] $DbFolder,
    # Where the results go (default out\bench\<date-time>).
    [string] $Out,
    [string] $Python = 'python',
    [switch] $SkipBuild,
    [switch] $NoPlot,
    [string] $VcpkgRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'lib\dev-env.ps1')

# "powershell -File bench.ps1 -Engines a,b" passes "a,b" as ONE string.
function Split-List([string[]] $Items) {
    return @($Items | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}
$Engines = Split-List $Engines
$Workloads = Split-List $Workloads
$SyncWorkloads = Split-List $SyncWorkloads

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $DbFolder) { $DbFolder = Join-Path $repoRoot 'out\bench-db' }
if (-not $Out) { $Out = Join-Path $repoRoot ('out\bench\' + (Get-Date -Format 'yyyyMMdd-HHmm')) }
$DbFolder = [IO.Path]::GetFullPath($DbFolder)
$Out = [IO.Path]::GetFullPath($Out)
$buildDir = Join-Path $repoRoot "out\build\$Preset"
$exe = Join-Path $buildDir 'tools\forgedb_bench.exe'

# The build steps run through cmd.exe, as in the other scripts, so their stderr
# never turns into PowerShell errors (their command lines have no quotes).
function Invoke-Logged([string] $CommandLine, [string] $Log, [switch] $Append) {
    Write-Host "==> $CommandLine" -ForegroundColor Cyan
    cmd.exe /c "$CommandLine 2>&1" | Tee-Object -FilePath $Log -Append:$Append | Out-Host
    return $LASTEXITCODE
}

# Runs a program with arguments that may hold paths with spaces (PowerShell quotes
# each one; cmd.exe would mangle several quoted arguments). Its stderr is logged as
# text, not raised as an error.
function Invoke-Program([string] $Log, [string] $Program, [string[]] $Arguments) {
    Write-Host "==> $Program $($Arguments -join ' ')" -ForegroundColor Cyan
    $ErrorActionPreference = 'Continue'  # this function's scope only
    & $Program @Arguments 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $Log | Out-Host
    return $LASTEXITCODE
}

$warnings = New-Object System.Collections.Generic.List[string]
function Add-Warning([string] $Message) {
    $warnings.Add($Message)
    Write-Host "WARNING: $Message" -ForegroundColor Yellow
}

# Runs a script block and returns its value, or $Fallback if it throws or gives
# nothing (machine details are best effort: a missing one is recorded, not fatal).
function Get-OrDefault([scriptblock] $Block, $Fallback = 'unknown') {
    try {
        $value = & $Block
        if ($null -eq $value) { return $Fallback }
        return $value
    } catch {
        return $Fallback
    }
}

# --- The machine -------------------------------------------------------------------------

function Get-PowerInfo {
    $line = (powercfg /getactivescheme) -join ' '
    $guid = if ($line -match '([0-9a-fA-F-]{36})') { $Matches[1].ToLower() } else { 'unknown' }
    $name = if ($line -match '\(([^)]+)\)\s*$') { $Matches[1] } else { 'unknown' }
    # Windows 11's power mode slider (Settings > System > Power) is an overlay on the plan.
    $overlay = Get-OrDefault {
        (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes').ActiveOverlayAcPowerScheme
    } 'none'
    $battery = @(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue)
    # BatteryStatus 2: on AC power (Win32_Battery).
    $onAc = if ($battery.Count -eq 0) { 'no battery' } elseif ($battery[0].BatteryStatus -eq 2) { 'yes' } else { 'no' }
    return [ordered]@{ plan = $name; plan_guid = $guid; power_mode_overlay = $overlay; on_ac_power = $onAc }
}

# Whether $Folder is $Root or inside it.
function Test-Inside([string] $Folder, [string] $Root) {
    $root = ([IO.Path]::GetFullPath($Root)).TrimEnd('\')
    return $Folder.Equals($root, 'OrdinalIgnoreCase') -or
           $Folder.StartsWith($root + '\', 'OrdinalIgnoreCase')
}

function Get-DefenderInfo([string] $Folder) {
    $realtime = Get-OrDefault { (Get-MpComputerStatus).RealTimeProtectionEnabled }
    $excluded = Get-OrDefault {
        $paths = @((Get-MpPreference).ExclusionPath | Where-Object { $_ })
        if ($paths.Count -gt 0 -and "$($paths[0])" -like 'N/A*') {
            'unknown (reading exclusions needs an administrator)'
        } elseif (@($paths | Where-Object { Test-Inside $Folder $_ }).Count -gt 0) {
            'yes'
        } else {
            'no'
        }
    }
    return [ordered]@{ realtime_protection = $realtime; db_folder_excluded = $excluded }
}

function Get-DiskInfo([string] $Folder) {
    return Get-OrDefault {
        $letter = (Split-Path -Qualifier $Folder).TrimEnd(':')
        $disk = Get-Partition -DriveLetter $letter | Get-Disk
        $physical = Get-PhysicalDisk | Where-Object { $_.DeviceId -eq "$($disk.Number)" } | Select-Object -First 1
        [ordered]@{
            drive      = "${letter}:"
            model      = $disk.FriendlyName
            bus        = "$($disk.BusType)"
            media      = $(if ($physical) { "$($physical.MediaType)" } else { 'unknown' })
            size_gb    = [math]::Round($disk.Size / 1GB)
            free_gb    = [math]::Round((Get-PSDrive $letter).Free / 1GB)
        }
    }
}

# The compiler (from the file CMake writes when it identifies it) and the flags
# (from the build's CMake cache).
function Get-CompilerInfo {
    $info = [ordered]@{}
    $identified = Get-ChildItem (Join-Path $buildDir 'CMakeFiles') -Filter 'CMakeCXXCompiler.cmake' -Recurse -Depth 1 `
                      -ErrorAction SilentlyContinue | Select-Object -First 1
    foreach ($name in 'CMAKE_CXX_COMPILER_ID', 'CMAKE_CXX_COMPILER_VERSION', 'CMAKE_CXX_COMPILER') {
        $line = if ($identified) {
            Select-String -Path $identified.FullName -Pattern "^set\(${name} `"(.*)`"\)$" | Select-Object -First 1
        }
        $info[$name] = if ($line) { $line.Matches[0].Groups[1].Value } else { 'unknown' }
    }
    $cache = Join-Path $buildDir 'CMakeCache.txt'
    foreach ($name in 'CMAKE_BUILD_TYPE', 'CMAKE_CXX_FLAGS', 'CMAKE_CXX_FLAGS_RELEASE') {
        $line = Select-String -Path $cache -Pattern "^${name}:[A-Z]+=(.*)$" -ErrorAction SilentlyContinue |
                Select-Object -First 1
        $info[$name] = if ($line) { $line.Matches[0].Groups[1].Value } else { 'unknown' }
    }
    $info['forgedb_flags'] = '/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor (cmake/ForgeDBCompilerOptions.cmake)'
    return $info
}

# The other engines' versions, from vcpkg's record of what it installed for this build.
function Get-EngineVersions {
    $status = Join-Path $buildDir 'vcpkg_installed\vcpkg\status'
    $versions = [ordered]@{}
    if (-not (Test-Path $status)) { return $versions }
    $package = $null
    foreach ($line in Get-Content $status) {
        if ($line -match '^Package: (.+)$') { $package = $Matches[1] }
        elseif ($package -and $line -match '^Version: (.+)$' -and
                @('rocksdb', 'leveldb', 'sqlite3', 'crc32c') -contains $package) {
            $versions[$package] = $Matches[1]
        }
        elseif ($line -eq '') { $package = $null }
    }
    return $versions
}

function Get-MachineRecord {
    $cpu = Get-OrDefault { Get-CimInstance Win32_Processor | Select-Object -First 1 } $null
    $os = Get-OrDefault { Get-CimInstance Win32_OperatingSystem } $null
    $nt = Get-OrDefault { Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' } $null
    $memory = @(Get-CimInstance Win32_PhysicalMemory -ErrorAction SilentlyContinue)
    $commit = Get-OrDefault { (git -C $repoRoot rev-parse HEAD).Trim() }
    $dirty = Get-OrDefault { if (git -C $repoRoot --no-optional-locks status --porcelain) { 'yes' } else { 'no' } }
    return [ordered]@{
        date_utc        = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm')
        computer_model  = Get-OrDefault { $cs = Get-CimInstance Win32_ComputerSystem; "$($cs.Manufacturer) $($cs.Model)" }
        cpu             = $(if ($cpu) { $cpu.Name.Trim() } else { 'unknown' })
        cpu_cores       = $(if ($cpu) { $cpu.NumberOfCores } else { 'unknown' })
        cpu_threads     = $(if ($cpu) { $cpu.NumberOfLogicalProcessors } else { 'unknown' })
        cpu_base_mhz    = $(if ($cpu) { $cpu.MaxClockSpeed } else { 'unknown' })
        ram_gb          = Get-OrDefault { [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1) }
        ram_mhz         = $(if ($memory.Count -gt 0) { $memory[0].ConfiguredClockSpeed } else { 'unknown' })
        disk            = Get-DiskInfo $DbFolder
        # The caption can carry invisible direction marks (U+200E, U+200F).
        windows         = $(if ($os) { ($os.Caption -replace '[\u200E\u200F]', '').Trim() } else { 'unknown' })
        windows_version = $(if ($nt) { "$($nt.DisplayVersion), build $($nt.CurrentBuild).$($nt.UBR)" } else { 'unknown' })
        power           = Get-PowerInfo
        defender        = Get-DefenderInfo $DbFolder
        compiler        = Get-CompilerInfo
        engines         = Get-EngineVersions
        commit          = $commit
        uncommitted     = $dirty
        db_folder       = $DbFolder
        preset          = $Preset
    }
}

# --- The runs ----------------------------------------------------------------------------

$timer = [Diagnostics.Stopwatch]::StartNew()
$summary = New-Object System.Collections.Generic.List[object]
$failed = $false
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force -Path $Out | Out-Null
    New-Item -ItemType Directory -Force -Path $DbFolder | Out-Null
    Enter-X64DevShell
    Use-VisualStudioTools
    $null = Resolve-VcpkgRoot $VcpkgRoot

    if (-not $SkipBuild) {
        $buildLog = Join-Path $Out 'build.log'
        foreach ($step in @("cmake --preset $Preset", "cmake --build --preset $Preset --target forgedb_bench")) {
            if ((Invoke-Logged $step $buildLog -Append) -ne 0) {
                throw "Failed: $step (see $buildLog)"
            }
        }
    }
    if (-not (Test-Path $exe)) { throw "Not built: $exe" }

    $machine = Get-MachineRecord
    $machine | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $Out 'machine.json') -Encoding UTF8
    if ($machine.power.plan -notmatch 'High performance|Ultimate Performance') {
        Add-Warning "power plan is '$($machine.power.plan)', not High performance (docs/benchmarks.md)"
    }
    if ($machine.power.on_ac_power -eq 'no') {
        Add-Warning 'running on battery: plug the laptop in'
    }
    if ($machine.defender.realtime_protection -ne $false -and $machine.defender.db_folder_excluded -ne 'yes') {
        Add-Warning "Defender real-time scanning may scan $DbFolder (excluded: $($machine.defender.db_folder_excluded)); see docs/benchmarks.md"
    }

    $plan = @()
    if ($Modes -ne 'synced') {
        $plan += @{ Name = 'unsynced'; Sync = 0; Num = $Num; Workloads = $Workloads }
    }
    if ($Modes -ne 'unsynced') {
        $plan += @{ Name = 'synced'; Sync = 1; Num = $SyncNum; Workloads = $SyncWorkloads }
    }
    foreach ($mode in $plan) {
        $json = Join-Path $Out "$($mode.Name).json"
        $log = Join-Path $Out "$($mode.Name).log"
        $modeDb = Join-Path $DbFolder $mode.Name
        $arguments = @(
            "--engine=$($Engines -join ',')",
            "--workloads=$($mode.Workloads -join ',')",
            "--num=$($mode.Num)",
            "--sync=$($mode.Sync)",
            "--runs=$Runs",
            "--db=$modeDb",
            "--json=$json"
        )
        $modeTimer = [Diagnostics.Stopwatch]::StartNew()
        $code = Invoke-Program $log $exe $arguments
        $summary.Add([pscustomobject]@{
            Mode    = $mode.Name
            Result  = $(if ($code -eq 0) { 'PASS' } else { "FAIL ($code)" })
            Minutes = [math]::Round($modeTimer.Elapsed.TotalMinutes, 1)
            Output  = $json
        })
        if ($code -ne 0) { $failed = $true }
    }

    if (-not $NoPlot -and -not $failed) {
        $plot = Join-Path $repoRoot 'bench\plot.py'
        $hasMatplotlib = $false
        if (Get-Command $Python -ErrorAction SilentlyContinue) {
            $null = Invoke-Program (Join-Path $Out 'plot.log') $Python @('-c', 'import matplotlib')
            $hasMatplotlib = $LASTEXITCODE -eq 0
        }
        if ($hasMatplotlib) {
            if ((Invoke-Program (Join-Path $Out 'plot.log') $Python @($plot, $Out)) -ne 0) {
                Add-Warning "bench\plot.py failed (see $Out\plot.log)"
            }
        } else {
            Add-Warning "no Python with matplotlib ('$Python'): no charts. Later: pip install matplotlib, then python bench\plot.py `"$Out`""
        }
    }
}
finally {
    Pop-Location
}

Write-Host ''
$summary | Format-Table -AutoSize -Wrap | Out-String -Width 250 | Write-Host
Write-Host "Results: $Out ($([math]::Round($timer.Elapsed.TotalMinutes, 1)) minutes in all)"
foreach ($message in $warnings) {
    Write-Host "WARNING: $message" -ForegroundColor Yellow
}
if ($failed) {
    Write-Host 'A benchmark run failed: see its .log file.' -ForegroundColor Red
    exit 1
}
exit 0
