#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <random>
#include <thread>

#include "lsm/db.h"

using namespace lsm;

namespace {

std::string Key(uint64_t i) {
  char b[24];
  std::snprintf(b, sizeof b, "key%010llu", static_cast<unsigned long long>(i));
  return b;
}

class DBTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = (std::filesystem::temp_directory_path() /
            ("lsm_dbtest_" + std::to_string(::getpid()) + "_" +
             ::testing::UnitTest::GetInstance()->current_test_info()->name()))
               .string();
    std::filesystem::remove_all(dir_);
    // Small sizes so flushes and multi-level compactions happen quickly.
    opts_.memtable_size = 64 << 10;
    opts_.target_file_size = 32 << 10;
    opts_.max_bytes_level1 = 128 << 10;
    opts_.block_size = 1024;
    Reopen();
  }
  void TearDown() override {
    db_.reset();
    std::filesystem::remove_all(dir_);
  }
  void Reopen() {
    db_.reset();
    ASSERT_TRUE(DB::Open(opts_, dir_, &db_).ok());
  }
  std::string Get(const std::string& k) {
    std::string v;
    Status s = db_->Get({}, k, &v);
    return s.ok() ? v : (s.IsNotFound() ? "NOT_FOUND" : s.ToString());
  }

  std::string dir_;
  Options opts_;
  std::unique_ptr<DB> db_;
};

}  // namespace

TEST_F(DBTest, PutGetDelete) {
  ASSERT_TRUE(db_->Put({}, "foo", "bar").ok());
  EXPECT_EQ(Get("foo"), "bar");
  ASSERT_TRUE(db_->Put({}, "foo", "baz").ok());
  EXPECT_EQ(Get("foo"), "baz");
  ASSERT_TRUE(db_->Delete({}, "foo").ok());
  EXPECT_EQ(Get("foo"), "NOT_FOUND");
  EXPECT_EQ(Get("missing"), "NOT_FOUND");
  ASSERT_TRUE(db_->Put({}, "", "empty-key").ok());
  EXPECT_EQ(Get(""), "empty-key");
}

TEST_F(DBTest, WriteBatchIsAtomicAndOrdered) {
  WriteBatch b;
  b.Put("a", "1");
  b.Put("b", "2");
  b.Delete("a");
  b.Put("c", "3");
  ASSERT_TRUE(db_->Write({}, b).ok());
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  EXPECT_EQ(Get("b"), "2");
  Reopen();
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  EXPECT_EQ(Get("c"), "3");
}

TEST_F(DBTest, RecoversFromWalWithoutFlush) {
  for (int i = 0; i < 100; ++i) ASSERT_TRUE(db_->Put({}, Key(i), "v" + std::to_string(i)).ok());
  ASSERT_TRUE(db_->Delete({}, Key(7)).ok());
  Reopen();
  for (int i = 0; i < 100; ++i) EXPECT_EQ(Get(Key(i)), i == 7 ? "NOT_FOUND" : "v" + std::to_string(i));
}

TEST_F(DBTest, RandomOpsMatchModelAcrossFlushCompactionAndReopen) {
  std::map<std::string, std::string> model;
  std::mt19937_64 rng(7);
  for (int round = 0; round < 4; ++round) {
    for (int i = 0; i < 20000; ++i) {
      std::string k = Key(rng() % 5000);
      if (rng() % 5 == 0) {
        ASSERT_TRUE(db_->Delete({}, k).ok());
        model.erase(k);
      } else {
        std::string v = std::to_string(rng()) + std::string(rng() % 64, 'x');
        ASSERT_TRUE(db_->Put({}, k, v).ok());
        model[k] = v;
      }
    }
    if (round == 1) ASSERT_TRUE(db_->CompactAll().ok());
    if (round == 2) Reopen();
  }
  DBStats st = db_->GetStats();
  EXPECT_GT(st.flushes, 0u);
  EXPECT_GT(st.compactions, 0u);

  for (int i = 0; i < 5000; ++i) {
    auto it = model.find(Key(i));
    EXPECT_EQ(Get(Key(i)), it == model.end() ? "NOT_FOUND" : it->second) << Key(i);
  }

  std::vector<std::pair<std::string, std::string>> out;
  ASSERT_TRUE(db_->Scan({}, "", "", 0, &out).ok());
  ASSERT_EQ(out.size(), model.size());
  size_t idx = 0;
  for (auto& [k, v] : model) {
    EXPECT_EQ(out[idx].first, k);
    EXPECT_EQ(out[idx].second, v);
    ++idx;
  }

  ASSERT_TRUE(db_->Scan({}, Key(1000), Key(1100), 0, &out).ok());
  auto lo = model.lower_bound(Key(1000)), hi = model.lower_bound(Key(1100));
  EXPECT_EQ(out.size(), static_cast<size_t>(std::distance(lo, hi)));

  Reopen();
  for (int i = 0; i < 5000; i += 37) {
    auto it = model.find(Key(i));
    EXPECT_EQ(Get(Key(i)), it == model.end() ? "NOT_FOUND" : it->second);
  }
}

TEST_F(DBTest, CompactAllDropsTombstonesAndOldVersions) {
  for (int i = 0; i < 3000; ++i) ASSERT_TRUE(db_->Put({}, Key(i), std::string(100, 'a')).ok());
  for (int i = 0; i < 3000; ++i) ASSERT_TRUE(db_->Delete({}, Key(i)).ok());
  ASSERT_TRUE(db_->CompactAll().ok());
  DBStats st = db_->GetStats();
  uint64_t bytes = 0;
  for (auto b : st.bytes_per_level) bytes += b;
  EXPECT_EQ(bytes, 0u) << "everything was deleted, nothing should survive a full compaction";
  EXPECT_EQ(Get(Key(5)), "NOT_FOUND");
}

TEST_F(DBTest, ConcurrentReadersAndWriter) {
  for (int i = 0; i < 2000; ++i) ASSERT_TRUE(db_->Put({}, Key(i), "init").ok());
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, t] {
      std::mt19937_64 rng(t);
      std::string v;
      while (!stop) {
        // Every key always has some value: either "init" or "w<round>".
        if (!db_->Get({}, Key(rng() % 2000), &v).ok()) errors++;
      }
    });
  }
  for (int round = 0; round < 20; ++round)
    for (int i = 0; i < 2000; ++i)
      ASSERT_TRUE(db_->Put({}, Key(i), "w" + std::to_string(round)).ok());
  stop = true;
  for (auto& r : readers) r.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(Get(Key(1999)), "w19");
}

TEST_F(DBTest, SecondOpenIsLocked) {
  std::unique_ptr<DB> other;
  EXPECT_FALSE(DB::Open(opts_, dir_, &other).ok());
}
