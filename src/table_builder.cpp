// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "table_builder.hpp"

#include <cassert>

#include "encoding.hpp"

namespace ambar {

TableBuilder::TableBuilder(const Options& options, WritableFile* file,
                           const Comparator* comparator)
    : options_(options),
      comparator_(comparator),
      file_(file),
      data_block_(comparator, options.block_restart_interval),
      index_block_(comparator, 1) {
  // The index block gets a restart interval of one: every entry stands alone.
  // Prefix compression would save little there -- index keys are already
  // shortened separators, so consecutive ones share almost nothing -- and it
  // would make the binary search that every lookup performs walk entries
  // instead of jumping to them.
  if (options_.filter_policy != nullptr) {
    filter_block_ = std::make_unique<FilterBlockBuilder>(options_.filter_policy);
    filter_block_->start_block(0);
  }
}

TableBuilder::~TableBuilder() {
  // A builder destroyed without finish() or abandon() leaves a partial file.
  // That is not cleaned up here: the file is not this object's to delete, and
  // a partial table is already harmless, because it has no footer and so
  // cannot be opened as a table.  The assertion catches the caller mistake --
  // forgetting to close -- rather than trying to repair its consequences.
  assert(closed_);
}

void TableBuilder::add(std::string_view key, std::string_view value) {
  if (!status_.is_ok()) return;
  assert(!closed_);
  assert(num_entries_ == 0 ||
         comparator_->compare(std::string_view(last_key_), key) < 0);

  // The index entry for the previous data block is written now, because only
  // now is the first key of the following block known -- and that is what lets
  // the separator be shortened.
  if (pending_index_entry_) {
    assert(data_block_.empty());
    comparator_->find_shortest_separator(&last_key_, key);
    std::string handle_encoding;
    pending_handle_.encode_to(&handle_encoding);
    index_block_.add(last_key_, handle_encoding);
    pending_index_entry_ = false;
  }

  if (filter_block_ != nullptr) {
    filter_block_->add_key(key);
  }

  last_key_.assign(key.data(), key.size());
  ++num_entries_;
  data_block_.add(key, value);

  if (data_block_.current_size_estimate() >= options_.block_size) {
    flush();
  }
}

void TableBuilder::flush() {
  if (!status_.is_ok()) return;
  assert(!closed_);
  if (data_block_.empty()) return;

  assert(!pending_index_entry_);
  write_block(&data_block_, &pending_handle_);
  if (status_.is_ok()) {
    pending_index_entry_ = true;
    // Nothing is fsynced here.  A table under construction is not referenced
    // by anything yet, so the cost of durability would buy nothing: if the
    // process dies now, the file is garbage that the next open will not have
    // been told about.
    status_ = file_->flush();
  }
  if (filter_block_ != nullptr) {
    filter_block_->start_block(offset_);
  }
}

void TableBuilder::write_block(BlockBuilder* block, BlockHandle* handle) {
  const std::string_view raw = block->finish();
  // Compression is a reserved field, not an implemented one; see format.hpp.
  write_raw_block(raw, CompressionType::kNone, handle);
  block->reset();
}

void TableBuilder::write_raw_block(std::string_view contents,
                                   CompressionType type, BlockHandle* handle) {
  handle->set_offset(offset_);
  handle->set_size(contents.size());

  status_ = file_->append(contents);
  if (!status_.is_ok()) return;

  char trailer[kBlockTrailerSize];
  trailer[0] = static_cast<char>(type);

  // The checksum covers the contents and the type byte together, so that a
  // flipped bit in the type cannot pass as a valid block of another kind.
  uint32_t crc = crc32c(contents);
  crc = crc32c_extend(crc, std::string_view(trailer, 1));
  encode_fixed32(trailer + 1, crc);

  status_ = file_->append(std::string_view(trailer, kBlockTrailerSize));
  if (status_.is_ok()) {
    offset_ += contents.size() + kBlockTrailerSize;
  }
}

Status TableBuilder::finish() {
  flush();
  assert(!closed_);
  closed_ = true;

  BlockHandle filter_handle;
  BlockHandle metaindex_handle;
  BlockHandle index_handle;

  // The filter block is not checksummed differently from any other block; it
  // goes through the same trailer, which is what makes the "read it only after
  // the CRC passes" rule in tests/test_bloom.cpp enforceable.
  if (status_.is_ok() && filter_block_ != nullptr) {
    write_raw_block(filter_block_->finish(), CompressionType::kNone,
                    &filter_handle);
  }

  // Metaindex: a name -> handle map, so a later version can add block kinds
  // without moving anything the footer points at.
  if (status_.is_ok()) {
    // Bytewise: the metaindex keys are names, not internal keys.
    BlockBuilder metaindex_block(bytewise_comparator(),
                                 options_.block_restart_interval);
    if (filter_block_ != nullptr) {
      std::string key = "filter.";
      key.append(options_.filter_policy->name());
      std::string handle_encoding;
      filter_handle.encode_to(&handle_encoding);
      metaindex_block.add(key, handle_encoding);
    }
    write_block(&metaindex_block, &metaindex_handle);
  }

  if (status_.is_ok()) {
    if (pending_index_entry_) {
      // The last block has no successor to be separated from, so the key only
      // has to sort at or after everything in it.
      comparator_->find_short_successor(&last_key_);
      std::string handle_encoding;
      pending_handle_.encode_to(&handle_encoding);
      index_block_.add(last_key_, handle_encoding);
      pending_index_entry_ = false;
    }
    write_block(&index_block_, &index_handle);
  }

  if (status_.is_ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_handle);
    footer.set_index_handle(index_handle);

    std::string encoding;
    footer.encode_to(&encoding);
    status_ = file_->append(encoding);
    if (status_.is_ok()) {
      offset_ += encoding.size();
    }
  }
  return status_;
}

void TableBuilder::abandon() {
  assert(!closed_);
  closed_ = true;
}

}  // namespace ambar
