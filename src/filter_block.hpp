// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The filter block: every data block's bloom filter, in one block at the end
// of the table.
//
// The obvious layout -- one filter per data block, in an array indexed by
// block number -- does not work, because the reader does not have a block
// number.  What a lookup has, after consulting the index, is a byte offset.
// So filters are keyed by offset instead: one filter covers each aligned
// 2 KiB region of the data section, and a lookup for a block at offset `o`
// consults filter number `o >> 11`.
//
// The invariant this rests on, stated plainly because it is load-bearing:
//
//     a lookup must arrive with the offset a data block STARTS at.
//
// That is the value the index block stores, so every lookup in the engine
// satisfies it. Nothing recovers if one does not. A block larger than a region
// spans several array entries, and the entries it skips are filled with
// whatever the *next* block's keys turn out to be -- so an offset taken from
// the middle of a block indexes a filter built for different keys and can
// report a key absent that is present. That is a lost key, not a wasted read.
//
// An earlier version of this comment claimed those interior entries were empty
// and answered "may match". They are not, and the test that was written to
// demonstrate it failed instead: tests/test_filter_block.cpp now pins what is
// actually true, including that the arrangement is correct for every block's
// own starting offset even when blocks are larger than a region -- which the
// default configuration makes routine, since block_size is 4 KiB and a region
// is 2 KiB.
//
//   filter 0 | filter 1 | ... | filter n-1
//   fixed32 offset of filter 0 | ... | fixed32 offset of filter n-1
//   fixed32 offset of the offset array
//   uint8 region size, as a power of two

#ifndef AMBAR_FILTER_BLOCK_HPP_
#define AMBAR_FILTER_BLOCK_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ambar/filter_policy.hpp"

namespace ambar {

// 2 KiB regions.  Small enough that a table of small blocks does not waste
// filters, large enough that the offset array stays a rounding error.
constexpr int kFilterBaseLog = 11;
constexpr size_t kFilterBase = size_t{1} << kFilterBaseLog;

class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const FilterPolicy* policy);

  FilterBlockBuilder(const FilterBlockBuilder&) = delete;
  FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

  // Called when a data block starts at `block_offset`.  Must be called before
  // the keys of that block are added.
  void start_block(uint64_t block_offset);

  void add_key(std::string_view key);

  // Finishes the block and returns it.  The result stays valid until this
  // builder is destroyed.
  std::string_view finish();

 private:
  void generate_filter();

  const FilterPolicy* policy_;

  // Keys of the current filter, flattened into one buffer with an index of
  // starts.  One allocation per filter instead of one per key.
  std::string keys_;
  std::vector<size_t> key_starts_;

  std::string result_;
  std::vector<uint32_t> filter_offsets_;
  std::vector<std::string_view> tmp_keys_;
};

class FilterBlockReader {
 public:
  // `contents` must outlive the reader.  A reader built from a block that does
  // not decode answers "may match" to everything, which costs reads and loses
  // nothing.
  FilterBlockReader(const FilterPolicy* policy, std::string_view contents);

  bool key_may_match(uint64_t block_offset, std::string_view key) const;

 private:
  const FilterPolicy* policy_;
  const char* data_ = nullptr;    // start of the block
  const char* offset_ = nullptr;  // start of the offset array
  size_t num_ = 0;                // number of filters
  size_t base_lg_ = 0;
};

}  // namespace ambar

#endif  // AMBAR_FILTER_BLOCK_HPP_
