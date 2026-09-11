// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Arbitrary bytes as a whole table file: footer, index, filter, data blocks.
// Everything the reader takes from the file -- every offset, size, count and
// length -- is checked against a file that is exactly the fuzzer's input.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "ambar/cache.hpp"
#include "ambar/filter_policy.hpp"
#include "ambar/iterator.hpp"
#include "ambar/options.hpp"
#include "ambar/status.hpp"
#include "comparator.hpp"
#include "dbformat.hpp"
#include "file.hpp"
#include "table.hpp"

namespace {

// The input is the file.  Reads are bounded by it the way the real
// implementation's are bounded by the file on disk, short read included.
class MemoryFile final : public ambar::RandomAccessFile {
 public:
  explicit MemoryFile(std::string_view bytes) : bytes_(bytes) {}

  ambar::Status read(uint64_t offset, size_t n, std::string_view* result,
                     char* scratch) const override {
    if (offset > bytes_.size()) {
      return ambar::Status::corruption("read starts past the end");
    }
    const size_t available = bytes_.size() - static_cast<size_t>(offset);
    const size_t got = std::min(n, available);
    if (got > 0) std::memcpy(scratch, bytes_.data() + offset, got);
    if (got != n) return ambar::Status::corruption("short read");
    *result = std::string_view(scratch, n);
    return ambar::Status::ok();
  }

 private:
  std::string_view bytes_;
};

void take_value(void*, std::string_view, std::string_view) {}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static const std::unique_ptr<const ambar::FilterPolicy> policy =
      ambar::new_bloom_filter_policy(10);

  const std::string_view input(reinterpret_cast<const char*>(data), size);

  // A fresh cache per input keeps each run independent of the ones before
  // it, which is what makes a crashing input reproducible on its own.
  const std::unique_ptr<ambar::Cache> cache = ambar::new_lru_cache(1 << 20);

  ambar::Options options;
  options.filter_policy = policy.get();
  options.block_cache = cache.get();

  MemoryFile file(input);
  std::unique_ptr<ambar::Table> table;
  if (!ambar::Table::open(options, ambar::internal_key_comparator(), &file,
                          input.size(), &table)
           .is_ok()) {
    return 0;
  }

  const ambar::ReadOptions read_options;
  const std::string lookup =
      ambar::make_lookup_key(input.substr(0, 8), ambar::kMaxSequenceNumber);

  {
    std::unique_ptr<ambar::Iterator> iter(table->new_iterator(read_options));
    for (iter->seek_to_first(); iter->valid(); iter->next()) {
      (void)iter->key();
      (void)iter->value();
    }
    (void)iter->status();

    iter->seek(lookup);
    if (iter->valid()) {
      (void)iter->key();
      iter->prev();
    }
    for (iter->seek_to_last(); iter->valid(); iter->prev()) {
      (void)iter->key();
    }
    (void)iter->status();
  }

  (void)table->internal_get(read_options, lookup, nullptr, take_value);
  (void)table->approximate_offset_of(lookup);
  return 0;
}
