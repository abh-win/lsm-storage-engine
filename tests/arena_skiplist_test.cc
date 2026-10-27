#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>

#include "arena.h"
#include "memtable.h"
#include "skiplist.h"

using namespace lsm;

namespace {
struct IntCmp {
  int operator()(uint64_t a, uint64_t b) const { return a < b ? -1 : (a > b ? 1 : 0); }
};
}  // namespace

TEST(SkipList, InsertAndIterateSorted) {
  Arena arena;
  SkipList<uint64_t, IntCmp> list(IntCmp{}, &arena);
  std::set<uint64_t> ref;
  std::mt19937_64 rng(1);
  for (int i = 0; i < 5000; ++i) {
    uint64_t k = rng() % 100000 + 1;
    if (ref.insert(k).second) list.Insert(k);
  }
  SkipList<uint64_t, IntCmp>::Iterator it(&list);
  it.SeekToFirst();
  for (uint64_t k : ref) {
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.key(), k);
    it.Next();
  }
  EXPECT_FALSE(it.Valid());
  for (uint64_t k : ref) EXPECT_TRUE(list.Contains(k));
  it.Seek(50000);
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), *ref.lower_bound(50000));
}

TEST(SkipList, ConcurrentReadersSeeConsistentPrefix) {
  Arena arena;
  SkipList<uint64_t, IntCmp> list(IntCmp{}, &arena);
  std::atomic<uint64_t> inserted{0};
  std::atomic<bool> done{false};
  std::thread reader([&] {
    while (!done) {
      uint64_t n = inserted.load(std::memory_order_acquire);
      // Every key published before we loaded `n` must be visible, in order.
      SkipList<uint64_t, IntCmp>::Iterator it(&list);
      it.SeekToFirst();
      uint64_t prev = 0, seen = 0;
      while (it.Valid()) {
        ASSERT_GT(it.key(), prev);
        prev = it.key();
        ++seen;
        it.Next();
      }
      ASSERT_GE(seen, n);
    }
  });
  for (uint64_t i = 1; i <= 20000; ++i) {
    list.Insert(i * 2);
    inserted.store(i, std::memory_order_release);
  }
  done = true;
  reader.join();
}

TEST(MemTable, NewestVersionWinsAndTombstones) {
  auto* m = new MemTable();
  m->Ref();
  m->Add(1, ValueType::kValue, "a", "v1");
  m->Add(2, ValueType::kValue, "a", "v2");
  m->Add(3, ValueType::kDeletion, "b", "");
  m->Add(4, ValueType::kValue, "c", "x");
  std::string v;
  EXPECT_EQ(m->Get("a", 10, &v), MemTable::LookupResult::kFound);
  EXPECT_EQ(v, "v2");
  EXPECT_EQ(m->Get("a", 1, &v), MemTable::LookupResult::kFound);  // snapshot read
  EXPECT_EQ(v, "v1");
  EXPECT_EQ(m->Get("b", 10, &v), MemTable::LookupResult::kDeleted);
  EXPECT_EQ(m->Get("zz", 10, &v), MemTable::LookupResult::kNotPresent);
  EXPECT_EQ(m->Get("c", 3, &v), MemTable::LookupResult::kNotPresent);  // not yet visible
  m->Unref();
}
