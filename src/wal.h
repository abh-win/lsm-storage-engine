#pragma once

// Write-ahead log. Each record is framed as
//   masked_crc32(length || payload) : fixed32
//   length                          : fixed32
//   payload                         : length bytes
// A record is written with a single write(2) call, so after SIGKILL the file
// holds every acknowledged record plus at most one torn tail record, which
// fails its checksum and is discarded on recovery.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "lsm/status.h"

namespace lsm {

class WalWriter {
 public:
  WalWriter() = default;
  ~WalWriter();
  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  Status Open(const std::string& path);
  Status AddRecord(std::string_view payload);
  Status Sync();
  Status Close();
  uint64_t BytesWritten() const { return bytes_; }

 private:
  int fd_ = -1;
  uint64_t bytes_ = 0;
  std::string buf_;
};

struct WalReadStats {
  uint64_t records = 0;
  uint64_t bytes = 0;
  bool truncated_tail = false;  // trailing bytes that did not form a valid record
};

// Calls `fn` for every intact record, in order. Stops at the first record
// that is truncated or fails its checksum (reported via stats, not as an error).
Status ReadWal(const std::string& path, const std::function<Status(std::string_view)>& fn,
               WalReadStats* stats);

}  // namespace lsm
