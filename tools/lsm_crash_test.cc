// Crash-consistency test with a real SIGKILL.
//
//   lsm_crash_test [--dir=/tmp/lsm_crash] [--ops=500000] [--rounds=5]
//                  [--memtable_mb=4] [--kill_at=done|random]
//
// A child process writes `ops` sequential Puts and publishes the number of
// *acknowledged* writes in a shared-memory counter. The parent SIGKILLs the
// child (after it finishes, or at a random point), reopens the database,
// times recovery and verifies that every acknowledged write is present with
// the right value and nothing beyond (ack + 1) exists.

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

#include "lsm/db.h"

namespace {

std::string Key(uint64_t i) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "key%016" PRIu64, i);
  return buf;
}

std::string Value(uint64_t i) {
  std::string v = "value-" + std::to_string(i) + "-";
  v.resize(100, static_cast<char>('a' + i % 26));
  return v;
}

struct Shared {
  std::atomic<uint64_t> acked;
  std::atomic<int> ready;
};

}  // namespace

int main(int argc, char** argv) {
  std::string dir = "/tmp/lsm_crash";
  uint64_t ops = 500000;
  int rounds = 5;
  size_t memtable_mb = 4;
  bool random_kill = false;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    if (s.rfind("--dir=", 0) == 0) dir = s.substr(6);
    else if (s.rfind("--ops=", 0) == 0) ops = std::stoull(s.substr(6));
    else if (s.rfind("--rounds=", 0) == 0) rounds = std::stoi(s.substr(9));
    else if (s.rfind("--memtable_mb=", 0) == 0) memtable_mb = std::stoull(s.substr(14));
    else if (s == "--kill_at=random") random_kill = true;
    else if (s == "--kill_at=done") random_kill = false;
    else {
      std::fprintf(stderr, "unknown flag %s\n", s.c_str());
      return 2;
    }
  }

  lsm::Options opts;
  opts.memtable_size = memtable_mb << 20;
  std::mt19937_64 rng(std::random_device{}());
  int failures = 0;

  for (int round = 0; round < rounds; ++round) {
    std::filesystem::remove_all(dir);
    auto* sh = static_cast<Shared*>(::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE,
                                           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    new (sh) Shared{};

    pid_t pid = ::fork();
    if (pid == 0) {
      std::unique_ptr<lsm::DB> db;
      if (!lsm::DB::Open(opts, dir, &db).ok()) _exit(3);
      sh->ready = 1;
      lsm::WriteOptions wo;
      for (uint64_t i = 0; i < ops; ++i) {
        if (!db->Put(wo, Key(i), Value(i)).ok()) _exit(4);
        sh->acked.store(i + 1, std::memory_order_release);
      }
      // Stay alive until killed so the kill always lands on a live process.
      while (true) ::pause();
    }

    while (!sh->ready.load()) std::this_thread::yield();
    if (random_kill) {
      // Kill somewhere inside the write stream.
      uint64_t target = rng() % ops;
      while (sh->acked.load(std::memory_order_acquire) < target) std::this_thread::yield();
    } else {
      while (sh->acked.load(std::memory_order_acquire) < ops) std::this_thread::yield();
    }
    ::kill(pid, SIGKILL);
    int status;
    ::waitpid(pid, &status, 0);
    uint64_t acked = sh->acked.load();

    auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<lsm::DB> db;
    lsm::Status s = lsm::DB::Open(opts, dir, &db);
    double open_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!s.ok()) {
      std::printf("round %d: reopen failed: %s\n", round, s.ToString().c_str());
      ++failures;
      continue;
    }
    lsm::DBStats st = db->GetStats();

    uint64_t missing = 0, wrong = 0;
    lsm::ReadOptions ro;
    std::string v;
    for (uint64_t i = 0; i < acked; ++i) {
      lsm::Status gs = db->Get(ro, Key(i), &v);
      if (!gs.ok()) ++missing;
      else if (v != Value(i)) ++wrong;
    }
    // At most one in-flight (un-acknowledged) write may have reached the log.
    uint64_t phantom = 0;
    for (uint64_t i = acked + 1; i < std::min(ops, acked + 1000); ++i)
      if (db->Get(ro, Key(i), &v).ok()) ++phantom;

    bool ok = missing == 0 && wrong == 0 && phantom == 0;
    if (!ok) ++failures;
    std::printf(
        "round %d: killed after %" PRIu64 " acked ops | recovered %" PRIu64
        " WAL records, open %.3f s (replay+flush %.3f s) | missing=%" PRIu64 " wrong=%" PRIu64
        " phantom=%" PRIu64 " -> %s\n",
        round, acked, st.recovered_records, open_s, st.recovery_seconds, missing, wrong, phantom,
        ok ? "OK" : "FAIL");
    ::munmap(sh, sizeof(Shared));
  }
  std::printf("%d/%d rounds passed\n", rounds - failures, rounds);
  return failures == 0 ? 0 : 1;
}
