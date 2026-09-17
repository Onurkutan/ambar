// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The block cache, through its public interface.  It had no test of its
// own until its table was rewritten to key by hash rather than by string,
// which is the kind of change that deserves one: everything below held
// before the rewrite and has to hold after it.
//
// The one case a workload cannot be relied on to produce is a hash
// collision -- two keys with the same 64 bits, chained behind one bucket
// -- so the last test brings its own pair, found once by cycle-finding
// over the hash and pinned here.  The hash is fixed by the filters on
// disk, so the pair stays a pair.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ambar/cache.hpp"
#include "harness.hpp"
#include "hash.hpp"

using namespace ambar;

namespace {

// Values are heap ints so the deleter has something to free, and a count
// of deletions says when the cache let go of what.
int deletions = 0;
void delete_int(void* value) {
  delete static_cast<int*>(value);
  ++deletions;
}

int value_of(Cache* cache, Cache::Handle* handle) {
  return *static_cast<int*>(cache->value(handle));
}

Cache::Handle* put(Cache* cache, const std::string& key, int value,
                   size_t charge = 1) {
  return cache->insert(key, new int(value), charge, delete_int);
}

}  // namespace

TEST(cache, holds_what_was_inserted_and_not_what_was_not) {
  deletions = 0;
  const auto cache = new_lru_cache(1000);
  for (int i = 0; i < 100; ++i) {
    cache->release(put(cache.get(), "key" + std::to_string(i), i));
  }
  for (int i = 0; i < 100; ++i) {
    Cache::Handle* handle = cache->lookup("key" + std::to_string(i));
    CHECK(handle != nullptr);
    if (handle == nullptr) return;
    CHECK_EQ(value_of(cache.get(), handle), i);
    cache->release(handle);
  }
  CHECK(cache->lookup("key100") == nullptr);
  CHECK(cache->lookup("") == nullptr);
  CHECK_EQ(cache->total_charge(), size_t{100});
  CHECK_EQ(deletions, 0);
}

TEST(cache, inserting_a_key_again_replaces_its_value) {
  deletions = 0;
  const auto cache = new_lru_cache(1000);
  cache->release(put(cache.get(), "k", 1));
  cache->release(put(cache.get(), "k", 2));
  // The first value left the table when the second arrived, and had no
  // user, so it is gone.
  CHECK_EQ(deletions, 1);
  Cache::Handle* handle = cache->lookup("k");
  CHECK(handle != nullptr);
  if (handle == nullptr) return;
  CHECK_EQ(value_of(cache.get(), handle), 2);
  cache->release(handle);
  CHECK_EQ(cache->total_charge(), size_t{1});
}

TEST(cache, a_replaced_or_erased_entry_survives_until_its_user_lets_go) {
  deletions = 0;
  const auto cache = new_lru_cache(1000);
  Cache::Handle* held = put(cache.get(), "k", 1);
  cache->release(put(cache.get(), "k", 2));
  CHECK_EQ(deletions, 0);  // the reader of 1 still holds it
  CHECK_EQ(value_of(cache.get(), held), 1);
  cache->release(held);
  CHECK_EQ(deletions, 1);

  held = cache->lookup("k");
  CHECK(held != nullptr);
  if (held == nullptr) return;
  cache->erase("k");
  CHECK(cache->lookup("k") == nullptr);
  CHECK_EQ(deletions, 1);
  CHECK_EQ(value_of(cache.get(), held), 2);
  cache->release(held);
  CHECK_EQ(deletions, 2);
  CHECK_EQ(cache->total_charge(), size_t{0});
}

TEST(cache, evicts_the_least_recently_used_and_never_what_is_in_use) {
  deletions = 0;
  // Sixteen shards share the capacity, so one shard is bounded at a
  // sixteenth of it; keys are spread over the shards by hash, and the
  // property checked is the one that holds whichever shard a key lands
  // in: total charge never exceeds capacity once every user has let go.
  const auto cache = new_lru_cache(160);
  std::vector<Cache::Handle*> held;
  for (int i = 0; i < 10; ++i) {
    held.push_back(put(cache.get(), "held" + std::to_string(i), i, 10));
  }
  for (int i = 0; i < 1000; ++i) {
    cache->release(put(cache.get(), "k" + std::to_string(i), i, 10));
  }
  // Every held entry is still there, whatever was evicted around it.
  for (int i = 0; i < 10; ++i) {
    CHECK_EQ(value_of(cache.get(), held[static_cast<size_t>(i)]), i);
  }
  CHECK(deletions > 0);
  for (Cache::Handle* handle : held) cache->release(handle);
  // With nothing in use the cache is within its capacity.
  CHECK(cache->total_charge() <= size_t{160});
}

