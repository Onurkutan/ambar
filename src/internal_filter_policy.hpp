// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Wraps a filter policy so that it summarises user keys rather than the
// internal keys a table actually stores.
//
// Without this the engine is broken in the worst available way, and the bug
// hides behind a passing test suite.  A table stores internal keys -- a user
// key plus a sequence number -- so a filter built over what the table writes
// is a filter over "key_42 at sequence 1017".  A lookup arrives asking for
// "key_42 as of the latest snapshot", which is a *different* internal key,
// one that was never inserted.  The filter answers, correctly for the question
// it was asked, that it has never seen it.  The table then reports the key
// missing without reading the block that contains it.
//
// Every symptom of that points away from the cause.  The file is intact, the
// checksums pass, the index is right, iteration returns every key -- only
// point lookups fail, and only when a filter is configured.  It was caught
// here by a test that counts disk reads: 1,000 lookups for keys that were
// present read twelve blocks between them, which is not a plausible number for
// a thousand successful lookups.
//
// So the trailer is stripped before the key reaches the filter, on both the
// building and the querying side.

#ifndef AMBAR_INTERNAL_FILTER_POLICY_HPP_
#define AMBAR_INTERNAL_FILTER_POLICY_HPP_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ambar/filter_policy.hpp"

namespace ambar {

// `user_policy` is borrowed and must outlive the result.
//
// The name reported is the wrapped policy's own, unchanged.  The wrapper is
// not a different filter -- it is the same filter over a different projection
// of the key -- and a table written by a build that wraps must stay readable
// by one that does too, which is every build of this engine.
std::unique_ptr<const FilterPolicy> new_internal_filter_policy(
    const FilterPolicy* user_policy);

}  // namespace ambar

#endif  // AMBAR_INTERNAL_FILTER_POLICY_HPP_
