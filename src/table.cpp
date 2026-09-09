// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "table.hpp"

#include "encoding.hpp"
#include "two_level_iterator.hpp"

namespace ambar {

struct Table::Rep {
  Options options;
  const Comparator* comparator = nullptr;
  RandomAccessFile* file = nullptr;

  // Kept so every block handle can be bounded by the file it came out of.
  uint64_t file_size = 0;

  Status status;

  // Prefixed to every cache key so two tables cannot collide on the same block
  // offset.  Taken from the cache, so it is unique across everything sharing
  // it rather than merely across this table.
  uint64_t cache_id = 0;

  BlockHandle metaindex_handle;
  std::unique_ptr<Block> index_block;

  // The filter block's bytes are owned here and the reader points into them,
  // so both have to live exactly as long as the table.
  std::unique_ptr<const char[]> filter_data;
  std::unique_ptr<FilterBlockReader> filter;
};

Table::Table(Rep* rep) : rep_(rep) {}

Table::~Table() = default;

Status Table::open(const Options& options, const Comparator* comparator,
                   RandomAccessFile* file, uint64_t size,
                   std::unique_ptr<Table>* table) {
  table->reset();

  if (size < Footer::kEncodedLength) {
    return Status::corruption("file is too short to be a table");
  }

  char footer_space[Footer::kEncodedLength];
  std::string_view footer_input;
  Status status = file->read(size - Footer::kEncodedLength,
                             Footer::kEncodedLength, &footer_input,
                             footer_space);
  if (!status.is_ok()) return status;

  Footer footer;
  status = footer.decode_from(&footer_input);
  if (!status.is_ok()) return status;

  BlockContents index_contents;
  status = read_block(file, size, footer.index_handle(), &index_contents);
  if (!status.is_ok()) return status;

  auto rep = std::make_unique<Rep>();
  rep->options = options;
  rep->comparator = comparator;
  rep->file = file;
  rep->file_size = size;
  rep->metaindex_handle = footer.metaindex_handle();
  rep->index_block = std::make_unique<Block>(index_contents);
  rep->cache_id =
      options.block_cache != nullptr ? options.block_cache->new_id() : 0;

  Rep* raw = rep.get();
  table->reset(new Table(rep.release()));

  // The filter is optional in both directions: a table written without one is
  // read without one, and a table written with a filter this build does not
  // know is read as if it had none.  Neither is an error -- a missing filter
  // costs reads and nothing else -- so a failure here does not fail the open.
  if (options.filter_policy != nullptr) {
    BlockContents meta_contents;
    if (read_block(file, size, raw->metaindex_handle, &meta_contents).is_ok()) {
      Block meta(meta_contents);
      std::unique_ptr<Iterator> iter(meta.new_iterator(bytewise_comparator()));

      std::string key = "filter.";
      key.append(options.filter_policy->name());
      iter->seek(key);
      if (iter->valid() && iter->key() == std::string_view(key)) {
        std::string_view handle_value = iter->value();
        BlockHandle filter_handle;
        if (filter_handle.decode_from(&handle_value).is_ok()) {
          BlockContents filter_contents;
          if (read_block(file, size, filter_handle, &filter_contents).is_ok()) {
            if (filter_contents.heap_allocated) {
              raw->filter_data.reset(filter_contents.data.data());
            }
            raw->filter = std::make_unique<FilterBlockReader>(
                options.filter_policy, filter_contents.data);
          }
        }
      }
    }
  }
  return Status::ok();
}

namespace {

// Owns a block outright: the uncached path, where the iterator is the only
// thing that will ever look at these bytes.
class OwningBlockIterator final : public Iterator {
 public:
  OwningBlockIterator(Block* block, Iterator* inner)
      : block_(block), inner_(inner) {}

  bool valid() const override { return inner_->valid(); }
  void seek_to_first() override { inner_->seek_to_first(); }
  void seek_to_last() override { inner_->seek_to_last(); }
  void seek(std::string_view t) override { inner_->seek(t); }
  void next() override { inner_->next(); }
  void prev() override { inner_->prev(); }
  std::string_view key() const override { return inner_->key(); }
  std::string_view value() const override { return inner_->value(); }
  Status status() const override { return inner_->status(); }

 private:
  std::unique_ptr<Block> block_;
  std::unique_ptr<Iterator> inner_;
};

// Holds a cache reference instead: the block belongs to the cache, and letting
// go is what allows it to be evicted.
class CachedBlockIterator final : public Iterator {
 public:
  CachedBlockIterator(Cache* cache, Cache::Handle* handle, Iterator* inner)
      : cache_(cache), handle_(handle), inner_(inner) {}
  ~CachedBlockIterator() override {
    inner_.reset();
    cache_->release(handle_);
  }

