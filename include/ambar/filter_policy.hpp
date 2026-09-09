// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// A compact summary of the keys in a table block, consulted before the block
// is read.
//
// The problem it solves is specific to the shape of an LSM tree.  A key that
// is absent has to be looked for in every level before the engine can say so,
// and each level that does not have it still costs a disk read.  A filter
// turns most of those reads into an in-memory test that answers "definitely
// not here" without touching the disk.  It is allowed to be wrong in one
// direction only: it may say a key might be present when it is not, which
// costs a wasted read, but it must never say a key is absent when it is
// present, which would lose data.
//
// The interface is abstract because the right filter depends on the workload,
// and because a database opened with one policy must refuse to be read with
// another -- the name is written into the table and checked on open.

#ifndef AMBAR_FILTER_POLICY_HPP_
#define AMBAR_FILTER_POLICY_HPP_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ambar {

class FilterPolicy {
 public:
  virtual ~FilterPolicy();

  // The key under which the filter is stored in the table's metaindex, as
  // "filter.<name>".
  //
  // What it does NOT do is protect you: a table written with one policy and
  // read with a differently named one is not refused, it is read as though it
  // had no filter at all.  No error, no log line -- just every absent-key
  // lookup turning from a memory probe into a disk read per level.  The name
  // is a lookup key, not a guard.
  //
  // So changing a policy's behaviour without changing its name gives wrong
  // answers, and changing the name silently gives slow ones.  The first is
  // worse, which is why the name exists; the second is the one that will
  // actually happen to somebody, which is why it is written down here.
  virtual const char* name() const = 0;

  // Appends a filter summarising `keys` to *dst.  The keys may contain
  // duplicates and are not required to be sorted.
  virtual void create_filter(const std::vector<std::string_view>& keys,
                             std::string* dst) const = 0;

  // Returns false only when `key` is definitely not among the keys the filter
  // in `filter` was built from.  Returning true is always permitted.
  //
  // Note what that does *not* say.  It is a statement about the filter it is
  // handed, not about the filter that was written.  A filter cannot detect
  // that its own bytes have been damaged: truncate one and the result is not
  // an invalid filter, it is a different valid filter over a different bit
  // array, and it will hide keys.  tests/test_bloom.cpp demonstrates that
  // happening, because it is the reason the block checksum is not optional --
  // integrity is the block layer's responsibility, and a filter read without
  // a verified checksum can silently lose data.
  //
  // Where the bytes are uninterpretable on their face -- too short to hold
  // even a header, or a probe count outside the encodable range -- the answer
  // is "might be present", so that a filter from a future format degrades
  // into an extra read rather than a missing key.
  virtual bool key_may_match(std::string_view key,
                             std::string_view filter) const = 0;
};

// A bloom filter using `bits_per_key` bits per key.
//
// Ten is the usual choice and gives roughly a 1 % false positive rate; the
// tests measure the rate rather than repeating that number on faith.  Raising
// it buys a lower rate at a linear cost in memory, and the curve flattens
// quickly -- see tests/test_bloom.cpp, which prints the measured rates.
std::unique_ptr<const FilterPolicy> new_bloom_filter_policy(int bits_per_key);

}  // namespace ambar

#endif  // AMBAR_FILTER_POLICY_HPP_
