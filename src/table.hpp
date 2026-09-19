// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// A finished table file, opened for reading.
//
// Opening reads the footer, then the metaindex block it names, then the filter
// block the metaindex names, then the index block -- four reads, of which the
// index and the filter are kept in memory for the life of the Table because
// every lookup consults them.  The metaindex is read once and discarded.  Data
// blocks are read on demand.
//
// A Table is immutable once opened and its reads are const, so one instance is
// shared by every thread that touches the file rather than opened per reader.

#ifndef AMBAR_TABLE_HPP_
#define AMBAR_TABLE_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ambar/cache.hpp"
#include "ambar/iterator.hpp"
#include "ambar/options.hpp"
#include "ambar/status.hpp"
#include "block.hpp"
#include "comparator.hpp"
#include "file.hpp"
#include "filter_block.hpp"
#include "format.hpp"

namespace ambar {

class Table {
 public:
  // `file` must stay alive for as long as the table, and must be the file the
  // table was written to: `file_size` is used to find the footer, so a
  // mismatched size is reported as "not a table" rather than followed.
  static Status open(const Options& options, const Comparator* comparator,
                     RandomAccessFile* file, uint64_t file_size,
                     std::unique_ptr<Table>* table);

  ~Table();

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // Over every key/value pair in the file, in order.  The caller owns it.
  Iterator* new_iterator(const ReadOptions& options) const;

  // Point lookup.
  //
  // The result is delivered to a callback rather than copied into an output
  // parameter, so that a caller who only needs to inspect the value -- a
  // compaction deciding whether to drop it, say -- pays nothing for a copy.
  // `handle_result` is called at most once, with views valid only during the
  // call.
  Status internal_get(const ReadOptions& options, std::string_view key,
                      void* arg,
                      void (*handle_result)(void* arg, std::string_view key,
                                            std::string_view value)) const;

  // Roughly where in the file `key` lives.  Used by compaction to estimate how
  // much work a key range represents; accurate to a block.
  uint64_t approximate_offset_of(std::string_view key) const;

 private:
  struct Rep;

  // Defined out of line: Rep is incomplete here, and a unique_ptr member needs
  // a complete type wherever it might be destroyed.
  explicit Table(Rep* rep);

  static Iterator* block_reader(void* arg, const ReadOptions& options,
                                std::string_view index_value);

  // A data block, from the cache or the file, and how to let it go: a
  // cache handle to release, or a block of this reader's own to delete.
  struct BlockRef {
    Block* block = nullptr;
    Cache* cache = nullptr;
    Cache::Handle* handle = nullptr;
    bool owned = false;
    BlockRef() = default;
    BlockRef(const BlockRef&) = delete;
    BlockRef& operator=(const BlockRef&) = delete;
    ~BlockRef() { release(); }
    void release();
  };
  Status block_for(const ReadOptions& options, std::string_view index_value,
                   BlockRef* ref) const;

  std::unique_ptr<Rep> rep_;
};

}  // namespace ambar

#endif  // AMBAR_TABLE_HPP_
