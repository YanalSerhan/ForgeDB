# ADR 0005: One bloom filter per table, over user keys, with enhanced double hashing

- **Status:** Accepted
- **Date:** 2026-10-27

## Context

A `Get` for a key that a table does not contain should not cost a data block read. A bloom
filter answers "might this table contain the key?" with no false negatives and a tunable rate of
false positives: about 0.82% at 10 bits per key with the optimal number of probes,
`(1 - e^(-k/10))^k` for `k = round(10 × ln 2) = 7`.

Three questions decide the format (Phase 3.2; the byte layout is in `docs/file-formats.md`,
"Bloom filter"):

1. **How many filters per table.** LevelDB stores one filter for every 2 KiB of data-block
   offsets, kept together in a filter block
   ([table_format.md](https://github.com/google/leveldb/blob/main/doc/table_format.md), "filter
   meta block"). RocksDB's default "full filter" is one filter per table, and its "partitioned
   filters" split that filter into blocks indexed like data blocks
   ([RocksDB Bloom Filter](https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter)).
2. **What the filter holds.** A table stores internal keys (user key + sequence + type), but a
   `Get` asks about a user key at any version visible to it.
3. **How a key picks its bits.** One hash computation should yield all `k` bit positions.

## Decision

1. **One filter per table**, stored as one block with the usual trailer (CRC-checked), loaded
   when the table is opened and kept in memory while it is open. A lookup is one hash and `k`
   bit tests.
2. **The filter holds user keys.** Each distinct user key is added once, however many versions
   the table holds (the builder skips a key equal to the one added just before it; table entries
   are sorted, so a key's versions are adjacent).
3. **Bits come from enhanced double hashing** of MurmurHash3 x64_128 (seed 0): with
   `(x, y)` = the hash's two 64-bit halves, probe `i = 1..k` sets bit `x mod m`, then `x += y`,
   `y += i` (Dillinger and Manolios, 2004).
4. `k = round(bits_per_key × ln 2)`, clamped to `[1, 30]`; at least 64 bits per filter.
5. A malformed filter (shorter than 2 bytes, or `k` outside `[1, 30]`) matches every key: it may
   cost reads, never hide a key.

## Alternatives considered

- **One filter per 2 KiB of data (LevelDB).** A lookup must first find the data block in the
  index, then test that block's filter, and every filter carries fixed overhead. It allows
  loading only part of the filter data, which matters only when filters are too large to keep
  in memory. Rejected: more format and code for no benefit at ForgeDB's table sizes.
- **Partitioned filters (RocksDB).** The right answer for very large tables under memory
  pressure, at the cost of a second index and an extra read on a filter-block cache miss.
  Rejected for now; see Consequences for when to revisit.
- **Internal keys in the filter.** A `Get` at sequence S does not know which sequence numbers a
  table holds, so it could not form the key to test. Rejected.
- **Plain double hashing, bit `(x + i·y) mod m`** (Kirsch and Mitzenmacher, 2006; LevelDB uses a
  32-bit variant). Measured in development (Linux, the C++ unit test and an independent Python
  model with the `mmh3` package, same keys): over filters for 1 to 10,000 keys (LevelDB's
  "varying lengths" test, 10,000 probes each), the worst false-positive rate was **2.32%** (7
  keys); a 1-key filter, which sets 7 of its 64 bits, still matched 0.25% of absent keys. The
  cause: `m` is always a multiple of 8, and when `y` shares a large power-of-two factor with `m`
  the probes cycle over 1, 2 or 4 bits. Enhanced double hashing on the same keys: worst **1.18%**,
  and 0.03% for the 1-key filter. For 100,000 keys both match the formula (C++ test, 1,000,000
  probes: plain 0.809%, enhanced 0.817%). Rejected in favor of enhanced double hashing, which
  costs one addition per probe.
- **CRC-32C as the hash.** Fast in hardware, but CRC is linear: keys with structured differences
  get correlated hashes, the wrong property for a bloom filter. Rejected.
- **LevelDB's 32-bit hash with `delta = rotate(h, 17)`.** One 32-bit value limits how independent
  the probes can be in large filters. Rejected; MurmurHash3 x64_128 gives two independent 64-bit
  halves for the same cost, and it can shard the block cache (Phase 3.4) too.

## Consequences

- While a table is open, its whole filter is in memory: `bits_per_key / 8` bytes per distinct
  key, e.g. 1.25 MB for a million keys at 10 bits per key. The table cache (Phase 3.4) bounds how
  many tables are open, and so the filter memory.
- MurmurHash3 x64_128, seed 0, and the probe sequence are part of the file format. The tests pin
  them: hash test vectors from an independent implementation, and golden filter bytes computed
  by a separate Python model of the documented layout.
- A table's filter lookups cost one hash per `Get`; the `Get` path (Phase 3.3) hashes the user key
  once per table it consults.
- Revisit (partitioned filters) if table files grow large enough that filter memory, rather than
  data, limits the number of open tables, or if profiling shows filter loading on the open path.

## References

- `docs/file-formats.md`, "Bloom filter"; `src/table/bloom_filter.*`, `src/util/hash.*`;
  `tests/unit/bloom_filter_test.cpp`, `tests/unit/hash_test.cpp`.
- P. C. Dillinger and P. Manolios, "Bloom Filters in Probabilistic Verification", FMCAD 2004
  (enhanced double hashing).
- A. Kirsch and M. Mitzenmacher, "Less Hashing, Same Performance: Building a Better Bloom
  Filter", ESA 2006 (double hashing).
- Austin Appleby, MurmurHash3, https://github.com/aappleby/smhasher (public domain).
- LevelDB table format: https://github.com/google/leveldb/blob/main/doc/table_format.md
- RocksDB bloom filters: https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter
