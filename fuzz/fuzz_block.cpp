// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Arbitrary bytes as a table block: walked forwards, sought into, and walked
// backwards, under both comparators the engine uses.  The reverse walk is the
// interesting one -- prev() re-parses from a restart point the file chose.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "ambar/iterator.hpp"
#include "block.hpp"
#include "comparator.hpp"
#include "dbformat.hpp"
#include "format.hpp"

namespace {

void walk(std::string_view bytes, const ambar::Comparator* comparator,
          std::string_view target) {
  ambar::BlockContents contents;
  contents.data = bytes;
  contents.heap_allocated = false;
  contents.cachable = false;

  const ambar::Block block(contents);
  std::unique_ptr<ambar::Iterator> iter(block.new_iterator(comparator));

  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    (void)iter->key();
    (void)iter->value();
  }

  iter->seek(target);
  if (iter->valid()) {
    (void)iter->key();
    (void)iter->value();
  }

  // Block::find is a second parser of the same bytes, the one every point
  // lookup goes through, and it must agree with the iterator's seek on
  // every block the fuzzer can make: found where seek is valid, with the
  // same key and value, and refused where seek reports damage.
  //
  // Against a *fresh* iterator, which is the whole of the claim.  An
  // iterator's status is sticky -- once anything it did found damage it
  // reports damage for the rest of its life -- so the scan above can have
  // recorded a fault in the tail of a block that a find over the intact
  // head is right not to report, and holding the two against each other
  // compares a history with an answer.  The first version of this check
  // reused the iterator and the fuzz job found the difference in three
  // runs.
  {
    std::unique_ptr<ambar::Iterator> fresh(block.new_iterator(comparator));
    fresh->seek(target);

    std::string key;
    std::string_view value;
    ambar::Status status;
    const bool found = block.find(comparator, target, &key, &value, &status);

    if (found != fresh->valid()) std::abort();
    if (found && (key != fresh->key() || value != fresh->value())) std::abort();
    if (status.is_ok() != fresh->status().is_ok()) std::abort();
  }

  iter->seek(target);
  if (iter->valid()) {
    iter->next();
    if (iter->valid()) iter->prev();
  }

  for (iter->seek_to_last(); iter->valid(); iter->prev()) {
    (void)iter->key();
  }
  (void)iter->status();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  // The seek target comes from the input, so the fuzzer can steer it towards
  // keys the block actually holds.  The internal comparator reads an
  // eight-byte trailer from the target, so that one is built as the engine
  // builds a lookup key rather than handed raw bytes.
  const std::string_view user = input.substr(0, 8);
  const std::string lookup =
      ambar::make_lookup_key(user, ambar::kMaxSequenceNumber);

  walk(input, ambar::bytewise_comparator(), user);
  walk(input, ambar::internal_key_comparator(), lookup);
  return 0;
}
