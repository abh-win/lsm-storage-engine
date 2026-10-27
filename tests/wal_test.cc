#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "wal.h"

using namespace lsm;

namespace {
std::string TempPath(const char* name) {
  return (std::filesystem::temp_directory_path() / (std::string(name) + std::to_string(::getpid())))
      .string();
}

std::vector<std::string> ReadAll(const std::string& path, WalReadStats* st) {
  std::vector<std::string> out;
  EXPECT_TRUE(ReadWal(path, [&](std::string_view r) { out.emplace_back(r); return Status::OK(); }, st).ok());
  return out;
}
}  // namespace

TEST(Wal, RoundTrip) {
  std::string p = TempPath("wal_rt");
  std::filesystem::remove(p);
  {
    WalWriter w;
    ASSERT_TRUE(w.Open(p).ok());
    for (int i = 0; i < 1000; ++i) ASSERT_TRUE(w.AddRecord("record-" + std::to_string(i)).ok());
    ASSERT_TRUE(w.AddRecord("").ok());
  }
  WalReadStats st;
  auto recs = ReadAll(p, &st);
  ASSERT_EQ(recs.size(), 1001u);
  EXPECT_EQ(recs[0], "record-0");
  EXPECT_EQ(recs[999], "record-999");
  EXPECT_EQ(recs[1000], "");
  EXPECT_FALSE(st.truncated_tail);
  std::filesystem::remove(p);
}

TEST(Wal, TornTailIsDropped) {
  std::string p = TempPath("wal_torn");
  std::filesystem::remove(p);
  {
    WalWriter w;
    ASSERT_TRUE(w.Open(p).ok());
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(w.AddRecord(std::string(100, 'a' + i)).ok());
  }
  auto size = std::filesystem::file_size(p);
  std::filesystem::resize_file(p, size - 37);  // tear the last record
  WalReadStats st;
  auto recs = ReadAll(p, &st);
  EXPECT_EQ(recs.size(), 9u);
  EXPECT_TRUE(st.truncated_tail);
  std::filesystem::remove(p);
}

TEST(Wal, CorruptedRecordStopsReplay) {
  std::string p = TempPath("wal_corrupt");
  std::filesystem::remove(p);
  {
    WalWriter w;
    ASSERT_TRUE(w.Open(p).ok());
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(w.AddRecord(std::string(100, 'a' + i)).ok());
  }
  {
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(5 * 108 + 50);  // inside record 5's payload
    f.put('Z');
  }
  WalReadStats st;
  auto recs = ReadAll(p, &st);
  EXPECT_EQ(recs.size(), 5u);
  EXPECT_TRUE(st.truncated_tail);
  std::filesystem::remove(p);
}
