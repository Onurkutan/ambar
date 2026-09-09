// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "table_cache.hpp"

#include <functional>
#include <utility>

#include "filename.hpp"

namespace ambar {

TableCache::TableCache(std::string dbname, const Options& options,
                       const Comparator* comparator, int entries)
    : dbname_(std::move(dbname)),
      options_(options),
      comparator_(comparator),
      capacity_(entries < 1 ? 1 : static_cast<size_t>(entries)) {}

TableCache::~TableCache() = default;

Status TableCache::find(uint64_t file_number, uint64_t file_size,
                        Entry** result) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(file_number);
    if (it != entries_.end()) {
      lru_.splice(lru_.begin(), lru_, it->second.second);
      ++it->second.first->refs;
      *result = it->second.first.get();
      return Status::ok();
    }
  }

  // Opened outside the lock: this is the slow part, and holding the lock
  // through it would serialise every reader behind one file open.  Two threads
  // racing to open the same file both succeed and one of the two Table objects
  // is dropped, which wastes an open and keeps the lock short.
  auto entry = std::make_unique<Entry>();
  std::unique_ptr<RandomAccessFile> file;
  Status status = RandomAccessFile::open(table_file_name(dbname_, file_number),
                                         &file);
  if (!status.is_ok()) return status;

  std::unique_ptr<Table> table;
  status = Table::open(options_, comparator_, file.get(), file_size, &table);
  if (!status.is_ok()) return status;

  entry->file = std::move(file);
  entry->table = std::move(table);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(file_number);
    if (it != entries_.end()) {
      // Someone else won the race; use theirs and drop ours.
      lru_.splice(lru_.begin(), lru_, it->second.second);
      ++it->second.first->refs;
      *result = it->second.first.get();
      return Status::ok();
    }

    entry->refs = 2;  // one for the cache, one for the caller
    Entry* raw = entry.get();
    lru_.push_front(file_number);
    entries_.emplace(file_number,
                     std::make_pair(std::move(entry), lru_.begin()));
    *result = raw;
    evict_if_over_capacity();
    return Status::ok();
  }
}

void TableCache::evict_if_over_capacity() {
  // Called with the lock held.
  while (entries_.size() > capacity_ && !lru_.empty()) {
    const uint64_t victim = lru_.back();
    const auto it = entries_.find(victim);
    if (it == entries_.end()) {
      lru_.pop_back();
      continue;
    }
    Entry* entry = it->second.first.get();
    entry->in_cache = false;
    lru_.pop_back();

    if (--entry->refs == 0) {
      entries_.erase(it);
    } else {
      // Still in use by an iterator.  The entry leaves the map so it can no
      // longer be found, and its owner is transferred to the last user, who
      // closes it in release().  Keeping it in the map instead would let a
      // later lookup hand out a table that is about to be deleted.
      it->second.first.release();
      entries_.erase(it);
    }
  }
}

void TableCache::release(Entry* entry) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (--entry->refs == 0) {
    delete entry;  // only reachable once it has left the map
  }
}

void TableCache::evict(uint64_t file_number) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = entries_.find(file_number);
  if (it == entries_.end()) return;

  Entry* entry = it->second.first.get();
  entry->in_cache = false;
  lru_.erase(it->second.second);
  if (--entry->refs == 0) {
    entries_.erase(it);
  } else {
    it->second.first.release();
    entries_.erase(it);
  }
}

namespace {

// Holds a cache reference for as long as an iterator over the table exists.
class CachedTableIterator final : public Iterator {
 public:
  CachedTableIterator(Iterator* inner, std::function<void()> release)
      : inner_(inner), release_(std::move(release)) {}
  ~CachedTableIterator() override {
    inner_.reset();
    release_();
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
  std::unique_ptr<Iterator> inner_;
  std::function<void()> release_;
};

}  // namespace

Iterator* TableCache::new_iterator(const ReadOptions& options,
                                   uint64_t file_number, uint64_t file_size,
                                   Table** table_out) {
  if (table_out != nullptr) *table_out = nullptr;

  Entry* entry = nullptr;
  const Status status = find(file_number, file_size, &entry);
  if (!status.is_ok()) return new_error_iterator(status);

  Iterator* inner = entry->table->new_iterator(options);
  if (table_out != nullptr) *table_out = entry->table.get();
  return new CachedTableIterator(inner, [this, entry] { release(entry); });
}

Status TableCache::get(const ReadOptions& options, uint64_t file_number,
                       uint64_t file_size, std::string_view key, void* arg,
                       void (*handle_result)(void*, std::string_view,
                                             std::string_view)) {
  Entry* entry = nullptr;
  Status status = find(file_number, file_size, &entry);
  if (!status.is_ok()) return status;

  status = entry->table->internal_get(options, key, arg, handle_result);
  release(entry);
  return status;
}

}  // namespace ambar
