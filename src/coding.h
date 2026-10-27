#pragma once

// Little-endian fixed-width and LEB128 varint encoding helpers.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace lsm {

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  std::memcpy(buf, &v, 4);
  dst->append(buf, 4);
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  std::memcpy(buf, &v, 8);
  dst->append(buf, 8);
}

inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

inline void EncodeFixed32(char* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void EncodeFixed64(char* p, uint64_t v) { std::memcpy(p, &v, 8); }

inline void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  int n = 0;
  while (v >= 0x80) {
    buf[n++] = static_cast<char>(v | 0x80);
    v >>= 7;
  }
  buf[n++] = static_cast<char>(v);
  dst->append(buf, n);
}

inline void PutVarint32(std::string* dst, uint32_t v) { PutVarint64(dst, v); }

inline int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++len;
  }
  return len;
}

inline char* EncodeVarint64(char* dst, uint64_t v) {
  auto* p = reinterpret_cast<unsigned char*>(dst);
  while (v >= 0x80) {
    *p++ = static_cast<unsigned char>(v | 0x80);
    v >>= 7;
  }
  *p++ = static_cast<unsigned char>(v);
  return reinterpret_cast<char*>(p);
}

// Returns pointer past the varint, or nullptr if malformed / out of bounds.
inline const char* DecodeVarint64(const char* p, const char* limit, uint64_t* v) {
  uint64_t result = 0;
  for (int shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = static_cast<unsigned char>(*p++);
    result |= (byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      *v = result;
      return p;
    }
  }
  return nullptr;
}

inline const char* DecodeVarint32(const char* p, const char* limit, uint32_t* v) {
  uint64_t x;
  const char* r = DecodeVarint64(p, limit, &x);
  if (r == nullptr || x > 0xffffffffu) return nullptr;
  *v = static_cast<uint32_t>(x);
  return r;
}

// Consumes a varint-length-prefixed slice from *in.
inline bool GetLengthPrefixed(std::string_view* in, std::string_view* out) {
  uint64_t len;
  const char* p = DecodeVarint64(in->data(), in->data() + in->size(), &len);
  if (p == nullptr) return false;
  size_t header = static_cast<size_t>(p - in->data());
  if (len > in->size() - header) return false;
  *out = std::string_view(p, len);
  in->remove_prefix(header + len);
  return true;
}

inline bool GetVarint64(std::string_view* in, uint64_t* v) {
  const char* p = DecodeVarint64(in->data(), in->data() + in->size(), v);
  if (p == nullptr) return false;
  in->remove_prefix(static_cast<size_t>(p - in->data()));
  return true;
}

inline void PutLengthPrefixed(std::string* dst, std::string_view s) {
  PutVarint64(dst, s.size());
  dst->append(s.data(), s.size());
}

}  // namespace lsm
