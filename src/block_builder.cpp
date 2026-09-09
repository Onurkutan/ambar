// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "block_builder.hpp"

#include <algorithm>
#include <cassert>

#include "encoding.hpp"

namespace ambar {

BlockBuilder::BlockBuilder(const Comparator* comparator, int restart_interval)
    : comparator_(comparator), restart_interval_(restart_interval) {
  assert(restart_interval >= 1);
  // Only the assertion in add() reads comparator_, and a release build has
  // none -- which makes the field itself look unused to a compiler that
  // checks per build, rather than per configuration.
  (void)comparator_;
  reset();
}

void BlockBuilder::reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);  // the first entry always restarts
  since_restart_ = 0;
  entries_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::current_size_estimate() const {
  return buffer_.size()                      // entries so far
         + restarts_.size() * sizeof(uint32_t)  // the restart array
         + sizeof(uint32_t);                 // the count that terminates it
}

void BlockBuilder::add(std::string_view key, std::string_view value) {
  assert(!finished_);
  assert(entries_ == 0 ||
         comparator_->compare(key, std::string_view(last_key_)) > 0);

  size_t shared = 0;
  if (since_restart_ < restart_interval_) {
    const size_t limit = std::min(last_key_.size(), key.size());
    while (shared < limit && last_key_[shared] == key[shared]) {
      ++shared;
    }
  } else {
    // Start a new prefix chain, so that a reader can enter the block here
    // without having decoded anything before it.
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    since_restart_ = 0;
  }

  const size_t non_shared = key.size() - shared;

  put_varint32(&buffer_, static_cast<uint32_t>(shared));
  put_varint32(&buffer_, static_cast<uint32_t>(non_shared));
  put_varint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  last_key_.assign(key.data(), key.size());
  ++since_restart_;
  ++entries_;
}

std::string_view BlockBuilder::finish() {
  for (const uint32_t restart : restarts_) {
    put_fixed32(&buffer_, restart);
  }
  put_fixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return buffer_;
}

}  // namespace ambar
