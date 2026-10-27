#pragma once

#include <memory>
#include <vector>

#include "iterator.h"

namespace lsm {

// Merges sorted child iterators into one stream in internal-key order using a
// binary min-heap. Equal internal keys cannot occur (sequence numbers are
// unique), so no tie-breaking is needed.
std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children);

}  // namespace lsm
