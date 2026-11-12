# ForgeDB benchmarks

Reproduce: `cmake --build build --target forgedb_bench && ./build/forgedb_bench benchmarks/results 5`

## Method

- Every configuration runs 5 times. All (configuration, run) pairs are executed in one shuffled order (fixed seed), so background load affects every configuration alike. One benchmark process runs at a time.
- Reported: median ops/sec, with min and max over the 5 runs. Raw per-run data is in `results/raw.csv`, the summary in `results/summary.csv`.
- Keys are 16 bytes, values 100 bytes (141-byte records). Single client thread.
- `nosync` = no fsync (data reaches the OS page cache; survives a process crash, not power loss). `sync` = one fsync (`FlushFileBuffers`) per commit.
- Lookups run against a 1,000,000-record log built once before the timed runs (1,000 batches of 1,000 puts).
  - `lookup/index`: `DB::get()` on random keys; each lookup reads and CRC-checks exactly one record from the file.
  - `lookup/scan_full`: decode the log from the start to the end. In an append-only log this is what a lookup without an index must do, because the latest version of a key can be anywhere.
  - `lookup/scan_early_exit`: stop at the first match. Only correct here because no key is ever overwritten; included as the most generous possible baseline.
  - Scans run over an in-memory copy of the log (best case for the scan); index lookups go through the OS file API.
- "Work" = records decoded and bytes examined, counted in the harness, not inferred from time.

## Results — 2026-11-11 run

| Configuration | Median ops/s | Min | Max |
|---|---:|---:|---:|
| seq_put / nosync | 106,465 | 100,049 | 112,003 |
| batch_put 16 / nosync | 286,704 | 282,469 | 303,284 |
| batch_put 64 / nosync | 375,636 | 328,101 | 425,176 |
| batch_put 256 / nosync | 380,476 | 278,385 | 405,892 |
| seq_put / sync | 289 | 231 | 302 |
| batch_put 16 / sync | 4,177 | 3,400 | 4,865 |
| batch_put 64 / sync | 14,906 | 13,400 | 17,202 |
| batch_put 256 / sync | 50,091 | 42,537 | 63,534 |
| lookup / index | 11,906 | 9,761 | 58,081 |
| lookup / scan_full | 2.05 | 1.94 | 2.14 |
| lookup / scan_early_exit | 3.59 | 3.51 | 5.39 |

Work per lookup (1M records):

| Method | Records decoded | Bytes examined |
|---|---:|---:|
| index | 1 | 141 |
| full scan | 1,001,000 (1M puts + 1,000 COMMIT records) | 141 MB |
| early-exit scan | 534,339 (mean over 100 lookups) | 75 MB |

## Machine and conditions

- Laptop: Intel Core i5-8365U (4 cores / 8 threads), 8 GB RAM, Samsung MZVLQ256HAJD NVMe SSD, Windows 11 Pro, on AC power, "Balanced" power plan.
- Compiler: GCC 16.2 (w64devkit), `-O3` Release build.
- Cache: **warm**. Write benchmarks write fresh files that stay in the OS page cache. The lookup dataset was written just before the timed runs. Cold-cache runs were not done (clearing the Windows file cache needs admin tools).
- Background load: other desktop apps running (CPU load 12% before the run; 51% in an earlier aborted attempt). **Only ~675 MB of RAM was free**, so the 141 MB dataset could not reliably stay in the page cache; the index-lookup numbers vary 6× between runs (9.8K–58K/s) for that reason. An earlier smoke run with more free memory measured ~81K lookups/s. Treat index lookup speed as "10K–60K/s on this machine under memory pressure".

## Caveats

- Single-threaded writer; no group commit across concurrent writers.
- Sync numbers depend heavily on the drive and on Windows `FlushFileBuffers`.
- The index-vs-scan work ratio is largely determined by the design (1 record vs all records); the time ratio is the measured part.
