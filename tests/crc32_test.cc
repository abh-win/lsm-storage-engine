#include <gtest/gtest.h>

#include <string>

#include "crc32.h"

using namespace lsm;

TEST(Crc32, KnownVectors) {
  EXPECT_EQ(crc32::Value("", 0), 0u);
  EXPECT_EQ(crc32::Value("123456789", 9), 0xCBF43926u);  // standard check value
  std::string fox = "The quick brown fox jumps over the lazy dog";
  EXPECT_EQ(crc32::Value(fox.data(), fox.size()), 0x414FA339u);
}

TEST(Crc32, ExtendMatchesOneShot) {
  std::string s(1000, 'x');
  for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(i * 31);
  uint32_t whole = crc32::Value(s.data(), s.size());
  for (size_t cut : {0, 1, 7, 8, 9, 500, 999}) {
    uint32_t part = crc32::Extend(crc32::Value(s.data(), cut), s.data() + cut, s.size() - cut);
    EXPECT_EQ(part, whole) << cut;
  }
}

TEST(Crc32, MaskRoundTrip) {
  for (uint32_t v : {0u, 1u, 0xdeadbeefu, 0xffffffffu}) EXPECT_EQ(crc32::Unmask(crc32::Mask(v)), v);
  EXPECT_NE(crc32::Mask(0x12345678u), 0x12345678u);
}
