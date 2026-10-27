#include "sstable.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>

#include "bloom.h"
#include "coding.h"
#include "crc32.h"
#include "dbformat.h"

namespace lsm {
namespace {

Status Errno(const std::string& ctx) { return Status::IOError(ctx + ": " + std::strerror(errno)); }

void EncodeHandle(std::string* dst, const BlockHandle& h) {
  PutVarint64(dst, h.offset);
  PutVarint64(dst, h.size);
}

bool DecodeHandle(std::string_view in, BlockHandle* h) {
  return GetVarint64(&in, &h->offset) && GetVarint64(&in, &h->size);
}

// Iterates over one block. Keys are compared as internal keys.
class BlockIterator final : public Iterator {
 public:
  explicit BlockIterator(std::string_view block) {
    if (block.size() < 4) {
      corrupt_ = true;
      return;
    }
    num_restarts_ = DecodeFixed32(block.data() + block.size() - 4);
    size_t restart_bytes = static_cast<size_t>(num_restarts_) * 4 + 4;
    if (num_restarts_ == 0 || restart_bytes > block.size()) {
      corrupt_ = true;
      return;
    }
    data_ = block.data();
    restarts_off_ = static_cast<uint32_t>(block.size() - restart_bytes);
    current_ = restarts_off_;
  }

  bool Valid() const override { return !corrupt_ && current_ < restarts_off_; }
  Status status() const override {
    return corrupt_ ? Status::Corruption("bad block") : Status::OK();
  }
  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }

  void SeekToFirst() override {
    if (corrupt_) return;
    SeekToRestart(0);
    ParseNext();
  }

  void Next() override {
    assert(Valid());
    ParseNext();
  }

  void Seek(std::string_view target) override {
    if (corrupt_) return;
    // Binary search for the last restart point whose key is < target.
    uint32_t lo = 0, hi = num_restarts_ - 1;
    while (lo < hi) {
      uint32_t mid = (lo + hi + 1) / 2;
      uint32_t off = RestartOffset(mid);
      uint32_t shared, non_shared, vlen;
      const char* p = DecodeEntry(data_ + off, data_ + restarts_off_, &shared, &non_shared, &vlen);
      if (p == nullptr || shared != 0) {
        corrupt_ = true;
        return;
      }
      if (CompareInternalKey(std::string_view(p, non_shared), target) < 0) {
        lo = mid;
      } else {
        hi = mid - 1;
      }
    }
    SeekToRestart(lo);
    while (ParseNext() && CompareInternalKey(key_, target) < 0) {
    }
  }

 private:
  static const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared,
                                 uint32_t* non_shared, uint32_t* vlen) {
    if ((p = DecodeVarint32(p, limit, shared)) == nullptr) return nullptr;
    if ((p = DecodeVarint32(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = DecodeVarint32(p, limit, vlen)) == nullptr) return nullptr;
    if (static_cast<size_t>(limit - p) < static_cast<size_t>(*non_shared) + *vlen) return nullptr;
    return p;
  }

  uint32_t RestartOffset(uint32_t i) const { return DecodeFixed32(data_ + restarts_off_ + i * 4); }

  void SeekToRestart(uint32_t i) {
    key_.clear();
    next_ = RestartOffset(i);
    current_ = next_;
  }

  // Parses the entry at next_. Returns false at end of block.
  bool ParseNext() {
    current_ = next_;
    if (current_ >= restarts_off_) {
      current_ = restarts_off_;
      return false;
    }
    uint32_t shared, non_shared, vlen;
    const char* p =
        DecodeEntry(data_ + current_, data_ + restarts_off_, &shared, &non_shared, &vlen);
    if (p == nullptr || shared > key_.size()) {
      corrupt_ = true;
      return false;
    }
    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = std::string_view(p + non_shared, vlen);
    next_ = static_cast<uint32_t>((p + non_shared + vlen) - data_);
    return true;
  }

  const char* data_ = nullptr;
  uint32_t restarts_off_ = 0;
  uint32_t num_restarts_ = 0;
  uint32_t current_ = 0;
  uint32_t next_ = 0;
  std::string key_;
  std::string_view value_;
  bool corrupt_ = false;
};

}  // namespace

// ---------------------------------------------------------------- BlockBuilder

BlockBuilder::BlockBuilder(int restart_interval) : restart_interval_(restart_interval) {
  restarts_.push_back(0);
}

