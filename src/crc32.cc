#include "crc32.h"

#include <array>
#include <cstring>

namespace lsm::crc32 {
namespace {

using Tables = std::array<std::array<uint32_t, 256>, 8>;

constexpr Tables MakeTables() {
  Tables t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    t[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xff];
  }
  return t;
}

constexpr Tables kTables = MakeTables();

}  // namespace

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  uint32_t c = ~crc;
  while (n >= 8) {
    uint32_t lo, hi;
    std::memcpy(&lo, p, 4);
    std::memcpy(&hi, p + 4, 4);
    lo ^= c;
    c = kTables[7][lo & 0xff] ^ kTables[6][(lo >> 8) & 0xff] ^ kTables[5][(lo >> 16) & 0xff] ^
        kTables[4][lo >> 24] ^ kTables[3][hi & 0xff] ^ kTables[2][(hi >> 8) & 0xff] ^
        kTables[1][(hi >> 16) & 0xff] ^ kTables[0][hi >> 24];
    p += 8;
    n -= 8;
  }
  while (n--) c = kTables[0][(c ^ *p++) & 0xff] ^ (c >> 8);
  return ~c;
}

}  // namespace lsm::crc32
