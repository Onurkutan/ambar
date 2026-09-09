// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// A bounded, shared cache of decoded blocks.
//
// Without one, every read of a table block is a read syscall plus a parse, and
// the same block is re-read for every key in it.  A scan pays that per key
// rather than per block, which is why the first version of this engine scanned
// an order of magnitude slower than SQLite while beating it on the lookups the
// bloom filter answered.  The difference was not the tree; it was the cache
// that was not there.
//
// Two things make this more than a hash map.
//
// It is bounded and evicts, which means an entry can be dropped while a reader
// is still using it.  So entries are reference counted, and eviction removes an
// entry from the table without freeing it: the last user frees it.  Anything
// less would hand out a pointer and then delete what it points at.
//
// It is shared by every reader thread, so it needs a lock, and one lock over
// the whole cache would serialise every block read in the process.  It is
// therefore split into independent shards chosen by the key's hash, and a
// thread only contends with others that happened to hash to the same shard.

#ifndef AMBAR_CACHE_HPP_
#define AMBAR_CACHE_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace ambar {

class Cache {
 public:
  virtual ~Cache();

  // Opaque; hold it while using the value, then release it.
  struct Handle;

  // Inserts, taking ownership of `value`: the deleter is called when the entry
  // is evicted and its last user has released it.  The returned handle is
  // already referenced by the caller and must be released.
  //
  // `charge` is what this entry costs against the capacity -- for a block, its
  // size in bytes, so the cache is bounded by memory rather than by a count of
  // entries whose sizes differ by orders of magnitude.
  virtual Handle* insert(std::string_view key, void* value, size_t charge,
                         void (*deleter)(void* value)) = 0;

  // Null when absent.  A returned handle must be released.
  virtual Handle* lookup(std::string_view key) = 0;

  virtual void release(Handle* handle) = 0;
  virtual void* value(Handle* handle) = 0;

  // Removes the entry, if any.  It is freed once its users let go.
  virtual void erase(std::string_view key) = 0;

  // A number no other caller of this cache will use, for prefixing keys so
  // that two tables cannot collide on the same block offset.
  virtual uint64_t new_id() = 0;

  virtual size_t total_charge() const = 0;
};

std::unique_ptr<Cache> new_lru_cache(size_t capacity);

}  // namespace ambar

#endif  // AMBAR_CACHE_HPP_
