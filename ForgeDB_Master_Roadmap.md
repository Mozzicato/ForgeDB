# FORGEDB — MASTER BUILD ROADMAP

### Project vision

> **ForgeDB is an embedded, crash-safe key-value storage engine written from scratch in C++, demonstrating durability, concurrency, and honest performance engineering — with every claim backed by a reproducible benchmark.**

The long-term experience is:

> **"A process can die at any instant, mid-write, and ForgeDB comes back consistent — and I can prove it."**

The key architectural principle:

> **Durability is a protocol, not a hope. Every design decision must survive the question: "what happens if the process dies right here?"**

---

## 0. SCOPE DECISION — READ THIS FIRST

ForgeDB follows the **Bitcask model** (append-only log + in-memory hash index), **not** an LSM-tree or B-tree.

**Why this is deliberate:**
- Bitcask gives you crash recovery, concurrency, and compaction in *teachable, testable layers*.
- An LSM-tree (memtables, SSTables, levels, merge compaction) roughly doubles scope and buries your concurrency/durability story under complexity.
- A B-tree pulls you into page management, buffer pools, and on-disk pointer fixups — a different (also valid, but longer) road.

**The interview story this buys you:**
> "I chose an append-only log + hash index over an LSM because my goal was to nail durability and concurrency semantics first, with measurable performance. Compaction was added later as an explicit, isolated problem."

Scope discipline is itself a senior signal. FAANG interviewers probe *why* more than *what*.

