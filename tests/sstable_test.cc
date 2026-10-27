#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <map>
#include <string>

#include "bloom.h"
#include "dbformat.h"
#include "sstable.h"

using namespace lsm;

namespace {
std::string Key(int i) {
  char b[16];
  std::snprintf(b, sizeof b, "k%08d", i);
  return b;
}
}  // namespace

class SSTableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / ("sst_" + std::to_string(::getpid()))).string();
    Options o;
    o.block_size = 512;  // many blocks
    int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TableBuilder b(o, fd);
    for (int i = 0; i < 5000; i += 2) {  // even keys only
      std::string v = "v" + std::to_string(i);
      // Two versions of each key: newer seq sorts first.
      b.Add(MakeInternalKey(Key(i), 2 * i + 2, i % 10 == 0 ? ValueType::kDeletion : ValueType::kValue),
            i % 10 == 0 ? "" : v);
      b.Add(MakeInternalKey(Key(i), 2 * i + 1, ValueType::kValue), "old");
    }
    ASSERT_TRUE(b.Finish().ok());
    ::close(fd);
    ASSERT_TRUE(Table::Open(path_, 1, &table_).ok());
  }
  void TearDown() override { std::filesystem::remove(path_); }

  Table::GetResult Get(const std::string& k, std::string* v, SequenceNumber snap = kMaxSequenceNumber) {
    Status s;
    auto r = table_->Get({}, k, Hash64(k), MakeLookupKey(k, snap), v, &s);
    EXPECT_TRUE(s.ok());
    return r;
  }

  std::string path_;
  std::shared_ptr<Table> table_;
};

TEST_F(SSTableTest, PointLookups) {
  std::string v;
  EXPECT_EQ(Get(Key(4), &v), Table::GetResult::kFound);
  EXPECT_EQ(v, "v4");
  EXPECT_EQ(Get(Key(4998), &v), Table::GetResult::kFound);
  EXPECT_EQ(Get(Key(10), &v), Table::GetResult::kDeleted);
  auto r = Get(Key(3), &v);  // odd keys absent
  EXPECT_TRUE(r == Table::GetResult::kNotPresent || r == Table::GetResult::kFilteredOut);
  EXPECT_EQ(Get(Key(4), &v, 9), Table::GetResult::kFound);  // seq 9 = older version
  EXPECT_EQ(v, "old");
}

TEST_F(SSTableTest, BloomFiltersMostMisses) {
  int filtered = 0;
  std::string v;
  for (int i = 1; i < 5000; i += 2) filtered += Get(Key(i), &v) == Table::GetResult::kFilteredOut;
  EXPECT_GT(filtered, 2400);  // of 2500 misses
}

TEST_F(SSTableTest, IteratorVisitsAllInOrder) {
  auto it = table_->NewIterator({});
  int n = 0;
  std::string prev;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (!prev.empty()) EXPECT_LT(CompareInternalKey(prev, it->key()), 0);
    prev.assign(it->key());
    ++n;
  }
  EXPECT_EQ(n, 5000);
  it->Seek(MakeLookupKey(Key(1001), kMaxSequenceNumber));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()), Key(1002));
  it->Seek(MakeLookupKey(Key(9999), kMaxSequenceNumber));
  EXPECT_FALSE(it->Valid());
}

TEST_F(SSTableTest, DetectsCorruption) {
  {
    int fd = ::open(path_.c_str(), O_WRONLY);
    ::pwrite(fd, "XXXX", 4, 100);
    ::close(fd);
  }
  std::shared_ptr<Table> t;
  ASSERT_TRUE(Table::Open(path_, 1, &t).ok());  // footer/index intact
  ReadOptions ro;
  ro.verify_checksums = true;
  Status s;
  std::string v, k = Key(0);
  t->Get(ro, Key(2), Hash64(Key(2)), MakeLookupKey(Key(2), kMaxSequenceNumber), &v, &s);
  EXPECT_TRUE(s.IsCorruption());
}
