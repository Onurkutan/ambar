// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Arbitrary bytes as a filter block, probed at offsets from zero to the
// largest, including the ones the shift byte at the end of the block would
// send out of range if it were trusted.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "ambar/filter_policy.hpp"
#include "filter_block.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static const std::unique_ptr<const ambar::FilterPolicy> policy =
      ambar::new_bloom_filter_policy(10);

  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const ambar::FilterBlockReader reader(policy.get(), input);

  // One key from the input, so the fuzzer can aim at whatever the block
  // holds, and one no filter was ever built over.
  const std::string_view key = input.substr(0, 16);
  const uint64_t base = static_cast<uint64_t>(ambar::kFilterBase);
  const uint64_t offsets[] = {0,        1,
                              base - 1, base,
                              base * 3, uint64_t{1} << 20,
                              uint64_t{1} << 40, ~uint64_t{0}};
  for (const uint64_t offset : offsets) {
    (void)reader.key_may_match(offset, key);
    (void)reader.key_may_match(offset, "never added");
  }
  return 0;
}
