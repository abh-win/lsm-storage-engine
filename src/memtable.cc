#include "memtable.h"

#include <cstring>

namespace lsm {
namespace {

std::string_view GetLengthPrefixedSlice(const char* p) {
  uint32_t len = 0;
  const char* q = DecodeVarint32(p, p + 5, &len);
  return {q, len};
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  // Head node of the skip list carries a null key; it is never compared.
  return CompareInternalKey(GetLengthPrefixedSlice(a), GetLengthPrefixedSlice(b));
}

MemTable::MemTable() : table_(KeyComparator{}, &arena_) {}

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key,
                   std::string_view value) {
  size_t ikey_len = key.size() + 8;
  size_t encoded = VarintLength(ikey_len) + ikey_len + VarintLength(value.size()) + value.size();
  char* buf = arena_.Allocate(encoded);
  char* p = EncodeVarint64(buf, ikey_len);
  std::memcpy(p, key.data(), key.size());
  p += key.size();
  EncodeFixed64(p, PackTag(seq, type));
  p += 8;
  p = EncodeVarint64(p, value.size());
  std::memcpy(p, value.data(), value.size());
  table_.Insert(buf);
  payload_bytes_.fetch_add(key.size() + value.size() + 8, std::memory_order_relaxed);
}

MemTable::LookupResult MemTable::Get(std::string_view user_key, SequenceNumber snapshot,
                                     std::string* value) const {
  std::string lookup;
  PutVarint64(&lookup, user_key.size() + 8);
  AppendInternalKey(&lookup, user_key, snapshot, ValueType::kValue);

  Table::Iterator it(&table_);
  it.Seek(lookup.data());
  if (!it.Valid()) return LookupResult::kNotPresent;

  const char* entry = it.key();
  std::string_view ikey = GetLengthPrefixedSlice(entry);
  if (ExtractUserKey(ikey) != user_key) return LookupResult::kNotPresent;
  if (ExtractType(ikey) == ValueType::kDeletion) return LookupResult::kDeleted;
  std::string_view v = GetLengthPrefixedSlice(ikey.data() + ikey.size());
  value->assign(v.data(), v.size());
  return LookupResult::kFound;
}

class MemTableIterator final : public Iterator {
 public:
  explicit MemTableIterator(const MemTable::Table* t) : it_(t) {}

  bool Valid() const override { return it_.Valid(); }
  void SeekToFirst() override { it_.SeekToFirst(); }
  void Seek(std::string_view target) override {
    scratch_.clear();
    PutVarint64(&scratch_, target.size());
    scratch_.append(target);
    it_.Seek(scratch_.data());
  }
  void Next() override { it_.Next(); }
  std::string_view key() const override { return GetLengthPrefixedSlice(it_.key()); }
  std::string_view value() const override {
    std::string_view k = key();
    return GetLengthPrefixedSlice(k.data() + k.size());
  }

 private:
  MemTable::Table::Iterator it_;
  std::string scratch_;
};

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  return std::make_unique<MemTableIterator>(&table_);
}

}  // namespace lsm
