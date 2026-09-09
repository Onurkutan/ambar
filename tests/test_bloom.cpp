// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// A bloom filter has one property that must never fail and one that is only a
// matter of degree, and the tests treat them differently.
//
// Must never fail: a key that was inserted always matches.  A single false
// negative loses data -- the engine would skip a block that holds the key and
// report it absent.  This is asserted exhaustively, at every size, including
// the sizes where the implementation switches behaviour.
//
// Matter of degree: the false positive rate.  A wrong bound here costs reads,
// not correctness, so instead of asserting a number taken from a paper the
// tests measure the rate and check it against the theoretical curve with room
// to spare.  The measured numbers are printed, so the figure quoted in
// docs/DESIGN.md is evidence rather than folklore.

#include "harness.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../include/ambar/filter_policy.hpp"

using namespace ambar;

namespace {

std::string key_of(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%08d", i);
  return std::string(buf);
}

// Builds a filter over n generated keys and returns it together with the key
// strings, which must outlive the string_views handed to create_filter.
struct Built {
  std::vector<std::string> keys;
  std::string filter;
};

Built build(const FilterPolicy& policy, int n, int stride = 1) {
  Built out;
  out.keys.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) out.keys.push_back(key_of(i * stride));

  std::vector<std::string_view> views;
  views.reserve(out.keys.size());
  for (const auto& k : out.keys) views.emplace_back(k);

  policy.create_filter(views, &out.filter);
  return out;
}

// Fraction of keys that were never inserted but match anyway.
double false_positive_rate(const FilterPolicy& policy,
                           const std::string& filter, int n, int stride) {
  int hits = 0;
  const int trials = 10000;
  for (int i = 0; i < trials; ++i) {
    // Keys drawn from the same generator but outside the inserted range, so
    // they look like real keys rather than random noise -- a filter can behave
    // differently on structured input.
    const std::string probe = key_of(n * stride + 1 + i * stride);
    if (policy.key_may_match(probe, filter)) ++hits;
  }
  return static_cast<double>(hits) / trials;
}

}  // namespace

// ------------------------------------------------------- no false negatives ---

TEST(bloom, every_inserted_key_matches) {
  const auto policy = new_bloom_filter_policy(10);
  for (const int n : {1, 2, 3, 10, 100, 1000, 10000}) {
    const Built built = build(*policy, n);
    for (const auto& key : built.keys) {
      if (!policy->key_may_match(key, built.filter)) {
        std::printf("    false negative at n=%d, key=%s\n", n, key.c_str());
        CHECK(false);
      }
    }
  }
}

// The floor on filter size and the switch to a multi-byte array are both
// boundaries where an off-by-one would show up only at one particular size.
TEST(bloom, no_false_negatives_across_every_small_size) {
  const auto policy = new_bloom_filter_policy(10);
  for (int n = 0; n <= 200; ++n) {
    const Built built = build(*policy, n);
    for (const auto& key : built.keys) {
      CHECK(policy->key_may_match(key, built.filter));
    }
  }
}

TEST(bloom, no_false_negatives_at_every_bits_per_key) {
  for (int bits = 1; bits <= 32; ++bits) {
    const auto policy = new_bloom_filter_policy(bits);
    const Built built = build(*policy, 500);
    for (const auto& key : built.keys) {
      CHECK(policy->key_may_match(key, built.filter));
    }
  }
}

// Keys are byte strings.  A hash that stops at a NUL would collapse many
// distinct keys onto one, which shows up as an inflated false positive rate
// rather than as a crash.
TEST(bloom, binary_keys_with_nul_bytes_match) {
  const auto policy = new_bloom_filter_policy(10);
  std::vector<std::string> keys;
  for (int i = 0; i < 500; ++i) {
    std::string k("pre\0fix", 7);
    k += key_of(i);
    k.push_back('\0');
    keys.push_back(k);
  }
  std::vector<std::string_view> views(keys.begin(), keys.end());
  std::string filter;
  policy->create_filter(views, &filter);

  for (const auto& key : keys) CHECK(policy->key_may_match(key, filter));

  // And a key differing only after an embedded NUL must be distinguishable,
  // which it cannot be if the hash treats the string as NUL-terminated.
  int distinct = 0;
  for (int i = 0; i < 500; ++i) {
    std::string k("pre\0fix", 7);
    k += key_of(i + 100000);
    k.push_back('\0');
    if (!policy->key_may_match(k, filter)) ++distinct;
  }
  CHECK(distinct > 400);
}

