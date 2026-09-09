// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "two_level_iterator.hpp"

#include <memory>
#include <string>

namespace ambar {
namespace {

class TwoLevelIterator final : public Iterator {
 public:
  TwoLevelIterator(Iterator* index_iter, BlockFunction function, void* arg,
                   const ReadOptions& options)
      : function_(function),
        arg_(arg),
        options_(options),
        index_iter_(index_iter) {}

  ~TwoLevelIterator() override = default;

  bool valid() const override {
    return data_iter_ != nullptr && data_iter_->valid();
  }

  std::string_view key() const override { return data_iter_->key(); }
  std::string_view value() const override { return data_iter_->value(); }

  void seek(std::string_view target) override {
    index_iter_->seek(target);
    init_data_block();
    if (data_iter_ != nullptr) data_iter_->seek(target);
    skip_empty_blocks_forward();
  }

  void seek_to_first() override {
    index_iter_->seek_to_first();
    init_data_block();
    if (data_iter_ != nullptr) data_iter_->seek_to_first();
    skip_empty_blocks_forward();
  }

  void seek_to_last() override {
    index_iter_->seek_to_last();
    init_data_block();
    if (data_iter_ != nullptr) data_iter_->seek_to_last();
    skip_empty_blocks_backward();
  }

  void next() override {
    data_iter_->next();
    skip_empty_blocks_forward();
  }

  void prev() override {
    data_iter_->prev();
    skip_empty_blocks_backward();
  }

  Status status() const override {
    // The first real error wins, and an error in either level counts.  A
    // caller that only checks the outer status would otherwise miss a block
    // that failed to decode, because a failed block looks exactly like the end
    // of the data.
    if (!index_iter_->status().is_ok()) return index_iter_->status();
    if (data_iter_ != nullptr && !data_iter_->status().is_ok()) {
      return data_iter_->status();
    }
    return status_;
  }

 private:
  void save_error(const Status& status) {
    if (status_.is_ok() && !status.is_ok()) status_ = status;
  }

  void skip_empty_blocks_forward() {
    while (data_iter_ == nullptr || !data_iter_->valid()) {
      if (data_iter_ != nullptr) save_error(data_iter_->status());
      if (!index_iter_->valid()) {
        set_data_iterator(nullptr);
        return;
      }
      index_iter_->next();
      init_data_block();
      if (data_iter_ != nullptr) data_iter_->seek_to_first();
    }
  }

  void skip_empty_blocks_backward() {
    while (data_iter_ == nullptr || !data_iter_->valid()) {
      if (data_iter_ != nullptr) save_error(data_iter_->status());
      if (!index_iter_->valid()) {
        set_data_iterator(nullptr);
        return;
      }
      index_iter_->prev();
      init_data_block();
      if (data_iter_ != nullptr) data_iter_->seek_to_last();
    }
  }

  void set_data_iterator(Iterator* iter) {
    if (data_iter_ != nullptr) save_error(data_iter_->status());
    data_iter_.reset(iter);
  }

  void init_data_block() {
    if (!index_iter_->valid()) {
      set_data_iterator(nullptr);
      return;
    }
    const std::string_view handle = index_iter_->value();
    // Reopening the block that is already open would re-read it from disk on
    // every seek within one block, which a scan does constantly.
    if (data_iter_ != nullptr && handle == data_block_handle_) return;

    data_block_handle_.assign(handle.data(), handle.size());
    set_data_iterator(function_(arg_, options_, handle));
  }

  const BlockFunction function_;
  void* const arg_;
  const ReadOptions options_;
  Status status_;

  std::unique_ptr<Iterator> index_iter_;
  std::unique_ptr<Iterator> data_iter_;

  // The index value the open data block came from, so a repeated seek into the
  // same block does not reopen it.
  std::string data_block_handle_;
};

}  // namespace

Iterator* new_two_level_iterator(Iterator* index_iter, BlockFunction function,
                                 void* arg, const ReadOptions& options) {
  return new TwoLevelIterator(index_iter, function, arg, options);
}

}  // namespace ambar
