# Benchmarks

ForgeDB against RocksDB, LevelDB and SQLite on one Windows machine, with the same workloads, the
same data and the same settings where the engines have them. Everything here comes from
`scripts\bench.ps1` and `bench\plot.py`; the numbers are measured, never estimated.

## Results: unsynced writes (2026-11-05)

**Run.** The final code (commit 9560246; the engine as of Phase 5.3's commit e016e15), set up as
the [method](#method) asks: High performance power plan, AC power, the database folder excluded
from Defender's scanning (confirmed), runs interleaved across engines. It replaces the first run
of 2026-11-02 (commit 314d43e, before Phase 5.3), whose tables are in this file's history.

**Machine.** Dell G5 5500 laptop: Intel Core i7-10750H (6 cores, 12 threads), 15.8 GB RAM at
2933 MHz, SK hynix PC611 1 TB NVMe SSD; Windows 11 Home 25H2 (build 26200.9711). MSVC 19.51
(Visual Studio 18), Release (`/O2 /Ob2`); RocksDB 11.8.1, LevelDB (vcpkg 2026-03-11), SQLite
3.53.4.

1,000,000 keys of 16 B with 100-byte values, one thread, 8 MiB cache, 4 MiB write buffer, 10-bit
Bloom filters, no compression, unsynced. Median [lowest–highest] of 5 runs.

| ops/s | ForgeDB | RocksDB | LevelDB | SQLite |
|---|---|---|---|---|
| fillseq | 180K [177K–183K] | 128K [122K–130K] | 193K [189K–198K] | 27.3K [23.6K–27.8K] |
| fillrandom | 155K [145K–158K] | 110K [108K–111K] | 163K [159K–168K] | 6,078 [5,949–6,149] |
| overwrite | 154K [151K–155K] | 105K [102K–106K] | 155K [153K–159K] | 8,743 [8,534–8,950] |
| readrandom | 188K [182K–192K] | 70.2K [68.2K–71.5K] | 351K [333K–372K] | 64.8K [58.7K–66.9K] |
| readmissing | 436K [311K–469K] | 573K [424K–604K] | 481K [416K–498K] | 65.7K [64.1K–69.5K] |
| readseq | 2.55M [2.43M–2.66M] | 2.74M [2.7M–2.82M] | 4.06M [4.02M–4.11M] | 2.35M [2.23M–2.47M] |
| seekrandom | 59.8K [58.2K–60.9K] | 36.8K [36K–37.6K] | 142K [120K–147K] | 49.2K [48.1K–51K] |
| mixed | 181K [176K–194K] | 70.2K [69.1K–71.6K] | 251K [237K–253K] | 40.6K [40.3K–40.9K] |

| p99 latency, µs | ForgeDB | RocksDB | LevelDB | SQLite |
|---|---|---|---|---|
| fillrandom | 20.5 | 18.4 | 20.5 | 156 |
| readrandom | 9.73 | 34.8 | 6.66 | 34.8 |
| readmissing | 7.17 | 13.8 | 6.14 | 34.8 |
| seekrandom | 29.7 | 59.4 | 12.3 | 43 |
| mixed | 12.8 | 34.8 | 17.4 | 45.1 |

| write amplification | ForgeDB | RocksDB | LevelDB | SQLite |
|---|---|---|---|---|
| fillseq | 2.18 | 2.19 | 2.18 | 54.3 |
| fillrandom | 6.04 [6.02–6.12] | 6.43 | 6.32 [6.30–6.34] | 115 |
| overwrite | 8.61 [8.51–8.64] | 11.2 | 8.03 [8.02–8.12] | 70.3 |
| mixed | 10.5 [10.1–10.5] | 12.1 | 44.7 [44.5–44.9] | 70.3 |

| space amplification | ForgeDB | RocksDB | LevelDB | SQLite |
|---|---|---|---|---|
| fillseq | 1.04 | 0.998 | 0.992 | 1.22 |
| fillrandom | 1.13 [1.11–1.15] | 0.999 | 0.993 | 1.21 |
| overwrite | 1.32 [1.32–1.39] | 1.00 | 1.15 [1.14–1.15] | 1.21 |

![Throughput, unsynced](benchmarks/throughput-unsynced.png)
![p99 latency, unsynced](benchmarks/latency-p99-unsynced.png)
![Write amplification, unsynced](benchmarks/write-amp-unsynced.png)
![Space amplification, unsynced](benchmarks/space-amp-unsynced.png)

### Where ForgeDB wins

- **Writes, against RocksDB and SQLite.** 40–47% more writes per second than RocksDB (fillrandom
  155K vs 110K, overwrite 154K vs 105K), and the lowest write amplification in fillrandom (6.04).
  RocksDB's adapter keeps its statistics switched on (for the counters the benchmark reports),
  which costs RocksDB some of that gap; how much was not measured.
- **Reads, against RocksDB and SQLite.** Point reads 2.7 times RocksDB's (188K vs 70.2K ops/s,
  p99 9.7 µs vs 34.8 µs) and 2.9 times SQLite's; seeks second of the four (59.8K, against 49.2K
  for SQLite and 36.8K for RocksDB); mixed 181K against 70.2K and 40.6K.