TEST(bloom, empty_key_and_empty_key_set) {
  const auto policy = new_bloom_filter_policy(10);

  std::string filter;
  policy->create_filter({}, &filter);
  // An empty set still produces a usable filter rather than an empty string,
  // because the reader has no way to represent "no filter here" separately.
  CHECK(filter.size() >= 2);

  std::string with_empty;
  std::vector<std::string_view> keys = {std::string_view(), "a"};
  policy->create_filter(keys, &with_empty);
  CHECK(policy->key_may_match(std::string_view(), with_empty));
  CHECK(policy->key_may_match("a", with_empty));
}

TEST(bloom, duplicate_keys_are_harmless) {
  const auto policy = new_bloom_filter_policy(10);
  std::vector<std::string_view> keys(100, "same");
  std::string filter;
  policy->create_filter(keys, &filter);
  CHECK(policy->key_may_match("same", filter));
}

// -------------------------------------------------- false positive rate ---

// The test that found the bug this file's hash exists to avoid.
//
// A false positive rate is only meaningful against a particular key shape, and
// the shape that matters is the one real keys have: a fixed prefix and a
// zero-padded counter.  Measuring on random keys hides the failure entirely --
// the first version of the hash scored 0.8 % on random input and 15.7 % on
// these, while passing every distribution check (distinct hashes, distinct
// deltas, distinct probe positions, optimal bit density).  So the rate is
// measured across several shapes, and each is held to the same bound.
TEST(bloom, the_rate_holds_across_realistic_key_shapes) {
  const auto policy = new_bloom_filter_policy(10);

  struct Shape {
    const char* name;
    std::string (*member)(int);
    std::string (*probe)(int);
  };

  static const Shape shapes[] = {
      {"prefix + zero-padded counter",
       [](int i) { return key_of(i * 7); },
       [](int i) { return key_of(700001 + i * 7); }},
      {"unpadded decimal",
       [](int i) { return std::to_string(i * 7); },
       [](int i) { return std::to_string(700001 + i * 7); }},
      {"dotted path",
       [](int i) { return "a.b.c." + std::to_string(i * 7) + ".leaf"; },
       [](int i) { return "a.b.c." + std::to_string(700001 + i * 7) + ".leaf"; }},
      {"long common prefix, tail differs",
       [](int i) { return std::string(48, 'p') + key_of(i * 7); },
       [](int i) { return std::string(48, 'p') + key_of(700001 + i * 7); }},
      {"fixed 8-byte keys (one whole word)",
       [](int i) { char b[16]; std::snprintf(b, sizeof(b), "%08d", i * 7);
                   return std::string(b); },
       [](int i) { char b[16]; std::snprintf(b, sizeof(b), "%08d", 700001 + i * 7);
                   return std::string(b); }},
  };

  const int n = 10000;
  std::printf("    %-38s  measured\n", "key shape");
  for (const Shape& shape : shapes) {
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) keys.push_back(shape.member(i));
    std::vector<std::string_view> views(keys.begin(), keys.end());

    std::string filter;
    policy->create_filter(views, &filter);

    int hits = 0;
    for (int i = 0; i < n; ++i) {
      if (policy->key_may_match(shape.probe(i), filter)) ++hits;
    }
    const double rate = static_cast<double>(hits) / n;
    std::printf("    %-38s  %8.4f\n", shape.name, rate);

    // Predicted 0.8 % at ten bits per key.  Four times that is a wide margin
    // for a rate that varies with input; the first version of the hash scored
    // twenty times it on the first shape.
    CHECK(rate < 0.035);
  }
}

// The hash is written into tables that outlive the process, so its output is
// part of the on-disk format.  A machine of the other byte order, or a
// refactor that changes the mixing, must not silently make existing tables
// unreadable -- it must fail here first.
TEST(bloom, the_filter_bytes_are_a_stable_format) {
  const auto policy = new_bloom_filter_policy(10);
  std::vector<std::string_view> keys = {"hello", "world", ""};
  std::string filter;
  policy->create_filter(keys, &filter);

  // 64-bit floor: eight bytes of array plus the probe-count byte.
  CHECK_EQ(filter.size(), 9u);

  std::string hex;
  for (const char c : filter) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(c));
    hex += buf;
  }
  std::printf("    filter over {hello, world, \"\"} = %s\n", hex.c_str());
  CHECK_EQ(hex, "0c04545404c5140206");
}

TEST(bloom, false_positive_rate_tracks_the_theoretical_curve) {
  std::printf("    bits/key   measured   theoretical\n");
  for (const int bits : {4, 6, 8, 10, 14, 20}) {
    const auto policy = new_bloom_filter_policy(bits);
    const Built built = build(*policy, 10000, 7);
    const double measured =
        false_positive_rate(*policy, built.filter, 10000, 7);

    // (1 - e^(-k/m'))^k with k = 0.69 * bits, m' = bits.
    const double k = std::floor(bits * 0.69) < 1 ? 1.0 : std::floor(bits * 0.69);
    const double theoretical =
        std::pow(1.0 - std::exp(-k / bits), k);

    std::printf("    %8d   %8.4f   %11.4f\n", bits, measured, theoretical);

    // Generous: the point is to catch a filter that has stopped filtering, or
    // one whose hash has collapsed, not to police the third decimal place.
    CHECK(measured < theoretical * 3.0 + 0.02);
  }
}

