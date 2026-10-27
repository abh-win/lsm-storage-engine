#pragma once

#include <string_view>

#include "lsm/status.h"

namespace lsm {

// Iterator over internal (key, value) pairs in internal-key order.
// key()/value() remain valid until the next mutation of the iterator.
class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view internal_target) = 0;
  virtual void Next() = 0;
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;
  virtual Status status() const { return Status::OK(); }
};

}  // namespace lsm
