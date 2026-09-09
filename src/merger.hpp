// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// One ordered stream out of several.
//
// This is where an LSM tree's central trick happens: the memtable, the
// memtable being flushed, and every level of table files are each sorted, and
// a read has to see them as one sequence with the newest version of each key
// first.  Because internal keys sort newest-first within a user key (see
// dbformat.hpp), the merge needs no notion of recency at all -- it just picks
// the smallest key, and recency falls out of the ordering.
//
// The implementation is a linear scan over the children rather than a heap.
// The reasoning: an engine merge has a handful of sources -- two memtables and
// seven levels, so ten at the outside, plus one per level-0 file during a
// compaction -- and a scan of ten pointers beats a heap whose sift costs
// branches and cache misses.  A heap wins when there are dozens.
//
// That is an argument, not a measurement, and it is worth saying so: nothing
// in this project times the two against each other.  tests/test_merger.cpp
// tests that the merge is *correct*, which is a different claim.

#ifndef AMBAR_MERGER_HPP_
#define AMBAR_MERGER_HPP_

#include "ambar/iterator.hpp"
#include "comparator.hpp"

namespace ambar {

// PRECONDITION: no two children may hold equal keys.
//
// This is a real constraint, not a tidiness preference, and it is stated here
// because it cannot be recovered from further down.  The direction-change path
// in prev() re-seeks each child to the current key and steps it back one, which
// is exactly right when only the current child holds that key and drops a
// duplicate when another one does.  Making reverse iteration the exact inverse
// of forward iteration in the presence of duplicates would need per-child
// state about which copy was last returned -- a real cost on every step, to
// support a case that cannot occur.
//
// It cannot occur because the engine only ever merges internal keys, and an
// internal key carries a sequence number that is unique across the whole
// database.  Two sources holding the same user key hold different internal
// keys.  A debug build checks the precondition on every step rather than
// trusting the argument.
//
// Takes ownership of `children[0..n)`; the array itself stays the caller's.
Iterator* new_merging_iterator(const Comparator* comparator,
                               Iterator** children, int n);

}  // namespace ambar

#endif  // AMBAR_MERGER_HPP_
