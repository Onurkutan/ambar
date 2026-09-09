// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// An iterator over an iterator: walks an index, and for each entry walks the
// block that entry points at.
//
// A table is two levels (index block -> data block) and a level of the tree is
// two levels (a list of files -> the contents of each file), so the same
// machinery serves both.  The part that differs -- how to turn an index value
// into an iterator -- is the function passed in.
//
// The awkward part, and the reason this is not a dozen lines: after a next()
// runs off the end of one block, the iterator must advance the index and open
// the following block, and if that block is empty it must keep going.  The
// skip loops below are that, and they are why an empty block anywhere in a
// table does not end a scan early.

#ifndef AMBAR_TWO_LEVEL_ITERATOR_HPP_
#define AMBAR_TWO_LEVEL_ITERATOR_HPP_

#include <string_view>

#include "ambar/iterator.hpp"
#include "ambar/options.hpp"

namespace ambar {

// Given the value stored in an index entry, produces an iterator over what it
// refers to.  `arg` carries whatever context the producer needs.
using BlockFunction = Iterator* (*)(void* arg, const ReadOptions& options,
                                    std::string_view index_value);

// Takes ownership of `index_iter`.
Iterator* new_two_level_iterator(Iterator* index_iter, BlockFunction function,
                                 void* arg, const ReadOptions& options);

}  // namespace ambar

#endif  // AMBAR_TWO_LEVEL_ITERATOR_HPP_
