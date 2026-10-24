# ForgeDB file formats

Every byte ForgeDB writes to disk is described here, next to the code that writes it.
A format change without a matching change to this file is a bug.

Rules that apply to every format:

- **Untrusted input.** Parsers treat file bytes as hostile: every length is checked against the
  bytes actually available, and nothing is used before its checksum has been verified.
- **Checksums.** Every on-disk structure carries a CRC-32C (below). Stored checksums are *masked*.
- **Endianness.** All multi-byte integers are little-endian.

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

## Write-ahead log

Phase 2.1.

## Table file (.sst): blocks, filter, index, footer

Phase 3.

## MANIFEST and CURRENT

Phase 3.6.