TEST(bloom, ten_bits_per_key_costs_about_one_percent) {
  const auto policy = new_bloom_filter_policy(10);
  const Built built = build(*policy, 10000, 7);
  const double rate = false_positive_rate(*policy, built.filter, 10000, 7);
  std::printf("    measured false positive rate at 10 bits/key: %.4f\n", rate);
  CHECK(rate < 0.03);

  // Size: one byte per key plus the k byte, give or take rounding.
  CHECK(built.filter.size() >= 10000 * 10 / 8);
  CHECK(built.filter.size() <= 10000 * 10 / 8 + 8);
}

// More bits must actually buy a lower rate.  A filter that ignores its
// parameter would pass every test above and fail this one.
TEST(bloom, more_bits_per_key_lowers_the_rate) {
  double previous = 1.0;
  for (const int bits : {2, 4, 8, 16}) {
    const auto policy = new_bloom_filter_policy(bits);
    const Built built = build(*policy, 10000, 7);
    const double rate = false_positive_rate(*policy, built.filter, 10000, 7);
    CHECK(rate < previous);
    previous = rate;
  }
}

// ---------------------------------------------------------- robustness ---

TEST(bloom, determinism) {
  const auto policy = new_bloom_filter_policy(10);
  const Built a = build(*policy, 1000);
  const Built b = build(*policy, 1000);
  CHECK_EQ(a.filter, b.filter);
}

// Why the block checksum is not optional.
//
// The instinct is that a filter should fail safe: damage it and it should
// answer "might be present" for everything, costing reads but losing nothing.
// A bloom filter cannot do that, and this test exists to make the reason
// concrete rather than to let a comment assert it.
//
// A filter's bit array is addressed modulo its own length.  Truncate it and
// every key hashes to a different position -- the bytes are not detectably
// invalid, they are a valid filter over a smaller array, one that never saw
// these keys.  So it reports absent, and the engine skips a block that holds
// the key.  That is data loss, and it is silent.
//
// The conclusion the table format takes from this: a filter is only ever read
// after its block's CRC has been verified.  Nothing downstream of a filter can
// recover from bytes that were wrong on arrival.
TEST(bloom, truncation_hides_keys_which_is_why_blocks_are_checksummed) {
  const auto policy = new_bloom_filter_policy(10);
  const Built built = build(*policy, 200);

  int lengths_that_hid_a_key = 0;
  for (size_t len = 2; len < built.filter.size(); ++len) {
    const std::string_view damaged(built.filter.data(), len);
    for (const auto& key : built.keys) {
      if (!policy->key_may_match(key, damaged)) {
        ++lengths_that_hid_a_key;
        break;
      }
    }
  }
  std::printf("    %d of %zu truncated lengths hid at least one key\n",
              lengths_that_hid_a_key, built.filter.size() - 2);

  // If this ever reaches zero, either the filter gained the ability to detect
  // truncation -- in which case the format can be simplified -- or the test
  // has stopped exercising anything.  Both are worth noticing.
  CHECK(lengths_that_hid_a_key > 0);
}

// The damage the filter *can* see, it must handle safely: bytes too short to
// carry a header at all.
TEST(bloom, an_uninterpretably_short_filter_reports_might_be_present) {
  const auto policy = new_bloom_filter_policy(10);
  const Built built = build(*policy, 200);
  for (size_t len = 0; len < 2; ++len) {
    const std::string_view damaged(built.filter.data(), len);
    for (const auto& key : built.keys) {
      CHECK(policy->key_may_match(key, damaged));
    }
  }
}

TEST(bloom, an_out_of_range_probe_count_is_treated_as_unreadable) {
  const auto policy = new_bloom_filter_policy(10);
  Built built = build(*policy, 200);
  built.filter.back() = static_cast<char>(200);  // k far beyond the maximum
  for (const auto& key : built.keys) {
    CHECK(policy->key_may_match(key, built.filter));
  }
  // And an arbitrary non-member is also allowed through, which is the safe
  // answer for a filter that cannot be interpreted.
  CHECK(policy->key_may_match("not_a_member_at_all", built.filter));
}

TEST(bloom, the_policy_name_is_stable) {
  const auto policy = new_bloom_filter_policy(10);
  // Written into every table and compared on open.  Changing it is a format
  // change; this test is here so that change is deliberate.
  CHECK_EQ(std::string(policy->name()), "ambar.BuiltinBloomFilter2");
}
