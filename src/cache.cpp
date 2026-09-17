// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "ambar/cache.hpp"

#include <cassert>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "hash.hpp"

namespace ambar {

Cache::~Cache() = default;

namespace {

struct LRUEntry {
  void* value = nullptr;
  void (*deleter)(void*) = nullptr;
  std::string key;
  uint64_t hash = 0;  // of key, computed once, by the caller
  size_t charge = 0;

  // References held by users, plus one while the entry is in the table.  An
  // evicted entry drops the table's reference and is freed by whoever holds
  // the last user reference -- which may be a reader that started before the
  // eviction and is still walking the block.
  uint32_t refs = 0;
  bool in_cache = false;

  LRUEntry* next = nullptr;
  LRUEntry* prev = nullptr;
  LRUEntry* hash_next = nullptr;  // the next entry with the same hash
};

// The shard's index: hash to entry, with the rare two keys that share a
// hash chained behind one another and told apart by their bytes.
//
// This used to be an unordered_map keyed by std::string, which built a
// string from the key on every lookup -- an allocation per block read, two
// per point lookup, on the path that eight threads were measured
// contending on.  Keyed by the 64-bit hash the sharding already computes,
// a lookup allocates nothing; the key's bytes are compared only when a
// hash matches, which for the block cache's sixteen-byte keys is the
// entry itself nearly every time.
class HashTable {
 public:
  LRUEntry* find(std::string_view key, uint64_t hash) const {
    const auto it = buckets_.find(hash);
    if (it == buckets_.end()) return nullptr;
    for (LRUEntry* entry = it->second; entry != nullptr;
         entry = entry->hash_next) {
      if (entry->key == key) return entry;
    }
    return nullptr;
  }

  // Inserts, returning the entry it replaced -- the one with the same key
  // -- or null.  The caller owns what comes back.
  LRUEntry* insert(LRUEntry* entry) {
    LRUEntry** slot = &buckets_[entry->hash];
    for (LRUEntry** link = slot; *link != nullptr;
         link = &(*link)->hash_next) {
      if ((*link)->key == entry->key) {
        LRUEntry* old = *link;
        entry->hash_next = old->hash_next;
        *link = entry;
        old->hash_next = nullptr;
        return old;
      }
    }
    entry->hash_next = *slot;
    *slot = entry;
    return nullptr;
  }

  void remove(LRUEntry* entry) {
    const auto it = buckets_.find(entry->hash);
    assert(it != buckets_.end());
    for (LRUEntry** link = &it->second; *link != nullptr;
         link = &(*link)->hash_next) {
      if (*link == entry) {
        *link = entry->hash_next;
        entry->hash_next = nullptr;
        break;
      }
    }
    if (it->second == nullptr) buckets_.erase(it);
  }

  template <typename F>
  void for_each(F f) {
    for (auto& [hash, head] : buckets_) {
      (void)hash;
      for (LRUEntry* entry = head; entry != nullptr;) {
        LRUEntry* next = entry->hash_next;
        f(entry);
        entry = next;
      }
    }
  }

 private:
  std::unordered_map<uint64_t, LRUEntry*> buckets_;
};

// A doubly linked list with a sentinel, so insert and remove have no special
// cases at the ends.
class LRUList {
 public:
  LRUList() {
    head_.next = &head_;
    head_.prev = &head_;
  }

  bool empty() const { return head_.next == &head_; }
  LRUEntry* oldest() const { return head_.next; }
  const LRUEntry* sentinel() const { return &head_; }

  static void remove(LRUEntry* entry) {
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
  }

  // Appended at the tail, which is therefore the most recently used.
  void append(LRUEntry* entry) {
    entry->next = &head_;
    entry->prev = head_.prev;
    entry->prev->next = entry;
    entry->next->prev = entry;
  }

 private:
  LRUEntry head_;
};

class LRUShard {
 public:
  ~LRUShard() {
    // Everything still in the table is unreferenced by users at this point --
    // the cache outlives its readers by construction -- so the entries can be
    // freed directly.
    table_.for_each([this](LRUEntry* entry) {
      entry->in_cache = false;
      assert(entry->refs == 1);
      unref(entry);
    });
  }

  void set_capacity(size_t capacity) { capacity_ = capacity; }

  Cache::Handle* insert(std::string_view key, uint64_t hash, void* value,
                        size_t charge, void (*deleter)(void*)) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* entry = new LRUEntry();
    entry->value = value;
    entry->deleter = deleter;
    entry->key.assign(key.data(), key.size());
    entry->hash = hash;
    entry->charge = charge;
    entry->refs = 2;  // the caller's, plus the table's
    entry->in_cache = true;

    usage_ += charge;
    in_use_.append(entry);

    if (LRUEntry* old = table_.insert(entry); old != nullptr) {
      // Replacing: the old entry has left the table but is not freed while
      // someone is reading it.
      finish_erase(old, /*in_table=*/false);
    }

