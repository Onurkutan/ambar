// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Arbitrary bytes as a WriteBatch -- the payload of every log record -- then
// what recovery does with one: apply it to a memtable and read that back.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>

#include "ambar/iterator.hpp"
#include "ambar/write_batch.hpp"
#include "memtable.hpp"
#include "write_batch_internal.hpp"

namespace {

class Counter final : public ambar::WriteBatch::Handler {
 public:
  void put(std::string_view key, std::string_view value) override {
    bytes += key.size() + value.size();
  }
  void del(std::string_view key) override { bytes += key.size(); }

  size_t bytes = 0;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  ambar::WriteBatch batch;
  if (!ambar::WriteBatchInternal::set_contents(&batch, input).is_ok()) {
    return 0;
  }

  Counter counter;
  const bool iterates = batch.iterate(&counter).is_ok();

  ambar::MemTable* mem = new ambar::MemTable();
  mem->ref();
  const bool inserted =
      ambar::WriteBatchInternal::insert_into(batch, mem).is_ok();
  // Both walk the same bytes through the same decoder, so they must agree on
  // whether those bytes were a batch.
  if (inserted != iterates) std::abort();

  {
    std::unique_ptr<ambar::Iterator> iter(mem->new_iterator());
    for (iter->seek_to_first(); iter->valid(); iter->next()) {
      (void)iter->key();
      (void)iter->value();
    }
  }
  mem->unref();
  return 0;
}
