#pragma once

// Version: an immutable snapshot of which SSTables make up each level.
// VersionSet: owns the current Version and persists it to the MANIFEST.
//
// The MANIFEST is rewritten in full on every change (write MANIFEST.tmp,
// fsync, rename over MANIFEST, fsync the directory). The rename is atomic, so
// a crash leaves either the old or the new manifest, never a mix.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lsm/status.h"
#include "sstable.h"

namespace lsm {

constexpr int kNumLevels = 7;

struct FileMeta {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // internal keys
  std::string largest;
  std::shared_ptr<Table> table;
};

struct Version {
  // L0 files may overlap and are ordered newest first. L1+ files are disjoint
  // and sorted by smallest key.
  std::vector<std::shared_ptr<FileMeta>> files[kNumLevels];

  uint64_t LevelBytes(int level) const {
    uint64_t b = 0;
    for (const auto& f : files[level]) b += f->file_size;
    return b;
  }
};

struct ManifestState {
  uint64_t next_file_number = 1;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;  // WALs with number >= this still need replay
};

class VersionSet {
 public:
  explicit VersionSet(std::string dbname) : dbname_(std::move(dbname)) {}

  // Loads the MANIFEST (if any) and opens every table it lists.
  Status Recover(bool* existed);

  // Atomically installs `v` and `state` and persists them.
  Status LogAndApply(std::shared_ptr<const Version> v, const ManifestState& state);

  std::shared_ptr<const Version> current() const { return current_; }
  const ManifestState& state() const { return state_; }

 private:
  std::string dbname_;
  std::shared_ptr<const Version> current_ = std::make_shared<Version>();
  ManifestState state_;
};

std::string TableFileName(const std::string& db, uint64_t number);
std::string WalFileName(const std::string& db, uint64_t number);
std::string ManifestFileName(const std::string& db);

}  // namespace lsm