    evict_to_capacity();
    return reinterpret_cast<Cache::Handle*>(entry);
  }

  Cache::Handle* lookup(std::string_view key, uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    LRUEntry* entry = table_.find(key, hash);
    if (entry == nullptr) return nullptr;

    ++entry->refs;
    // Moved to the in-use list: an entry someone is reading must not be
    // evicted out from under them, and moving it also makes it the most
    // recently used when they let go.
    LRUList::remove(entry);
    in_use_.append(entry);
    return reinterpret_cast<Cache::Handle*>(entry);
  }

  void release(Cache::Handle* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* entry = reinterpret_cast<LRUEntry*>(handle);
    if (entry->refs == 2 && entry->in_cache) {
      // The last user has let go and the entry is still in the table: it
      // becomes evictable, at the young end of the list.
      LRUList::remove(entry);
      lru_.append(entry);
    }
    unref(entry);
    evict_to_capacity();
  }

  void erase(std::string_view key, uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (LRUEntry* entry = table_.find(key, hash); entry != nullptr) {
      finish_erase(entry, /*in_table=*/true);
    }
  }

  size_t usage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return usage_;
  }

 private:
  // Takes an entry out of the cache -- out of the table too, unless insert
  // has already unlinked it by replacing it -- and drops the table's
  // reference.  The entry survives until its users release it.
  void finish_erase(LRUEntry* entry, bool in_table) {
    assert(entry->in_cache);
    LRUList::remove(entry);
    entry->in_cache = false;
    usage_ -= entry->charge;
    if (in_table) table_.remove(entry);
    unref(entry);
  }

  void unref(LRUEntry* entry) {
    assert(entry->refs > 0);
    if (--entry->refs == 0) {
      assert(!entry->in_cache);
      entry->deleter(entry->value);
      delete entry;
    }
  }

  void evict_to_capacity() {
    // Only the LRU list is eligible: everything in the in-use list has a
    // reader.  If every entry is in use the cache overshoots its capacity,
    // which is the right failure -- the alternative is freeing memory someone
    // is reading.
    while (usage_ > capacity_ && !lru_.empty()) {
      finish_erase(lru_.oldest(), /*in_table=*/true);
    }
  }

  mutable std::mutex mutex_;
  size_t capacity_ = 0;
  size_t usage_ = 0;

  LRUList lru_;     // in the table, no users: evictable
  LRUList in_use_;  // in the table, with users: not evictable
  HashTable table_;
};

class ShardedLRUCache final : public Cache {
 public:
  explicit ShardedLRUCache(size_t capacity) {
    // Rounded up, so the total is at least what was asked for rather than
    // silently a little less.
    const size_t per_shard = (capacity + kShards - 1) / kShards;
    for (auto& shard : shards_) shard.set_capacity(per_shard);
  }

  // The hash is computed once per call, here, and does double duty: it
  // picks the shard and it keys the shard's table.
  Handle* insert(std::string_view key, void* value, size_t charge,
                 void (*deleter)(void*)) override {
    const uint64_t hash = hash64(key);
    return shard_for(hash).insert(key, hash, value, charge, deleter);
  }

  Handle* lookup(std::string_view key) override {
    const uint64_t hash = hash64(key);
    return shard_for(hash).lookup(key, hash);
  }

  void release(Handle* handle) override {
    // The shard is found from the entry's own hash rather than remembered
    // in the handle: the handle is opaque to callers and this keeps it so.
    auto* entry = reinterpret_cast<LRUEntry*>(handle);
    shard_for(entry->hash).release(handle);
  }

  void* value(Handle* handle) override {
    return reinterpret_cast<LRUEntry*>(handle)->value;
  }

  void erase(std::string_view key) override {
    const uint64_t hash = hash64(key);
    shard_for(hash).erase(key, hash);
  }

  uint64_t new_id() override {
    std::lock_guard<std::mutex> lock(id_mutex_);
    return ++last_id_;
  }

  size_t total_charge() const override {
    size_t total = 0;
    for (const auto& shard : shards_) total += shard.usage();
    return total;
  }

 private:
  static constexpr int kShardBits = 4;
  static constexpr size_t kShards = size_t{1} << kShardBits;

  // The shard comes from the top of the hash and the table's bucket from
  // the rest, so that a shard, which holds only keys that agree in the
  // bits that chose it, does not hand its map keys that agree in the bits
  // the map uses: a standard library whose unordered_map masks the low
  // bits into a power-of-two bucket count would otherwise use one bucket
  // in sixteen.
  LRUShard& shard_for(uint64_t hash) {
    return shards_[hash >> (64 - kShardBits)];
  }

  LRUShard shards_[kShards];
  std::mutex id_mutex_;
  uint64_t last_id_ = 0;
};

}  // namespace

std::unique_ptr<Cache> new_lru_cache(size_t capacity) {
  return std::make_unique<ShardedLRUCache>(capacity);
}

}  // namespace ambar