- **Sequential writes are written once.** fillseq write amplification 2.18, as LevelDB's and
  RocksDB's (it was 3.09 before Phase 5.3).

### Where ForgeDB loses, and why

- **Point reads and seeks: LevelDB is 1.9 and 2.4 times as fast** (readrandom 351K vs 188K
  ops/s, typical latency 2.6 µs vs 5.1 µs; seekrandom 142K vs 59.8K), and mixed follows (251K vs
  181K). Both engines now read table blocks from memory-mapped files (Phase 5.3 halved ForgeDB's
  gap). Two differences remain that are known, not measured: ForgeDB copies each block out of the
  mapping and keeps it in its block cache, which keeps a disk error an error instead of a crash;
  LevelDB uses the mapped bytes in place and does not put those blocks in its block cache. And LevelDB keeps compacting while it
  only reads (below), which may leave fewer tables for a read to look in. Not profiled further.
- **Scans: LevelDB is 1.6 times as fast, RocksDB 7%** (readseq 4.06M and 2.74M entries/s vs
  2.55M). Not profiled.
- **Absent keys: RocksDB is 1.3 times as fast** (readmissing 573K vs 436K ops/s; typical latency
  1.3 µs vs 2.0 µs). These are answered by the Bloom filters without reading a block; RocksDB's
  filter is a different design from ForgeDB's (and LevelDB's). Not investigated.
- **Writes: LevelDB is 5–7% faster** in the fills (fillseq 193K vs 180K, fillrandom 163K vs 155K;
  overwrite is even). Not investigated.
- **Space: 13% over the data after fillrandom** (1.13, against 0.99–1.00 for LevelDB and RocksDB),
  and 32% after overwrite. fillrandom writes every key once, so this is not old versions of keys.
  One lead: ForgeDB keeps the log its last flush retired until the next MANIFEST record (ADR 0007),
  up to one write buffer's worth (4 MiB, about 4% of this data), where LevelDB deletes it after
  the flush. The rest is not explained.

### The other engines

- **LevelDB compacts while it only reads.** During readrandom, with no writes at all, LevelDB's
  compactions wrote 133 MB: its seek-triggered compactions, which compact a table that reads keep
  having to look into before finding their key in a deeper level. The same explains its mixed
  write amplification of 44.7. This work runs on another thread, outside the measured operations.
- **SQLite** is a B-tree with one transaction per write here, so every write rewrites at least
  one 4 KiB page into its log: write amplification 54–115, and 6–27K writes per second. Its
  scans are fast (2.35M entries/s): a B-tree keeps each key once, in order.

## Results: synced writes (2026-11-05)

The same run: the four write workloads, 20,000 keys, every write synced; 5 runs each.

| ops/s | ForgeDB | RocksDB | LevelDB | SQLite |
|---|---|---|---|---|
| fillseq, median [min–max] | 1,693 [835–1,758] | 1,685 [1,129–1,716] | 715 [485–1,677] | 1,269 [531–1,590] |
| fillrandom | 1,675 [1,650–1,739] | 1,671 [1,189–1,681] | 734 [671–897] | 1,506 [1,086–1,557] |
| overwrite | 806 [712–1,718] | 723 [642–798] | 1,633 [816–1,701] | 1,573 [1,560–1,610] |
| mixed (10% synced writes) | 16.7K [3,957–17.6K] | 11.3K [4,571–16.6K] | 16.0K [4,103–16.5K] | 15.7K [15.3K–15.9K] |

![Throughput, synced](benchmarks/throughput-synced.png)

**What these numbers measure: mostly the SSD.** A synced write waits for `FlushFileBuffers`, and
on this machine that wait comes in two sizes: about 0.5–0.6 ms, or about 2–3 ms. A run whose
writes mostly wait the short time does about 1,600–1,750 writes per second, whatever the engine;
a run with many long waits, 500–900. The medians therefore mostly say which runs the long waits
fell in. Where they fall is not plain noise, though: in both interleaved runs (2026-11-02 and
this one) they hit LevelDB's fills in 9 of 10 runs, RocksDB's overwrites in every run, ForgeDB's
overwrites in 3 and 4 of 5 and SQLite's overwrites in 2 and 0 of 5, with other engines' runs
fast in between. In the first run, which ran each engine's runs back to back, it was nearly the other way
round. The cause is not known. One hypothesis for overwrite, where the LSM engines flush their
write buffer partway through (40,000 writes in all) and SQLite has nothing to flush: a flush's
table write and sync slow the log syncs beside it. Finding out needs a disk-level trace (ETW),
not done.

**What does hold.** When the flushes are fast, every engine is held to the drive's flush time:
the best fillseq runs are within 10% of each other (ForgeDB 1,758 writes/s, RocksDB 1,716,
LevelDB 1,677, SQLite 1,590). Write amplification is 1.19 for the three LSM engines in the fills
(just the log; 20,000 keys never fill a write buffer) and 54–78 for SQLite.

