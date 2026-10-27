// DB implementation: write path (WAL + MemTable), read path (MemTable ->
// immutable MemTable -> L0 -> L1..L6), background flush and leveled
// compaction, and crash recovery.
//
// Concurrency
//   writer_mu_  serialises writers (WAL append + MemTable insert).
//   mu_         guards mem_/imm_/current_/background state; held only for
//               pointer swaps, never across I/O.
//   Readers take mu_ just long enough to reference mem_, imm_ and current_,
//   then search lock-free (skip list reads are lock-free; tables are immutable).
//   A single background thread performs flushes and compactions and is the
//   only thread that installs new Versions.

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "bloom.h"
#include "coding.h"
#include "dbformat.h"
#include "lsm/db.h"
#include "memtable.h"
#include "merger.h"
#include "sstable.h"
#include "version.h"
#include "wal.h"

namespace lsm {

void WriteBatch::Put(std::string_view key, std::string_view value) {
  ops_.push_back({false, std::string(key), std::string(value)});
}

void WriteBatch::Delete(std::string_view key) { ops_.push_back({true, std::string(key), {}}); }

namespace {

Status Errno(const std::string& ctx) { return Status::IOError(ctx + ": " + std::strerror(errno)); }

enum class FileType { kTable, kWal, kOther };

FileType ParseFileName(const std::string& name, uint64_t* number) {
  auto dot = name.find('.');
  if (dot == std::string::npos || dot == 0) return FileType::kOther;
  for (size_t i = 0; i < dot; ++i)
    if (name[i] < '0' || name[i] > '9') return FileType::kOther;
  *number = std::stoull(name.substr(0, dot));
  std::string ext = name.substr(dot + 1);
  if (ext == "sst") return FileType::kTable;
  if (ext == "wal") return FileType::kWal;
  return FileType::kOther;
}

std::vector<std::string> ListDir(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return out;
  while (dirent* e = ::readdir(d)) out.emplace_back(e->d_name);
  ::closedir(d);
  return out;
}

bool UserKeyInRange(std::string_view ukey, const FileMeta& f) {
  return ukey >= ExtractUserKey(f.smallest) && ukey <= ExtractUserKey(f.largest);
}

bool RangesOverlap(std::string_view lo, std::string_view hi, const FileMeta& f) {
  return !(ExtractUserKey(f.largest) < lo || ExtractUserKey(f.smallest) > hi);
}

}  // namespace

class DBImpl final : public DB {
 public:
  DBImpl(const Options& o, std::string name) : options_(o), dbname_(std::move(name)), versions_(dbname_) {}
  ~DBImpl() override;

  Status Recover();
  void StartBackground() { bg_thread_ = std::thread([this] { BackgroundLoop(); }); }

  Status Put(const WriteOptions& o, std::string_view k, std::string_view v) override {
    WriteBatch b;
    b.Put(k, v);
    return Write(o, b);
  }
  Status Delete(const WriteOptions& o, std::string_view k) override {
    WriteBatch b;
    b.Delete(k);
    return Write(o, b);
  }
  Status Write(const WriteOptions& o, const WriteBatch& batch) override;
  Status Get(const ReadOptions& o, std::string_view key, std::string* value) override;
  Status Scan(const ReadOptions& o, std::string_view start, std::string_view end, size_t limit,
              std::vector<std::pair<std::string, std::string>>* out) override;
  Status Flush() override;
  Status CompactAll() override;
  DBStats GetStats() override;

 private:
  struct Compaction {
    std::vector<std::pair<int, std::shared_ptr<FileMeta>>> inputs;
    int output_level = 1;
  };

  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force);
  Status NewWal(uint64_t* number, std::unique_ptr<WalWriter>* w);
  Status BuildTable(Iterator* it, std::shared_ptr<FileMeta>* out);
  Status ReplayWal(uint64_t number, MemTable** mem, SequenceNumber* max_seq,
                   std::vector<std::shared_ptr<FileMeta>>* l0);

  void BackgroundLoop();
  bool NeedsCompaction(const Version& v) const;
  std::optional<Compaction> PickCompaction(const Version& v);
  Status FlushImmutable();
  Status RunCompaction(const Compaction& c);
  bool IsBaseLevelForKey(const Version& v, int output_level, std::string_view ukey,
                         const std::set<uint64_t>& inputs) const;
  Status InstallVersion(std::shared_ptr<Version> v, uint64_t log_number);
  void DeleteObsoleteFiles();
  uint64_t MaxBytesForLevel(int level) const;

