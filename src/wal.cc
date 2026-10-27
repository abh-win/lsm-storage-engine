#include "wal.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "coding.h"
#include "crc32.h"

namespace lsm {
namespace {

constexpr size_t kHeaderSize = 8;

Status Errno(const std::string& ctx) { return Status::IOError(ctx + ": " + std::strerror(errno)); }

}  // namespace

WalWriter::~WalWriter() { Close(); }

Status WalWriter::Open(const std::string& path) {
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd_ < 0) return Errno("open " + path);
  struct stat st;
  if (::fstat(fd_, &st) == 0) bytes_ = static_cast<uint64_t>(st.st_size);
  return Status::OK();
}

Status WalWriter::AddRecord(std::string_view payload) {
  buf_.resize(kHeaderSize);
  EncodeFixed32(&buf_[4], static_cast<uint32_t>(payload.size()));
  buf_.append(payload.data(), payload.size());
  uint32_t crc = crc32::Value(buf_.data() + 4, buf_.size() - 4);
  EncodeFixed32(&buf_[0], crc32::Mask(crc));

  const char* p = buf_.data();
  size_t left = buf_.size();
  while (left > 0) {
    ssize_t n = ::write(fd_, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Errno("wal write");
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  bytes_ += buf_.size();
  return Status::OK();
}

Status WalWriter::Sync() {
  if (::fdatasync(fd_) != 0) return Errno("wal fdatasync");
  return Status::OK();
}

Status WalWriter::Close() {
  if (fd_ >= 0) {
    int r = ::close(fd_);
    fd_ = -1;
    if (r != 0) return Errno("wal close");
  }
  return Status::OK();
}

Status ReadWal(const std::string& path, const std::function<Status(std::string_view)>& fn,
               WalReadStats* stats) {
  WalReadStats local;
  if (stats == nullptr) stats = &local;
  *stats = {};

  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return Errno("open " + path);
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Errno("fstat " + path);
  }
  size_t size = static_cast<size_t>(st.st_size);
  if (size == 0) {
    ::close(fd);
    return Status::OK();
  }
  void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (map == MAP_FAILED) return Errno("mmap " + path);
  ::madvise(map, size, MADV_SEQUENTIAL);

  const char* base = static_cast<const char*>(map);
  size_t off = 0;
  Status s;
  while (off < size) {
    if (size - off < kHeaderSize) {
      stats->truncated_tail = true;
      break;
    }
    uint32_t masked = DecodeFixed32(base + off);
    uint32_t len = DecodeFixed32(base + off + 4);
    if (len > size - off - kHeaderSize) {
      stats->truncated_tail = true;
      break;
    }
    uint32_t crc = crc32::Value(base + off + 4, 4 + len);
    if (crc32::Unmask(masked) != crc) {
      stats->truncated_tail = true;
      break;
    }
    s = fn(std::string_view(base + off + kHeaderSize, len));
    if (!s.ok()) break;
    off += kHeaderSize + len;
    stats->records++;
  }
  stats->bytes = off;
  ::munmap(map, size);
  return s;
}

}  // namespace lsm
