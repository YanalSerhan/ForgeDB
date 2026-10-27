# Architecture decision records

One file per non-obvious decision, written when the decision is made. Copy
[0000-template.md](0000-template.md) to start a new one.

| ADR | Title | Status |
|-----|-------|--------|
| [0001](0001-windows-only-scope-and-toolchain.md) | Windows-only scope and toolchain | Accepted |
| [0002](0002-durability-on-windows.md) | Durability rules on Windows | Proposed (open question on CURRENT) |
| [0003](0003-db-iterator-reseek.md) | The DB iterator re-seeks past long runs of hidden entries | Accepted |
| [0004](0004-log-recovery-torn-tail-vs-corruption.md) | Log recovery tells a torn tail from mid-log corruption, and stops on corruption | Accepted |
| [0005](0005-one-bloom-filter-per-table.md) | One bloom filter per table, over user keys, with enhanced double hashing | Accepted |