  const Options options_;
  const std::string dbname_;
  int lock_fd_ = -1;

  std::mutex writer_mu_;
  std::mutex mu_;
  std::condition_variable bg_cv_;    // wakes the background thread
  std::condition_variable done_cv_;  // background progress (writers, Flush)

  MemTable* mem_ = nullptr;
  MemTable* imm_ = nullptr;
  std::shared_ptr<const Version> current_ = std::make_shared<Version>();
  std::unique_ptr<WalWriter> wal_;
  uint64_t wal_number_ = 0;
  uint64_t log_number_ = 0;  // persisted: oldest WAL still needed
  std::atomic<uint64_t> next_file_number_{1};
  std::atomic<SequenceNumber> last_sequence_{0};

  VersionSet versions_;  // touched only by the background thread / Open
  std::thread bg_thread_;
  bool shutting_down_ = false;
  bool manual_compaction_ = false;
  bool bg_busy_ = false;
  Status bg_error_;
  std::set<uint64_t> pending_outputs_;
  int compact_pointer_[kNumLevels] = {};

  std::atomic<uint64_t> flushes_{0}, compactions_{0}, bloom_negatives_{0}, table_probes_{0};
  uint64_t recovered_records_ = 0;
  double recovery_seconds_ = 0;
  std::string scratch_;
};

// ---------------------------------------------------------------- lifecycle

Status DB::Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db) {
  auto impl = std::make_unique<DBImpl>(options, path);
  Status s = impl->Recover();
  if (!s.ok()) return s;
  impl->StartBackground();
  *db = std::move(impl);
  return Status::OK();
}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> l(mu_);
    shutting_down_ = true;
  }
  bg_cv_.notify_all();
  if (bg_thread_.joinable()) bg_thread_.join();
  if (wal_) wal_->Close();
  if (mem_) mem_->Unref();
  if (imm_) imm_->Unref();
  if (lock_fd_ >= 0) {
    ::flock(lock_fd_, LOCK_UN);
    ::close(lock_fd_);
  }
}

Status DBImpl::NewWal(uint64_t* number, std::unique_ptr<WalWriter>* w) {
  *number = next_file_number_.fetch_add(1);
  auto writer = std::make_unique<WalWriter>();
  Status s = writer->Open(WalFileName(dbname_, *number));
  if (s.ok()) *w = std::move(writer);
  return s;
}

Status DBImpl::Recover() {
  auto t0 = std::chrono::steady_clock::now();
  if (::mkdir(dbname_.c_str(), 0755) != 0 && errno != EEXIST) return Errno("mkdir " + dbname_);
  if (!options_.create_if_missing && ::access(ManifestFileName(dbname_).c_str(), F_OK) != 0)
    return Status::InvalidArgument("database does not exist: " + dbname_);

  lock_fd_ = ::open((dbname_ + "/LOCK").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock_fd_ < 0) return Errno("open LOCK");
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) return Status::IOError("database is locked by another process");

  bool existed = false;
  Status s = versions_.Recover(&existed);
  if (!s.ok()) return s;
  ManifestState st = versions_.state();
  auto version = std::make_shared<Version>(*versions_.current());

  // Find WALs that still need replay and the highest file number in use.
  std::vector<uint64_t> wals;
  uint64_t max_number = st.next_file_number;
  for (const auto& name : ListDir(dbname_)) {
    uint64_t n;
    FileType t = ParseFileName(name, &n);
    if (t == FileType::kOther) continue;
    max_number = std::max(max_number, n + 1);
    if (t == FileType::kWal && n >= st.log_number) wals.push_back(n);
  }
  std::sort(wals.begin(), wals.end());
  next_file_number_ = max_number;

  SequenceNumber max_seq = st.last_sequence;
  MemTable* mem = new MemTable();
  mem->Ref();
  std::vector<std::shared_ptr<FileMeta>> new_l0;
  for (uint64_t n : wals) {
    s = ReplayWal(n, &mem, &max_seq, &new_l0);
    if (!s.ok()) {
      mem->Unref();
      return s;
    }
  }
  if (!mem->Empty()) {
    std::shared_ptr<FileMeta> f;
    auto it = mem->NewIterator();
    s = BuildTable(it.get(), &f);
    if (!s.ok()) {
      mem->Unref();
      return s;
    }
    new_l0.push_back(f);
  }
  mem->Unref();

  // Newest L0 file first.
  for (auto& f : new_l0) version->files[0].insert(version->files[0].begin(), f);
  last_sequence_ = max_seq;

  s = NewWal(&wal_number_, &wal_);
  if (!s.ok()) return s;
  log_number_ = wal_number_;
  st.last_sequence = max_seq;
  st.log_number = log_number_;
  st.next_file_number = next_file_number_;
  s = versions_.LogAndApply(version, st);
  if (!s.ok()) return s;
  current_ = versions_.current();

  mem_ = new MemTable();
  mem_->Ref();
  DeleteObsoleteFiles();
  recovery_seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return Status::OK();
}

