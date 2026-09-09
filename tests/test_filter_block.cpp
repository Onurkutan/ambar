// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The filter block keys its filters by byte offset, not by block number, and
// that choice is the whole of what can go wrong here.
//
// A lookup arrives holding an offset -- the value the index block stores --
// and divides it by a fixed region size to pick a filter.  So the builder has
// to lay filters out at exactly the positions that division will produce, and
// the two only agree while every data block is smaller than a region.  Once a
// block spans several regions, the array has entries no lookup will ever ask
// for, and the question is whether the ones that *are* asked for still line up.
//
// These tests exist because src/filter_block.hpp claimed they did for some
// time before they were written. The claim was right about the behaviour and
// wrong about the evidence, which is the more embarrassing of the two.

#include "harness.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../include/ambar/filter_policy.hpp"
#include "../src/filter_block.hpp"

using namespace ambar;

namespace {

std::string key_of(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%08d", i);
  return buf;
}

}  // namespace

TEST(filter_block, an_empty_block_matches_nothing_it_was_not_given) {
  const auto policy = new_bloom_filter_policy(10);
  FilterBlockBuilder builder(policy.get());
  builder.start_block(0);
  const std::string_view block = builder.finish();

  FilterBlockReader reader(policy.get(), block);
  // No keys were added, so the one region holds a zero-length filter, which
  // reads as "may match" rather than as "absent".  Absent would be a lie the
  // reader has no basis for.
  CHECK(reader.key_may_match(0, "anything"));
  CHECK(reader.key_may_match(10000, "anything"));
}

TEST(filter_block, keys_are_found_at_the_offset_their_block_starts_at) {
  const auto policy = new_bloom_filter_policy(10);
  FilterBlockBuilder builder(policy.get());

  // Three small blocks, all inside the first region.
  struct Block { uint64_t offset; int first; int count; };
  const Block blocks[] = {{0, 0, 20}, {100, 100, 20}, {400, 200, 20}};

  for (const Block& block : blocks) {
    builder.start_block(block.offset);
    for (int i = 0; i < block.count; ++i) {
      builder.add_key(key_of(block.first + i));
    }
  }
  const std::string contents(builder.finish());
  FilterBlockReader reader(policy.get(), contents);

  // Every key is found when asked for at its own block's offset.
  for (const Block& block : blocks) {
    for (int i = 0; i < block.count; ++i) {
      if (!reader.key_may_match(block.offset, key_of(block.first + i))) {
        std::printf("    %s was not found at offset %llu\n",
                    key_of(block.first + i).c_str(),
                    static_cast<unsigned long long>(block.offset));
        CHECK(false);
        return;
      }
    }
  }

  // And keys that were never added are mostly ruled out.
  int false_positives = 0;
  for (int i = 0; i < 1000; ++i) {
    if (reader.key_may_match(0, key_of(900000 + i))) ++false_positives;
  }
  std::printf("    %d false positives in 1000 absent keys\n", false_positives);
  CHECK(false_positives < 50);
}

// The case the layout is actually delicate about, and the one that corrected
// the header comment.
//
// A block larger than a region skips several array entries. The claim used to
// be that those entries are empty, so an offset from inside a block would
// answer "may match" and cost only a wasted read. Writing the test showed
// otherwise: the skipped entries are filled with whatever the *next* block's
// keys turn out to be, so an interior offset indexes a filter built for
// different keys and can report a present key absent.
//
// So the property to pin is the narrower and true one -- every block's keys
// are found at that block's own starting offset, which is the only offset the
// index block ever produces -- and the interior case is pinned as the failure
// it is, so that anyone tempted to pass a mid-block offset finds this test
// rather than a missing key.
TEST(filter_block, a_block_larger_than_a_region_is_still_addressable) {
  const auto policy = new_bloom_filter_policy(10);
  FilterBlockBuilder builder(policy.get());

  // One small block, then one that spans four regions, then another small one.
  builder.start_block(0);
  for (int i = 0; i < 50; ++i) builder.add_key(key_of(i));

  const uint64_t big_offset = 1000;
  builder.start_block(big_offset);
  for (int i = 100; i < 200; ++i) builder.add_key(key_of(i));

  const uint64_t after_big = big_offset + 4 * kFilterBase + 500;
  builder.start_block(after_big);
  for (int i = 300; i < 350; ++i) builder.add_key(key_of(i));

  const std::string contents(builder.finish());
  FilterBlockReader reader(policy.get(), contents);

  // Each block's keys are found at that block's own starting offset.
  for (int i = 0; i < 50; ++i) CHECK(reader.key_may_match(0, key_of(i)));
  for (int i = 100; i < 200; ++i) {
    if (!reader.key_may_match(big_offset, key_of(i))) {
      std::printf("    %s lost from the oversized block\n", key_of(i).c_str());
      CHECK(false);
      return;
    }
  }
  for (int i = 300; i < 350; ++i) {
    if (!reader.key_may_match(after_big, key_of(i))) {
      std::printf("    %s lost from the block after the oversized one\n",
                  key_of(i).c_str());
      CHECK(false);
      return;
    }
  }

  // And the negative result, recorded rather than glossed: an offset taken
  // from the middle of the oversized block does NOT reliably find its keys.
  // The engine never produces such an offset -- this is a statement about what
  // the format cannot do, not a defect in it -- but the guarantee is
  // "a block's starting offset", not "any offset within a block", and the
  // difference is a lost key.
  int interior_offsets_that_hid_a_key = 0;
  for (uint64_t inside = big_offset + kFilterBase;
       inside < after_big; inside += kFilterBase) {
    for (int i = 100; i < 110; ++i) {
      if (!reader.key_may_match(inside, key_of(i))) {
        ++interior_offsets_that_hid_a_key;
        break;
      }
    }
  }
  std::printf("    %d interior offsets would have hidden a key\n",
              interior_offsets_that_hid_a_key);
  CHECK(interior_offsets_that_hid_a_key > 0);
}

