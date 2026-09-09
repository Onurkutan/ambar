// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The one shape every ordered source of key/value pairs takes.
//
// An LSM tree spends most of its time merging sorted streams: a memtable with
// an immutable memtable, a block with the blocks around it, a level with the
// level below.  Every one of those is a different data structure, and the
// merging code should not care which.  So they all present this interface, and
// the merge logic is written once.
//
// Two rules the implementations share, because callers depend on them.
//
// First, key() and value() are valid only while the iterator has not moved.
// They return views into the iterator's own storage -- a block held in the
// cache, a node in a skip list -- and copying on every step would dominate the
// cost of a scan.  A caller that needs a key to outlive a next() copies it.
//
// Second, an error makes the iterator invalid rather than throwing, and
// status() explains it.  A caller that only walks forward can check status()
// once at the end, because valid() going false is the same signal for "the end
// of the data" and "a block that would not decode"; a caller that must
// distinguish them asks.

#ifndef AMBAR_ITERATOR_HPP_
#define AMBAR_ITERATOR_HPP_

#include <string_view>

#include "ambar/status.hpp"

namespace ambar {

class Iterator {
 public:
  Iterator() = default;
  virtual ~Iterator();

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  // False before the first positioning call, and after running off either end.
  virtual bool valid() const = 0;

  virtual void seek_to_first() = 0;
  virtual void seek_to_last() = 0;

  // Positions at the first key at or after `target`, leaving the iterator
  // invalid if there is none.
  virtual void seek(std::string_view target) = 0;

  virtual void next() = 0;
  virtual void prev() = 0;

  // Only valid() may be called on an invalid iterator; these two require it.
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;

  virtual Status status() const = 0;
};

// An iterator over nothing, or over a failure.  Returned instead of a null
// pointer so that callers merging several sources need no special case for
// "this level does not exist" or "this block would not open" -- an empty
// iterator merges correctly, and a null pointer crashes.
Iterator* new_empty_iterator();
Iterator* new_error_iterator(const Status& status);

}  // namespace ambar

#endif  // AMBAR_ITERATOR_HPP_