// WAL payload: fixed64 first_seq | varint count | count * (type | key | [value]).
Status DBImpl::ReplayWal(uint64_t number, MemTable** mem, SequenceNumber* max_seq,
                         std::vector<std::shared_ptr<FileMeta>>* l0) {
  WalReadStats stats;
  Status s = ReadWal(
      WalFileName(dbname_, number),
      [&](std::string_view rec) -> Status {
        if (rec.size() < 8) return Status::Corruption("wal record too small");
        SequenceNumber seq = DecodeFixed64(rec.data());
        rec.remove_prefix(8);
        uint64_t count;
        if (!GetVarint64(&rec, &count)) return Status::Corruption("wal count");
        for (uint64_t i = 0; i < count; ++i, ++seq) {
          if (rec.empty()) return Status::Corruption("wal op");
          auto type = static_cast<ValueType>(rec[0]);
          rec.remove_prefix(1);
          std::string_view key, value;
          if (!GetLengthPrefixed(&rec, &key)) return Status::Corruption("wal key");
          if (type == ValueType::kValue && !GetLengthPrefixed(&rec, &value))
            return Status::Corruption("wal value");
          (*mem)->Add(seq, type, key, value);
        }
        *max_seq = std::max(*max_seq, seq - 1);
        ++recovered_records_;
        if ((*mem)->ApproximateSize() >= options_.memtable_size) {
          std::shared_ptr<FileMeta> f;
          auto it = (*mem)->NewIterator();
          Status bs = BuildTable(it.get(), &f);
          it.reset();
          if (!bs.ok()) return bs;
          l0->push_back(f);
          (*mem)->Unref();
          *mem = new MemTable();
          (*mem)->Ref();
        }
        return Status::OK();
      },
      &stats);
  return s;
}

// ---------------------------------------------------------------- writes

Status DBImpl::Write(const WriteOptions& o, const WriteBatch& batch) {
  if (batch.Count() == 0) return Status::OK();
  std::lock_guard<std::mutex> wl(writer_mu_);
  {
    std::unique_lock<std::mutex> lock(mu_);
    Status s = MakeRoomForWrite(lock, false);
    if (!s.ok()) return s;
  }

  SequenceNumber first = last_sequence_.load(std::memory_order_relaxed) + 1;
  scratch_.clear();
  PutFixed64(&scratch_, first);
  PutVarint64(&scratch_, batch.ops_.size());
  for (const auto& op : batch.ops_) {
    scratch_.push_back(static_cast<char>(op.is_delete ? ValueType::kDeletion : ValueType::kValue));
    PutLengthPrefixed(&scratch_, op.key);
    if (!op.is_delete) PutLengthPrefixed(&scratch_, op.value);
  }
  Status s = wal_->AddRecord(scratch_);
  if (s.ok() && (o.sync || options_.sync_wal)) s = wal_->Sync();
  if (!s.ok()) return s;

  SequenceNumber seq = first;
  for (const auto& op : batch.ops_) {
    mem_->Add(seq++, op.is_delete ? ValueType::kDeletion : ValueType::kValue, op.key, op.value);
  }
  last_sequence_.store(seq - 1, std::memory_order_release);
  return Status::OK();
}

