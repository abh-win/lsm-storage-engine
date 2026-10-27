#include "merger.h"

#include <algorithm>

#include "dbformat.h"

namespace lsm {
namespace {

class MergingIterator final : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
      : children_(std::move(children)) {}

  bool Valid() const override { return !heap_.empty(); }
  std::string_view key() const override { return children_[heap_.front()]->key(); }
  std::string_view value() const override { return children_[heap_.front()]->value(); }

  Status status() const override {
    for (const auto& c : children_) {
      Status s = c->status();
      if (!s.ok()) return s;
    }
    return Status::OK();
  }

  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    RebuildHeap();
  }

  void Seek(std::string_view target) override {
    for (auto& c : children_) c->Seek(target);
    RebuildHeap();
  }

  void Next() override {
    std::pop_heap(heap_.begin(), heap_.end(), cmp_);
    size_t top = heap_.back();
    children_[top]->Next();
    if (children_[top]->Valid()) {
      std::push_heap(heap_.begin(), heap_.end(), cmp_);
    } else {
      heap_.pop_back();
    }
  }

 private:
  struct Greater {
    const std::vector<std::unique_ptr<Iterator>>* children;
    bool operator()(size_t a, size_t b) const {
      return CompareInternalKey((*children)[a]->key(), (*children)[b]->key()) > 0;
    }
  };

  void RebuildHeap() {
    heap_.clear();
    for (size_t i = 0; i < children_.size(); ++i)
      if (children_[i]->Valid()) heap_.push_back(i);
    std::make_heap(heap_.begin(), heap_.end(), cmp_);
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  std::vector<size_t> heap_;
  Greater cmp_{&children_};
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

}  // namespace lsm
