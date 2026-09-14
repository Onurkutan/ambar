// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Opens table files, and keeps them open.
//
// Opening a table means a syscall and a read of its index and filter blocks.
// A read that touches four levels would pay that four times per lookup if
// tables were opened on demand, so they are opened once and held.
//
// Holding them costs a file descriptor and the index in memory, and a process
// has a finite number of descriptors, so the cache is bounded and evicts the
// least recently used entry when it is full.  Eviction is where the ownership
// gets interesting: a table being evicted may still be in use by an iterator
// that a caller is holding, so entries are reference counted and an evicted
// entry is closed when the last user lets go, not when it is evicted.

#ifndef AMBAR_TABLE_CACHE_HPP_
#define AMBAR_TABLE_CACHE_HPP_

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ambar/iterator.hpp"
#include "ambar/options.hpp"
#include "ambar/status.hpp"
#include "comparator.hpp"
#include "file.hpp"
#include "table.hpp"

namespace ambar {

class TableCache {
 public:
  TableCache(std::string dbname, const Options& options,
             const Comparator* comparator, int entries);
  ~TableCache();

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // An iterator over the whole file.  If `table` is not null it is set to the
  // underlying table, which stays valid for as long as the iterator does.
  Iterator* new_iterator(const ReadOptions& options, uint64_t file_number,
                         uint64_t file_size, Table** table = nullptr);

  // Point lookup, delivering the result through a callback for the same reason
  // Table::internal_get does.
  Status get(const ReadOptions& options, uint64_t file_number,
             uint64_t file_size, std::string_view key, void* arg,
             void (*handle_result)(void*, std::string_view, std::string_view));

  // Called when a file is removed from every version, so its descriptor is not
  // held until eviction happens to reach it.
  void evict(uint64_t file_number);

  // Reads of table files since this cache was created -- every data block
  // the block cache did not answer, and the footer, index and, with a
  // filter configured, metaindex and filter read when a table is opened --
  // and the
  // bytes they brought in.  Every table file the engine reads is opened
  // here and read through a counting wrapper, so no read can bypass the
  // count; atomic because reads happen on any thread, under no lock.
  // Divided by the lookups that caused them, this is read amplification.
  uint64_t reads() const { return reads_.load(std::memory_order_relaxed); }
  uint64_t bytes_read() const {
    return bytes_read_.load(std::memory_order_relaxed);
  }

 private:
  struct Entry {
    std::unique_ptr<RandomAccessFile> file;
    std::unique_ptr<Table> table;
    int refs = 0;         // users plus one for being in the cache
    bool in_cache = true;
  };

  // Returns a referenced entry, or an error.  The caller must release().
  Status find(uint64_t file_number, uint64_t file_size, Entry** entry);
  void release(Entry* entry);
  void evict_if_over_capacity();

  const std::string dbname_;
  const Options options_;
  const Comparator* const comparator_;
  const size_t capacity_;

  // One lock over the whole cache.  A sharded cache would scale further, and
  // this one does not need to: the critical section is a hash lookup and a
  // list splice, and the work it guards -- opening a file -- happens outside
  // it.  Sharding is the kind of complexity that should follow a measurement,
  // and there is not one yet.
  std::mutex mutex_;
  std::list<uint64_t> lru_;  // front is most recently used

  std::atomic<uint64_t> reads_{0};
  std::atomic<uint64_t> bytes_read_{0};
  std::unordered_map<uint64_t, std::pair<std::unique_ptr<Entry>,
                                         std::list<uint64_t>::iterator>>
      entries_;
};

}  // namespace ambar

#endif  // AMBAR_TABLE_CACHE_HPP_