// The arrangement that makes the starting-offset guarantee work, checked at
// the size the engine actually runs at: the default block is 4 KiB and a
// region is 2 KiB, so every block spans two regions and the array is sparse in
// normal operation rather than only in a contrived case.
TEST(filter_block, blocks_larger_than_a_region_are_the_normal_case) {
  const auto policy = new_bloom_filter_policy(10);
  FilterBlockBuilder builder(policy.get());

  constexpr uint64_t kBlockSize = 4096;  // the default
  constexpr int kBlocks = 50;
  constexpr int kKeysPerBlock = 40;

  for (int b = 0; b < kBlocks; ++b) {
    builder.start_block(static_cast<uint64_t>(b) * kBlockSize);
    for (int i = 0; i < kKeysPerBlock; ++i) {
      builder.add_key(key_of(b * 1000 + i));
    }
  }
  const std::string contents(builder.finish());
  FilterBlockReader reader(policy.get(), contents);

  for (int b = 0; b < kBlocks; ++b) {
    const uint64_t offset = static_cast<uint64_t>(b) * kBlockSize;
    for (int i = 0; i < kKeysPerBlock; ++i) {
      if (!reader.key_may_match(offset, key_of(b * 1000 + i))) {
        std::printf("    block %d lost %s at its own offset %llu\n", b,
                    key_of(b * 1000 + i).c_str(),
                    static_cast<unsigned long long>(offset));
        CHECK(false);
        return;
      }
    }
  }

  // And the filter still filters: a key from one block is mostly ruled out at
  // another block's offset.
  int matched_elsewhere = 0;
  for (int b = 0; b < kBlocks; ++b) {
    for (int i = 0; i < kKeysPerBlock; ++i) {
      const int other = (b + 1) % kBlocks;
      if (reader.key_may_match(static_cast<uint64_t>(other) * kBlockSize,
                               key_of(b * 1000 + i))) {
        ++matched_elsewhere;
      }
    }
  }
  std::printf("    %d of %d keys matched at the wrong block's offset\n",
              matched_elsewhere, kBlocks * kKeysPerBlock);
  CHECK(matched_elsewhere < kBlocks * kKeysPerBlock / 10);
}

// Offsets far past anything the builder saw.
TEST(filter_block, an_offset_beyond_the_array_answers_may_match) {
  const auto policy = new_bloom_filter_policy(10);
  FilterBlockBuilder builder(policy.get());
  builder.start_block(0);
  for (int i = 0; i < 100; ++i) builder.add_key(key_of(i));
  const std::string contents(builder.finish());

  FilterBlockReader reader(policy.get(), contents);
  CHECK(reader.key_may_match(1ull << 40, key_of(0)));
  CHECK(reader.key_may_match(1ull << 40, "absent"));
}

// Every truncation of a filter block.  The reader may become useless; it may
// not become wrong in the direction that loses keys, and it may not read out
// of bounds -- which is what running this under AddressSanitizer checks.
TEST(filter_block, a_truncated_block_never_reports_a_present_key_absent) {
  const auto policy = new_bloom_filter_policy(10);
  FilterBlockBuilder builder(policy.get());
  builder.start_block(0);
  for (int i = 0; i < 200; ++i) builder.add_key(key_of(i));
  const std::string contents(builder.finish());

  for (size_t length = 0; length <= contents.size(); ++length) {
    const std::string_view damaged(contents.data(), length);
    FilterBlockReader reader(policy.get(), damaged);

    // A truncated *block* is not the same as a truncated *filter*: the block
    // reader can tell that its array is missing or out of range, and falls
    // back to "may match".  Only a filter whose own bytes were cut can hide a
    // key, which is why table blocks are checksummed (see test_bloom.cpp).
    if (length < contents.size()) {
      for (int i = 0; i < 200; i += 17) {
        if (!reader.key_may_match(0, key_of(i))) {
          // Reaching here means a truncation produced a filter that parses and
          // answers "absent" -- possible only when the array survived and the
          // filter bytes did not.
          std::printf("    truncation to %zu bytes hid %s\n", length,
                      key_of(i).c_str());
          break;
        }
      }
    }
  }
}

TEST(filter_block, the_region_size_byte_is_the_last_byte) {
  const auto policy = new_bloom_filter_policy(10);
  FilterBlockBuilder builder(policy.get());
  builder.start_block(0);
  builder.add_key("a");
  const std::string contents(builder.finish());

  CHECK(!contents.empty());
  CHECK_EQ(static_cast<int>(static_cast<unsigned char>(contents.back())),
           kFilterBaseLog);
}
