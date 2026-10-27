# ForgeDB file formats

Every byte ForgeDB writes to disk is described here, next to the code that writes it.
A format change without a matching change to this file is a bug.

Rules that apply to every format:

- **Untrusted input.** Parsers treat file bytes as hostile: every length is checked against the
  bytes actually available, and nothing is used before its checksum has been verified.
- **Checksums.** Every on-disk structure carries a CRC-32C (below). Stored checksums are *masked*.
- **Endianness.** All multi-byte integers are little-endian.

## Database directory

Code: `src/db/filename.*`, `OpenDB` in `src/db/db_impl.cpp`. Tests: `tests/unit/filename_test.cpp`,
`tests/unit/db_recovery_test.cpp`.

| Name | What it is | Since |
|---|---|---|
| `000005.log` | write-ahead log number 5 (see "Write-ahead log") | 2.2 |
| `000007.sst` | table file number 7 | Phase 3 |
| `MANIFEST-000003` | descriptor number 3 | Phase 3.6 |
| `CURRENT` | names the current MANIFEST | Phase 3.6 |
| `LOCK` | empty; locked (`LockFileEx`, exclusive) while the database is open | 2.3 |

- File numbers are decimal with at least six digits (`000005`); from 1,000,000 on the name has
  more digits and no leading zero. All file types share one number counter (from Phase 3).
- Names in the directory are untrusted. Only these exact spellings are database files; anything
  else (`5.log`, `0000005.log`, `000005.LOG`, a number above 2^64 - 1, other files) is ignored and
  never modified or deleted.
- **Open (Phase 2).** Lock `LOCK` (if it is held: `Busy`), then replay every `.log` file in
  increasing number order. Sequence numbers must be contiguous: the first record starts at 1 and
  every later record, in the same log or the next, starts at the previous record's sequence plus
  its count (an empty batch uses no numbers); otherwise the open fails with `Corruption`.
  A torn tail is ignored (each log is read on its own: a torn tail ends that log, not the replay);
  mid-log damage fails the open. Every replayed log is then synced (`FileSystem::SyncFile`), so
  data recovered from it cannot be lost by a later crash once newer writes depend on it. New
  writes go to a new log numbered one above the highest existing log, so a torn tail is never
  appended to. Logs are not deleted before Phase 3.

## Primitive encodings

Code: `src/util/coding.h`, `src/util/coding.cpp`. Tests: `tests/unit/coding_test.cpp`.

### fixed32, fixed64

4 or 8 bytes, least significant byte first.

| Value                 | Bytes                     |
|-----------------------|---------------------------|
| fixed32 `0x04030201`  | `01 02 03 04`             |
| fixed64 `0x0807060504030201` | `01 02 03 04 05 06 07 08` |

### varint32, varint64

Unsigned LEB128. Each byte holds 7 bits of the value, lowest bits first; the top bit (`0x80`) is
set on every byte except the last.

| Value          | Bytes               |
|----------------|---------------------|
| 0              | `00`                |
| 127            | `7F`                |
| 300            | `AC 02`             |
| 2^32 - 1       | `FF FF FF FF 0F`    |
| 2^64 - 1       | `FF FF FF FF FF FF FF FF FF 01` |

- varint32 is at most 5 bytes; varint64 at most 10.
- **Rejected:** truncated input (input ends while the top bit says "more"), a value that does not fit
  (a 5th varint32 byte above `0x0F`, a 10th varint64 byte above `0x01`), and anything longer than
  the maximum.
- **Accepted:** overlong encodings such as `80 00` for 0. Same as LevelDB; they can't arrive
  unnoticed because every record is checksummed.

### length-prefixed bytes

A varint32 byte count, then that many bytes. The count is checked against the bytes remaining
before anything is read.

| Value   | Bytes            |
|---------|------------------|
| `""`    | `00`             |
| `"foo"` | `03 66 6F 6F`    |

## Checksums: CRC-32C

Code: `src/util/crc32c.h`, `src/util/crc32c.cpp`. Tests: `tests/unit/crc32c_test.cpp`.

- Polynomial: Castagnoli `0x1EDC6F41` (bit-reversed `0x82F63B78`), initial value `0xFFFFFFFF`,
  final XOR `0xFFFFFFFF`. This is the CRC used by iSCSI (RFC 3720), ext4, LevelDB and RocksDB.
- Check value: CRC-32C of the ASCII bytes `123456789` is `0xE3069283`.
- **Masking.** A CRC is stored as `Mask(crc) = rotate_right(crc, 15) + 0xA282EAD8` (mod 2^32), and
  verified with `Unmask(stored) = rotate_left(stored - 0xA282EAD8, 15)`. Reason: a CRC computed over
  bytes that contain their own CRC can cancel some corruptions out, which matters when a checksummed
  record is later checksummed again as part of a larger block. Same scheme as LevelDB.
  Example: `Mask(0xE3069283) = 0xC78AB0E5`.

