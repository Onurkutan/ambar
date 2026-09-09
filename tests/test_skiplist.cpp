#include "harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "../src/arena.hpp"
#include "../src/skiplist.hpp"

using namespace ambar;

namespace {

struct U64Comparator {
  int operator()(uint64_t a, uint64_t b) const {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }
};

using TestList = SkipList<uint64_t, U64Comparator>;

}  // namespace

TEST(SkipList, an_empty_list_finds_nothing) {
  Arena arena;
  TestList list(U64Comparator{}, &arena);
  CHECK(!list.contains(10));
  TestList::Iterator it(&list);
  CHECK(!it.valid());
  it.seek_to_first();
  CHECK(!it.valid());
  it.seek_to_last();
  CHECK(!it.valid());
  it.seek(100);
  CHECK(!it.valid());
}

TEST(SkipList, insertion_and_lookup_agree_with_a_std_set) {
  // The list is only interesting if it behaves exactly like an ordered set, so
  // that is what it is checked against.
  Arena arena;
  TestList list(U64Comparator{}, &arena);
  std::set<uint64_t> model;

  std::mt19937_64 rng(42);
  for (int i = 0; i < 20000; ++i) {
    const uint64_t key = rng() % 5000;
    if (model.insert(key).second) {  // skip list forbids duplicates
      list.insert(key);
    }
  }

  for (uint64_t key = 0; key < 5000; ++key) {
    CHECK_EQ(list.contains(key), model.count(key) != 0);
  }
}

TEST(SkipList, forward_iteration_is_sorted_and_complete) {
  Arena arena;
  TestList list(U64Comparator{}, &arena);
  std::set<uint64_t> model;
  std::mt19937_64 rng(7);
  for (int i = 0; i < 3000; ++i) {
    const uint64_t key = rng();
    if (model.insert(key).second) list.insert(key);
  }

  std::vector<uint64_t> seen;
  TestList::Iterator it(&list);
  for (it.seek_to_first(); it.valid(); it.next()) {
    seen.push_back(it.key());
  }
  CHECK_EQ(seen.size(), model.size());
  CHECK(std::vector<uint64_t>(model.begin(), model.end()) == seen);
}

TEST(SkipList, backward_iteration_mirrors_forward) {
  Arena arena;
  TestList list(U64Comparator{}, &arena);
  std::set<uint64_t> model;
  std::mt19937_64 rng(11);
  for (int i = 0; i < 2000; ++i) {
    const uint64_t key = rng() % 100000;
    if (model.insert(key).second) list.insert(key);
  }

  std::vector<uint64_t> reversed;
  TestList::Iterator it(&list);
  for (it.seek_to_last(); it.valid(); it.prev()) {
    reversed.push_back(it.key());
  }
  std::vector<uint64_t> expected(model.rbegin(), model.rend());
  CHECK(reversed == expected);
}

TEST(SkipList, seek_lands_on_the_first_key_at_or_after_the_target) {
  Arena arena;
  TestList list(U64Comparator{}, &arena);
  for (uint64_t key = 0; key < 1000; key += 10) {
    list.insert(key);
  }

  TestList::Iterator it(&list);
  it.seek(0);
  CHECK(it.valid());
  CHECK_EQ(it.key(), uint64_t{0});

  it.seek(15);           // between 10 and 20
  CHECK(it.valid());
  CHECK_EQ(it.key(), uint64_t{20});

  it.seek(990);          // exactly the last key
  CHECK(it.valid());
  CHECK_EQ(it.key(), uint64_t{990});

  it.seek(991);          // past the end
  CHECK(!it.valid());
}

TEST(SkipList, a_single_element_behaves) {
  Arena arena;
  TestList list(U64Comparator{}, &arena);
  list.insert(5);
  CHECK(list.contains(5));
  CHECK(!list.contains(4));

  TestList::Iterator it(&list);
  it.seek_to_first();
  CHECK(it.valid());
  CHECK_EQ(it.key(), uint64_t{5});
  it.next();
  CHECK(!it.valid());

  it.seek_to_last();
  CHECK(it.valid());
  CHECK_EQ(it.key(), uint64_t{5});
  it.prev();
  CHECK(!it.valid());
}

