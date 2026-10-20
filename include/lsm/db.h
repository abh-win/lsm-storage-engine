#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lsm/options.h"
#include "lsm/status.h"

namespace lsm {

// A group of updates applied atomically (single WAL record).
class WriteBatch {
 public:
  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear() { ops_.clear(); }
  size_t Count() const { return ops_.size(); }

 private:
  friend class DBImpl;
  struct Op {
    bool is_delete;
    std::string key;
    std::string value;
  };
  std::vector<Op> ops_;
};

struct DBStats {
  uint64_t memtable_bytes = 0;
  int files_per_level[7] = {};
  uint64_t bytes_per_level[7] = {};
  uint64_t flushes = 0;
  uint64_t compactions = 0;
  uint64_t bloom_negatives = 0;   // table probes skipped by the filter
  uint64_t table_probes = 0;      // tables actually searched
  uint64_t recovered_records = 0; // WAL records replayed at open
  double recovery_seconds = 0;
};

// Persistent ordered key-value store built on a log-structured merge tree.
// Thread-safe: any number of concurrent readers and writers.
class DB {
 public:
  static Status Open(const Options& options, const std::string& path,
                     std::unique_ptr<DB>* db);

  virtual ~DB() = default;

  virtual Status Put(const WriteOptions& o, std::string_view key, std::string_view value) = 0;
  virtual Status Delete(const WriteOptions& o, std::string_view key) = 0;
  virtual Status Write(const WriteOptions& o, const WriteBatch& batch) = 0;
  virtual Status Get(const ReadOptions& o, std::string_view key, std::string* value) = 0;

  // Returns live key/value pairs with start <= key < end (end empty = unbounded),
  // at most `limit` entries (0 = unlimited), in key order.
  virtual Status Scan(const ReadOptions& o, std::string_view start, std::string_view end,
                      size_t limit, std::vector<std::pair<std::string, std::string>>* out) = 0;

  // Block until the active memtable is flushed and compactions are idle.
  virtual Status Flush() = 0;
  virtual Status CompactAll() = 0;

  virtual DBStats GetStats() = 0;
};

}  // namespace lsm