  bool valid() const override { return inner_->valid(); }
  void seek_to_first() override { inner_->seek_to_first(); }
  void seek_to_last() override { inner_->seek_to_last(); }
  void seek(std::string_view t) override { inner_->seek(t); }
  void next() override { inner_->next(); }
  void prev() override { inner_->prev(); }
  std::string_view key() const override { return inner_->key(); }
  std::string_view value() const override { return inner_->value(); }
  Status status() const override { return inner_->status(); }

 private:
  Cache* cache_;
  Cache::Handle* handle_;
  std::unique_ptr<Iterator> inner_;
};

void delete_cached_block(void* value) {
  delete reinterpret_cast<Block*>(value);
}

}  // namespace

Iterator* Table::block_reader(void* arg, const ReadOptions& options,
                              std::string_view index_value) {
  auto* table = reinterpret_cast<Table*>(arg);
  Rep* rep = table->rep_.get();

  BlockHandle handle;
  std::string_view input = index_value;
  Status status = handle.decode_from(&input);
  if (!status.is_ok()) {
    return new_error_iterator(status);
  }

  Cache* cache = rep->options.block_cache;

  if (cache != nullptr) {
    // Keyed by (cache id, offset).  The offset alone would collide between
    // tables; the file name would work and would cost a string compare on
    // every block read.
    char cache_key[16];
    encode_fixed64(cache_key, rep->cache_id);
    encode_fixed64(cache_key + 8, handle.offset());
    const std::string_view key(cache_key, sizeof(cache_key));

    if (Cache::Handle* found = cache->lookup(key); found != nullptr) {
      auto* block = reinterpret_cast<Block*>(cache->value(found));
      return new CachedBlockIterator(cache, found,
                                     block->new_iterator(rep->comparator));
    }

    BlockContents contents;
    status = read_block(rep->file, rep->file_size, handle, &contents);
    if (!status.is_ok()) return new_error_iterator(status);

    auto* block = new Block(contents);
    if (contents.cachable && options.fill_cache) {
      // The charge is the block's own size, floored at the cost of the entry
      // itself.  Block::size() returns zero for a block whose header did not
      // decode -- that is how the constructor signals the failure -- and a
      // zero charge never counts against the capacity, so a file full of
      // undecodable blocks would fill the cache with entries eviction can
      // never reclaim.
      const size_t charge =
          block->size() > 0 ? block->size() : sizeof(Block) + key.size();
      Cache::Handle* inserted =
          cache->insert(key, block, charge, &delete_cached_block);
      return new CachedBlockIterator(cache, inserted,
                                     block->new_iterator(rep->comparator));
    }
    // Not cachable, or a scan that asked not to pollute the cache: the
    // iterator owns the block and it dies with the read.
    return new OwningBlockIterator(block, block->new_iterator(rep->comparator));
  }

  BlockContents contents;
  status = read_block(rep->file, rep->file_size, handle, &contents);
  if (!status.is_ok()) return new_error_iterator(status);

  auto* block = new Block(contents);
  (void)options;
  return new OwningBlockIterator(block, block->new_iterator(rep->comparator));
}

Iterator* Table::new_iterator(const ReadOptions& options) const {
  return new_two_level_iterator(
      rep_->index_block->new_iterator(rep_->comparator), &Table::block_reader,
      const_cast<Table*>(this), options);
}

Status Table::internal_get(const ReadOptions& options, std::string_view key,
                           void* arg,
                           void (*handle_result)(void*, std::string_view,
                                                 std::string_view)) const {
  std::unique_ptr<Iterator> index_iter(
      rep_->index_block->new_iterator(rep_->comparator));
  index_iter->seek(key);
  if (!index_iter->valid()) return index_iter->status();

  BlockHandle handle;
  std::string_view handle_value = index_iter->value();

  // The filter is consulted before the data block is read, which is the whole
  // point of having one: a key that is not in this table costs an in-memory
  // test instead of a disk read.
  if (rep_->filter != nullptr && handle.decode_from(&handle_value).is_ok() &&
      !rep_->filter->key_may_match(handle.offset(), key)) {
    return Status::ok();  // definitely not here
  }

  std::unique_ptr<Iterator> block_iter(
      block_reader(const_cast<Table*>(this), options, index_iter->value()));
  block_iter->seek(key);
  if (block_iter->valid()) {
    handle_result(arg, block_iter->key(), block_iter->value());
  }
  Status status = block_iter->status();
  if (status.is_ok()) status = index_iter->status();
  return status;
}

uint64_t Table::approximate_offset_of(std::string_view key) const {
  std::unique_ptr<Iterator> index_iter(
      rep_->index_block->new_iterator(rep_->comparator));
  index_iter->seek(key);
  if (!index_iter->valid()) {
    // Past the last key: the metaindex block is where the data ends, so its
    // offset is the closest honest answer.
    return rep_->metaindex_handle.offset();
  }
  BlockHandle handle;
  std::string_view input = index_iter->value();
  if (!handle.decode_from(&input).is_ok()) {
    return rep_->metaindex_handle.offset();
  }
  return handle.offset();
}

}  // namespace ambar