// REQUIRES: writer_mu_ held and `lock` holds mu_.
Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force) {
  while (true) {
    if (!bg_error_.ok()) return bg_error_;
    if (static_cast<int>(current_->files[0].size()) >= options_.l0_stop_writes_trigger) {
      bg_cv_.notify_one();
      done_cv_.wait(lock);
    } else if (!force && mem_->ApproximateSize() < options_.memtable_size) {
      return Status::OK();
    } else if (imm_ != nullptr) {
      done_cv_.wait(lock);
    } else if (force && mem_->Empty()) {
      return Status::OK();
    } else {
      uint64_t number;
      std::unique_ptr<WalWriter> w;
      Status s = NewWal(&number, &w);
      if (!s.ok()) return s;
      wal_->Close();
      wal_ = std::move(w);
      wal_number_ = number;
      imm_ = mem_;
      mem_ = new MemTable();
      mem_->Ref();
      bg_cv_.notify_one();
      return Status::OK();
    }
  }
}

// ---------------------------------------------------------------- reads

Status DBImpl::Get(const ReadOptions& o, std::string_view key, std::string* value) {
  MemTable* mem;
  MemTable* imm;
  std::shared_ptr<const Version> v;
  {
    std::lock_guard<std::mutex> l(mu_);
    mem = mem_;
    imm = imm_;
    mem->Ref();
    if (imm) imm->Ref();
    v = current_;
  }
  SequenceNumber snapshot = last_sequence_.load(std::memory_order_acquire);
  struct Unref {
    MemTable* m;
    MemTable* i;
    ~Unref() {
      m->Unref();
      if (i) i->Unref();
    }
  } unref{mem, imm};

  auto r = mem->Get(key, snapshot, value);
  if (r == MemTable::LookupResult::kNotPresent && imm) r = imm->Get(key, snapshot, value);
  if (r == MemTable::LookupResult::kFound) return Status::OK();
  if (r == MemTable::LookupResult::kDeleted) return Status::NotFound();

  std::string lookup = MakeLookupKey(key, snapshot);
  uint64_t hash = Hash64(key);
  Status s;
  auto probe = [&](const FileMeta& f) -> std::optional<Status> {
    auto res = f.table->Get(o, key, hash, lookup, value, &s);
    if (res == Table::GetResult::kFilteredOut) {
      bloom_negatives_.fetch_add(1, std::memory_order_relaxed);
      return std::nullopt;
    }
    table_probes_.fetch_add(1, std::memory_order_relaxed);
    if (!s.ok()) return s;
    if (res == Table::GetResult::kFound) return Status::OK();
    if (res == Table::GetResult::kDeleted) return Status::NotFound();
    return std::nullopt;
  };

  for (const auto& f : v->files[0]) {
    if (!UserKeyInRange(key, *f)) continue;
    if (auto r0 = probe(*f)) return *r0;
  }
  for (int level = 1; level < kNumLevels; ++level) {
    const auto& files = v->files[level];
    if (files.empty()) continue;
    // First file whose largest user key >= key.
    auto it = std::lower_bound(files.begin(), files.end(), key,
                               [](const std::shared_ptr<FileMeta>& f, std::string_view k) {
                                 return ExtractUserKey(f->largest) < k;
                               });
    if (it == files.end() || ExtractUserKey((*it)->smallest) > key) continue;
    if (auto rl = probe(**it)) return *rl;
  }
  return Status::NotFound();
}

Status DBImpl::Scan(const ReadOptions& o, std::string_view start, std::string_view end,
                    size_t limit, std::vector<std::pair<std::string, std::string>>* out) {
  out->clear();
  MemTable* mem;
  MemTable* imm;
  std::shared_ptr<const Version> v;
  {
    std::lock_guard<std::mutex> l(mu_);
    mem = mem_;
    imm = imm_;
    mem->Ref();
    if (imm) imm->Ref();
    v = current_;
  }
  SequenceNumber snapshot = last_sequence_.load(std::memory_order_acquire);

  Status s;
  {
    std::vector<std::unique_ptr<Iterator>> children;
    children.push_back(mem->NewIterator());
    if (imm) children.push_back(imm->NewIterator());
    for (int level = 0; level < kNumLevels; ++level) {
      for (const auto& f : v->files[level]) {
        if (!end.empty() && ExtractUserKey(f->smallest) >= end) continue;
        if (ExtractUserKey(f->largest) < start) continue;
        children.push_back(f->table->NewIterator(o));
      }
    }
    auto it = NewMergingIterator(std::move(children));
    it->Seek(MakeLookupKey(start, snapshot));
    std::string last_key;
    bool have_last = false;
    for (; it->Valid(); it->Next()) {
      std::string_view ik = it->key();
      std::string_view uk = ExtractUserKey(ik);
      if (!end.empty() && uk >= end) break;
      if (ExtractSequence(ik) > snapshot) continue;
      if (have_last && uk == last_key) continue;  // older version
      last_key.assign(uk);
      have_last = true;
      if (ExtractType(ik) == ValueType::kDeletion) continue;
      out->emplace_back(std::string(uk), std::string(it->value()));
      if (limit != 0 && out->size() >= limit) break;
    }
    s = it->status();
  }
  mem->Unref();
  if (imm) imm->Unref();
  return s;
}

