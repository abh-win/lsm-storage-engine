#pragma once

// Concurrent skip list: one writer at a time (externally synchronised),
// any number of lock-free readers. Nodes are never removed; next pointers are
// published with release stores, so a reader that observes a node also
// observes its fully-initialised contents.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <random>

#include "arena.h"

namespace lsm {

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  explicit SkipList(Comparator cmp, Arena* arena)
      : compare_(cmp), arena_(arena), head_(NewNode(Key{}, kMaxHeight)), rng_(0xdeadbeef) {
    for (int i = 0; i < kMaxHeight; ++i) head_->SetNext(i, nullptr);
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // REQUIRES: no entry comparing equal to key is present.
  void Insert(const Key& key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);
    assert(x == nullptr || !Equal(key, x->key));
    (void)x;

    int height = RandomHeight();
    int cur = max_height_.load(std::memory_order_relaxed);
    if (height > cur) {
      for (int i = cur; i < height; ++i) prev[i] = head_;
      // Readers that see the new height before the new node simply find
      // head_->next == nullptr at those levels and drop down.
      max_height_.store(height, std::memory_order_relaxed);
    }

    x = NewNode(key, height);
    for (int i = 0; i < height; ++i) {
      x->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
      prev[i]->SetNext(i, x);
    }
  }

  bool Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    return x != nullptr && Equal(key, x->key);
  }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}
    bool Valid() const { return node_ != nullptr; }
    const Key& key() const { return node_->key; }
    void Next() { node_ = node_->Next(0); }
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  static constexpr int kMaxHeight = 12;
  static constexpr unsigned kBranching = 4;

  struct Node {
    explicit Node(const Key& k) : key(k) {}
    Key const key;

    Node* Next(int n) { return next_[n].load(std::memory_order_acquire); }
    void SetNext(int n, Node* x) { next_[n].store(x, std::memory_order_release); }
    Node* NoBarrierNext(int n) { return next_[n].load(std::memory_order_relaxed); }
    void NoBarrierSetNext(int n, Node* x) { next_[n].store(x, std::memory_order_relaxed); }

   private:
    // Flexible tail: height entries are allocated, only next_[0] is declared.
    std::atomic<Node*> next_[1];
  };

  Node* NewNode(const Key& key, int height) {
    char* mem = arena_->AllocateAligned(sizeof(Node) +
                                        sizeof(std::atomic<Node*>) * (height - 1));
    return new (mem) Node(key);
  }

  int RandomHeight() {
    int h = 1;
    while (h < kMaxHeight && (rng_() % kBranching) == 0) ++h;
    return h;
  }

  bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }

  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return n != nullptr && compare_(n->key, key) < 0;
  }

  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = max_height_.load(std::memory_order_relaxed) - 1;
    while (true) {
      Node* next = x->Next(level);
      if (KeyIsAfterNode(key, next)) {
        x = next;
      } else {
        if (prev != nullptr) prev[level] = x;
        if (level == 0) return next;
        --level;
      }
    }
  }

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_{1};
  std::minstd_rand rng_;
};

}  // namespace lsm
