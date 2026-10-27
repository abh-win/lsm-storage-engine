#pragma once

// Internal key format: user_key || fixed64((sequence << 8) | type).
// Ordering: user key ascending, then sequence descending, so the newest
// version of a key is encountered first.

#include <cstdint>
#include <string>
#include <string_view>

#include "coding.h"

namespace lsm {

using SequenceNumber = uint64_t;
constexpr SequenceNumber kMaxSequenceNumber = (1ull << 56) - 1;

enum class ValueType : uint8_t { kDeletion = 0, kValue = 1 };

inline uint64_t PackTag(SequenceNumber seq, ValueType t) {
  return (seq << 8) | static_cast<uint8_t>(t);
}

inline void AppendInternalKey(std::string* dst, std::string_view user_key, SequenceNumber seq,
                              ValueType t) {
  dst->append(user_key.data(), user_key.size());
  PutFixed64(dst, PackTag(seq, t));
}

inline std::string MakeInternalKey(std::string_view user_key, SequenceNumber seq, ValueType t) {
  std::string s;
  s.reserve(user_key.size() + 8);
  AppendInternalKey(&s, user_key, seq, t);
  return s;
}

inline std::string_view ExtractUserKey(std::string_view ikey) {
  return ikey.substr(0, ikey.size() - 8);
}

inline uint64_t ExtractTag(std::string_view ikey) {
  return DecodeFixed64(ikey.data() + ikey.size() - 8);
}

inline SequenceNumber ExtractSequence(std::string_view ikey) { return ExtractTag(ikey) >> 8; }

inline ValueType ExtractType(std::string_view ikey) {
  return static_cast<ValueType>(ExtractTag(ikey) & 0xff);
}

// Three-way compare of internal keys.
inline int CompareInternalKey(std::string_view a, std::string_view b) {
  int r = ExtractUserKey(a).compare(ExtractUserKey(b));
  if (r != 0) return r;
  uint64_t ta = ExtractTag(a), tb = ExtractTag(b);
  if (ta > tb) return -1;  // higher sequence sorts first
  if (ta < tb) return 1;
  return 0;
}

// A lookup key for `user_key` visible at `seq`: seeks to the newest entry
// with sequence <= seq.
inline std::string MakeLookupKey(std::string_view user_key, SequenceNumber seq) {
  return MakeInternalKey(user_key, seq, ValueType::kValue);
}

}  // namespace lsm