## Internal key

Code: `src/db/internal_key.h`, `src/db/internal_key.cpp`. Tests: `tests/unit/internal_key_test.cpp`.

Every stored key is an *internal key*: the user key followed by an 8-byte tag.

```
+----------------------+------------------------------------------+
| user key (n bytes)   | tag: fixed64 = (sequence << 8) | type    |
+----------------------+------------------------------------------+
```

- `sequence`: 56 bits, at most `2^56 - 1`. Every write gets the next sequence number.
- `type` (low byte of the tag): `0x00` = deletion (tombstone), `0x01` = value. Any other value is
  rejected as corruption. These numbers are part of the format and never change.
- Example: user key `ab`, sequence 5, value: `61 62 01 05 00 00 00 00 00 00`.
- Ordering: user key ascending, compared as unsigned bytes; then tag descending, so the newest
  version of a key comes first, and at equal sequence numbers a value comes before a deletion.

**Memtable key** (used inside the memtable and by `LookupKey`): `varint32(n + 8)` followed by the
internal key. A lookup at snapshot `S` uses tag `(S << 8) | 0x01`, the largest tag at `S`, so it
sorts just before every version written at or before `S`.

## Memtable entry

Code: `src/memtable/memtable.h`, `src/memtable/memtable.cpp`. Tests: `tests/unit/memtable_test.cpp`.

In memory only (never written to disk). Each `MemTable::Add` writes one entry into the memtable's
arena; the skip list stores a pointer to its first byte and orders entries by their internal key.

```
+---------------------+--------------------------+-----------------------+---------------+
| varint32: n + 8     | internal key (n + 8)     | varint32: value size  | value bytes   |
|                     | user key | fixed64 tag   |                       |               |
+---------------------+--------------------------+-----------------------+---------------+
```

- The first two fields are exactly a memtable key (see "Internal key"), so a `LookupKey` can be
  compared with entries directly.
- A deletion's value is empty (size `0x00`), and readers ignore it.
- Example: `Add(5, value, "ab", "xyz")` →
  `0A 61 62 01 05 00 00 00 00 00 00 03 78 79 7A`.
- Entries are produced by this process and are trusted; nothing reads them from disk.

## Write batch

Code: `include/forgedb/write_batch.h`, `src/db/write_batch_internal.h`, `src/db/write_batch.cpp`.
Tests: `tests/unit/write_batch_test.cpp`.

A `WriteBatch` is stored as these bytes, and the write-ahead log (Phase 2) stores them unchanged as
the payload of one log record.

```
+--------------------+-----------------+---------------------------------+
| sequence: fixed64  | count: fixed32  | count records, back to back     |
+--------------------+-----------------+---------------------------------+

Put record:     0x01 | varint32 key size | key | varint32 value size | value
Delete record:  0x00 | varint32 key size | key
```

- `sequence` is the sequence number of the first record; record `i` (from 0) gets
  `sequence + i`. It is 0 until the DB assigns it at write time.
- The record type bytes are the internal key types (`0x01` value, `0x00` deletion).
- Example: sequence 100, `Put("ab", "xyz")`, `Delete("c")` →
  `64 00 00 00 00 00 00 00 | 02 00 00 00 | 01 02 61 62 03 78 79 7A | 00 01 63` (23 bytes).

Validation (the bytes may come from a damaged log, so all of it is checked before any record is
applied, and a malformed batch is applied not at all):

- At least 12 bytes (the header).
- Every record has a known type byte and complete, in-bounds varint32 lengths and payloads.
- The records end exactly at the end of the bytes, and their number equals `count`.
- When applied: `sequence + count - 1` must not exceed `2^56 - 1` (the largest sequence number).

Any violation is a `Corruption` status saying what was wrong and at which byte offset.

## Write-ahead log

Code: `src/log/log_format.h`, `src/log/log_writer.*`, `src/log/log_reader.*`. Tests:
`tests/unit/log_test.cpp`.

Every write batch is appended to the current log file (`NNNNNN.log`; see "Database directory") as one *record*
before it is applied to the memtable; recovery replays the records in order. The record's payload
is the batch's bytes (see "Write batch").

**Blocks.** The file is a sequence of 32 KiB (32768-byte) blocks; the last one may be partial. A
record is stored as one or more *fragments*, and a fragment never crosses a block boundary:

```
+------------------------+-------------------+--------------+---------------------+
| CRC: fixed32 (masked)  | length: fixed16   | type: 1 byte | payload: length B   |
+------------------------+-------------------+--------------+---------------------+
 <------------------------- 7-byte header --------------------------------------->
```

