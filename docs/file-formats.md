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
  mid-log damage fails the open. New writes go to a new log numbered one above the highest
  existing log, so a torn tail is never appended to. Logs are not deleted before Phase 3.

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


## Table file (.sst): blocks, filter, index, footer

Phase 3.

## MANIFEST and CURRENT

Phase 3.6.