// ---------------------------------------------------------------- background

uint64_t DBImpl::MaxBytesForLevel(int level) const {
  uint64_t b = options_.max_bytes_level1;
  for (int l = 1; l < level; ++l) b *= static_cast<uint64_t>(options_.level_size_multiplier);
  return b;
}

bool DBImpl::NeedsCompaction(const Version& v) const {
  if (static_cast<int>(v.files[0].size()) >= options_.l0_compaction_trigger) return true;
  for (int level = 1; level < kNumLevels - 1; ++level)
    if (v.LevelBytes(level) > MaxBytesForLevel(level)) return true;
  return false;
}

std::optional<DBImpl::Compaction> DBImpl::PickCompaction(const Version& v) {
  Compaction c;
  int level = -1;
  if (static_cast<int>(v.files[0].size()) >= options_.l0_compaction_trigger) {
    level = 0;
    for (const auto& f : v.files[0]) c.inputs.emplace_back(0, f);
  } else {
    double best = 1.0;
    for (int l = 1; l < kNumLevels - 1; ++l) {
      double score = static_cast<double>(v.LevelBytes(l)) / static_cast<double>(MaxBytesForLevel(l));
      if (score > best) {
        best = score;
        level = l;
      }
    }
    if (level < 0) return std::nullopt;
    // Round-robin over the level's key space so compactions spread evenly.
    const auto& files = v.files[level];
    int idx = compact_pointer_[level] % static_cast<int>(files.size());
    compact_pointer_[level] = idx + 1;
    c.inputs.emplace_back(level, files[idx]);
  }
  c.output_level = level + 1;

  // Key range of the inputs, then pull in every overlapping file of the next level.
  std::string lo, hi;
  for (const auto& [l, f] : c.inputs) {
    std::string_view s = ExtractUserKey(f->smallest), e = ExtractUserKey(f->largest);
    if (lo.empty() && hi.empty()) {
      lo = s;
      hi = e;
    } else {
      if (s < lo) lo = s;
      if (e > hi) hi = e;
    }
  }
  for (const auto& f : v.files[c.output_level])
    if (RangesOverlap(lo, hi, *f)) c.inputs.emplace_back(c.output_level, f);
  return c;
}

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    bg_cv_.wait(lock, [&] {
      return shutting_down_ || imm_ != nullptr || manual_compaction_ ||
             (bg_error_.ok() && NeedsCompaction(*current_));
    });
    if (shutting_down_) break;
    if (!bg_error_.ok()) {
      manual_compaction_ = false;
      done_cv_.notify_all();
      continue;
    }

    bg_busy_ = true;
    Status s;
    if (imm_ != nullptr) {
      lock.unlock();
      s = FlushImmutable();
      lock.lock();
    } else if (manual_compaction_) {
      // Merge every table into a single sorted run at the deepest used level.
      Compaction c;
      c.output_level = 1;
      for (int l = 0; l < kNumLevels; ++l) {
        for (const auto& f : current_->files[l]) c.inputs.emplace_back(l, f);
        if (!current_->files[l].empty()) c.output_level = std::max(c.output_level, l);
      }
      manual_compaction_ = false;
      if (!c.inputs.empty()) {
        lock.unlock();
        s = RunCompaction(c);
        lock.lock();
      }
    } else {
      auto c = PickCompaction(*current_);
      if (c) {
        lock.unlock();
        s = RunCompaction(*c);
        lock.lock();
      }
    }
    bg_busy_ = false;
    if (!s.ok()) bg_error_ = s;
    done_cv_.notify_all();
  }
}