**What we explicitly do NOT build (v1):** replication, networking, SQL, range queries (a hash index can't do them — and admitting that is a talking point, not a flaw).

---

## 1. THE FORGEDB DEVELOPMENT MODEL

Same loop as Orphic:

```text
Problem
   ↓
Smallest implementation
   ↓
Experiment
   ↓
Measure
   ↓
Test (incl. crash tests)
   ↓
Document (ADR / experiment log)
   ↓
Ship (tagged release)
   ↓
Next problem
```

Every phase has: **Objective → Deliverables → Experiments → Exit criteria.**

Four standing rules:
1. **Every number on the resume = a file in `benchmarks/` produced by a committed script.** No measured number, no claim.
2. **Every commit is real and dated.** Green GitHub history is authenticity evidence.
3. **Every phase ends with a tagged release**, not a half-finished `main`.
4. **Interview framing is written down** per phase (Section 10) — you rehearse the story as you build it, not the night before the interview.

---

## 2. PHASE 0 — THE FOUNDATION (Week 1)

### Objective
Build the engineering environment. Prevent six months of CMake chaos.

### Deliverables
```text
ForgeDB/
├── src/                 # public headers + impl
├── tests/               # GoogleTest suites
├── benchmarks/          # benchmark harness + results/ (committed numbers)
├── crash_tests/         # fault-injection harness
├── scripts/             # plot.py, gen_data.py, corruption.py
├── docs/
│   ├── decisions/       # ADRs
│   ├── experiments/     # numbered experiment logs
│   ├── research/        # topic notes
│   └── architecture/
├── .github/workflows/   # CI: build + tests + sanitizers
├── CMakeLists.txt
├── README.md
├── .clang-format
└── .gitignore
```

Toolchain: CMake ≥ 3.20, C++20, GoogleTest (FetchContent), clang-format, GitHub Actions matrix (gcc + clang), **ThreadSanitizer + AddressSanitizer + UBSan jobs from day one**.

### Research before/while doing it
- C++20 refresher: RAII, move semantics, `std::span`, `std::filesystem`
- POSIX file I/O: `open/read/write/pread`, `fdatasync` vs `fsync`, `O_APPEND`
- CMake targets, sanitizers flags, ctest

### Exit criteria
Fresh clone → `cmake -B build && cmake --build build && ctest` → green, including sanitizer jobs. Hello-world engine `db.put("k","v"); db.get("k")` compiles and runs.

---

## 3. PHASE 1 — THE SPINE: Append-Only Log Store (Weeks 2–4)

### Objective
ForgeDB can durably store and retrieve key-value pairs. No WAL yet, no concurrency — just a correct, crash-aware log format.

### Deliverables
**Record format** (design this carefully — everything builds on it):
```text
[ length | crc32 | type | key_len | key | value_len | value ]
```
- CRC32 over the payload (type + key + value), computed before write, validated on read.
- Length prefix enables **framing**: on read, know exactly how many bytes a record occupies.
- Record types from day one: `PUT`, `DELETE` (tombstone). Even if DELETE isn't exposed yet.

**Core engine:**
- `put(key, value)` → append encoded record to `forgedb.log`, update in-memory `std::unordered_map<std::string, RecordLocation>` (file offset + length).
- `get(key)` → index lookup → `pread` at offset → validate CRC → return value.
- `delete(key)` → append tombstone → remove from index.
- Startup: scan log sequentially, rebuild index, **skip-and-count** records with bad CRC (log these; do not crash).

### Experiments
- **Torn-write lab:** open the log, truncate to N-1 bytes mid-record, restart. Engine must recover valid records up to the last complete one.
- **Corruption lab:** flip one byte in the middle of the log. Engine must skip exactly that record, recover everything after it (length-prefixed framing makes this possible — this is *why* the format matters).
- Measure: bytes written per `put`, index rebuild time for 1M records, `pread` vs sequential read latency.

### Exit criteria
- 1M `put`s, kill -9 at a random point during a run, restart → **all complete records present, no partial record ever visible.**
- GoogleTest: encode/decode round-trips, CRC acceptance/rejection, truncated tail, corrupted middle record, empty log, 1-byte keys, 1MB values, unicode keys.

### Interview story (start rehearsing now)
> "My record format uses length-prefix framing plus CRC32 per record, which lets recovery skip a corrupted record and continue — a torn write at the tail simply truncates. That's the same principle as Kafka's message format."

---

## 4. PHASE 2 — DURABILITY SEMANTICS: fsync Discipline (Weeks 5–6)

### Objective
Understand — and *measure* — what "durable" actually costs. This phase is small but it's the soul of the project.

### Deliverables
- Explicit durability API: `put(k, v, Durability::SYNC)` vs `Durability::ASYNC`.
- SYNC: append → `fdatasync` → then update index → ack.
- ASYNC: append → return; background `fdatasync` every N ms or M bytes.
- A `page_cache_bypass` experiment flag: `O_DIRECT` (or `fcntl` readahead experiments) to *see* the page cache's effect.

### Experiments (this is the phase's heart)
```text
1. ASYNC, no fsync          → ops/sec?   (this number will shock you)
2. SYNC, fdatasync per op   → ops/sec?   (this number will also shock you)
3. SYNC, fdatasync every 32 ops (group commit) → ?
4. SYNC, fdatasync every 1ms → ?
5. O_DIRECT vs buffered reads → latency?
```
Plot all of it. Commit `benchmarks/results/phase2_durability.csv` + matplotlib chart.

### Exit criteria
You can explain, with *your own graphs*: why fsync dominates write latency, why group commit amortizes it, and what the page cache does to your numbers. This is a full OS lesson you can now teach in an interview.

### Interview story
> "My synchronous path does ~X ops/sec and my group-commit path does ~Y, and I can show you exactly why — fdatasync cost measured in isolation, amortized over batch sizes 1–256."

---

## 5. PHASE 3 — THE WAL: Write-Ahead Logging & Crash Recovery (Weeks 7–10)

### Objective
Separate bookkeeping from durability. Introduce the real WAL protocol and a recovery procedure that provably works.

### Design
```text
PUT(key, value):
  1. encode WAL record (seqno, type, key, value) + CRC
  2. append to WAL, fdatasync WAL          <-- durability boundary
  3. append to data log                    <-- optional/rebuildable
  4. update in-memory index
  5. ack to client

STARTUP (recovery):
  1. scan data log → rebuild index, remember high-water offset
  2. scan WAL from last checkpoint:
       replay PUT/DELETE records with seqno > data-log high-water
  3. validate: replayed state == index state (assert)
  4. checkpoint: fold WAL into data log, truncate WAL
```

Key idea you must internalize: **the WAL is the source of truth; the data log and index are rebuildable derivations.** Saying that sentence correctly in an interview is worth more than half the code.

### Deliverables
- `wal.log` with its own record format (add monotonic `seqno` — enables ordering and idempotent replay).
- Checkpoint API: `db.checkpoint()` → merge WAL into data log, fsync, atomically rename, truncate WAL. (Atomic rename via `rename(2)` is your "commit point" trick — learn it.)
- `recover()` with explicit phases, logged diagnostics, and a recovery report (`records_replayed`, `corruptions_skipped`, `wal_bytes`).

### Experiments — the crash harness (your crown jewel)
`crash_tests/harness.cpp`:
- Fork child → child performs random `put`/`delete` mix against a *known expected state* maintained by the parent → parent sends `SIGKILL` at a uniformly random moment → child restarts engine → verify: **every acked write is present; no partial write is visible.**
- Run **1,000+ randomized crash cycles** with different seeds. Commit the seeds and results. (This is your "100+ simulated crash/fault scenarios" — now real.)
- Injection variants: kill during fsync (use a wrapper that sleeps around fdatasync), kill during checkpoint, kill *during WAL truncation*, kill with OOM (`ulimit -v`). Corrupt 1 random WAL byte per 100 runs. Corrupt the checkpoint's tail.
- Prove replay idempotency: replay WAL twice, assert identical state.

### Exit criteria
- 1,000 randomized crash-recovery cycles, zero state divergence, zero leaked fds, zero crashes-on-recovery.
- Recovery report generated for every run; corrupted records skipped and counted, never fatal.
- Tests: replay idempotency, checkpoint atomicity (kill during rename — use a fault hook), WAL truncation recovery, empty-WAL startup, seqno gaps.

### Interview story
> "The WAL is my source of truth; the data log and index are rebuildable. Durability boundary is WAL-append + fdatasync. I ran a randomized SIGKILL harness for 1,000+ cycles with a fault oracle and zero divergence."

---

## 6. PHASE 4 — TRANSACTIONS (Weeks 11–13)

### Objective
Atomic multi-key writes. One WAL record, all-or-nothing visibility.

### Deliverables
- `Batch` API:
```cpp
auto txn = db.begin_batch();
txn.put("a", "1");
txn.put("b", "2");
txn.remove("c");
db.commit(txn);   // single WAL record, type=BATCH
// or txn.abort();
```
- WAL record type `BATCH` containing N sub-records + sub-CRCs; single top-level CRC.
- Atomicity rule: index is updated for all keys or none. Implementation: apply sub-records to a *staging* index first; only swap on full decode success.
- Durability knobs per batch: SYNC / ASYNC (composes with Phase 2 group commit — group-commit *batches* of batches).
- Rollback story: an uncommitted batch's WAL record is simply never replayed past its commit marker — design the format so recovery can tell "complete batch" from "torn batch" (a trailing COMMIT flag/CRC; a torn tail batch is discarded — **compare with MySQL binlog and SQLite commit records here; read both**).

### Experiments
- Crash during a 1,000-op batch → verify zero sub-records visible.
- Batch sizes 1/10/100/1000 × SYNC/ASYNC throughput matrix → find where batching flattens fsync cost.
- Interleaved batches under crash: two clients, random commits/aborts, oracle checks atomicity per batch.

### Exit criteria
- Atomicity oracle green across randomized crash runs; torn-batch detection works; group-commit numbers published in `benchmarks/`.

### Interview story
> "Atomicity comes from single-record WAL append plus staging-then-swap index application; a torn tail batch is detected by a missing commit marker and discarded — same idea as MySQL's binlog event boundaries."

---

## 7. PHASE 5 — CONCURRENCY: Locks, Shards, and Honest Parallelism (Weeks 14–17)

### Objective
Serve concurrent readers and writers safely, with the *coarsest design that is correct* first, then justify every refinement with measurement.

### Deliverables (three deliberate generations)
- **Gen 1 — one `std::shared_mutex`:** shared lock for `get`, unique for `put`/`delete`. Correct, simple. Ship it. Measure.
- **Gen 2 — sharded index:** 256 shards keyed by `hash(key) % 256`, each shard its own `shared_mutex`. Writes lock one shard; reads scale across shards. Now concurrent read/write is real.
- **Gen 3 — serialized WAL appends:** WAL is a single append-only file — concurrent writers must serialize *the append*, but can pipeline everything else. Design: writers encode records concurrently → enqueue → single appender thread does WAL append + fdatasync + seqno assignment → index update happens after WAL ack. This is a mini write pipeline and a perfect teaching artifact.

### Experiments
- Read-heavy (95/5), balanced (50/50), write-heavy (5/95) workloads × worker counts 1/2/4/8/16 → ops/sec, p50/p95/p99 latency tables.
- Prove race-freedom: **ThreadSanitizer clean on every CI run**; a stress test with 16 workers × 50 clients doing random ops for 60s, verified against a mutex-protected oracle model.
- Compare Gen 1 vs Gen 2 vs Gen 3 under each workload; publish the tables. If sharding doesn't help write-heavy (it won't much — WAL serialization dominates), **say so**. That's the lesson.

