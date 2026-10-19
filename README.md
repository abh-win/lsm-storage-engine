# LSM-Tree Storage Engine

A persistent, thread-safe key-value store in C++20 built on a log-structured merge tree:
skip-list MemTables, CRC32-framed write-ahead log, Bloom-filtered SSTables, and a
background thread doing flushes and leveled compaction. No third-party dependencies
for the engine (GoogleTest for tests).

```
            Put/Delete                                  Get
                │                                        │
                ▼                                        ▼
   ┌───────── WAL (CRC32 records, one write(2) each)   MemTable ─► Immutable MemTable
   │            │                                        │
   │            ▼                                        ▼
   │      MemTable (skip list, arena) ──freeze──►  L0  (overlapping SSTables, newest first)
   │                                                     │  Bloom filter → index block → data block
   │      background thread: flush imm → L0              ▼
   │                         compact L0 → L1 → … → L6   L1..L6 (disjoint, binary search by key range)
   └── MANIFEST (atomic rewrite + rename) records level layout + oldest live WAL
```

## Design

| Component | File | Notes |
|---|---|---|
| MemTable | `src/skiplist.h`, `src/memtable.*` | LevelDB-style skip list: one writer, lock-free readers via acquire/release next pointers; entries live in an arena. Ref-counted so readers can pin an immutable memtable during flush. |
| Internal keys | `src/dbformat.h` | `user_key ‖ (seq<<8 \| type)`, sorted by user key ↑ then sequence ↓, so the newest version is found first; deletes are tombstones. |
| WAL | `src/wal.*` | Record = `masked_crc32 ‖ len ‖ payload`. Replay stops at the first torn/corrupt record (point-in-time recovery). One WAL per memtable; old WALs are deleted once their memtable is in an SSTable recorded by the MANIFEST. |
| CRC32 | `src/crc32.*` | IEEE polynomial, slice-by-8 tables generated at compile time (`constexpr`). |
| SSTable | `src/sstable.*` | 4 KB prefix-compressed data blocks with restart points (binary search + linear scan), an index block, a per-table Bloom filter, and a fixed footer. Each block carries a CRC32 trailer. Tables are `mmap`ed read-only. |
| Bloom filter | `src/bloom.*` | 10 bits/key, k = 7, Kirsch–Mitzenmacher double hashing over a 64-bit wyhash-style hash. Measured FP rate ≈ 0.85–0.88 % (theory 0.82 %). |
| Compaction | `src/db_impl.cc` | L0 → L1 when L0 reaches 4 files; Ln → Ln+1 when a level exceeds 10 MB × 10ⁿ⁻¹ (round-robin file choice). Keeps only the newest version per key and drops tombstones at the base level. Writers stall at 12 L0 files. |
| Versions | `src/version.*` | Immutable `Version` snapshots (shared_ptr) so readers never block compaction; the MANIFEST is rewritten in full and `rename`d into place, then the directory is fsynced. |
| Concurrency | `src/db_impl.cc` | `writer_mu_` serialises writers; `mu_` guards pointer swaps only, never held across I/O. |

## Build & run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/lsm_tests                       # 23 GoogleTest cases
./build/lsm_bench                       # throughput / latency
./build/lsm_crash_test                  # SIGKILL crash-consistency test
./build/lsm_cli /tmp/mydb               # interactive shell: put/get/del/scan/flush/compact/stats
```

Sanitizers (all tests pass clean):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=thread"
```

## Results

Measured on a laptop (12th Gen Intel i5-1235U, Linux), 1M keys, 24-byte keys, 100-byte
values, Release build. Latency is per-call wall time measured around the DB call only.

**`lsm_bench` (1 thread, after `CompactAll`)**

| workload | throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| fillrandom (WAL, no fsync) | 530 K ops/s | 1.42 µs | 3.57 µs | 10.3 µs |
| readrandom (hits) | **525 K ops/s** | 1.71 µs | **2.77 µs** | 15.2 µs |
| readmissing (in-range absent keys) | 1.93 M ops/s | 0.34 µs | 1.17 µs | 3.26 µs |
| scan 100 keys | 22 K scans/s | 43.6 µs | 79.1 µs | 142 µs |

Bloom filters skipped 991 K of 1 M table probes on misses (FP rate 0.88 %), which is why absent-key
lookups are ~4× faster than hits.

Without a full compaction (L0 = 3, L1 = 4, L2 = 47 files): readrandom 441 K ops/s, p99 5.6 µs;
filters skipped 2.07 M table probes for 500 K misses. With 4 reader threads: 1.54 M reads/s
(p99 7.0 µs; the short `mu_` critical section becomes the contention point).

**`lsm_crash_test` (500 K sequential Puts, child killed with SIGKILL, parent reopens and verifies)**

| scenario | WAL records replayed | reopen time | result |
|---|---|---|---|
| kill after all 500 K acked, 4 MB memtable | 37.6 K (rest already in SSTables) | 0.034 s | 3/3 rounds, 0 missing / 0 wrong |
| kill at a random point mid-stream | 32–38 K | 0.03 s | 5/5 rounds, 0 missing / 0 wrong / 0 phantom |
| kill after 500 K acked, 1 GB memtable (**all 500 K ops only in the WAL**) | 500 K | **0.42 s** | 3/3 rounds, 0 missing / 0 wrong |

"Acked" = `Put` returned OK in the child (tracked through a shared-memory counter). Recovery replays
every WAL newer than the MANIFEST's `log_number`, flushes the result to L0, and starts a fresh WAL.
Note that SIGKILL durability only requires the record to reach the page cache; set
`Options::sync_wal` (or `WriteOptions::sync`) for power-loss durability at the cost of an
`fdatasync` per write.

## Tests

`tests/` covers CRC32 check vectors, Bloom FP rate, skip-list ordering under a concurrent reader,
MemTable snapshot visibility, WAL torn-tail and mid-log corruption, SSTable lookups/iteration/
checksum detection, and DB-level randomized operations checked against a `std::map` model across
flushes, compactions and reopens, plus tombstone GC, concurrent readers, and the directory lock.

## Limitations / possible extensions

- No user-visible snapshots or iterators (Scan materialises results); compaction therefore keeps
  only the newest version of each key.
- No block cache or compression (tables are mmapped; the OS page cache does the caching).
- Single background thread; no sub-compactions or rate limiting.
