// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The in-memory half of the tree: everything written but not yet on disk as a
// table.
//
// Entries live in an arena, encoded once and never moved:
//
//     entry := varint32(internal_key_len) | internal_key | varint32(value_len) | value
//
// The skip list's "key" is a pointer to the start of that record, so the list
// stores one word per entry and the comparator decodes just enough to compare.
// Nothing is copied between the arena and the list, and freeing the memtable is
// freeing the arena.
//
// Lifetime is reference-counted because a memtable outlives the moment it stops
// being written to: once full it becomes immutable and a background thread
// writes it out, while readers are still answering queries from it.
#pragma once

#include <atomic>
#include <cassert>
#include <string>
#include <string_view>

#include "ambar/status.hpp"
#include "ambar/iterator.hpp"
#include "arena.hpp"
#include "dbformat.hpp"
#include "skiplist.hpp"

namespace ambar {

class MemTable {
 private:
  // Compares two arena records by the internal keys they begin with.  Declared
  // first because the skip list type below is built from it.
  struct KeyComparator {
    int operator()(const char* a, const char* b) const;
  };
  using Table = SkipList<const char*, KeyComparator>;

 public:
  MemTable();

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  void ref() { refs_.fetch_add(1, std::memory_order_relaxed); }
  void unref() {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete this;
    }
  }

  // What the flush threshold is compared against: arena bytes, not entry count,
  // because a memtable full of large values must flush sooner than one full of
  // small ones.
  size_t approximate_memory_usage() const { return arena_.memory_usage(); }

  // Adds one version of one key.  `sequence` must be unique and increasing --
  // the skip list forbids duplicates and the ordering depends on it.
  void add(SequenceNumber sequence, ValueType type, std::string_view key,
           std::string_view value);

  // Looks up `key` as of `snapshot`.
  //
  // Returns true when this memtable answers the question, which includes
  // answering it with "deleted": a tombstone is a definitive negative and must
  // stop the search rather than let an older table supply a stale value.  In
  // that case *status is set to not-found.
  bool get(std::string_view key, SequenceNumber snapshot, std::string* value,
           Status* status) const;

  // Ordered traversal over internal keys, used when flushing to a table.
  class Iterator {
   public:
    explicit Iterator(const MemTable* table) : iter_(&table->table_) {}

    bool valid() const { return iter_.valid(); }
    void seek_to_first() { iter_.seek_to_first(); }
    void seek_to_last() { iter_.seek_to_last(); }
    void seek(std::string_view internal_key);
    void next() { iter_.next(); }
    void prev() { iter_.prev(); }

    std::string_view key() const;    // the internal key
    std::string_view value() const;

   private:
    Table::Iterator iter_;
    std::string seek_buffer_;
  };

  // The same traversal, behind the general Iterator interface, so a memtable
  // can be merged with table files without the merge knowing what it is.
  //
  // A separate type rather than making the nested one derive from Iterator:
  // flushing a memtable walks it several million times and has no use for
  // virtual dispatch, so the cheap version stays available and the wrapper is
  // paid for only where polymorphism is actually needed.  The caller owns the
  // result, and must keep a reference to the memtable for as long as it lives.
  ambar::Iterator* new_iterator() const;

 private:
  ~MemTable() = default;  // only unref() may destroy a memtable

  Arena arena_;
  Table table_;
  std::atomic<int> refs_{0};
};

}  // namespace ambar