- `length` is the payload size in this fragment, little-endian, at most 32761 (32768 - 7).
- `type`: `1` FULL (a whole record), `2` FIRST, `3` MIDDLE, `4` LAST (parts of a record that
  spans blocks, in that order). `0` is never written; it marks zero-filled space.
- `CRC` is the masked CRC-32C (see "Checksums") of the type byte followed by the payload.
- A record that fits in the rest of the current block is one FULL fragment, otherwise FIRST,
  MIDDLE ..., LAST, each filling its block. An empty record is a FULL fragment with length 0.
- When fewer than 7 bytes are left in a block, they are zero-filled and the next fragment starts
  at the next block. When exactly 7 are left, a non-empty record starts there with an empty FIRST fragment.
- Example: the first record of a log, the 3 bytes `abc`, is
  `<crc> 03 00 01 61 62 63`, where `<crc>` = `Mask(CRC-32C(01 61 62 63))`, stored little-endian.

**Reading.** Every fragment is checked before use: a complete header, a length within its
block, a known type, a matching CRC, and fragments in a legal order (FIRST, MIDDLE..., LAST; no
MIDDLE or LAST without a FIRST, no new record before the last one ended). The first failed check
is a *problem*, classified by one rule:

- **Torn tail** (ignored; the log ends there): no valid fragment exists anywhere after the
  problem. This is what a crash leaves: a header or payload cut off by the end of the file, a
  record whose last fragment is missing, garbage, or zeros. The incomplete record is dropped.
- **Mid-log corruption** (recovery fails with `Corruption`, the offset and the reason): a valid
  fragment exists after the problem, so the damage did not come from a crash at the end.

The search for a later valid fragment checks every byte offset in the rest of the problem's
block and in the next block, then only the start of each later block (where the writer always
starts a fragment).

When the damaged fragment's header is intact (a known type and a length within its block), its
own payload is not searched: it is user data and may contain bytes that look like a fragment.
Instead, if a prefix of that payload shorter than the stated length matches the stored CRC, the
length field is damaged and data follows, so it is mid-log corruption (ADR 0004, rule 5).


## Table file (.sst): blocks, filter, index, footer

Code: `src/table/table_format.*`, `src/table/table_builder.*`, `src/table/table.*`. Tests:
`tests/unit/table_test.cpp`. Design: ADR 0006.

A table file (`NNNNNN.sst`; see "Database directory") holds immutable entries sorted by internal
key (see "Internal key"), each with a value; a deletion's value is empty. It is a sequence of
blocks, each followed by a trailer (below), and a fixed-size footer:

```
+--------------+---------+-----+--------------+---------+----------------+---------+--------------+
| data block 0 | trailer | ... | filter block | trailer |  index block   | trailer | footer (48 B)|
+--------------+---------+-----+--------------+---------+----------------+---------+--------------+
```

