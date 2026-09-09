// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "comparator.hpp"

#include <algorithm>
#include <string_view>

#include "dbformat.hpp"

namespace ambar {
namespace {

int bytewise_compare(std::string_view a, std::string_view b) {
  const int r = a.compare(b);
  return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

void bytewise_shortest_separator(std::string* start, std::string_view limit) {
  const size_t min_length = std::min(start->size(), limit.size());
  size_t diff = 0;
  while (diff < min_length &&
         (*start)[diff] == limit[diff]) {
    ++diff;
  }

  // One is a prefix of the other: no shorter separator exists between them,
  // because anything shorter than `start` sorts before it.
  if (diff >= min_length) return;

  const auto byte = static_cast<uint8_t>((*start)[diff]);
  // 0xff cannot be incremented, and incrementing would otherwise have to carry
  // into the preceding byte -- which is possible but buys nothing here, since
  // the caller keeps the original key when no shortening is found.
  if (byte < 0xff && byte + 1 < static_cast<uint8_t>(limit[diff])) {
    (*start)[diff] = static_cast<char>(byte + 1);
    start->resize(diff + 1);
  }
}

void bytewise_short_successor(std::string* key) {
  // The first byte that can be incremented; everything after it is dropped.
  for (size_t i = 0; i < key->size(); ++i) {
    const auto byte = static_cast<uint8_t>((*key)[i]);
    if (byte != 0xff) {
      (*key)[i] = static_cast<char>(byte + 1);
      key->resize(i + 1);
      return;
    }
  }
  // All 0xff: no short successor exists, so the key is left as it is.
}

const Comparator kBytewise = {
    "ambar.BytewiseComparator",
    0,  // any length of key is readable
    bytewise_compare,
    bytewise_shortest_separator,
    bytewise_short_successor,
};

// -------------------------------------------------------- internal keys ---

void internal_shortest_separator(std::string* start, std::string_view limit) {
  const std::string_view start_user = extract_user_key(*start);
  const std::string_view limit_user = extract_user_key(limit);

  std::string shortened(start_user);
  bytewise_shortest_separator(&shortened, limit_user);

  // Only accept the shortening if it really is shorter and still sorts where
  // it must.  A separator that is not strictly between the two blocks sends
  // lookups to the wrong one, and the keys in the skipped block stop being
  // found -- a silent read failure, not a crash.
  if (shortened.size() >= start_user.size()) return;
  if (bytewise_compare(start_user, shortened) >= 0) return;
  if (bytewise_compare(shortened, limit_user) >= 0) return;

  // The trailer is rebuilt rather than truncated into.  kMaxSequenceNumber
  // with the seek type sorts *before* every real entry for that user key,
  // because the trailer orders descending -- so the separator lands after
  // everything in the earlier block and before everything in the later one.
  start->assign(
      make_internal_key(shortened, kMaxSequenceNumber, kValueTypeForSeek));
}

void internal_short_successor(std::string* key) {
  const std::string_view user = extract_user_key(*key);

  std::string shortened(user);
  bytewise_short_successor(&shortened);

  if (shortened.size() >= user.size()) return;
  if (bytewise_compare(user, shortened) >= 0) return;

  key->assign(
      make_internal_key(shortened, kMaxSequenceNumber, kValueTypeForSeek));
}

const Comparator kInternal = {
    "ambar.InternalKeyComparator",
    8,  // the sequence-and-type trailer
    compare_internal_keys,
    internal_shortest_separator,
    internal_short_successor,
};

}  // namespace

const Comparator* bytewise_comparator() { return &kBytewise; }
const Comparator* internal_key_comparator() { return &kInternal; }

}  // namespace ambar
