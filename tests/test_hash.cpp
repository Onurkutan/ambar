// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Why the hash is tested apart from the filter that uses it.
//
// The filter's false positive rate is the number that matters, and measuring
// it is the obvious test.  It is also not sufficient, and the way it fails is
// worth recording, because the same mistake is easy to repeat.
//
// The first hash in this project produced a 15.7 % false positive rate on keys
// of the form "key_%08d" against a predicted 0.8 %.  It passed every check
// that treats a hash as a way of spreading keys out: 10,000 keys gave 10,000
// distinct hash values, 10,000 distinct probe deltas, six distinct probe
// positions per key, and a bit array at the theoretically optimal density.
// Nothing about the distribution was wrong.  What was wrong was that the
// probe sets of *different* keys stayed correlated, which no measurement of a
// single key's output can see.
//
// The property that separates the two hashes is avalanche: flipping one input
// bit should flip each output bit with probability one half.  That is a
// property of the function, not of any key set, so it is measured here rather
// than inferred from a rate on whichever inputs a test happened to pick.

#include "harness.hpp"

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "../src/hash.hpp"

using namespace ambar;

namespace {

// Fraction of output bits that flip when one input bit flips, over many keys.
// A good hash sits at 0.5 for every (input bit, output bit) pair; the worst
// pair is the one that matters, because a single sticky bit is enough to
// correlate probe sequences.
struct Avalanche {
  double worst;
  double mean;
};

// `samples` distinct keys are examined; when the whole input space is smaller
// than that, it is enumerated exhaustively instead, because sampling 2000 keys
// from 256 possible ones measures the sampler, not the hash.
Avalanche measure_avalanche(size_t key_bytes, int samples, long* used) {
  const size_t input_bits = key_bytes * 8;
  // Up to two bytes the whole input space fits in 65,536 keys, so it is
  // enumerated.  Sampling instead would draw duplicates -- 12,000 draws from
  // 65,536 repeat about a thousand times -- and a repeated key adds no
  // information while still counting towards the sample size the error bound
  // is computed from, which quietly tightens the bound below what the data
  // supports.  That showed up as the two-byte case being the worst at every
  // run, which is backwards: fewer input bits should mean fewer chances to
  // deviate, not more.
  const bool exhaustive = key_bytes <= 2;
  const long total = exhaustive ? (1L << input_bits) : samples;
  *used = total;

  std::vector<std::vector<int>> flips(input_bits, std::vector<int>(32, 0));

  uint64_t rng = 0x243f6a8885a308d3ull;
  auto next = [&rng]() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };

  for (long s = 0; s < total; ++s) {
    std::string key(key_bytes, '\0');
    for (size_t i = 0; i < key_bytes; ++i) {
      key[i] = exhaustive ? static_cast<char>((s >> (8 * i)) & 0xff)
                          : static_cast<char>(next() & 0xff);
    }
    const uint32_t base = bloom_hash(key);

    for (size_t bit = 0; bit < input_bits; ++bit) {
      std::string flipped = key;
      flipped[bit / 8] = static_cast<char>(
          flipped[bit / 8] ^ static_cast<char>(1 << (bit % 8)));
      const uint32_t diff = base ^ bloom_hash(flipped);
      for (int out = 0; out < 32; ++out) {
        if ((diff >> out) & 1u) ++flips[bit][static_cast<size_t>(out)];
      }
    }
  }

  double worst = 0.0;
  double sum = 0.0;
  size_t pairs = 0;
  for (const auto& row : flips) {
    for (const int count : row) {
      const double rate = static_cast<double>(count) / static_cast<double>(total);
      worst = std::max(worst, std::abs(rate - 0.5));
      sum += std::abs(rate - 0.5);
      ++pairs;
    }
  }
  return {worst, sum / static_cast<double>(pairs)};
}

}  // namespace

