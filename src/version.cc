#include "version.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "coding.h"
#include "crc32.h"

namespace lsm {
namespace {

constexpr uint32_t kManifestMagic = 0x4d414e31;  // "MAN1"

Status Errno(const std::string& ctx) { return Status::IOError(ctx + ": " + std::strerror(errno)); }

std::string NumberedName(const std::string& db, uint64_t n, const char* ext) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s", static_cast<unsigned long long>(n), ext);
  return db + buf;
}

Status WriteFileSync(const std::string& path, const std::string& data) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return Errno("open " + path);
  const char* p = data.data();
  size_t left = data.size();
  while (left > 0) {
    ssize_t n = ::write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return Errno("write " + path);
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    return Errno("fsync " + path);
  }
  ::close(fd);
  return Status::OK();
}

Status SyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return Errno("open dir " + dir);
  ::fsync(fd);
  ::close(fd);
  return Status::OK();
}

}  // namespace

std::string TableFileName(const std::string& db, uint64_t n) { return NumberedName(db, n, "sst"); }
std::string WalFileName(const std::string& db, uint64_t n) { return NumberedName(db, n, "wal"); }
std::string ManifestFileName(const std::string& db) { return db + "/MANIFEST"; }

Status VersionSet::LogAndApply(std::shared_ptr<const Version> v, const ManifestState& state) {
  std::string body;
  PutFixed32(&body, kManifestMagic);
  PutVarint64(&body, state.next_file_number);
  PutVarint64(&body, state.last_sequence);
  PutVarint64(&body, state.log_number);
  for (int level = 0; level < kNumLevels; ++level) {
    PutVarint64(&body, v->files[level].size());
    for (const auto& f : v->files[level]) {
      PutVarint64(&body, f->number);
      PutVarint64(&body, f->file_size);
      PutLengthPrefixed(&body, f->smallest);
      PutLengthPrefixed(&body, f->largest);
    }
  }
  PutFixed32(&body, crc32::Mask(crc32::Value(body.data(), body.size())));

  std::string tmp = ManifestFileName(dbname_) + ".tmp";
  Status s = WriteFileSync(tmp, body);
  if (!s.ok()) return s;
  if (::rename(tmp.c_str(), ManifestFileName(dbname_).c_str()) != 0) return Errno("rename manifest");
  s = SyncDir(dbname_);
  if (!s.ok()) return s;
  current_ = std::move(v);
  state_ = state;
  return Status::OK();
}

Status VersionSet::Recover(bool* existed) {
  std::ifstream in(ManifestFileName(dbname_), std::ios::binary);
  if (!in) {
    *existed = false;
    return Status::OK();
  }
  *existed = true;
  std::stringstream ss;
  ss << in.rdbuf();
  std::string data = ss.str();
  if (data.size() < 8) return Status::Corruption("manifest too small");
  uint32_t stored = crc32::Unmask(DecodeFixed32(data.data() + data.size() - 4));
  if (crc32::Value(data.data(), data.size() - 4) != stored)
    return Status::Corruption("manifest checksum mismatch");
  if (DecodeFixed32(data.data()) != kManifestMagic) return Status::Corruption("manifest magic");

  std::string_view in_view(data.data() + 4, data.size() - 8);
  ManifestState st;
  if (!GetVarint64(&in_view, &st.next_file_number) || !GetVarint64(&in_view, &st.last_sequence) ||
      !GetVarint64(&in_view, &st.log_number))
    return Status::Corruption("manifest header");

  auto v = std::make_shared<Version>();
  for (int level = 0; level < kNumLevels; ++level) {
    uint64_t count;
    if (!GetVarint64(&in_view, &count)) return Status::Corruption("manifest level count");
    for (uint64_t i = 0; i < count; ++i) {
      auto f = std::make_shared<FileMeta>();
      std::string_view smallest, largest;
      if (!GetVarint64(&in_view, &f->number) || !GetVarint64(&in_view, &f->file_size) ||
          !GetLengthPrefixed(&in_view, &smallest) || !GetLengthPrefixed(&in_view, &largest))
        return Status::Corruption("manifest file entry");
      f->smallest.assign(smallest);
      f->largest.assign(largest);
      Status s = Table::Open(TableFileName(dbname_, f->number), f->number, &f->table);
      if (!s.ok()) return s;
      v->files[level].push_back(std::move(f));
    }
  }
  current_ = std::move(v);
  state_ = st;
  return Status::OK();
}

}  // namespace lsm
