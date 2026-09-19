// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "harness.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "../src/dbformat.hpp"

using namespace ambar;

TEST(DbFormat, internal_key_round_trips) {
  const std::string key = make_internal_key("abc", 42, ValueType::kValue);
  CHECK_EQ(key.size(), size_t{11});
  ParsedInternalKey parsed;
  CHECK(parse_internal_key(key, &parsed));
  CHECK_EQ(std::string(parsed.user_key), std::string("abc"));
  CHECK_EQ(parsed.sequence, SequenceNumber{42});
  CHECK(parsed.type == ValueType::kValue);
}

TEST(DbFormat, an_empty_user_key_is_legal) {
  const std::string key = make_internal_key("", 1, ValueType::kDeletion);
  ParsedInternalKey parsed;
  CHECK(parse_internal_key(key, &parsed));
  CHECK_EQ(parsed.user_key.size(), size_t{0});
  CHECK(parsed.type == ValueType::kDeletion);
}

TEST(DbFormat, keys_holding_nul_bytes_survive) {
  // Keys are bytes.  A NUL in the middle must not truncate anything.
  const std::string user("a\0b", 3);
  const std::string key = make_internal_key(user, 7, ValueType::kValue);
  ParsedInternalKey parsed;
  CHECK(parse_internal_key(key, &parsed));
  CHECK_EQ(std::string(parsed.user_key), user);
}

TEST(DbFormat, a_too_short_key_is_rejected_rather_than_read_out_of_bounds) {
  ParsedInternalKey parsed;
  CHECK(!parse_internal_key("", &parsed));
  CHECK(!parse_internal_key("1234567", &parsed));  // 7 bytes: no room for a trailer
}

TEST(DbFormat, an_unknown_value_type_is_rejected) {
  std::string key = make_internal_key("abc", 5, ValueType::kValue);
  key[key.size() - 8] = static_cast<char>(0x7f);  // neither 0 nor 1
  ParsedInternalKey parsed;
  CHECK(!parse_internal_key(key, &parsed));
}

TEST(DbFormat, user_keys_order_bytewise) {
  CHECK(compare_internal_keys(make_internal_key("a", 1, ValueType::kValue),
                              make_internal_key("b", 1, ValueType::kValue)) < 0);
  CHECK(compare_internal_keys(make_internal_key("b", 1, ValueType::kValue),
                              make_internal_key("a", 1, ValueType::kValue)) > 0);
  // A prefix sorts before the longer key that extends it.
  CHECK(compare_internal_keys(make_internal_key("ab", 1, ValueType::kValue),
                              make_internal_key("abc", 1, ValueType::kValue)) < 0);
}

TEST(DbFormat, the_newest_version_of_a_key_sorts_first) {
  // The property the merging iterator depends on for correctness.
  const std::string older = make_internal_key("k", 10, ValueType::kValue);
  const std::string newer = make_internal_key("k", 20, ValueType::kValue);
  CHECK(compare_internal_keys(newer, older) < 0);
}

TEST(DbFormat, a_tombstone_sorts_ahead_of_older_values_of_the_same_key) {
  // Delete at sequence 20 must shadow a value written at 10.
  const std::string tombstone = make_internal_key("k", 20, ValueType::kDeletion);
  const std::string value = make_internal_key("k", 10, ValueType::kValue);
  CHECK(compare_internal_keys(tombstone, value) < 0);
}

TEST(DbFormat, a_value_sorts_ahead_of_a_tombstone_at_the_same_sequence) {
  // Within one sequence, type breaks the tie.  Nothing depends on which way
  // round, but it must be total and stable, so it is pinned here.
  const std::string value = make_internal_key("k", 5, ValueType::kValue);
  const std::string tombstone = make_internal_key("k", 5, ValueType::kDeletion);
  CHECK(compare_internal_keys(value, tombstone) < 0);
}

TEST(DbFormat, a_lookup_key_sorts_at_or_before_every_visible_entry) {
  // A seek to (key, snapshot) must land on the newest entry at or below the
  // snapshot, and must never land on something newer.
  const SequenceNumber snapshot = 100;
  const std::string lookup = make_lookup_key("k", snapshot);

  CHECK(compare_internal_keys(lookup,
                              make_internal_key("k", 100, ValueType::kValue)) <= 0);
  CHECK(compare_internal_keys(lookup,
                              make_internal_key("k", 99, ValueType::kValue)) < 0);
  CHECK(compare_internal_keys(lookup,
                              make_internal_key("k", 101, ValueType::kValue)) > 0);
}

TEST(DbFormat, a_lookup_key_holds_the_three_forms_a_lookup_needs) {
  // The memtable's form is the internal key behind a varint32 of its
  // length; the internal key is the user key and the tag; and each is a
  // view into the one buffer, equal to what the string builders make.
  const LookupKey key("apple", 42);
  CHECK_EQ(std::string(key.user_key()), std::string("apple"));
  CHECK_EQ(std::string(key.internal_key()), make_lookup_key("apple", 42));
  std::string memtable_form;
  put_varint32(&memtable_form, 13);
  memtable_form += make_lookup_key("apple", 42);
  CHECK_EQ(std::string(key.memtable_key()), memtable_form);
  CHECK(key.internal_key().data() == key.user_key().data());
  CHECK(key.memtable_key().data() + 1 == key.internal_key().data());

  // Longer than the space inside the key: the same forms, from the heap.
  const std::string big(5000, 'x');
  const LookupKey long_key(big, 7);
  CHECK_EQ(std::string(long_key.user_key()), big);
  CHECK_EQ(std::string(long_key.internal_key()), make_lookup_key(big, 7));
  CHECK_EQ(long_key.memtable_key().size(), size_t{2 + 5000 + 8});

  // And an empty user key is a legal one.
  const LookupKey empty("", 1);
  CHECK_EQ(empty.user_key().size(), size_t{0});
  CHECK_EQ(empty.internal_key().size(), size_t{8});
}

TEST(DbFormat, sorting_a_shuffled_set_gives_the_documented_order) {
  std::vector<std::string> keys = {
      make_internal_key("a", 1, ValueType::kValue),
      make_internal_key("a", 3, ValueType::kValue),
      make_internal_key("a", 2, ValueType::kDeletion),
      make_internal_key("b", 1, ValueType::kValue),
      make_internal_key("", 9, ValueType::kValue),
  };
  std::mt19937 rng(5);
  std::shuffle(keys.begin(), keys.end(), rng);
  std::sort(keys.begin(), keys.end(), InternalKeyComparator{});

  std::vector<std::pair<std::string, SequenceNumber>> got;
  for (const auto& key : keys) {
    got.emplace_back(std::string(extract_user_key(key)), extract_sequence(key));
  }
  const std::vector<std::pair<std::string, SequenceNumber>> want = {
      {"", 9}, {"a", 3}, {"a", 2}, {"a", 1}, {"b", 1},
  };
  CHECK_EQ(got.size(), want.size());
  for (size_t i = 0; i < got.size() && i < want.size(); ++i) {
    CHECK_EQ(got[i].first, want[i].first);
    CHECK_EQ(got[i].second, want[i].second);
  }
}

TEST(DbFormat, the_maximum_sequence_number_round_trips) {
  const std::string key =
      make_internal_key("k", kMaxSequenceNumber, ValueType::kValue);
  CHECK_EQ(extract_sequence(key), kMaxSequenceNumber);
  CHECK(extract_value_type(key) == ValueType::kValue);
}
