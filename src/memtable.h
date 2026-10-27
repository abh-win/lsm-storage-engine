#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "arena.h"
#include "dbformat.h"
#include "iterator.h"
#include "skiplist.h"

namespace lsm {

// In-memory write buffer: a skip list of encoded entries
//   varint32 ikey_len | internal_key | varint32 value_len | value
// living in an arena. Reference counted so readers can keep an immutable
// memtable alive while the background thread flushes it.
class MemTable {
 public:
  MemTable();
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void Ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
  void Unref() {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
  }

  // REQUIRES: external synchronisation between writers.
  void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

  // Result of a point lookup in the memtable.
  enum class LookupResult { kNotPresent, kFound, kDeleted };
  LookupResult Get(std::string_view user_key, SequenceNumber snapshot, std::string* value) const;

  // Payload bytes (keys + values) inserted so far.
  size_t ApproximateSize() const { return payload_bytes_.load(std::memory_order_relaxed); }
  size_t MemoryUsage() const { return arena_.MemoryUsage(); }
  bool Empty() const { return payload_bytes_.load(std::memory_order_relaxed) == 0; }

  // Caller must keep the memtable referenced while the iterator lives.
  std::unique_ptr<Iterator> NewIterator() const;

 private:
  ~MemTable() = default;

  struct KeyComparator {
    int operator()(const char* a, const char* b) const;
  };
  using Table = SkipList<const char*, KeyComparator>;
  friend class MemTableIterator;

  std::atomic<int> refs_{0};
  Arena arena_;
  Table table_;
  std::atomic<size_t> payload_bytes_{0};
};

}  // namespace lsm
