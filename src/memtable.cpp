// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "memtable.hpp"

#include <cstring>

#include "encoding.hpp"

namespace ambar {
namespace {

// Reads the length-prefixed field an arena record starts with, and leaves
// *data pointing just past it.
//
// decode_varint32_exact rather than the bounded reader: inside an arena there
// is no buffer length to bound against, and guessing one reads past short
// records into the neighbouring bytes.
std::string_view read_length_prefixed(const char** data) {
  uint32_t length = 0;
  const char* start = decode_varint32_exact(*data, &length);
  assert(start != nullptr);
  *data = start + length;
  return std::string_view(start, length);
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  const char* pa = a;
  const char* pb = b;
  const std::string_view key_a = read_length_prefixed(&pa);
  const std::string_view key_b = read_length_prefixed(&pb);
  return compare_internal_keys(key_a, key_b);
}

MemTable::MemTable() : table_(KeyComparator{}, &arena_) {}

void MemTable::add(SequenceNumber sequence, ValueType type,
                   std::string_view key, std::string_view value) {
  // Layout: varint(klen) | internal key | varint(vlen) | value.
  // klen counts the 8-byte trailer, so the record is self-describing and the
  // comparator needs no external length.
  const size_t key_size = key.size() + 8;
  const size_t value_size = value.size();

  std::string encoded;
  encoded.reserve(key_size + value_size + 10);
  put_varint32(&encoded, static_cast<uint32_t>(key_size));
  append_internal_key(&encoded, {key, sequence, type});
  put_varint32(&encoded, static_cast<uint32_t>(value_size));
  encoded.append(value.data(), value.size());

  char* buffer = arena_.allocate(encoded.size());
  std::memcpy(buffer, encoded.data(), encoded.size());
  table_.insert(buffer);
}

bool MemTable::get(const LookupKey& key, std::string* value,
                   Status* status) const {
  // Seek to the newest version of the key at or below the snapshot.  Because
  // internal keys sort by user key ascending then sequence descending, that
  // entry is the first one at or after the lookup key, whose memtable form
  // is the length-prefixed one the skip list holds.
  Table::Iterator iter(&table_);
  iter.seek(key.memtable_key().data());
  if (!iter.valid()) return false;

  const char* entry = iter.key();
  const std::string_view internal_key = read_length_prefixed(&entry);
  if (extract_user_key(internal_key) != key.user_key()) {
    // Landed on a different user key: this memtable has nothing to say.
    return false;
  }

  if (extract_value_type(internal_key) == ValueType::kDeletion) {
    // A tombstone answers the query.  Returning false here would let an older
    // table resurrect the deleted value -- the classic LSM bug.
    *status = Status::not_found("deleted");
    return true;
  }

  const std::string_view stored = read_length_prefixed(&entry);
  value->assign(stored.data(), stored.size());
  *status = Status::ok();
  return true;
}

void MemTable::Iterator::seek(std::string_view internal_key) {
  seek_buffer_.clear();
  put_varint32(&seek_buffer_, static_cast<uint32_t>(internal_key.size()));
  seek_buffer_.append(internal_key.data(), internal_key.size());
  iter_.seek(seek_buffer_.data());
}

std::string_view MemTable::Iterator::key() const {
  const char* entry = iter_.key();
  return read_length_prefixed(&entry);
}

std::string_view MemTable::Iterator::value() const {
  const char* entry = iter_.key();
  read_length_prefixed(&entry);          // skip the key
  return read_length_prefixed(&entry);   // and return the value
}

namespace {

// Adapts MemTable::Iterator, which is forward-and-back but not virtual, to the
// interface the merging iterator speaks.
class MemTableIteratorAdapter final : public ambar::Iterator {
 public:
  explicit MemTableIteratorAdapter(const MemTable* table) : iter_(table) {}

  bool valid() const override { return iter_.valid(); }
  void seek_to_first() override { iter_.seek_to_first(); }
  void seek_to_last() override { iter_.seek_to_last(); }
  void seek(std::string_view target) override { iter_.seek(target); }
  void next() override { iter_.next(); }
  void prev() override { iter_.prev(); }
  std::string_view key() const override { return iter_.key(); }
  std::string_view value() const override { return iter_.value(); }

  // A memtable cannot fail: it is memory this process wrote, with no decoding
  // step that could reject it.
  Status status() const override { return Status::ok(); }

 private:
  MemTable::Iterator iter_;
};

}  // namespace

ambar::Iterator* MemTable::new_iterator() const {
  return new MemTableIteratorAdapter(this);
}

}  // namespace ambar