void BlockBuilder::Reset() {
  buf_.clear();
  restarts_.assign(1, 0);
  counter_ = 0;
  counter_total_ = 0;
  last_key_.clear();
  finished_ = false;
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  assert(!finished_);
  size_t shared = 0;
  if (counter_ < restart_interval_) {
    size_t n = std::min(last_key_.size(), key.size());
    while (shared < n && last_key_[shared] == key[shared]) ++shared;
  } else {
    restarts_.push_back(static_cast<uint32_t>(buf_.size()));
    counter_ = 0;
  }
  size_t non_shared = key.size() - shared;
  PutVarint32(&buf_, static_cast<uint32_t>(shared));
  PutVarint32(&buf_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buf_, static_cast<uint32_t>(value.size()));
  buf_.append(key.data() + shared, non_shared);
  buf_.append(value.data(), value.size());
  last_key_.assign(key.data(), key.size());
  ++counter_;
  ++counter_total_;
}

std::string_view BlockBuilder::Finish() {
  for (uint32_t r : restarts_) PutFixed32(&buf_, r);
  PutFixed32(&buf_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return buf_;
}

// ---------------------------------------------------------------- TableBuilder

TableBuilder::TableBuilder(const Options& opts, int fd) : opts_(opts), fd_(fd) {
  if (opts_.bloom_bits_per_key > 0)
    filter_ = std::make_unique<BloomFilterBuilder>(opts_.bloom_bits_per_key);
}

void TableBuilder::Add(std::string_view ikey, std::string_view value) {
  if (!status_.ok()) return;
  std::string_view ukey = ExtractUserKey(ikey);
  if (filter_ && (entries_ == 0 || ukey != last_user_key_)) {
    filter_->AddKey(ukey);
    last_user_key_.assign(ukey.data(), ukey.size());
  }
  data_block_.Add(ikey, value);
  ++entries_;
  if (data_block_.CurrentSizeEstimate() >= opts_.block_size) FlushDataBlock();
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.Empty()) return;
  std::string last(data_block_.LastKey());
  BlockHandle h = WriteBlock(data_block_.Finish());
  data_block_.Reset();
  std::string enc;
  EncodeHandle(&enc, h);
  index_block_.Add(last, enc);
}

void TableBuilder::WriteRaw(std::string_view data) {
  out_buf_.append(data.data(), data.size());
  offset_ += data.size();
  if (out_buf_.size() >= (256 << 10)) {
    const char* p = out_buf_.data();
    size_t left = out_buf_.size();
    while (left > 0 && status_.ok()) {
      ssize_t n = ::write(fd_, p, left);
      if (n < 0) {
        if (errno == EINTR) continue;
        status_ = Errno("table write");
        break;
      }
      p += n;
      left -= static_cast<size_t>(n);
    }
    out_buf_.clear();
  }
}

BlockHandle TableBuilder::WriteBlock(std::string_view contents) {
  BlockHandle h{offset_, contents.size()};
  WriteRaw(contents);
  char trailer[4];
  EncodeFixed32(trailer, crc32::Mask(crc32::Value(contents.data(), contents.size())));
  WriteRaw(std::string_view(trailer, 4));
  return h;
}

