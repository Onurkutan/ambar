// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Writes one table file.
//
// Keys arrive in order and leave in order; nothing is buffered beyond the
// block being filled, so a table of any size costs a block of memory to write.
// The index and filter accumulate, but both are small next to the data.

#ifndef AMBAR_TABLE_BUILDER_HPP_
#define AMBAR_TABLE_BUILDER_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ambar/options.hpp"
#include "ambar/status.hpp"
#include "block.hpp"
#include "comparator.hpp"
#include "block_builder.hpp"
#include "file.hpp"
#include "filter_block.hpp"
#include "format.hpp"

namespace ambar {

class TableBuilder {
 public:
  // `file` is borrowed, not owned: the caller flushes and syncs it, because
  // only the caller knows whether this table is a memtable flush that must be
  // durable before the log is dropped, or a compaction output that is not yet
  // referenced by anything.
  TableBuilder(const Options& options, WritableFile* file,
               const Comparator* comparator);
  ~TableBuilder();

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // Keys must be strictly increasing under the comparator.  Checked with an
  // assertion in debug builds; in release the result is a file whose binary
  // search returns wrong answers, silently, which is why the callers that feed
  // this are the ones with tests about ordering.
  void add(std::string_view key, std::string_view value);

  // Finishes the current data block early.  Used when a caller wants a block
  // boundary at a particular key.
  void flush();

  // Writes the filter, metaindex, index and footer.  After this the file holds
  // a complete table, but not necessarily a durable one -- see the note on the
  // constructor.
  Status finish();

  // Stops without finishing.  The file is left incomplete on purpose: a
  // partial table with no footer cannot be mistaken for a whole one, so a
  // crash mid-compaction leaves garbage that is recognisably garbage.
  void abandon();

  Status status() const { return status_; }
  uint64_t num_entries() const { return num_entries_; }
  uint64_t file_size() const { return offset_; }

 private:
  // Finishes `block` and writes it, compressed when `compressible` and the
  // options ask for it and it pays -- a block that would not shrink is
  // written as it is.  Data blocks are compressible; the index and
  // metaindex are not, being read once and held.
  void write_block(BlockBuilder* block, bool compressible, BlockHandle* handle);
  void write_raw_block(std::string_view contents, CompressionType type,
                       BlockHandle* handle);

  const Options options_;
  const Comparator* const comparator_;
  WritableFile* const file_;

  uint64_t offset_ = 0;
  Status status_;

  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::unique_ptr<FilterBlockBuilder> filter_block_;

  std::string last_key_;
  std::string compressed_;  // scratch for write_block, reused per block
  uint64_t num_entries_ = 0;
  bool closed_ = false;

  // The index entry for a finished data block is not written until the next
  // key arrives.  That is not laziness: the index key only has to separate one
  // block from the next, so knowing the first key of the following block lets
  // it be shortened, and a shorter index is a smaller index block held in
  // memory for every open table.
  bool pending_index_entry_ = false;
  BlockHandle pending_handle_;
};

}  // namespace ambar

#endif  // AMBAR_TABLE_BUILDER_HPP_
