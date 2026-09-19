// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Reads a block written by BlockBuilder.
//
// The block is not parsed on construction.  Only the restart array is located,
// which is four bytes of arithmetic; the entries themselves are decoded lazily
// by the iterator as it walks.  A lookup that touches three entries out of a
// few hundred should pay for three.

#ifndef AMBAR_BLOCK_HPP_
#define AMBAR_BLOCK_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ambar/iterator.hpp"
#include "comparator.hpp"
#include "format.hpp"

namespace ambar {

class Block {
 public:
  explicit Block(const BlockContents& contents);
  ~Block();

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  size_t size() const { return size_; }

  // The caller owns the returned iterator.  If the block's header did not
  // decode, an error iterator is returned rather than a null pointer, so a
  // caller merging several blocks needs no special case.
  Iterator* new_iterator(const Comparator* comparator) const;

  // A point lookup: the first entry whose key is at or after `target`.  What
  // the iterator's seek does, without the iterator -- nothing on the heap,
  // no position kept -- which is what a lookup wants from a block it will
  // touch once.  The key is rebuilt into *key, since entries share prefixes
  // and a key rarely exists whole in the block; *value is a view into the
  // block.  Returns false with an ok *status past the last entry, and false
  // with a corruption where the block does not parse.
  bool find(const Comparator* comparator, std::string_view target,
            std::string* key, std::string_view* value, Status* status) const;

 private:
  class Iter;

  const char* data_;
  size_t size_;
  uint32_t restart_offset_ = 0;  // where the restart array begins
  uint32_t num_restarts_ = 0;
  bool owned_ = false;           // data_ must be delete[]'d
};

}  // namespace ambar

#endif  // AMBAR_BLOCK_HPP_
