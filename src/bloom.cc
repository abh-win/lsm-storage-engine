#include "bloom.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lsm {

// 64-bit hash: wyhash-style multiply-mix over 8-byte words. Fast and with
// good avalanche, which is all a Bloom filter needs.
uint64_t Hash64(std::string_view s) {
  constexpr uint64_t kP0 = 0xa0761d6478bd642full, kP1 = 0xe7037ed1a0b428dbull;
  auto mix = [](uint64_t a, uint64_t b) {
    __uint128_t r = static_cast<__uint128_t>(a) * b;
    return static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64);
  };
  const char* p = s.data();
  size_t n = s.size();
  uint64_t h = 0x9e3779b97f4a7c15ull ^ (n * kP0);
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    h = mix(h ^ w, kP1);
    p += 8;
    n -= 8;
  }
  uint64_t tail = 0;
  std::memcpy(&tail, p, n);
  h = mix(h ^ tail ^ (static_cast<uint64_t>(n) << 56), kP0);
  return mix(h, kP1);
}

BloomFilterBuilder::BloomFilterBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {
  // Optimal k = bits_per_key * ln 2, clamped to a sane range.
  k_ = std::clamp(static_cast<int>(std::lround(bits_per_key * 0.69)), 1, 30);
}

std::string BloomFilterBuilder::Finish() {
  size_t bits = std::max<size_t>(64, hashes_.size() * static_cast<size_t>(bits_per_key_));
  size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;
  std::string out(bytes + 1, '\0');
  auto* array = reinterpret_cast<uint8_t*>(out.data());
  for (uint64_t h : hashes_) {
    uint64_t h1 = h, h2 = (h >> 33) | (h << 31);
    for (int i = 0; i < k_; ++i) {
      uint64_t bit = (h1 + static_cast<uint64_t>(i) * h2) % bits;
      array[bit >> 3] |= static_cast<uint8_t>(1u << (bit & 7));
    }
  }
  out[bytes] = static_cast<char>(k_);
  hashes_.clear();
  return out;
}

bool BloomMayContainHash(std::string_view filter, uint64_t h) {
  if (filter.size() < 2) return true;
  size_t bytes = filter.size() - 1;
  uint64_t bits = bytes * 8;
  int k = static_cast<uint8_t>(filter.back());
  if (k < 1 || k > 30) return true;
  const auto* array = reinterpret_cast<const uint8_t*>(filter.data());
  uint64_t h1 = h, h2 = (h >> 33) | (h << 31);
  for (int i = 0; i < k; ++i) {
    uint64_t bit = (h1 + static_cast<uint64_t>(i) * h2) % bits;
    if (!(array[bit >> 3] & (1u << (bit & 7)))) return false;
  }
  return true;
}

bool BloomMayContain(std::string_view filter, std::string_view user_key) {
  return BloomMayContainHash(filter, Hash64(user_key));
}

}  // namespace lsm
