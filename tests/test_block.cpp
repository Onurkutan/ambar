// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Block::find, the point lookup that does what the iterator's seek does
// without an iterator.  It is a second implementation of the same search,
// so the test that matters is agreement: for every target a block can be
// asked about, both land on the same entry or both on none.
#include "harness.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../src/block.hpp"
#include "../src/block_builder.hpp"
#include "../src/comparator.hpp"
#include "../src/encoding.hpp"

using namespace ambar;

namespace {

// A block over the given entries, in the caller's string; the Block views it.
std::string build(const std::vector<std::pair<std::string, std::string>>& entries,
                  int restart_interval = 16) {
  BlockBuilder builder(bytewise_comparator(), restart_interval);
  for (const auto& [key, value] : entries) builder.add(key, value);
  return std::string(builder.finish());
}

Block over(const std::string& bytes) {
  BlockContents contents;
  contents.data = bytes;
  contents.cachable = false;
  contents.heap_allocated = false;
  return Block(contents);
}

// Keys that share prefixes, so that the block's prefix compression has
// something to compress and find has keys to rebuild.
std::vector<std::pair<std::string, std::string>> shared_prefix_entries(int n) {
  std::vector<std::pair<std::string, std::string>> out;
  for (int i = 0; i < n; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "user/%04d/item%02d", i / 7, i % 7);
    out.emplace_back(key, "v" + std::to_string(i));
  }
  return out;
}

}  // namespace

TEST(block, find_agrees_with_seek_on_every_target) {
  const auto entries = shared_prefix_entries(500);
  for (const int restart_interval : {1, 3, 16}) {
    const std::string bytes = build(entries, restart_interval);
    const Block block = over(bytes);
    std::unique_ptr<Iterator> iter(block.new_iterator(bytewise_comparator()));

    std::vector<std::string> targets;
    for (const auto& [key, value] : entries) {
      targets.push_back(key);        // present
      targets.push_back(key + "!");  // just after: the next entry
      targets.push_back(key.substr(0, key.size() - 1));  // just before
    }
    targets.push_back("");            // before the first
    targets.push_back("user/9999");   // after the last
    targets.push_back("zzz");

    int disagreements = 0;
    for (const std::string& target : targets) {
      iter->seek(target);
      std::string key;
      std::string_view value;
      Status status;
      const bool found =
          block.find(bytewise_comparator(), target, &key, &value, &status);
      CHECK_OK(status);
      if (found != iter->valid() ||
          (found && (key != iter->key() || value != iter->value()))) {
        if (++disagreements <= 3) {
          std::printf("    restart %d, target \"%s\": find %s, seek %s\n",
                      restart_interval, target.c_str(),
                      found ? key.c_str() : "nothing",
                      iter->valid() ? std::string(iter->key()).c_str()
                                    : "nothing");
        }
      }
    }
    CHECK_EQ(disagreements, 0);
  }
}

TEST(block, find_on_an_empty_block_finds_nothing) {
  const std::string bytes = build({});
  const Block block = over(bytes);
  std::string key;
  std::string_view value;
  Status status;
  CHECK(!block.find(bytewise_comparator(), "a", &key, &value, &status));
  CHECK_OK(status);
}