TEST(cache, what_was_used_most_recently_is_what_survives) {
  deletions = 0;
  // One shard's worth of keys would need the hash; instead the capacity
  // is large enough that every shard holds several entries, and the
  // check is on order rather than on any one key: of the first hundred
  // keys and the last hundred, the last are what remain.
  const auto cache = new_lru_cache(16 * 20);
  for (int i = 0; i < 1000; ++i) {
    cache->release(put(cache.get(), "k" + std::to_string(i), i, 1));
  }
  int early = 0;
  int late = 0;
  for (int i = 0; i < 100; ++i) {
    if (Cache::Handle* h = cache->lookup("k" + std::to_string(i))) {
      ++early;
      cache->release(h);
    }
    if (Cache::Handle* h = cache->lookup("k" + std::to_string(900 + i))) {
      ++late;
      cache->release(h);
    }
  }
  CHECK_EQ(early, 0);
  CHECK(late > 50);

  // Touching an old survivor keeps it: the most recently used among the
  // survivors outlives a hundred newcomers.
  std::string keeper;
  for (int i = 900; i < 1000; ++i) {
    if (Cache::Handle* h = cache->lookup("k" + std::to_string(i))) {
      keeper = "k" + std::to_string(i);
      cache->release(h);
      break;
    }
  }
  CHECK(!keeper.empty());
  for (int i = 1000; i < 1100; ++i) {
    // Between each newcomer, the keeper is touched again.
    if (Cache::Handle* h = cache->lookup(keeper)) cache->release(h);
    cache->release(put(cache.get(), "k" + std::to_string(i), i, 1));
  }
  Cache::Handle* h = cache->lookup(keeper);
  CHECK(h != nullptr);
  if (h != nullptr) cache->release(h);
}

TEST(cache, ids_are_distinct) {
  const auto cache = new_lru_cache(10);
  const uint64_t a = cache->new_id();
  const uint64_t b = cache->new_id();
  CHECK(a != b);
}

TEST(cache, frees_everything_it_still_holds_when_destroyed) {
  deletions = 0;
  {
    const auto cache = new_lru_cache(1000);
    for (int i = 0; i < 50; ++i) {
      cache->release(put(cache.get(), "k" + std::to_string(i), i));
    }
    CHECK_EQ(deletions, 0);
  }
  CHECK_EQ(deletions, 50);
}

// Two sixteen-byte keys with the same 64-bit hash, so that a bucket holds
// a chain of two and every operation has to tell them apart by their
// bytes: found by cycle-finding over hash64 on keys of the form x||x, and
// checked against the hash here before anything else, so that a change to
// the hash fails this line rather than quietly testing a chain of one.
TEST(cache, keys_that_share_a_hash_are_told_apart) {
  deletions = 0;
  static const unsigned char kA[16] = {0xfd, 0xb0, 0xdc, 0x84, 0x80, 0x65,
                                       0x5a, 0x9e, 0xfd, 0xb0, 0xdc, 0x84,
                                       0x80, 0x65, 0x5a, 0x9e};
  static const unsigned char kB[16] = {0x17, 0x12, 0x57, 0x4b, 0x2d, 0x37,
                                       0xc4, 0x07, 0x17, 0x12, 0x57, 0x4b,
                                       0x2d, 0x37, 0xc4, 0x07};
  const std::string a(reinterpret_cast<const char*>(kA), sizeof(kA));
  const std::string b(reinterpret_cast<const char*>(kB), sizeof(kB));
  CHECK(a != b);
  CHECK_EQ(hash64(a), hash64(b));
  CHECK_EQ(hash64(a), uint64_t{0x5ae3c4fbf4c1b020ull});

  const auto cache = new_lru_cache(1000);
  cache->release(put(cache.get(), a, 1));
  cache->release(put(cache.get(), b, 2));

  // Both present, each with its own value, whichever is at the head.
  Cache::Handle* ha = cache->lookup(a);
  Cache::Handle* hb = cache->lookup(b);
  CHECK(ha != nullptr);
  CHECK(hb != nullptr);
  if (ha == nullptr || hb == nullptr) return;
  CHECK_EQ(value_of(cache.get(), ha), 1);
  CHECK_EQ(value_of(cache.get(), hb), 2);
  cache->release(ha);
  cache->release(hb);

  // Replacing one leaves the other alone.
  cache->release(put(cache.get(), a, 3));
  CHECK_EQ(deletions, 1);
  ha = cache->lookup(a);
  hb = cache->lookup(b);
  CHECK(ha != nullptr && hb != nullptr);
  if (ha == nullptr || hb == nullptr) return;
  CHECK_EQ(value_of(cache.get(), ha), 3);
  CHECK_EQ(value_of(cache.get(), hb), 2);
  cache->release(ha);
  cache->release(hb);

  // Erasing one leaves the other findable, and erasing the other empties
  // the bucket without touching anything else.
  cache->erase(a);
  CHECK(cache->lookup(a) == nullptr);
  hb = cache->lookup(b);
  CHECK(hb != nullptr);
  if (hb != nullptr) cache->release(hb);
  cache->erase(b);
  CHECK(cache->lookup(b) == nullptr);
  CHECK_EQ(deletions, 3);
  CHECK_EQ(cache->total_charge(), size_t{0});
}