**Space after synced overwrite: ForgeDB 3.78, LevelDB 2.11, RocksDB 1.64.** At this size one flush
has happened, and ForgeDB keeps the log that flush retired as well as the current one (the lead
for the space amplification above).

## Phase 5.3: two optimizations

Profiled with `scripts\cpu-profile.ps1` (Visual Studio's CPU sampling, Release build with
symbols), then measured with `bench.ps1 -Modes unsynced -Engines forgedb`: ForgeDB alone,
1,000,000 keys, 5 runs, median. **Before:** commit 45b186f, 2026-11-03 (High performance plan;
Defender exclusion not confirmed). **After:** commit e016e15, 2026-11-04 (Balanced plan; the
exclusion confirmed). Absolute numbers move between days on this laptop: the final run
(2026-11-05, High performance) measured ForgeDB 6–18% above the "after" run even where neither
change applies (readmissing 436K vs 370K). The comparison below rests on the workloads neither
change touches staying within 2% between these two runs (readmissing, fillrandom, overwrite).

| ForgeDB, unsynced | before | after | change |
|---|---|---|---|
| readrandom, ops/s (p50 latency) | 89.6K (10.8 µs) | 137.5K (6.9 µs) | +53% |
| seekrandom, ops/s (p50 latency) | 28.6K (34.8 µs) | 53.9K (18.4 µs) | +89% |
| readseq, entries/s | 1.94M | 2.43M | +25% |
| mixed (90% reads), ops/s | 97.5K | 155.7K | +60% |
| readmissing, ops/s | 369K | 370K | none (no block is read) |
| fillseq, ops/s | 157K | 164K | +4% |
| fillseq, write amplification | 3.09 | **2.18** | tables written: 220 MB → 115 MB |
| fillrandom, overwrite, ops/s | 147K, 139K | 145K, 136K | −1%, −2% |
| fillrandom, overwrite, write amplification | 6.04, 8.61 | 6.04, 8.61 | none |

### 1. Table blocks read from a memory mapping

**Profile before.** In seekrandom (with its fillrandom setup), 61% of the samples were in the
Windows kernel, and `Table::ReadDataBlock` took 54.7% of the time, nearly all of it in the
`ReadFile` call under it. In readrandom, half of `DBImpl::Get`'s time (26% of all samples) was
in `ReadFile`.

**Change.** A table file is mapped read-only into memory when it is opened, and a read copies
the requested bytes from the mapping instead of calling `ReadFile` (`src/env/win_file_system.cpp`;
ADR 0002, rule 12). Copying (rather than handing out pointers into the mapping, as LevelDB does)
keeps a disk error an `IOError`: the copy catches the in-page error Windows raises, and nothing
above `src/env` changed.

**After.** The deep kernel stacks under `ReadDataBlock` are gone from both profiles; what remains
in the kernel is the log's `WriteFile` under `Put` in the setup fill. Point reads +53%, seeks
+89%, scans +25%. Absent keys, which the Bloom filters answer without reading a block, did not
move: the gain is where the reads were.

![readrandom, before](benchmarks/readrandom-flame-before.png)
![readrandom, after](benchmarks/readrandom-flame-after.png)
![seekrandom, before](benchmarks/seekrandom-flame-before.png)
![seekrandom, after](benchmarks/seekrandom-flame-after.png)

### 2. Level-0 tables that overlap nothing move down whole

**Before.** A level-0 compaction rewrote every level-0 table, even when the tables overlapped
neither each other nor level 1, as sequential writes make them: fillseq wrote 220 MB of tables
(35 flushes and 59 compactions) for 116 MB of data.

**Change.** Such tables now move to level 1 unchanged, as a single table already did from deeper
levels (`Compaction::IsTrivialMove`; ADR 0008, rule 2).

**After.** fillseq writes only its flushes (115 MB of tables; the 37 compactions are all moves),
write amplification 2.18, the same as LevelDB's 2.18 in the first run. Throughput +4%. Random
writes are unchanged, to the byte: their tables overlap, so they never qualified.

Both changes keep the full test suite green in every preset (2026-11-03, commit e016e15: 567–570
tests per preset, including new tests for each change).

### Still behind

In the final run, ForgeDB's point reads (188K ops/s) and seeks (59.8K) are second only to
LevelDB's (351K and 142K); see [where ForgeDB loses](#where-forgedb-loses-and-why).

## Running them

Once per machine, in an administrator PowerShell (run `bench.ps1` from one too: reading
Defender's exclusions needs an administrator, so otherwise the script cannot confirm the
exclusion):

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
  The runs are interleaved (run 1 of every engine, then run 2 of every engine, ...), so a slow
  spell of the machine falls on every engine alike: the first full run, with each engine's runs
  back to back, saw synced writes take either about 0.55 ms or 1.5–2 ms, a state that lasted
  across several runs.
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