Status DBImpl::BuildTable(Iterator* it, std::shared_ptr<FileMeta>* out) {
  auto meta = std::make_shared<FileMeta>();
  meta->number = next_file_number_.fetch_add(1);
  std::string path = TableFileName(dbname_, meta->number);
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return Errno("create " + path);
  TableBuilder builder(options_, fd);
  it->SeekToFirst();
  bool first = true;
  for (; it->Valid(); it->Next()) {
    if (first) {
      meta->smallest.assign(it->key());
      first = false;
    }
    meta->largest.assign(it->key());
    builder.Add(it->key(), it->value());
  }
  Status s = builder.Finish();
  ::close(fd);
  if (s.ok()) s = Table::Open(path, meta->number, &meta->table);
  if (!s.ok()) {
    ::unlink(path.c_str());
    return s;
  }
  meta->file_size = builder.FileSize();
  *out = std::move(meta);
  flushes_++;
  return Status::OK();
}

Status DBImpl::InstallVersion(std::shared_ptr<Version> v, uint64_t log_number) {
  ManifestState st;
  st.next_file_number = next_file_number_.load();
  st.last_sequence = last_sequence_.load();
  st.log_number = log_number;
  Status s = versions_.LogAndApply(v, st);
  if (!s.ok()) return s;
  std::lock_guard<std::mutex> l(mu_);
  current_ = versions_.current();
  log_number_ = log_number;
  return Status::OK();
}

Status DBImpl::FlushImmutable() {
  MemTable* imm;
  uint64_t new_log_number;
  {
    std::lock_guard<std::mutex> l(mu_);
    imm = imm_;
    new_log_number = wal_number_;  // every WAL older than the live one is covered by imm
  }
  std::shared_ptr<FileMeta> f;
  {
    auto it = imm->NewIterator();
    Status s = BuildTable(it.get(), &f);
    if (!s.ok()) return s;
  }
  std::shared_ptr<Version> v;
  {
    std::lock_guard<std::mutex> l(mu_);
    v = std::make_shared<Version>(*current_);
  }
  v->files[0].insert(v->files[0].begin(), f);
  Status s = InstallVersion(v, new_log_number);
  if (!s.ok()) return s;
  {
    std::lock_guard<std::mutex> l(mu_);
    imm_ = nullptr;
  }
  imm->Unref();
  DeleteObsoleteFiles();
  return Status::OK();
}

bool DBImpl::IsBaseLevelForKey(const Version& v, int output_level, std::string_view ukey,
                               const std::set<uint64_t>& inputs) const {
  for (int l = output_level + 1; l < kNumLevels; ++l)
    for (const auto& f : v.files[l])
      if (!inputs.count(f->number) && UserKeyInRange(ukey, *f)) return false;
  return true;
}

Status DBImpl::RunCompaction(const Compaction& c) {
  std::shared_ptr<const Version> base;
  {
    std::lock_guard<std::mutex> l(mu_);
    base = current_;
  }
  std::set<uint64_t> input_numbers;
  std::vector<std::unique_ptr<Iterator>> children;
  for (const auto& [l, f] : c.inputs) {
    input_numbers.insert(f->number);
    children.push_back(f->table->NewIterator(ReadOptions{}));
  }
  auto it = NewMergingIterator(std::move(children));

  std::vector<std::shared_ptr<FileMeta>> outputs;
  std::unique_ptr<TableBuilder> builder;
  std::shared_ptr<FileMeta> cur;
  int fd = -1;
  Status s;

  auto finish_output = [&]() -> Status {
    Status fs = builder->Finish();
    ::close(fd);
    fd = -1;
    std::string path = TableFileName(dbname_, cur->number);
    if (fs.ok()) fs = Table::Open(path, cur->number, &cur->table);
    if (!fs.ok()) return fs;
    cur->file_size = builder->FileSize();
    outputs.push_back(cur);
    builder.reset();
    cur.reset();
    return Status::OK();
  };

  std::string last_user_key;
  bool have_last = false;
  for (it->SeekToFirst(); it->Valid() && s.ok(); it->Next()) {
    std::string_view ik = it->key();
    std::string_view uk = ExtractUserKey(ik);
    bool same_as_last = have_last && uk == last_user_key;
    if (!same_as_last) {
      last_user_key.assign(uk);
      have_last = true;
    }
    // No snapshots are exposed, so only the newest version of a key matters.
    if (same_as_last) continue;
    if (ExtractType(ik) == ValueType::kDeletion &&
        IsBaseLevelForKey(*base, c.output_level, uk, input_numbers))
      continue;

    // Split output files only on user-key boundaries (guaranteed here since
    // each user key emits at most one entry).
    if (builder && builder->FileSize() >= options_.target_file_size) s = finish_output();
    if (!s.ok()) break;
    if (!builder) {
      cur = std::make_shared<FileMeta>();
      cur->number = next_file_number_.fetch_add(1);
      std::string path = TableFileName(dbname_, cur->number);
      fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
      if (fd < 0) {
        s = Errno("create " + path);
        break;
      }
      builder = std::make_unique<TableBuilder>(options_, fd);
      cur->smallest.assign(ik);
    }
    cur->largest.assign(ik);
    builder->Add(ik, it->value());
  }
  if (s.ok()) s = it->status();
  if (s.ok() && builder) s = finish_output();
  if (fd >= 0) ::close(fd);
  if (!s.ok()) {
    for (const auto& f : outputs) ::unlink(TableFileName(dbname_, f->number).c_str());
    return s;
  }

  std::shared_ptr<Version> v;
  uint64_t log_number;
  {
    std::lock_guard<std::mutex> l(mu_);
    v = std::make_shared<Version>(*current_);
    log_number = log_number_;
  }
  for (int l = 0; l < kNumLevels; ++l) {
    auto& files = v->files[l];
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const auto& f) { return input_numbers.count(f->number) > 0; }),
                files.end());
  }
  auto& out_files = v->files[c.output_level];
  out_files.insert(out_files.end(), outputs.begin(), outputs.end());
  std::sort(out_files.begin(), out_files.end(), [](const auto& a, const auto& b) {
    return CompareInternalKey(a->smallest, b->smallest) < 0;
  });
  s = InstallVersion(v, log_number);
  if (!s.ok()) return s;
  compactions_++;
  DeleteObsoleteFiles();
  return Status::OK();
}