// The load-bearing test.  A hash that fails it can still pass every false
// positive measurement in test_bloom.cpp on the shapes those tests happen to
// use, and then cost reads on a shape nobody thought to try.
TEST(hash, one_input_bit_flips_every_output_bit_half_the_time) {
  std::printf("    key bytes    inputs    worst    bound     mean\n");
  for (const size_t bytes : {1u, 2u, 4u, 7u, 8u, 12u, 16u, 33u}) {
    long used = 0;
    const Avalanche a = measure_avalanche(bytes, 12000, &used);

    // The thresholds are derived, not chosen.  Each (input bit, output bit)
    // pair is a proportion over `used` trials, so a perfect hash still
    // deviates from one half by about sigma = sqrt(0.25 / used), and the mean
    // absolute deviation of a normal is 0.8 sigma.
    //
    // The multiples are calibrated against a hash already known to be sound,
    // rather than picked to make this one pass.  FNV-1a with a 64-bit
    // finalizer, measured identically, reaches about five sigma on the
    // exhaustive two-byte space and four on the one-byte space -- further out
    // than independent pairs would predict, because 32 output bits derived
    // from 16 input bits are not independent.  Six sigma clears that and still
    // fails a hash with a sticky bit by a wide margin.
    //
    // Short keys make the test weak, and the bound says so honestly: with one
    // byte there are 256 possible inputs, and no function can be told from
    // chance at any useful confidence.  The reference measures 0.125 there
    // against this hash's 0.141, both against a chance floor of 0.094.
    const double sigma = std::sqrt(0.25 / static_cast<double>(used));
    const double worst_bound = 6.0 * sigma;
    const double mean_bound = 2.0 * 0.8 * sigma;

    std::printf("    %9zu  %8ld   %6.4f   %6.4f   %6.4f\n", bytes, used,
                a.worst, worst_bound, a.mean);

    CHECK(a.worst < worst_bound);
    CHECK(a.mean < mean_bound);
  }
}

// A hash whose output is dominated by the last word would give identical
// values to keys sharing a long tail.  Cheap to check, and it is the shape a
// reversed-key or suffix-heavy workload produces.
TEST(hash, keys_differing_only_in_early_bytes_hash_apart) {
  std::set<uint32_t> seen;
  const std::string suffix(64, 's');
  for (int i = 0; i < 20000; ++i) {
    seen.insert(bloom_hash(std::to_string(i) + suffix));
  }
  // Birthday bound: 20,000 values in a 32-bit space collide about 46 times.
  CHECK(seen.size() > 19900);
}

TEST(hash, keys_differing_only_in_late_bytes_hash_apart) {
  std::set<uint32_t> seen;
  const std::string prefix(64, 'p');
  for (int i = 0; i < 20000; ++i) {
    seen.insert(bloom_hash(prefix + std::to_string(i)));
  }
  CHECK(seen.size() > 19900);
}

// Length must be part of the hash, or "a" and "a\0" collide -- and a
// length-prefixed store produces exactly those pairs.
TEST(hash, length_is_part_of_the_hash) {
  std::set<uint32_t> seen;
  for (size_t len = 0; len <= 64; ++len) {
    seen.insert(bloom_hash(std::string(len, 'x')));
  }
  CHECK_EQ(seen.size(), 65u);
}

TEST(hash, the_empty_key_is_hashable) {
  const uint32_t h = bloom_hash(std::string_view());
  CHECK(h != bloom_hash(std::string_view("\0", 1)));
}

// The hash is written into tables that outlive the process, so its value for a
// given input is part of the on-disk format.  Pinning a few values makes an
// accidental change -- a refactor, a different byte order -- fail here instead
// of making every existing table's filter useless.
TEST(hash, values_are_pinned_because_they_reach_disk) {
  struct Golden {
    const char* input;
    uint32_t expected;
  };
  static const Golden golden[] = {
      {"", 0x3836f068u},
      {"a", 0x23f68bc0u},
      {"hello", 0x9f757df4u},
      {"key_00000000", 0x0f250840u},
      {"0123456789abcdef", 0x04856abcu},
  };
  for (const Golden& g : golden) {
    const uint32_t actual = bloom_hash(g.input);
    if (actual != g.expected) {
      std::printf("    bloom_hash(\"%s\") = 0x%08x, pinned 0x%08x\n", g.input,
                  actual, g.expected);
    }
    CHECK_EQ(actual, g.expected);
  }
}
