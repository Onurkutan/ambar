// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The hash the bloom filter is built on.
//
// It lives in its own header so the tests can measure it directly.  That is
// not a convenience: the reason this file exists at all is that measuring the
// filter's false positive rate was not enough to tell a good hash from a bad
// one, and the property that does distinguish them can only be measured on
// the hash itself.

#ifndef AMBAR_HASH_HPP_
#define AMBAR_HASH_HPP_

#include <cstdint>
#include <string_view>

#include "encoding.hpp"

namespace ambar {

// A bloom filter needs k independent hash functions.  Computing k real hashes
// is wasteful, and it is not necessary: Kirsch and Mitzenmacher showed that
// h_i(x) = h1(x) + i * h2(x) gives the same asymptotic false positive rate as
// k independent functions.  So one hash is computed and the second is derived
// from it by rotation, which is what the delta below is.
//
// That result assumes h1 avalanches -- one input bit changing flips each
// output bit with probability one half.  A hash that merely spreads keys out
// is not enough, and the difference is not academic.  The obvious
// word-at-a-time construction (LevelDB's, and the first version of this file)
// passes every distribution check that is easy to write: 10,000 keys give
// 10,000 distinct hashes, 10,000 distinct deltas, six distinct probe
// positions per key, and a bit array at the theoretically optimal 45 %
// density.  On keys of the form "key_%08d" at ten bits per key it still
// returns a false positive rate of 15.7 % against a predicted 0.8 %, because
// the probe sets of different keys stay correlated even though each one is
// individually well spread.  Those keys -- a fixed prefix and a zero-padded
// number -- are not a contrived input; they are what database keys usually
// look like.
//
// The construction below mixes each word into a 64-bit state and then applies
// a full finalizer.  Measured on the same input: 1.0 %.  tests/test_bloom.cpp
// uses those keys deliberately, so a future simplification here fails the
// suite rather than quietly costing reads.
//
// The bytes are assembled explicitly little-endian rather than read as native
// words: a filter is written to disk, so a machine of the other byte order
// must compute the same hash or every lookup in an existing table fails.
inline uint64_t fmix64(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}

// The full 64-bit state, finalised.  bloom_hash below is its high half, and
// the block cache keys its table by the whole of it -- a fixed-width key
// that costs no allocation to compare, which the std::string it replaced
// did on every lookup.  Two block keys hashing to the same 64 bits is a
// collision the cache handles by comparing the bytes, not one it ignores.
inline uint64_t hash64(std::string_view key) {
  constexpr uint64_t kPrime = 0x9e3779b97f4a7c15ull;
  const char* data = key.data();
  size_t remaining = key.size();

  uint64_t h = kPrime ^ (static_cast<uint64_t>(key.size()) * 0xff51afd7ed558ccdull);

  while (remaining >= 8) {
    h ^= decode_fixed64(data);
    h *= kPrime;
    h ^= h >> 29;
    data += 8;
    remaining -= 8;
  }

  // The tail is folded in as one word rather than byte by byte, so that two
  // keys differing only in their last byte differ in the state before the
  // finalizer runs.
  uint64_t tail = 0;
  for (size_t i = 0; i < remaining; ++i) {
    tail |= static_cast<uint64_t>(static_cast<unsigned char>(data[i])) << (8 * i);
  }
  h ^= tail;
  h *= kPrime;
  return fmix64(h);
}

inline uint32_t bloom_hash(std::string_view key) {
  // The high half, because the low bits of a multiplicative hash are the
  // weakest and the probe positions are taken modulo the array length.
  return static_cast<uint32_t>(hash64(key) >> 32);
}

}  // namespace ambar

#endif  // AMBAR_HASH_HPP_