Status TableBuilder::Finish() {
  FlushDataBlock();
  std::string filter = filter_ ? filter_->Finish() : std::string();
  BlockHandle fh = WriteBlock(filter);
  BlockHandle ih = WriteBlock(index_block_.Finish());

  std::string footer;
  PutFixed64(&footer, fh.offset);
  PutFixed64(&footer, fh.size);
  PutFixed64(&footer, ih.offset);
  PutFixed64(&footer, ih.size);
  PutFixed64(&footer, kTableMagic);
  WriteRaw(footer);

  // Drain the buffer.
  const char* p = out_buf_.data();
  size_t left = out_buf_.size();
  while (left > 0 && status_.ok()) {
    ssize_t n = ::write(fd_, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      status_ = Errno("table write");
      break;
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  out_buf_.clear();
  if (status_.ok() && ::fdatasync(fd_) != 0) status_ = Errno("table fdatasync");
  return status_;
}

// ---------------------------------------------------------------- Table

Table::~Table() {
  if (base_ != nullptr) ::munmap(const_cast<char*>(base_), size_);
}

Status Table::ReadBlock(const BlockHandle& h, bool verify, std::string_view* contents) const {
  if (h.offset > size_ || h.size + 4 > size_ - h.offset) return Status::Corruption("block handle");
  const char* p = base_ + h.offset;
  if (verify) {
    uint32_t expected = crc32::Unmask(DecodeFixed32(p + h.size));
    if (crc32::Value(p, h.size) != expected) return Status::Corruption("block checksum mismatch");
  }
  *contents = std::string_view(p, h.size);
  return Status::OK();
}

Status Table::Open(const std::string& path, uint64_t file_number, std::shared_ptr<Table>* out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return Errno("open " + path);
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Errno("fstat " + path);
  }
  size_t size = static_cast<size_t>(st.st_size);
  if (size < kFooterSize) {
    ::close(fd);
    return Status::Corruption("table too small: " + path);
  }
  void* map = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  ::close(fd);
  if (map == MAP_FAILED) return Errno("mmap " + path);
  ::madvise(map, size, MADV_RANDOM);

  std::shared_ptr<Table> t(new Table());
  t->file_number_ = file_number;
  t->base_ = static_cast<const char*>(map);
  t->size_ = size;

  const char* f = t->base_ + size - kFooterSize;
  if (DecodeFixed64(f + 32) != kTableMagic) return Status::Corruption("bad magic: " + path);
  BlockHandle fh{DecodeFixed64(f), DecodeFixed64(f + 8)};
  BlockHandle ih{DecodeFixed64(f + 16), DecodeFixed64(f + 24)};
  Status s = t->ReadBlock(fh, true, &t->filter_);
  if (s.ok()) s = t->ReadBlock(ih, true, &t->index_);
  if (!s.ok()) return s;
  *out = std::move(t);
  return Status::OK();
}

Table::GetResult Table::Get(const ReadOptions& ro, std::string_view user_key, uint64_t hash,
                            std::string_view lookup_key, std::string* value, Status* s) const {
  if (!filter_.empty() && !BloomMayContainHash(filter_, hash)) return GetResult::kFilteredOut;

  BlockIterator index(index_);
  index.Seek(lookup_key);
  if (!index.Valid()) {
    *s = index.status();
    return GetResult::kNotPresent;
  }
  BlockHandle h;
  if (!DecodeHandle(index.value(), &h)) {
    *s = Status::Corruption("bad index entry");
    return GetResult::kNotPresent;
  }
  std::string_view block;
  *s = ReadBlock(h, ro.verify_checksums, &block);
  if (!s->ok()) return GetResult::kNotPresent;

  BlockIterator it(block);
  it.Seek(lookup_key);
  if (!it.Valid()) {
    *s = it.status();
    return GetResult::kNotPresent;
  }
  if (ExtractUserKey(it.key()) != user_key) return GetResult::kNotPresent;
  if (ExtractType(it.key()) == ValueType::kDeletion) return GetResult::kDeleted;
  value->assign(it.value().data(), it.value().size());
  return GetResult::kFound;
}

// Two-level iterator: index block -> data blocks.
class TableIterator final : public Iterator {
 public:
  TableIterator(const Table* t, const ReadOptions& ro) : table_(t), ro_(ro), index_(t->index_) {}

  bool Valid() const override { return data_ && data_->Valid(); }
  std::string_view key() const override { return data_->key(); }
  std::string_view value() const override { return data_->value(); }
  Status status() const override {
    if (!status_.ok()) return status_;
    if (!index_.status().ok()) return index_.status();
    return data_ ? data_->status() : Status::OK();
  }

  void SeekToFirst() override {
    index_.SeekToFirst();
    InitDataBlock();
    if (data_) data_->SeekToFirst();
    SkipEmptyBlocks();
  }

  void Seek(std::string_view target) override {
    index_.Seek(target);
    InitDataBlock();
    if (data_) data_->Seek(target);
    SkipEmptyBlocks();
  }

  void Next() override {
    data_->Next();
    SkipEmptyBlocks();
  }

 private:
  void InitDataBlock() {
    data_.reset();
    if (!index_.Valid()) return;
    BlockHandle h;
    if (!DecodeHandle(index_.value(), &h)) {
      status_ = Status::Corruption("bad index entry");
      return;
    }
    std::string_view block;
    Status s = table_->ReadBlock(h, ro_.verify_checksums, &block);
    if (!s.ok()) {
      status_ = s;
      return;
    }
    data_ = std::make_unique<BlockIterator>(block);
  }

  void SkipEmptyBlocks() {
    while (data_ == nullptr || !data_->Valid()) {
      if (!status_.ok() || (data_ && !data_->status().ok())) return;
      if (!index_.Valid()) {
        data_.reset();
        return;
      }
      index_.Next();
      InitDataBlock();
      if (data_) data_->SeekToFirst();
      else if (!index_.Valid()) return;
    }
  }

  const Table* table_;
  ReadOptions ro_;
  BlockIterator index_;
  std::unique_ptr<BlockIterator> data_;
  Status status_;
};

std::unique_ptr<Iterator> Table::NewIterator(const ReadOptions& ro) const {
  return std::make_unique<TableIterator>(this, ro);
}

}  // namespace lsm
