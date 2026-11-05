# Changelog

## v1.0.0 (2026-11-05)

The first release: a complete LSM-tree storage engine for Windows 10/11 x64, built in six phases
([TODO.md](TODO.md) has every step and how each was measured). Not for production use.

### The engine

- **API** (`include/forgedb/`): `DB::Open`, `Put`, `Delete`, `Write` of an atomic `WriteBatch`,
  `Get`, forward iterators (`SeekToFirst`, `Seek`, `Next`), snapshots, `GetStats`. Errors come
  back as `Status` or `Result<T>`.
- **Write path:** a checksummed write-ahead log, group commit (one log record and one sync for a
  whole group of writers), a skip-list memtable with lock-free reads.
- **Tables:** sorted table files with prefix-compressed blocks, a 10-bit-per-key Bloom filter, an
  index, and a CRC-32C on every block; a block cache (8 MiB, LRU) and a table cache; table files
  read through memory mappings.
- **Background thread:** flushes memtables to level 0 and runs leveled compaction through levels
  1 to 6 (10 MiB at level 1, ten times more per level); tables that overlap nothing below move
  down without being rewritten; writes slow down at 8 level-0 tables and wait at 12.
- **Versions and recovery:** a MANIFEST of synced edits; at open, the newest MANIFEST with a
  complete first record is used (CURRENT is only a hint), and the logs it does not cover are
  replayed. A torn write at the end of the newest log is dropped and reported in `RecoveryInfo`.
- **Threading:** one DB mutex, never held during I/O, checked at compile time by Clang's
  thread-safety analysis in the clang-cl builds.
- **Tools:** `forgedb_cli` (put, get, delete, scan, stats), `forgedb_sst_dump` (prints and checks a
  table), `forgedb_bench` (ForgeDB against RocksDB, LevelDB and SQLite).
- **The only code that calls Win32** is `src/env/`; the core library has no third-party
  dependencies.

### How it was tested

- 10,002 simulated crashes in each of 6 variants with no synced write lost (Phase 2), then 30,000
  per variant in 11 variants with compaction running (330,000 in all, Phase 4), on a
  fault-injection file system that models what Windows documents about crashes and power loss.
- 500 real process kills with `TerminateProcess`: 100,218 acknowledged keys checked, none lost.
- 3 seeds × 1,000,000 random operations with reopens, flushes and compactions agree with
  `std::map`, snapshots included.
- A corruption suite (bit flips, zeroed runs, truncations in tables, logs and MANIFESTs): every
  read gives the right answer or a `Corruption` status, never a wrong one.
- Six fuzz targets on untrusted bytes; three 10-minute multi-threaded stress runs under ASan,
  clean.
- CI on every push: eight build presets (MSVC and clang-cl) with warnings as errors, the tests in
  debug, release and both ASan builds, clang-tidy, `/analyze`, a fuzz smoke test, line coverage
  of at least 85%, and a clang-format check.

### Performance

On one laptop (Intel Core i7-10750H, NVMe SSD, Windows 11), 1,000,000 keys, unsynced, median of 5
runs ([docs/benchmarks.md](docs/benchmarks.md)): 155K random writes/s (LevelDB 163K, RocksDB 110K,
SQLite 6.1K), 188K point reads/s (LevelDB 351K, RocksDB 70K, SQLite 65K), write amplification 6.04
for random writes (the lowest of the four). Behind LevelDB on reads, seeks and scans; the
benchmark page says why.

### Known limitations

See the README's [Known limitations](README.md#known-limitations): one process per database, no
transactions beyond an atomic `WriteBatch`, power-loss safety verified only by simulation,
snapshots not saved across a close, Windows x64 only, benchmarks from one machine.

### File formats

Log, table (format version 1), MANIFEST and CURRENT as described in
[docs/file-formats.md](docs/file-formats.md).
