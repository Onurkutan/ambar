// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "table_cache.hpp"

#include <atomic>
#include <functional>
#include <utility>

#include "filename.hpp"

namespace ambar {

namespace {

// Counts what goes through it.  One more virtual call per block read,
// beside the syscall it counts; a read that failed brought nothing in and
// is not counted, which is also how the simulated disk in tests/ counts.
class CountingFile final : public RandomAccessFile {
 public:
  CountingFile(std::unique_ptr<RandomAccessFile> inner,
               std::atomic<uint64_t>* reads, std::atomic<uint64_t>* bytes)
      : inner_(std::move(inner)), reads_(reads), bytes_(bytes) {}

  Status read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override {
    const Status status = inner_->read(offset, n, result, scratch);
    if (status.is_ok()) {
      reads_->fetch_add(1, std::memory_order_relaxed);
      bytes_->fetch_add(n, std::memory_order_relaxed);
    }
    return status;
  }

 private:
  const std::unique_ptr<RandomAccessFile> inner_;
  std::atomic<uint64_t>* const reads_;
  std::atomic<uint64_t>* const bytes_;
};

}  // namespace

TableCache::TableCache(std::string dbname, const Options& options,
                       const Comparator* comparator, int entries)
    : dbname_(std::move(dbname)),
      options_(options),
      comparator_(comparator),
      // Each shard gets an equal share, rounded up, as the block cache
      // divides its capacity.  The total can therefore exceed the limit by
      // up to fifteen files when the keys spread unevenly, which is inside
      // the ten-descriptor headroom DBImpl leaves and, at worst, a few
      // descriptors past it; an exact limit would need a shared count and
      // the lock the shards exist to avoid.
      capacity_(((entries < 1 ? size_t{1} : static_cast<size_t>(entries)) +
                 kShards - 1) /
                kShards) {}

TableCache::~TableCache() = default;

Status TableCache::find(uint64_t file_number, uint64_t file_size,
                        Entry** result) {
  Shard& shard = shard_for(file_number);
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    const auto it = shard.entries.find(file_number);
    if (it != shard.entries.end()) {
      shard.lru.splice(shard.lru.begin(), shard.lru, it->second.second);
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
  std::unique_ptr<RandomAccessFile> opened;
  Status status = RandomAccessFile::open(table_file_name(dbname_, file_number),
                                         &opened);
  if (!status.is_ok()) return status;
  std::unique_ptr<RandomAccessFile> file = std::make_unique<CountingFile>(
      std::move(opened), &reads_, &bytes_read_);

  std::unique_ptr<Table> table;
  status = Table::open(options_, comparator_, file.get(), file_size, &table);
  if (!status.is_ok()) return status;

  entry->file = std::move(file);
  entry->table = std::move(table);
  entry->file_number = file_number;

  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    const auto it = shard.entries.find(file_number);
    if (it != shard.entries.end()) {
      // Someone else won the race; use theirs and drop ours.
      shard.lru.splice(shard.lru.begin(), shard.lru, it->second.second);
      ++it->second.first->refs;
      *result = it->second.first.get();
      return Status::ok();
    }

    entry->refs = 2;  // one for the cache, one for the caller
    Entry* raw = entry.get();
    shard.lru.push_front(file_number);
    shard.entries.emplace(file_number,
                          std::make_pair(std::move(entry), shard.lru.begin()));
    *result = raw;
    evict_if_over_capacity(&shard);
    return Status::ok();
  }
}

void TableCache::evict_if_over_capacity(Shard* shard) {
  while (shard->entries.size() > capacity_ && !shard->lru.empty()) {
    const uint64_t victim = shard->lru.back();
    const auto it = shard->entries.find(victim);
    if (it == shard->entries.end()) {
      shard->lru.pop_back();
      continue;
    }
    Entry* entry = it->second.first.get();
    entry->in_cache = false;
    shard->lru.pop_back();

    if (--entry->refs == 0) {
      shard->entries.erase(it);
    } else {
      // Still in use by an iterator.  The entry leaves the map so it can no
      // longer be found, and its owner is transferred to the last user, who
      // closes it in release().  Keeping it in the map instead would let a
      // later lookup hand out a table that is about to be deleted.
      it->second.first.release();
      shard->entries.erase(it);
    }
  }
}

void TableCache::release(Entry* entry) {
  std::lock_guard<std::mutex> lock(shard_for(entry->file_number).mutex);
  if (--entry->refs == 0) {
    delete entry;  // only reachable once it has left the map
  }
}

void TableCache::evict(uint64_t file_number) {
  Shard& shard = shard_for(file_number);
  std::lock_guard<std::mutex> lock(shard.mutex);
  const auto it = shard.entries.find(file_number);
  if (it == shard.entries.end()) return;

  Entry* entry = it->second.first.get();
  entry->in_cache = false;
  shard.lru.erase(it->second.second);
  if (--entry->refs == 0) {
    shard.entries.erase(it);
  } else {
    it->second.first.release();
    shard.entries.erase(it);
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
