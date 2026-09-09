// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Builds one block of a table file.
//
// Keys arriving in sorted order share long prefixes -- "user:1000001" follows
// "user:1000000" -- so each entry stores only the part of its key that differs
// from the one before it.  On real key sets that is most of a table's size.
//
// The saving costs something: an entry can only be understood by walking every
// entry before it, which would make a lookup inside a block linear and would
// make backwards iteration impossible.  So the prefix chain is broken every
// `restart_interval` entries at a *restart point* -- an entry that stores its
// key in full -- and the offsets of those points are written at the end of the
// block.  A lookup binary-searches the restart array, then scans forward at
// most `restart_interval` entries.
//
//   entry:  varint32 shared | varint32 non_shared | varint32 value_len
//           | key_delta[non_shared] | value[value_len]
//
//   block:  entry* | fixed32 restart_offset* | fixed32 restart_count
//
// The restart count goes last because the reader finds it by subtracting four
// from the block length, and from it derives where the entries end.

#ifndef AMBAR_BLOCK_BUILDER_HPP_
#define AMBAR_BLOCK_BUILDER_HPP_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "comparator.hpp"

namespace ambar {

class BlockBuilder {
 public:
  // `comparator` is used only to check, in debug builds, that keys arrive in
  // order.  It cannot be assumed: internal keys sort by user key ascending and
  // then by sequence *descending*, so a plain byte comparison rejects a
  // perfectly ordered run the moment two versions of one key appear.
  //
  // A restart every `restart_interval` entries.  Sixteen is the usual
  // compromise: the restart array costs four bytes per restart point, and a
  // lookup scans at most that many entries after the binary search.
  explicit BlockBuilder(const Comparator* comparator, int restart_interval = 16);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // Keys must arrive strictly increasing.  Violating that does not corrupt the
  // block in a way the writer notices -- it produces a block whose binary
  // search silently returns wrong answers -- so it is asserted in debug builds
  // and documented here as the caller's obligation.
  void add(std::string_view key, std::string_view value);

  // Appends the restart array and returns the finished block.  The builder
  // must not be added to again until reset().
  std::string_view finish();

  void reset();

  bool empty() const { return entries_ == 0; }

  // Size the block would occupy if finished now, used to decide when to cut a
  // block.  Exact rather than approximate, including the restart array that
  // has not been written yet.
  size_t current_size_estimate() const;

  // The last key added, which the index block needs.
  std::string_view last_key() const { return last_key_; }

 private:
  const Comparator* const comparator_;
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int since_restart_ = 0;
  int entries_ = 0;
  bool finished_ = false;
  std::string last_key_;
};

}  // namespace ambar

#endif  // AMBAR_BLOCK_BUILDER_HPP_
