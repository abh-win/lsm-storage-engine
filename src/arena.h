#pragma once

// Bump allocator for MemTable nodes. Memory is released only when the arena
// is destroyed, i.e. when the whole MemTable is dropped after a flush.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

namespace lsm {

class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  char* Allocate(size_t bytes) {
    assert(bytes > 0);
    if (bytes <= remaining_) {
      char* r = ptr_;
      ptr_ += bytes;
      remaining_ -= bytes;
      return r;
    }
    return AllocateFallback(bytes);
  }

  char* AllocateAligned(size_t bytes) {
    constexpr size_t kAlign = alignof(std::max_align_t);
    size_t mod = reinterpret_cast<uintptr_t>(ptr_) & (kAlign - 1);
    size_t slop = mod == 0 ? 0 : kAlign - mod;
    size_t needed = bytes + slop;
    if (needed <= remaining_) {
      char* r = ptr_ + slop;
      ptr_ += needed;
      remaining_ -= needed;
      return r;
    }
    return AllocateFallback(bytes);  // fresh blocks from new[] are max-aligned
  }

  size_t MemoryUsage() const { return usage_.load(std::memory_order_relaxed); }

 private:
  static constexpr size_t kBlockSize = 64 * 1024;

  char* AllocateFallback(size_t bytes) {
    if (bytes > kBlockSize / 4) return NewBlock(bytes);  // big object: own block
    ptr_ = NewBlock(kBlockSize);
    remaining_ = kBlockSize;
    char* r = ptr_;
    ptr_ += bytes;
    remaining_ -= bytes;
    return r;
  }

  char* NewBlock(size_t bytes) {
    blocks_.push_back(std::make_unique<char[]>(bytes));
    usage_.fetch_add(bytes + sizeof(char*), std::memory_order_relaxed);
    return blocks_.back().get();
  }

  char* ptr_ = nullptr;
  size_t remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> usage_{0};
};

}  // namespace lsm
