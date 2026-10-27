#pragma once

// Bloom filter over user keys, one per SSTable. Uses Kirsch–Mitzenmacher
// double hashing: probe_i = h1 + i*h2 over a 64-bit hash.
// Serialized layout: bit array || k (1 byte).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

uint64_t Hash64(std::string_view s);

class BloomFilterBuilder {
 public:
  explicit BloomFilterBuilder(int bits_per_key);
  void AddKey(std::string_view user_key) { hashes_.push_back(Hash64(user_key)); }
  size_t NumKeys() const { return hashes_.size(); }
  // Builds the filter for all added keys and resets the builder.
  std::string Finish();

 private:
  int bits_per_key_;
  int k_;
  std::vector<uint64_t> hashes_;
};

// Returns true if the key may be in the set (false means definitely absent).
// An empty or malformed filter conservatively returns true.
bool BloomMayContain(std::string_view filter, std::string_view user_key);
bool BloomMayContainHash(std::string_view filter, uint64_t hash);

}  // namespace lsm
