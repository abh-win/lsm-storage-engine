// Throughput / latency benchmark.
//
//   lsm_bench [--dir=/tmp/lsm_bench] [--num=1000000] [--value_size=100]
//             [--reads=1000000] [--threads=1] [--compact=1]
//
// Phases: fillrandom -> (flush + compact) -> readrandom (hits) ->
// readmissing (absent keys; exercises the Bloom filters) -> scan.

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lsm/db.h"

using Clock = std::chrono::steady_clock;

namespace {

struct Args {
  std::string dir = "/tmp/lsm_bench";
  uint64_t num = 1'000'000;
  size_t value_size = 100;
  uint64_t reads = 1'000'000;
  int threads = 1;
  bool compact = true;
};

Args Parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    auto val = [&](const char* name) -> const char* {
      size_t n = std::strlen(name);
      return s.compare(0, n, name) == 0 ? s.c_str() + n : nullptr;
    };
    if (auto v = val("--dir=")) a.dir = v;
    else if (auto v = val("--num=")) a.num = std::strtoull(v, nullptr, 10);
    else if (auto v = val("--value_size=")) a.value_size = std::strtoull(v, nullptr, 10);
    else if (auto v = val("--reads=")) a.reads = std::strtoull(v, nullptr, 10);
    else if (auto v = val("--threads=")) a.threads = std::atoi(v);
    else if (auto v = val("--compact=")) a.compact = std::atoi(v) != 0;
    else {
      std::fprintf(stderr, "unknown flag %s\n", s.c_str());
      std::exit(2);
    }
  }
  return a;
}

std::string Key(uint64_t i) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "key%016" PRIu64, i);
  return buf;
}

void Check(const lsm::Status& s, const char* what) {
  if (!s.ok()) {
    std::fprintf(stderr, "%s: %s\n", what, s.ToString().c_str());
    std::exit(1);
  }
}

struct LatencyReport {
  double ops_per_sec;
  double p50_us, p99_us, p999_us, max_us;
};

LatencyReport Summarise(std::vector<uint32_t>& ns, double seconds) {
  std::sort(ns.begin(), ns.end());
  auto pct = [&](double p) {
    size_t idx = std::min(ns.size() - 1, static_cast<size_t>(p * static_cast<double>(ns.size())));
    return ns[idx] / 1000.0;
  };
  return {static_cast<double>(ns.size()) / seconds, pct(0.50), pct(0.99), pct(0.999),
          ns.back() / 1000.0};
}

void Print(const char* name, const LatencyReport& r) {
  std::printf("%-12s %10.0f ops/s   p50 %6.2f us   p99 %6.2f us   p99.9 %7.2f us   max %8.1f us\n",
              name, r.ops_per_sec, r.p50_us, r.p99_us, r.p999_us, r.max_us);
}

// Runs `fn(thread_id, i)` for `total` ops split across threads. `fn` does any
// per-op preparation (key formatting) and returns the nanoseconds spent in
// the measured DB call only.
template <typename Fn>
LatencyReport RunTimed(int threads, uint64_t total, Fn fn) {
  std::vector<std::vector<uint32_t>> lat(threads);
  auto start = Clock::now();
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; ++t) {
    ts.emplace_back([&, t] {
      uint64_t n = total / threads;
      lat[t].reserve(n);
      for (uint64_t i = 0; i < n; ++i) lat[t].push_back(fn(t, i));
    });
  }
  for (auto& th : ts) th.join();
  double secs = std::chrono::duration<double>(Clock::now() - start).count();
  std::vector<uint32_t> all;
  for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
  return Summarise(all, secs);
}

template <typename Op>
uint32_t TimeIt(Op op) {
  auto a = Clock::now();
  op();
  auto b = Clock::now();
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

}  // namespace

int main(int argc, char** argv) {
  Args a = Parse(argc, argv);
  std::filesystem::remove_all(a.dir);

  lsm::Options opts;
  std::unique_ptr<lsm::DB> db;
  Check(lsm::DB::Open(opts, a.dir, &db), "open");

  std::printf("LSM bench: %" PRIu64 " keys, %zu-byte values, %d thread(s)\n", a.num, a.value_size,
              a.threads);

  // fillrandom: keys inserted in a random permutation.
  std::vector<uint64_t> order(a.num);
  std::iota(order.begin(), order.end(), 0);
  std::shuffle(order.begin(), order.end(), std::mt19937_64(42));
  std::string value(a.value_size, 'v');
  lsm::WriteOptions wo;
  auto fill = RunTimed(1, a.num, [&](int, uint64_t i) {
    std::memcpy(value.data(), &order[i], std::min<size_t>(8, value.size()));
    std::string k = Key(order[i]);
    return TimeIt([&] { Check(db->Put(wo, k, value), "put"); });
  });
  Print("fillrandom", fill);

  auto t0 = Clock::now();
  Check(a.compact ? db->CompactAll() : db->Flush(), "compact");
  std::printf("%-12s %.2f s\n", a.compact ? "compact" : "flush",
              std::chrono::duration<double>(Clock::now() - t0).count());

  lsm::DBStats st = db->GetStats();
  std::printf("levels:");
  for (int l = 0; l < 7; ++l)
    if (st.files_per_level[l]) std::printf("  L%d=%d files/%.1f MB", l, st.files_per_level[l],
                                           st.bytes_per_level[l] / 1048576.0);
  std::printf("\n");

  lsm::ReadOptions ro;
  std::vector<std::mt19937_64> rngs;
  for (int t = 0; t < a.threads; ++t) rngs.emplace_back(1000 + t);
  uint64_t found = 0;
  std::vector<uint64_t> found_t(a.threads, 0);
  auto hits = RunTimed(a.threads, a.reads, [&](int t, uint64_t) {
    thread_local std::string v;
    std::string k = Key(rngs[t]() % a.num);
    return TimeIt([&] {
      if (db->Get(ro, k, &v).ok()) found_t[t]++;
    });
  });
  for (auto f : found_t) found += f;
  Print("readrandom", hits);
  std::printf("             found %" PRIu64 " / %" PRIu64 "\n", found, (a.reads / a.threads) * a.threads);

  auto before = db->GetStats();
  auto miss = RunTimed(a.threads, a.reads, [&](int t, uint64_t) {
    thread_local std::string v;
    // Never inserted, but inside the key range of the tables, so every
    // lookup has to consult a filter (or search a table without one).
    std::string k = Key(rngs[t]() % a.num) + "-miss";
    return TimeIt([&] { db->Get(ro, k, &v); });
  });
  auto after = db->GetStats();
  Print("readmissing", miss);
  uint64_t neg = after.bloom_negatives - before.bloom_negatives;
  uint64_t probes = after.table_probes - before.table_probes;
  std::printf("             bloom skipped %" PRIu64 " table probes, %" PRIu64
              " false-positive probes (FP rate %.3f%%)\n",
              neg, probes, 100.0 * static_cast<double>(probes) / static_cast<double>(neg + probes));

  std::vector<std::pair<std::string, std::string>> out;
  auto scan = RunTimed(1, 10000, [&](int, uint64_t i) {
    std::string k = Key((i * 7919) % a.num);
    return TimeIt([&] { db->Scan(ro, k, "", 100, &out); });
  });
  Print("scan100", scan);
  return 0;
}