// Removes tables not referenced by the current version and WALs older than
// log_number_. Unlinking a table that a reader still has mmapped is safe: the
// mapping keeps the inode alive until the last reference drops.
void DBImpl::DeleteObsoleteFiles() {
  std::set<uint64_t> live;
  uint64_t log_number;
  uint64_t in_flight_floor;
  {
    std::lock_guard<std::mutex> l(mu_);
    for (int level = 0; level < kNumLevels; ++level)
      for (const auto& f : current_->files[level]) live.insert(f->number);
    log_number = log_number_;
    in_flight_floor = next_file_number_.load();
  }
  for (const auto& name : ListDir(dbname_)) {
    uint64_t n;
    FileType t = ParseFileName(name, &n);
    bool remove = (t == FileType::kTable && !live.count(n) && n < in_flight_floor) ||
                  (t == FileType::kWal && n < log_number);
    if (remove) ::unlink((dbname_ + "/" + name).c_str());
  }
}

// ---------------------------------------------------------------- admin

Status DBImpl::Flush() {
  {
    std::lock_guard<std::mutex> wl(writer_mu_);
    std::unique_lock<std::mutex> lock(mu_);
    Status s = MakeRoomForWrite(lock, true);
    if (!s.ok()) return s;
  }
  std::unique_lock<std::mutex> lock(mu_);
  done_cv_.wait(lock, [&] {
    return !bg_error_.ok() || (imm_ == nullptr && !bg_busy_ && !NeedsCompaction(*current_));
  });
  return bg_error_;
}

Status DBImpl::CompactAll() {
  Status s = Flush();
  if (!s.ok()) return s;
  std::unique_lock<std::mutex> lock(mu_);
  manual_compaction_ = true;
  bg_cv_.notify_one();
  done_cv_.wait(lock, [&] { return !bg_error_.ok() || (!manual_compaction_ && !bg_busy_); });
  return bg_error_;
}

DBStats DBImpl::GetStats() {
  DBStats st;
  std::lock_guard<std::mutex> l(mu_);
  st.memtable_bytes = mem_->ApproximateSize();
  for (int level = 0; level < kNumLevels; ++level) {
    st.files_per_level[level] = static_cast<int>(current_->files[level].size());
    st.bytes_per_level[level] = current_->LevelBytes(level);
  }
  st.flushes = flushes_;
  st.compactions = compactions_;
  st.bloom_negatives = bloom_negatives_;
  st.table_probes = table_probes_;
  st.recovered_records = recovered_records_;
  st.recovery_seconds = recovery_seconds_;
  return st;
}

}  // namespace lsm