### Exit criteria
- TSan-clean, oracle-verified stress runs; measured scaling story per generation; your "16 workers / 50 clients, race-free" claim now backed by committed artifacts.

### Interview story
> "I went from one shared_mutex to 256 lock shards to a serialized-WAL write pipeline — each step driven by a workload matrix. Writes don't scale past the WAL append, and my numbers show exactly where and why."

---

## 8. PHASE 6 — PERFORMANCE ENGINEERING (Weeks 18–20)

### Objective
Publish defensible headline numbers, and know *why* they are what they are.

### Deliverables
- Benchmark harness: configurable key/value size distributions, workloads (uniform/zipfian — write a zipf generator), threads, durability mode. Output CSV + charts. Seedable, reproducible, committed.
- Index-lookup-vs-scan benchmark: `get` via hash index vs full sequential scan over 1M keys → back your ">100× less work" claim with the real factor (whatever it is — measure it).
- Sequential vs batched/pipelined writes: publish your ops/sec at batch sizes 1/16/64/256.
- Profiling pass: `perf` on the hot path; eliminate what actually shows up (likely: copies in encode/decode — fix with `std::string_view` zero-copy reads; syscall batching).

### Exit criteria
A `benchmarks/` directory a stranger can re-run and reproduce within a few percent. Your resume numbers updated to *your* measured values — even if they're lower than the current draft. **A true 80K ops/sec beats a fabricated 250K in every interview that matters.**

