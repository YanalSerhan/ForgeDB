# Architecture decision records

One file per non-obvious decision, written when the decision is made. Copy
[0000-template.md](0000-template.md) to start a new one.

| ADR | Title | Status |
|-----|-------|--------|
| [0001](0001-windows-only-scope-and-toolchain.md) | Windows-only scope and toolchain | Accepted |
| [0002](0002-durability-on-windows.md) | Durability rules on Windows | Accepted (CURRENT question decided by 0007) |
| [0003](0003-db-iterator-reseek.md) | The DB iterator re-seeks past long runs of hidden entries | Accepted |
| [0004](0004-log-recovery-torn-tail-vs-corruption.md) | Log recovery tells a torn tail from mid-log corruption, and stops on corruption (except in the newest log, rule 6) | Accepted |
| [0005](0005-one-bloom-filter-per-table.md) | One bloom filter per table, over user keys, with enhanced double hashing | Accepted |
| [0006](0006-table-format.md) | Table file format | Accepted |
| [0007](0007-manifest-and-current.md) | The MANIFEST, and switching CURRENT without trusting the rename | Accepted (rule 5 extended to tables by 0008) |
| [0008](0008-leveled-compaction.md) | Leveled compaction: what is compacted, what is dropped, and when files go | Accepted |