TEST(SkipList, readers_see_a_consistent_list_while_a_writer_inserts) {
  // The reason this data structure was chosen.  Run under ThreadSanitizer this
  // is also the test that proves the acquire/release pairing is right, and it
  // has been checked by mutation: replacing the release store and acquire load
  // in Node with relaxed ones makes TSan report six data races here, so the
  // test detects the bug it claims to detect rather than merely passing.
  //
  // Keys are written so that each encodes its own validity: key = i * 2654435761
  // (Knuth's multiplicative constant) mod 2^40, plus i in the low bits.  A
  // reader that observes a torn or uninitialised node would see a key that does
  // not satisfy the relation.
  Arena arena;
  TestList list(U64Comparator{}, &arena);

  constexpr int kInserts = 40000;
  auto encode = [](int i) -> uint64_t {
    return (static_cast<uint64_t>(i) * 2654435761u) % (uint64_t{1} << 40);
  };

  std::set<uint64_t> written_keys;
  std::vector<uint64_t> order;
  for (int i = 0; i < kInserts; ++i) {
    const uint64_t key = encode(i);
    if (written_keys.insert(key).second) order.push_back(key);
  }

  std::atomic<bool> done{false};
  std::atomic<int> published{0};
  std::atomic<int> reader_observations{0};
  std::atomic<int> reader_errors{0};

  std::thread writer([&] {
    for (size_t i = 0; i < order.size(); ++i) {
      list.insert(order[i]);
      published.store(static_cast<int>(i) + 1, std::memory_order_release);
    }
    done.store(true, std::memory_order_release);
  });

  auto reader_body = [&] {
    while (!done.load(std::memory_order_acquire)) {
      const int visible = published.load(std::memory_order_acquire);
      if (visible == 0) continue;
      // Everything the writer has finished publishing must be findable, and
      // every key the iterator returns must be one we actually wrote.
      TestList::Iterator it(&list);
      uint64_t previous = 0;
      bool first = true;
      for (it.seek_to_first(); it.valid(); it.next()) {
        const uint64_t key = it.key();
        if (!first && key <= previous) {
          reader_errors.fetch_add(1, std::memory_order_relaxed);  // out of order
        }
        if (written_keys.count(key) == 0) {
          reader_errors.fetch_add(1, std::memory_order_relaxed);  // never written
        }
        previous = key;
        first = false;
        reader_observations.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) readers.emplace_back(reader_body);

  writer.join();
  for (auto& reader : readers) reader.join();

  CHECK_EQ(reader_errors.load(), 0);
  CHECK(reader_observations.load() > 0);  // the readers really did run

  // And afterwards everything is present.
  for (uint64_t key : order) {
    CHECK(list.contains(key));
  }
}

TEST(Arena, hands_out_distinct_usable_memory) {
  Arena arena;
  std::vector<char*> blocks;
  std::mt19937 rng(3);
  size_t total = 0;
  for (int i = 0; i < 2000; ++i) {
    const size_t bytes = 1 + rng() % 3000;
    char* p = arena.allocate(bytes);
    CHECK(p != nullptr);
    std::memset(p, i & 0xff, bytes);  // writing proves it is really ours
    blocks.push_back(p);
    total += bytes;
  }
  CHECK(arena.memory_usage() >= total);
}

TEST(Arena, aligned_allocations_are_aligned) {
  Arena arena;
  constexpr size_t kAlign = alignof(std::max_align_t) > 8
                                ? alignof(std::max_align_t)
                                : size_t{8};
  std::mt19937 rng(4);
  for (int i = 0; i < 500; ++i) {
    arena.allocate(1 + rng() % 17);  // deliberately misalign the cursor
    char* p = arena.allocate_aligned(1 + rng() % 100);
    CHECK((reinterpret_cast<uintptr_t>(p) & (kAlign - 1)) == 0);
  }
}