---

## 9. PHASE 7 — COMPACTION & SPACE RECLAMATION (Weeks 21–23)

### Objective
Solve the append-only tax: overwritten and deleted keys waste space. Rewrite the log safely, online.

### Deliverables
- Track per-key staleness: for each live key, count how many obsolete records precede its current location (maintain in index: `obsolete_bytes`).
- `db.compact()`: write a new data log containing only live records (iterate index), fsync, atomic rename, delete old files, adjust offsets in index.
- **Online compaction (stretch):** readers/writers keep working during compaction. Requires careful offset remapping — attempt only after offline compaction is rock solid.
- Configurable trigger: `obsolete_ratio > X%`.

### Experiments
- Workload: repeatedly overwrite the same 10K keys 100× → show space bloat → compact → show reclamation.
- Crash during compaction: kill mid-rewrite; old log must remain authoritative; recovery must not see the half-written new log. (This is a genuinely tricky test — exactly why it belongs in the project.)

### Exit criteria
Compaction reclaims space; crash-during-compaction leaves the DB fully consistent; tests cover both.

### Interview story
> "Compaction is the one place I touch both files and index atomically: write-validate-rename. My crash tests kill the process mid-rewrite and the old log stays authoritative."

---

## 10. PHASE 8 — ADVERSARIAL HARDENING (Weeks 24–26)

### Objective
Break your own engine professionally before anyone else does.

### Deliverables
- **Fuzzing:** libFuzzer or AFL++ on record decode (throw garbage at the decoder; assert no crashes, no OOB reads — ASan watches). Seed corpus = real logs.
- **Corruption matrix:** for each file (data log, WAL, index snapshot if any) × corruption site (header, length, CRC, payload) × corruption type (bit flip, zeroed, truncated, extended) → engine must recover, skip, or report — never crash, never serve wrong data.
- **Longevity test:** 10GB dataset; delete 50%; verify index memory stays bounded and lookups don't degrade.
- **fd leak check:** run 10K open/close cycles, assert fd count constant (`/proc/self/fd`).

### Exit criteria
Fuzzer clean for N hours of CPU; corruption matrix all green; no leaks under sanitizers.

---

## 11. PHASE 9 — PRODUCTIZATION & THE PORTFOLIO PACKAGE (Weeks 27–28)

### Deliverables
- README arc: **Problem → Design → Record format diagram → Durability protocol → Crash-test harness demo (GIF) → Benchmarks (your real charts) → Safety model → Limitations → Future work.** Limitations section is mandatory — range queries unsupported, single-writer WAL, etc. Maturity = knowing what you didn't build.
- ADRs: why Bitcask, why CRC32 (vs xxhash — you tested both, right?), why shared_mutex, why 256 shards, why group commit, why atomic rename.
- Architecture doc with the ASCII/data-flow diagrams.
- **One blog post**: "What building a storage engine taught me about fsync" — with your Phase 2 graphs. This is your distribution asset.
- Demo: 3-minute terminal recording — crash it live, watch it recover.

---

## 12. RELEASE MILESTONES & STOPPING POINTS

