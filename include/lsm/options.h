#pragma once

#include <cstddef>
#include <cstdint>

namespace lsm {

struct Options {
  // Create the database directory if it does not exist.
  bool create_if_missing = true;

  // MemTable size (bytes of key+value payload) at which it is frozen and
  // flushed to an L0 SSTable by the background thread.
  size_t memtable_size = 4 << 20;

  // Target size of an SSTable produced by compaction.
  size_t target_file_size = 2 << 20;

  // Uncompressed data-block size inside an SSTable.
  size_t block_size = 4 << 10;

  // Bloom filter bits per key (0 disables filters).
  int bloom_bits_per_key = 10;

  // Number of L0 files that triggers an L0->L1 compaction.
  int l0_compaction_trigger = 4;

  // Writers stall when L0 reaches this many files.
  int l0_stop_writes_trigger = 12;

  // Max bytes for L1; each deeper level is `level_size_multiplier` larger.
  uint64_t max_bytes_level1 = 10ull << 20;
  int level_size_multiplier = 10;

  // fsync() the WAL after every write. SIGKILL durability only needs the
  // write() to reach the page cache; power-loss durability needs fsync.
  bool sync_wal = false;
};

struct ReadOptions {
  // Verify block checksums on every read (blocks are always verified when the
  // table is opened for its index/filter).
  bool verify_checksums = false;
};

struct WriteOptions {
  // Overrides Options::sync_wal for this write when true.
  bool sync = false;
};

}  // namespace lsm