TEST(block, find_refuses_a_block_that_does_not_parse) {
  // The same damage the iterator refuses, refused the same way: a restart
  // point past the entries, and an entry that claims to share more of the
  // previous key than there is.
  const auto entries = shared_prefix_entries(40);
  const std::string good = build(entries, 4);

  {
    // The first restart point is at offset 0 by construction; pointing it
    // past the entry region is a contradiction the binary search must see.
    std::string bytes = good;
    const uint32_t restarts = decode_fixed32(bytes.data() + bytes.size() - 4);
    CHECK(restarts >= 2);
    const size_t array = bytes.size() - 4 - restarts * 4;
    encode_fixed32(&bytes[array + 4], 0xffffffffu);  // the second point
    const Block block = over(bytes);
    std::string key;
    std::string_view value;
    Status status;
    // A target the binary search reaches through that point: below the
    // middle, above the first.
    CHECK(!block.find(bytewise_comparator(), "user/0000/item05", &key, &value,
                      &status));
    CHECK(status.is_corruption());
    std::unique_ptr<Iterator> iter(block.new_iterator(bytewise_comparator()));
    iter->seek("user/0000/item05");
    CHECK(!iter->valid());
    CHECK(iter->status().is_corruption());
  }
  {
    // The second entry of the first region: its shared length is byte 0 of
    // the entry.  Set it past the first key's length.
    std::string bytes = good;
    // Entry 0 is: shared(0) non_shared(k) value(v), key, value.
    const size_t first_key = static_cast<unsigned char>(bytes[1]);
    const size_t first_value = static_cast<unsigned char>(bytes[2]);
    const size_t second = 3 + first_key + first_value;
    bytes[second] = static_cast<char>(200);
    const Block block = over(bytes);
    std::string key;
    std::string_view value;
    Status status;
    // A target in the first region, so that the walk from its restart
    // point passes through the damaged entry; a target in a later region
    // would be found without ever reading it, which is what the restart
    // array is for.
    CHECK(!block.find(bytewise_comparator(), "user/0000/item02", &key, &value,
                      &status));
    CHECK(status.is_corruption());
    // By that check and not a later one: a walk that trusted the length
    // would desynchronise and be refused further on, after reading what
    // it should not have.
    CHECK(status.to_string().find("shared prefix longer") !=
          std::string::npos);
  }
  {
    // Keys shorter than the comparator needs: the internal-key comparator
    // reads an eight-byte trailer, so a two-byte key would be read from
    // before its start.  Refused at the restart key the binary search
    // touches, before any comparison -- and by that check, whose words
    // differ from the walk's, since a walk would refuse the same key later
    // with the comparison already made.
    const std::string bytes = build({{"ab", "1"}, {"cd", "2"}, {"ef", "3"}}, 1);
    const Block block = over(bytes);
    std::string key;
    std::string_view value;
    Status status;
    CHECK(!block.find(internal_key_comparator(), "cd", &key, &value, &status));
    CHECK(status.is_corruption());
    CHECK(status.to_string().find("restart key is too short") !=
          std::string::npos);
  }
  {
    // And a short key inside a region, reached by the walk: the first key
    // is long enough to stand as a restart key, the second shares ten
    // bytes of it and adds one -- until its shared length is set to zero,
    // which leaves it one byte long.
    std::string bytes = build({{"abcdefghij", "1"}, {"abcdefghijk", "2"}}, 16);
    // Entry 1 begins after entry 0's three header bytes, ten key bytes and
    // one value byte; its first byte is the shared length.
    bytes[14] = 0;
    const Block block = over(bytes);
    std::string key;
    std::string_view value;
    Status status;
    CHECK(!block.find(internal_key_comparator(), "abcdefghijZZZZZZZZ", &key,
                      &value, &status));
    CHECK(status.is_corruption());
    CHECK(status.to_string().find("key is too short") != std::string::npos);
    CHECK(status.to_string().find("restart key") == std::string::npos);
  }
}

TEST(block, damage_in_the_tail_is_seen_only_by_what_reaches_it) {
  // Both parsers read lazily, so a block damaged near its end still
  // answers a lookup that lands before the damage, and refuses one that
  // has to walk into it -- find and a fresh iterator's seek alike.  What
  // does see the damage early is an iterator that has already walked the
  // whole block, and its status stays damaged for the rest of its life:
  // the fuzz target's first comparison of find with seek reused such an
  // iterator, and the fuzz job reported the difference as a disagreement.
  const auto entries = shared_prefix_entries(40);
  const std::string whole = build(entries, 16);

  // The last entry starts where the entries of the first thirty-nine end,
  // which is the restart array of a block built from those alone.
  const std::vector<std::pair<std::string, std::string>> head(
      entries.begin(), entries.end() - 1);
  const std::string shorter = build(head, 16);
  const uint32_t head_restarts =
      decode_fixed32(shorter.data() + shorter.size() - 4);
  const size_t last_entry = shorter.size() - 4 - head_restarts * 4;
  CHECK_EQ(whole.substr(0, last_entry), shorter.substr(0, last_entry));

  // Its third header byte is the value's length; claim more than the
  // block holds.
  std::string damaged = whole;
  damaged[last_entry + 2] = 0x7f;
  const Block block = over(damaged);

  std::unique_ptr<Iterator> scan(block.new_iterator(bytewise_comparator()));
  int scanned = 0;
  for (scan->seek_to_first(); scan->valid(); scan->next()) ++scanned;
  CHECK_EQ(scanned, 39);
  CHECK(scan->status().is_corruption());

  // Before the damage: found, by both, with nothing wrong reported.
  {
    const std::string& target = entries[5].first;
    std::string key;
    std::string_view value;
    Status status;
    CHECK(block.find(bytewise_comparator(), target, &key, &value, &status));
    CHECK_OK(status);
    CHECK_EQ(key, target);
    CHECK_EQ(std::string(value), entries[5].second);

    std::unique_ptr<Iterator> fresh(block.new_iterator(bytewise_comparator()));
    fresh->seek(target);
    CHECK(fresh->valid());
    CHECK_OK(fresh->status());
    // The scanned iterator lands on the same entry and still says damaged.
    scan->seek(target);
    CHECK(scan->valid());
    CHECK(scan->status().is_corruption());
  }
  // Into the damage: refused, by both.
  {
    const std::string& target = entries.back().first;
    std::string key;
    std::string_view value;
    Status status;
    CHECK(!block.find(bytewise_comparator(), target, &key, &value, &status));
    CHECK(status.is_corruption());

    std::unique_ptr<Iterator> fresh(block.new_iterator(bytewise_comparator()));
    fresh->seek(target);
    CHECK(!fresh->valid());
    CHECK(fresh->status().is_corruption());
  }
}