| Release | Contains | Portfolio value |
|---|---|---|
| **ForgeDB 0.1** | Phase 1: log store, framing, CRC, startup rebuild | Solid systems project |
| **ForgeDB 0.2** | + fsync semantics, durability modes | Now you can *talk OS internals* |
| **ForgeDB 0.3** | + WAL protocol, crash harness, recovery | **Interview-grade. This alone carries weight.** |
| **ForgeDB 0.4** | + transactions, batch atomicity | Google-tier talking point |
| **ForgeDB 0.5** | + concurrency generations, TSan-verified | The "how do you handle concurrency?" answer |
| **ForgeDB 0.6** | + published benchmarks, profiling | Defensible performance claims |
| **ForgeDB 0.7** | + compaction | Completes the storage story |
| **ForgeDB 0.8** | + fuzzing, corruption matrix, polish | Exceptional, rare portfolio piece |

**Stopping-point principle:** if recruiting season hits at 0.3 or 0.4, you apply. Every release is a legitimate product. You never say "I can't apply because ForgeDB isn't finished."

**Interview-mapping cheat sheet (rehearse as you build):**
| Phase | Interview topic it arms you for |
|---|---|
| 1 | File I/O, binary formats, error handling |
| 2 | OS: page cache, syscalls, I/O latency, fsync |
| 3 | Durability, recovery, logging = distributed-systems fundamentals |
| 4 | Atomicity, isolation, how real DBs commit |
| 5 | Concurrency, locks, contention, Amdahl's law in practice |
| 6 | Performance methodology, profiling, honest measurement |
| 7 | Storage management, online vs offline maintenance |
| 8 | Testing philosophy, fuzzing, failure injection |
| 0–9 | "Tell me about a hard bug" — you now have 30 real ones |

---

## 13. RESEARCH / EXPERIMENT / DECISION SYSTEMS

Same pattern as Orphic, non-negotiable:

```text
docs/research/    what is X, alternatives, tradeoffs → what we chose, why
docs/experiments/ numbered logs: Question, Hypothesis, Setup, Variables, Results, Interpretation, Decision
docs/decisions/   ADRs only for architecture-level choices
benchmarks/       every claim reproducible from a committed script
```

Minimum research notes: `posix-file-io.md`, `fsync-and-page-cache.md`, `wal-protocols.md` (compare MySQL binlog, SQLite WAL, PostgreSQL WAL at a conceptual level), `concurrency-primitives.md`, `crc32-vs-xxhash.md`, `atomic-rename.md`.

---

## 14. READING LIST — PHASED, NOT ALL AT ONCE

- **Phase 0–1:** *A Tour of C++* (Stroustrup) selective chapters; man pages for `open/pread/fdatasync`; GoogleTest primer.
- **Phase 2–3:** the **Bitcask paper** (Riak — your design ancestor, read it twice); SQLite docs on WAL and atomic commit; MySQL binlog format overview (conceptual).
- **Phase 4–5:** *C++ Concurrency in Action* (Williams) chapters on mutexes, shared_mutex, memory model basics; DDIA chapters 3 (storage) and 7 (transactions) — Kleppmann.
- **Phase 6–9:** CMU 15-445 BusTub lectures (YouTube) — not to build it, to steal vocabulary and standards of rigor; `perf` documentation; libFuzzer tutorial.

Do **not** read the LSM literature yet. It will tempt you to rebuild scope.

---

## 15. YOUR FIRST CHECKPOINT

Not "build ForgeDB." This:

```text
Repository + CMake + GoogleTest + CI + sanitizers   (Phase 0)
      ↓
Record format designed on paper first — draw it      (Phase 1 prep)
      ↓
Encode/decode round-trip tests                       (Phase 1)
      ↓
put/get/delete against the log                       (Phase 1)
      ↓
Truncate the file mid-record, restart, recover       (Phase 1)
      ↓
Corrupt one byte mid-file, restart, skip-and-continue(Phase 1)
      ↓
Benchmark: 1M puts, index rebuild time               (Phase 1)
      ↓
TAG: v0.1.0 → stop, inspect, write the experiment log
```

**Time budget:** ~28 weeks at 10–15 hrs/week alongside school — which lines up with the Orphic → EventMesh → ForgeDB sequence, ForgeDB starting around week 19.

The end goal is not "a key-value store."
It's:

> **"Ask me what happens when the power cuts at any nanosecond of any operation — I can show you, with a test that kills the process there."**

That's ForgeDB.
