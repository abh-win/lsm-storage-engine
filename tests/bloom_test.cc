#include <gtest/gtest.h>

#include <string>

#include "bloom.h"

using namespace lsm;

TEST(Bloom, NoFalseNegatives) {
  BloomFilterBuilder b(10);
  for (int i = 0; i < 10000; ++i) b.AddKey("key" + std::to_string(i));
  std::string f = b.Finish();
  for (int i = 0; i < 10000; ++i) EXPECT_TRUE(BloomMayContain(f, "key" + std::to_string(i)));
}

TEST(Bloom, FalsePositiveRateNearTheory) {
  BloomFilterBuilder b(10);
  for (int i = 0; i < 10000; ++i) b.AddKey("key" + std::to_string(i));
  std::string f = b.Finish();
  int fp = 0;
  const int trials = 100000;
  for (int i = 0; i < trials; ++i) fp += BloomMayContain(f, "absent" + std::to_string(i));
  double rate = static_cast<double>(fp) / trials;
  EXPECT_LT(rate, 0.02);  // theory ~0.8% at 10 bits/key
}

TEST(Bloom, EmptyFilterIsConservative) {
  EXPECT_TRUE(BloomMayContain("", "anything"));
}
