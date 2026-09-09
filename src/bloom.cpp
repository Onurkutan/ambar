// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "ambar/filter_policy.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "hash.hpp"

namespace ambar {

FilterPolicy::~FilterPolicy() = default;

namespace {

class BloomFilterPolicy final : public FilterPolicy {
 public:
  explicit BloomFilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
    // The optimal number of probes for m bits per key is m * ln2.  Clamped:
    // below one the filter stops filtering, and above thirty the extra probes
    // cost more time than the false positives they prevent.
    auto k = static_cast<int>(static_cast<double>(bits_per_key) * 0.69);
    if (k < 1) k = 1;
    if (k > 30) k = 30;
    k_ = static_cast<size_t>(k);
  }

  const char* name() const override { return "ambar.BuiltinBloomFilter2"; }

  void create_filter(const std::vector<std::string_view>& keys,
                     std::string* dst) const override {
    size_t bits = keys.size() * static_cast<size_t>(bits_per_key_);

    // A filter over very few keys is dominated by rounding, and a short one is
    // dense enough to match almost anything.  Sixty-four bits is the floor.
    if (bits < 64) bits = 64;

    const size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);

    // k is stored in the last byte rather than assumed, so a table written
    // with one setting stays readable after the setting changes.
    dst->push_back(static_cast<char>(k_));

    char* array = &(*dst)[init_size];
    for (const std::string_view key : keys) {
      uint32_t h = bloom_hash(key);
      const uint32_t delta = (h >> 17) | (h << 15);  // rotate right by 17
      for (size_t j = 0; j < k_; ++j) {
        const size_t bitpos = h % bits;
        array[bitpos / 8] =
            static_cast<char>(array[bitpos / 8] |
                              static_cast<char>(1 << (bitpos % 8)));
        h += delta;
      }
    }
  }

  bool key_may_match(std::string_view key,
                     std::string_view filter) const override {
    const size_t len = filter.size();
    // Too short to hold even the trailing k byte: unreadable, so fall back to
    // reading the block rather than declaring the key absent.
    if (len < 2) return true;

    const char* array = filter.data();
    const size_t bits = (len - 1) * 8;

    const size_t k = static_cast<unsigned char>(array[len - 1]);
    // A k beyond the encodable range marks a filter from some future format.
    // Same rule: unreadable means "might be here".
    if (k > 30) return true;

    uint32_t h = bloom_hash(key);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (size_t j = 0; j < k; ++j) {
      const size_t bitpos = h % bits;
      if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
      h += delta;
    }
    return true;
  }

 private:
  int bits_per_key_;
  size_t k_;
};

}  // namespace

std::unique_ptr<const FilterPolicy> new_bloom_filter_policy(int bits_per_key) {
  return std::make_unique<BloomFilterPolicy>(bits_per_key);
}

}  // namespace ambar
