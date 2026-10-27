#pragma once

#include <cstddef>
#include <cstdint>

namespace lsm::crc32 {

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) using slice-by-8 tables.
// Extend() continues a running CRC so records can be checksummed in pieces.
uint32_t Extend(uint32_t crc, const char* data, size_t n);
inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

// Masking keeps a CRC stored inside data from being "CRC of CRC"-degenerate
// (same trick as LevelDB).
constexpr uint32_t kMaskDelta = 0xa282ead8u;
inline uint32_t Mask(uint32_t crc) { return ((crc >> 15) | (crc << 17)) + kMaskDelta; }
inline uint32_t Unmask(uint32_t m) {
  uint32_t rot = m - kMaskDelta;
  return (rot >> 17) | (rot << 15);
}

}  // namespace lsm::crc32
