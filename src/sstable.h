#pragma once

// Sorted String Table (immutable on-disk run).
//
// File layout:
//   [data block 0 | crc] ... [data block N | crc]
//   [filter block | crc]          Bloom filter over user keys
//   [index block  | crc]          last internal key of each data block -> handle
//   [footer: filter_off, filter_size, index_off, index_size, magic]  (40 bytes)
//
// Block format (data and index blocks share it): prefix-compressed entries
//   varint shared | varint non_shared | varint value_len | key_delta | value
// with a restart point (shared == 0) every kRestartInterval entries, followed
// by fixed32 restart offsets and fixed32 restart count. Seeks binary-search the
// restart array, then scan forward.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bloom.h"
#include "iterator.h"
#include "lsm/options.h"
#include "lsm/status.h"

namespace lsm {

constexpr uint64_t kTableMagic = 0x4c534d5441424c45ull;  // "LSMTABLE"
constexpr size_t kFooterSize = 40;

struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;  // excluding the 4-byte crc trailer
};

class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval = 16);
  void Add(std::string_view key, std::string_view value);
  std::string_view Finish();
  void Reset();
  bool Empty() const { return counter_total_ == 0; }
  size_t CurrentSizeEstimate() const {
    return buf_.size() + restarts_.size() * 4 + 4;
  }
  std::string_view LastKey() const { return last_key_; }

 private:
  int restart_interval_;
  std::string buf_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;
  size_t counter_total_ = 0;
  std::string last_key_;
  bool finished_ = false;
};

class TableBuilder {
 public:
  TableBuilder(const Options& opts, int fd);
  // Keys must be added in strictly increasing internal-key order.
  void Add(std::string_view internal_key, std::string_view value);
  Status Finish();
  uint64_t FileSize() const { return offset_; }
  uint64_t NumEntries() const { return entries_; }

 private:
  void FlushDataBlock();
  BlockHandle WriteBlock(std::string_view contents);
  void WriteRaw(std::string_view data);

  Options opts_;
  int fd_;
  uint64_t offset_ = 0;
  uint64_t entries_ = 0;
  BlockBuilder data_block_;
  BlockBuilder index_block_{1};
  std::unique_ptr<BloomFilterBuilder> filter_;
  std::string last_user_key_;
  std::string out_buf_;
  Status status_;
};

class Table {
 public:
  static Status Open(const std::string& path, uint64_t file_number, std::shared_ptr<Table>* out);
  ~Table();
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  enum class GetResult { kNotPresent, kFound, kDeleted, kFilteredOut };
  GetResult Get(const ReadOptions& ro, std::string_view user_key, uint64_t user_key_hash,
                std::string_view lookup_key, std::string* value, Status* s) const;

  std::unique_ptr<Iterator> NewIterator(const ReadOptions& ro) const;

  uint64_t file_number() const { return file_number_; }
  uint64_t file_size() const { return size_; }

 private:
  Table() = default;
  Status ReadBlock(const BlockHandle& h, bool verify, std::string_view* contents) const;
  friend class TableIterator;

  uint64_t file_number_ = 0;
  const char* base_ = nullptr;
  size_t size_ = 0;
  std::string_view filter_;
  std::string_view index_;
};

}  // namespace lsm
