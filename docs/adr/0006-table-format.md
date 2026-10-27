# ADR 0006: Table file format

- **Status:** Accepted
- **Date:** 2026-10-27

## Context

Phase 3 stores flushed memtables in immutable sorted files (tables, `.sst`). The format is
permanent once tables exist on disk, so its choices are recorded here; the byte layout is in
`docs/file-formats.md`, "Table file", "Block", "Block trailer" and "Bloom filter". The bloom
filter's own design is ADR 0005.

Constraints: every on-disk structure is checksummed and length-checked before it is trusted
(TODO ground rules); every parser treats file bytes as hostile; a lookup should cost at most one
data block read; the core library has no dependencies (so no compression library for now).

LevelDB's table format
([table_format.md](https://github.com/google/leveldb/blob/main/doc/table_format.md)) is the
model: data blocks, a filter, an index of block handles, a fixed footer with a magic number.

## Decision

1. **Blocks** are prefix-compressed with restart points (16 entries apart in data blocks) and end
   with a 5-byte trailer: a type byte (`0` = uncompressed, the only type) and a masked CRC-32C of
   contents + type. Every block read from the file is checked; there is no option to skip it.
2. **Data blocks are cut at 4 KiB** of contents (`TableOptions::block_size`): the first entry that
   takes a block to 4096 bytes or more ends it. 4 KiB is the NTFS cluster and x64 page size, and
   LevelDB's default; one entry is never split, so large values make large blocks.
3. **The index holds each data block's last internal key** and its handle, with restart interval
   1 (every index key is stored whole, so the binary search lands directly on entries). The first
   index entry at or after a lookup key names the only block that can contain it, so `Get` reads
   exactly one data block.
4. **One bloom filter per table**, over user keys (ADR 0005), stored as a block.
5. **A fixed 48-byte footer** at the end of the file: filter and index handles as four fixed64
   fields, a format version (1), a masked CRC-32C of those 36 bytes, and the magic `ForgeSST`.
   Open checks the magic, then the CRC, then the version (unknown: `NotSupported`), then that both
   handles lie inside the file in order. The index and filter are then read, checked and kept in
   memory while the table is open.
6. **Every handle is checked against the file size before it is used or allocated for**: index
   and filter handles against the footer, data block handles against the start of the filter (or
   index). A damaged handle cannot make the reader allocate more than the file holds.
7. **Errors stop reads**: a bad data block makes `Get` return `Corruption`, and an iterator stops
   at the first bad block with `Corruption` instead of skipping it (skipping would silently hide
   data). Iterators are forward-only, like `forgedb::Iterator`.
8. **An empty table is valid** (no data blocks, an empty index). Flush never writes one, but
   reading one is not an error.

## Alternatives considered

- **No checksum on the footer (LevelDB).** The magic number catches "not a table" and truncation
  at the end, but not a flipped bit in a handle, which would then send the reader to the wrong
  bytes; their block CRC would probably catch it, but "probably" breaks the ground rule. Rejected:
  4 bytes is cheap.
- **Varint handles in the footer, padded (LevelDB).** Saves nothing (the footer is fixed size
  anyway) and needs a padding rule. Rejected for four fixed64 fields.
- **Shortened separator index keys (LevelDB's `FindShortestSeparator`).** A key between the last
  key of one block and the first of the next, as short as possible, makes the index smaller. It
  is an optimization that changes no reader logic, so it can come later behind a new format
  version. Deferred to Phase 5 (measure first).
- **A properties / metaindex block** (entry counts, key range, filter name). Nothing reads one yet:
  the key range lives in the MANIFEST (Phase 3.6), and `sst_dump` (3.8) can compute counts by
  scanning. The version field leaves room to add one. Deferred.
- **Compression.** Needs a dependency or a lot of code; the type byte reserves room. Stretch goal.
- **Skipping bad blocks during iteration** (LevelDB's two-level iterator remembers the error and
  moves on to the next block). The caller sees a subset of the data before it learns of the
  error. Rejected: ForgeDB stops at the error.

## Consequences

- One `Get` on a table: one filter probe in memory, one binary search of the in-memory index, at
  most one data block read (none on a block cache hit). Measured in the unit test (Linux, 100,000
  keys at 10 bits per key): 100,000 lookups of missing keys inside the table's range read 845 data
  blocks (0.845%); the filter answered the rest.
- An open table holds its index and filter in memory; the table cache (Phase 3.4) bounds how many
  are open.
- Bit flips anywhere in a table are detected: `TableCorruptionTest.EveryBitFlipIsCaughtOrHarmless`
  flips every bit of a small multi-block table, and each flip fails the open or makes the scan
  report `Corruption`; no flip returns wrong data.
- Changing any of rules 1-5 needs a new format version and a reader for both.

## References

- `docs/file-formats.md`, "Table file"; ADR 0005 (bloom filter); ADR 0002 (the flush syncs and
  closes the file, not the builder).
- `src/table/`; `tests/unit/table_test.cpp`, `tests/unit/block_test.cpp`.
- LevelDB table format: https://github.com/google/leveldb/blob/main/doc/table_format.md