- **Data blocks** ("Block" format, restart interval 16) hold the entries in order. A block is cut
  after the entry that makes its contents reach 4096 bytes (the builder's `block_size`), so a block
  is at least 4096 bytes, except the last, and an entry is never split.
- **Filter block** (optional): the table's bloom filter (see "Bloom filter") stored as a block's
  contents, over the user keys of all entries.
- **Index block** ("Block" format, restart interval 1, keys compared as internal keys): one entry
  per data block, in order. Key: the data block's *last* internal key. Value: the data block's
  *block handle*, `varint64 offset | varint64 size` (the offset of its first byte in the file and
  the size of its contents, without the trailer). The value must be exactly one handle.
  The first index entry whose key is at or after a lookup key names the only data block that can
  hold that key.
- An empty table (no entries) is valid: no data blocks, an empty index block.

**Footer** (the last 48 bytes of the file):

```
+--------------+--------------+--------------+--------------+-------------+--------------+----------+
| filter       | filter size  | index        | index size   | version:    | CRC: fixed32 | magic:   |
| offset: f64  | : fixed64    | offset: f64  | : fixed64    | fixed32 = 1 | (masked)     | 8 bytes  |
+--------------+--------------+--------------+--------------+-------------+--------------+----------+
 0              8              16             24             32            36             40      48
```

- The handles have the same meaning as in the index; filter size `0` (with offset `0`) means the
  table has no filter.
- `CRC` is the masked CRC-32C of bytes 0-35. `magic` is the ASCII bytes `ForgeSST`.

**Opening** a table checks, in order: the file is at least 48 bytes; the magic (else "not a
ForgeDB table"); the footer CRC; the version (an unknown version is `NotSupported`, anything else
wrong is `Corruption`); that the index block and its trailer end at or before the footer, and the
filter block and its trailer (if any) at or before the index block. The index and filter blocks
are then read and their trailers checked. A data block is read and checked when it is first
needed; its handle must end at or before the first byte of the filter (or index) block. Every
handle is checked against the file size before anything is allocated for it.

### Block trailer

Code: `src/table/block_format.h`, `src/table/block_format.cpp`. Tests:
`tests/unit/block_test.cpp` (`BlockTrailerTest`).

Every block in a table file is followed by 5 bytes:

```
+-------------------+---------------+-------------------------+
| contents (n B)    | type: 1 byte  | CRC: fixed32 (masked)   |
+-------------------+---------------+-------------------------+
                     <------------- 5-byte trailer ----------->
```

- `type`: `0` uncompressed. No other type is written yet; reading any other value is
  `Corruption` ("unknown block type").
- `CRC` is the masked CRC-32C (see "Checksums") of the contents followed by the type byte.
- The trailer is checked (CRC first, then type) before any of the contents are used.

### Block

Code: `src/table/block_builder.*`, `src/table/block.*`. Tests: `tests/unit/block_test.cpp`.

The contents of a data or index block: entries sorted by key, then an array of *restart points*.

```
+-----------+-----------+-----+------------------------------+----------------------+
| entry 0   | entry 1   | ... | restart offsets: fixed32 × r | restart count r:     |
|           |           |     | (byte offset of an entry)    | fixed32              |
+-----------+-----------+-----+------------------------------+----------------------+

entry:  varint32 shared | varint32 unshared | varint32 value size | key bytes [shared..] | value
```

- **Prefix compression.** An entry's key is the first `shared` bytes of the previous entry's key
  followed by the `unshared` bytes stored in the entry.
- **Restart points.** Every *interval*-th entry (16 in data blocks, 1 in the index block, so
  every index entry) starts a restart point: it stores its whole key (`shared = 0`), and its
  offset is listed in the restart array. Entry 0 is always a restart point, so the first offset
  is `0`. A reader binary-searches the restart points' keys, then decodes forward.
- Keys are in strictly increasing order of the block's comparison: internal keys (see
  "Internal key") in data and index blocks.
- An empty block is the 8 bytes `00 00 00 00 01 00 00 00`: one restart point at offset 0, no
  entries.
- Example: the entries `ab` → `1`, `ac` → `2` are 19 bytes:
  `00 02 01 61 62 31 | 01 01 01 63 32 | 00 00 00 00 | 01 00 00 00`.

Validation (after the trailer has been checked):

- When opened: at least 4 bytes; a restart count of at least 1 whose array fits in the block; the
  first restart offset is `0`; the others strictly increase and point into the entry area (before
  the restart array). An empty entry area allows exactly one restart point.
- While reading: every entry's varints and bytes lie inside the entry area; `shared` is at most
  the previous key's length and `0` at a restart point; no restart offset falls inside an entry.
- Any violation is a `Corruption` status naming the problem and the block offset. Key order is
  not checked on read (the CRC rules out damage; the builder checks order in debug builds).

### Bloom filter

Code: `src/table/bloom_filter.*`, `src/util/hash.*`. Tests: `tests/unit/bloom_filter_test.cpp`,
`tests/unit/hash_test.cpp`. Design: ADR 0005 (one filter per table).

A table has one bloom filter over the user keys of all its entries (each distinct user key once,
whatever its number of versions). A lookup that the filter rejects skips the table.

```
+---------------------------------------+-------------+
| bit array: m bits (m / 8 bytes)       | k: 1 byte   |
+---------------------------------------+-------------+
```

- Bit `b` is bit `b mod 8` (least significant first) of byte `b / 8`.
- `m = 8 × ceil(max(64, n × bits_per_key) / 8)` for `n` keys; `k = round(bits_per_key × ln 2)`,
  clamped to `[1, 30]` (k = 7 at 10 bits per key).
- Each key sets `k` bits by *enhanced double hashing* (Dillinger and Manolios, FMCAD 2004) from
  `(low, high)` = MurmurHash3 x64_128 of the key with seed 0 (`low` is the reference
  implementation's first output word), in unsigned 64-bit arithmetic that wraps around:

  ```
  x = low; y = high
  for i = 1 .. k:  set bit (x mod m);  x = x + y;  y = y + i
  ```

  A lookup tests the same bits; any bit clear means the key is not in the table.
- MurmurHash3 x64_128 reads the input as little-endian 64-bit words (Austin Appleby's public
  domain reference, `MurmurHash3_x64_128`). It is part of the format and never changes.
- A filter for no keys is 8 zero bytes and `k`: it rejects every key.
- Example: `hello` and `world` at 10 bits per key → `04 00 0A 98 11 06 21 08 07`.
- A filter shorter than 2 bytes, or with `k` outside `[1, 30]`, matches every key: a damaged
  filter can cost reads, never hide a key.

## MANIFEST and CURRENT

Phase 3.6.
