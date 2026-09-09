// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "filter_block.hpp"

#include "encoding.hpp"

namespace ambar {

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy) {}

void FilterBlockBuilder::start_block(uint64_t block_offset) {
  const uint64_t wanted = block_offset >> kFilterBaseLog;

  // A loop rather than a single step: a data block larger than the region size
  // skips several regions at once, and the array is addressed by region index,
  // so every skipped one needs an entry or every later index is shifted.  The
  // first pass emits the real filter for the keys accumulated so far; the rest
  // emit zero-length ones, which no lookup consults (see the header) and which
  // read as "may match" if one ever does.
  while (wanted > filter_offsets_.size()) {
    generate_filter();
  }
}

void FilterBlockBuilder::add_key(std::string_view key) {
  key_starts_.push_back(keys_.size());
  keys_.append(key.data(), key.size());
}

void FilterBlockBuilder::generate_filter() {
  const size_t num_keys = key_starts_.size();
  if (num_keys == 0) {
    // No keys since the last filter: record the current position so this
    // region resolves to a zero-length filter, which the reader treats as
    // "may match" rather than as "empty, so absent".
    filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
    return;
  }

  key_starts_.push_back(keys_.size());  // sentinel, so every key has an end
  tmp_keys_.resize(num_keys);
  for (size_t i = 0; i < num_keys; ++i) {
    tmp_keys_[i] = std::string_view(keys_.data() + key_starts_[i],
                                    key_starts_[i + 1] - key_starts_[i]);
  }

  filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
  policy_->create_filter(tmp_keys_, &result_);

  tmp_keys_.clear();
  keys_.clear();
  key_starts_.clear();
}

std::string_view FilterBlockBuilder::finish() {
  if (!key_starts_.empty()) {
    generate_filter();
  }

  const uint32_t array_offset = static_cast<uint32_t>(result_.size());
  for (const uint32_t offset : filter_offsets_) {
    put_fixed32(&result_, offset);
  }
  put_fixed32(&result_, array_offset);
  result_.push_back(static_cast<char>(kFilterBaseLog));
  return result_;
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     std::string_view contents)
    : policy_(policy) {
  // One offset, the array position, and the base byte.
  constexpr size_t kMinimum = 5;
  if (contents.size() < kMinimum) return;

  base_lg_ = static_cast<unsigned char>(contents[contents.size() - 1]);

  // The shift below is `block_offset >> base_lg_`, and a shift by 64 or more
  // is undefined rather than merely wrong: on x86 the hardware masks it to
  // &63 and the answer is nonsense, but the compiler is entitled to delete the
  // branch that contains it.  The byte came from the file, so it can be
  // anything at all.  Leaving data_ null makes every query answer "may match",
  // which costs a read and loses nothing.
  if (base_lg_ >= 64) return;

  const uint32_t last_word =
      decode_fixed32(contents.data() + contents.size() - 5);
  if (last_word > contents.size() - kMinimum) return;

  data_ = contents.data();
  offset_ = data_ + last_word;
  num_ = (contents.size() - kMinimum - last_word) / 4;
}

bool FilterBlockReader::key_may_match(uint64_t block_offset,
                                      std::string_view key) const {
  // Every early return below is `true`, and deliberately so.  A filter block
  // that will not parse must degrade into an extra disk read, never into a
  // missing key -- the same rule the policy itself follows, for the same
  // reason.
  if (data_ == nullptr) return true;

  const uint64_t index = block_offset >> base_lg_;
  if (index >= num_) return true;

  const uint32_t start = decode_fixed32(offset_ + index * 4);
  const uint32_t limit = decode_fixed32(offset_ + index * 4 + 4);
  if (start > limit || limit > static_cast<uint32_t>(offset_ - data_)) {
    return true;
  }
  if (start == limit) {
    // An empty filter: this region held no keys of its own.  Nothing can be
    // ruled out.
    return true;
  }
  return policy_->key_may_match(key, std::string_view(data_ + start,
                                                      limit - start));
}

}  // namespace ambar
