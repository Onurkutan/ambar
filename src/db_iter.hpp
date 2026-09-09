// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Turns the engine's internal view into the one an application expects.
//
// The merged iterator underneath yields every version of every key: several
// entries per user key, newest first, with tombstones among them.  What a
// caller wants is one entry per key, the newest one visible at their snapshot,
// and no tombstones at all.
//
// Collapsing that is more delicate than it sounds, and the delicacy is all in
// one rule: a tombstone hides every older version of its key, so a deleted key
// must produce nothing rather than producing the value underneath it.  Get the
// rule wrong in one direction and deletes stop working; get it wrong in the
// other and a scan skips live keys.

#ifndef AMBAR_DB_ITER_HPP_
#define AMBAR_DB_ITER_HPP_

#include "ambar/iterator.hpp"
#include "comparator.hpp"
#include "dbformat.hpp"

namespace ambar {

class DBImpl;

// Takes ownership of `internal_iter`.
Iterator* new_db_iterator(DBImpl* db, const Comparator* user_comparator,
                          Iterator* internal_iter, SequenceNumber sequence,
                          uint32_t seed);

}  // namespace ambar

#endif  // AMBAR_DB_ITER_HPP_
