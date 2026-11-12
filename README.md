# ForgeDB

An embedded, crash-safe key-value storage engine in C++20 (Bitcask-style).

- **Append-only log = write-ahead log.** Every record carries a CRC-32 and a length prefix.
- **Atomic multi-key transactions.** A transaction's records are followed by a COMMIT record (with the op count) in a single append; recovery applies a transaction only if its COMMIT is intact.
- **Crash recovery.** On open, the log is replayed and the in-memory hash index rebuilt; replay stops at the first torn or corrupt record and the log is truncated to the last good COMMIT (point-in-time recovery).
- **Concurrency.** A writer mutex serialises append + fsync; a `std::shared_mutex` guards the index, so readers run in parallel and never wait on an fsync.

## Build and test

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

ThreadSanitizer (Linux, clang or gcc): add `-DFORGEDB_TSAN=ON`. CI runs gcc, clang, MSVC and a TSan job.

## Tests

69 GoogleTest tests: record format, CRC, replay, file layer, DB API, transactions, concurrency, a 16-worker / 50-client stress test, and a fault-injection suite that generates ~1,400 scenarios (truncation at every byte, byte flips in every field of every record, zeroed sectors, whole-transaction corruption, tail garbage) plus real process kills at each commit stage.

## Benchmarks

See [benchmarks/README.md](benchmarks/README.md) for method, machine and caveats. Median of 5 shuffled runs on a laptop (i5-8365U, NVMe, Windows 11):

| | ops/s |
|---|---:|
| sequential puts, no fsync | 106K |
| batched puts (256/commit), no fsync | 380K |
| sequential puts, fsync per commit | 289 |
| batched puts (256/commit), fsync per commit | 50K |

Index lookup decodes 1 record; a full scan of the 1M-record log decodes 1,001,000.

## Limitations

- Point lookups only: a hash index cannot serve range queries.
- No compaction: overwritten and deleted data stays in the log; startup replay time grows with log size.
- All keys must fit in memory.
- Writers are serialised; no group commit across concurrent writers.
- Transactions are write-only batches (read committed; not serializable).
