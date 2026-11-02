# Benchmarks

ForgeDB against RocksDB, LevelDB and SQLite on one Windows machine, with the same workloads, the
same data and the same settings where the engines have them. Everything here comes from
`scripts\bench.ps1` and `bench\plot.py`; the numbers are measured, never estimated.

**Results:** not yet. They go here after the first full run, with the charts, the machine record
and a section on where ForgeDB loses and why (the Phase 5 exit gate).

## Running them

Once per machine, in an administrator PowerShell:

```powershell
# Defender scans every file a program writes; the benchmark writes gigabytes.
# The folder is bench.ps1's -DbFolder: by default out\bench-db in the repository.
Add-MpPreference -ExclusionPath <repository>\out\bench-db
# The High performance plan (on laptops that hide it, use Settings > System > Power >
# Power mode: Best performance instead).
powercfg /setactive SCHEME_MIN
```

Remove the exclusion afterwards with `Remove-MpPreference -ExclusionPath <folder>` if you like. Then, in a normal PowerShell, with
the laptop plugged in and other heavy programs (browsers, IDE indexing, sync clients, updates)
closed:

```powershell
pip install matplotlib        # once, for the charts
powershell -ExecutionPolicy Bypass -File scripts\bench.ps1
```

The script builds `forgedb_bench` with the `msvc-bench` preset (Release; the first build compiles
RocksDB through vcpkg, about 15 minutes), checks the power plan, AC power and the Defender
exclusion (it warns, it never changes them), records the machine, runs everything, and draws the
charts. The results go to `out\bench\<date-time>\`: `machine.json`, `unsynced.json`,
`synced.json`, their console logs, `*.png` and `results.md`. Expect one to two hours with the
defaults; `-Runs 1 -Num 100000` gives a quick look, and `python bench\plot.py <folder>` redraws the
charts.

## Method

- **Runs.** Every engine and workload runs 5 times (`-Runs`), each time on a new database, and
  the charts and tables show the median with the lowest and highest run. One run is not a result.
- **Two modes.** *Unsynced*: every workload, 1,000,000 keys; a write returns once the engine has
  it in its log, which survives a process crash but not a power loss. *Synced*: the four write
  workloads, 50,000 keys; every write waits until its log is on the disk. Reads do not depend on
  the mode, so the synced runs skip them.
- **Data.** 16-byte keys (zero-padded numbers: key 2*i* exists, 2*i* + 1 does not), 100-byte
  values that no engine could compress (none does). 1,000,000 keys are about 116 MB of keys and
  values.
- **Workloads** (`tools/bench/bench_options.h`). `fillseq` and `fillrandom` write every key once
  into an empty database, in order or shuffled. Every other workload first fills a database with
  `fillrandom` (not measured) and waits for its background work to finish, then measures:
  `overwrite` (random writes to existing keys), `readrandom` (random existing keys),
  `readmissing` (random absent keys: the Bloom filter's case), `readseq` (one full scan),
  `seekrandom` (seek to a random key, read 10 entries) and `mixed` (90% reads, 10% writes).
  Every read checks its answer; a wrong one fails the run.
- **Measured.** Operations per second; latency percentiles from a histogram that reads at most
  6.25% high; write amplification = bytes the process wrote to disk (Windows I/O counters, logs
  included) from the start of the workload until the background work settles ÷ bytes of keys and
  values written; space amplification = size of the database's files ÷ live keys and values.
- **One thread** by default (`forgedb_bench --threads=N` for more).

### The same settings for every engine

Each adapter (`tools/bench/*_engine.h`) maps the shared settings to its engine's options and
leaves everything else at the engine's defaults.

| Setting | ForgeDB | RocksDB | LevelDB | SQLite |
|---|---|---|---|---|
| Block cache, 8 MiB | block cache | LRU block cache | LRU block cache | page cache (per connection) |
| Write buffer, 4 MiB | memtable | write buffer | write buffer | log checkpointed at 4 MiB |
| Bloom filters, 10 bits per key | yes | yes | yes | none (a B-tree has no filters) |
| Compression | none (has none) | off | off | none (has none) |
| Sync mode | `WriteOptions::sync` | `WriteOptions::sync` | `WriteOptions::sync` | `synchronous = FULL`; unsynced `NORMAL` |
| Waiting for background work | its own wait | `WaitForCompact` | polls its table list (see below) | a log checkpoint |

Defaults that differ and matter:

- **RocksDB** sizes its levels from the bottom up (`level_compaction_dynamic_level_bytes`) with
  a 256 MiB level 1 and 64 MiB files, and runs two background jobs. ForgeDB and LevelDB use a
  10 MiB level 1, 2 MiB files and one background thread.
- **LevelDB** has no call that waits for its compactions, so its adapter waits until LevelDB's
  compaction scores are all below 1 and its table list has not changed for 1 second. That second
  is in LevelDB's settle time, not in its throughput.
- **SQLite** is a B-tree in one file (`WITHOUT ROWID` table, WAL journal mode, 4 KiB pages), one
  transaction per write. Its cache belongs to a connection, and each thread has its own
  connection, so with N threads it has N caches.

## Caveats

- **The data fits in memory.** 116 MB of keys and values fit in the operating system's file
  cache, and every engine reads through it, so the read workloads measure each engine's code
  (lookups, filters, caching) more than the disk. A larger data set would show disk behavior
  and is not part of these runs.
- **Builds.** vcpkg builds RocksDB with `PORTABLE=1` (no instructions beyond the baseline x86-64
  ones at compile time) and LevelDB with its `crc32c` feature (hardware CRC-32C, as ForgeDB
  uses). All four engines are built by the same compiler with the build's release flags; the
  exact versions and flags are in each run's `machine.json`.
- **One machine, one disk.** The numbers say how these engines compare on that machine. Laptop
  CPUs change speed with temperature and power state, which the 5 runs and the min–max ranges
  show but do not remove.
- **Defaults are not tuning.** Each engine runs with its defaults apart from the shared settings;
  any of them could be tuned for a given workload, ForgeDB included.
