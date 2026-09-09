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
  size_t charge = 0;

  // References held by users, plus one while the entry is in the table.  An
  // evicted entry drops the table's reference and is freed by whoever holds
  // the last user reference -- which may be a reader that started before the
  // eviction and is still walking the block.
  uint32_t refs = 0;
  bool in_cache = false;

  LRUEntry* next = nullptr;
  LRUEntry* prev = nullptr;
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
    for (auto& [key, entry] : table_) {
      (void)key;
      entry->in_cache = false;
      assert(entry->refs == 1);
      unref(entry);
    }
  }

  void set_capacity(size_t capacity) { capacity_ = capacity; }

  Cache::Handle* insert(std::string_view key, void* value, size_t charge,
                        void (*deleter)(void*)) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* entry = new LRUEntry();
    entry->value = value;
    entry->deleter = deleter;
    entry->key.assign(key.data(), key.size());
    entry->charge = charge;
    entry->refs = 2;  // the caller's, plus the table's
    entry->in_cache = true;

    usage_ += charge;
    in_use_.append(entry);

    const auto existing = table_.find(entry->key);
    if (existing != table_.end()) {
      // Replacing: the old entry leaves the table but is not freed while
      // someone is reading it.
      finish_erase(existing->second);
    }
    table_[entry->key] = entry;

    evict_to_capacity();
    return reinterpret_cast<Cache::Handle*>(entry);
  }

  Cache::Handle* lookup(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = table_.find(std::string(key));
    if (it == table_.end()) return nullptr;

    LRUEntry* entry = it->second;
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

  void erase(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = table_.find(std::string(key));
    if (it != table_.end()) finish_erase(it->second);
  }

  size_t usage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return usage_;
  }

 private:
  // Removes an entry from the table and drops the table's reference.  The
  // entry survives until its users release it.
  void finish_erase(LRUEntry* entry) {
    assert(entry->in_cache);
    LRUList::remove(entry);
    entry->in_cache = false;
    usage_ -= entry->charge;
    table_.erase(entry->key);
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
      finish_erase(lru_.oldest());
    }
  }

  mutable std::mutex mutex_;
  size_t capacity_ = 0;
  size_t usage_ = 0;

  LRUList lru_;     // in the table, no users: evictable
  LRUList in_use_;  // in the table, with users: not evictable
  std::unordered_map<std::string, LRUEntry*> table_;
};

class ShardedLRUCache final : public Cache {
 public:
  explicit ShardedLRUCache(size_t capacity) {
    // Rounded up, so the total is at least what was asked for rather than
    // silently a little less.
    const size_t per_shard = (capacity + kShards - 1) / kShards;
    for (auto& shard : shards_) shard.set_capacity(per_shard);
  }

  Handle* insert(std::string_view key, void* value, size_t charge,
                 void (*deleter)(void*)) override {
    return shard_for(key).insert(key, value, charge, deleter);
  }

  Handle* lookup(std::string_view key) override {
    return shard_for(key).lookup(key);
  }

  void release(Handle* handle) override {
    // The shard is found from the entry's own key rather than remembered in
    // the handle: the handle is opaque to callers and this keeps it that way.
    auto* entry = reinterpret_cast<LRUEntry*>(handle);
    shard_for(entry->key).release(handle);
  }

  void* value(Handle* handle) override {
    return reinterpret_cast<LRUEntry*>(handle)->value;
  }

  void erase(std::string_view key) override { shard_for(key).erase(key); }

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
  static constexpr size_t kShards = 16;

  LRUShard& shard_for(std::string_view key) {
    return shards_[bloom_hash(key) % kShards];
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
