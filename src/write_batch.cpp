// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "ambar/write_batch.hpp"

#include <cassert>

#include "dbformat.hpp"
#include "encoding.hpp"
#include "memtable.hpp"
#include "write_batch_internal.hpp"

namespace ambar {

WriteBatch::Handler::~Handler() = default;

WriteBatch::WriteBatch() { clear(); }

WriteBatch::~WriteBatch() = default;

void WriteBatch::clear() {
  rep_.clear();
  rep_.resize(WriteBatchInternal::kHeader);
}

size_t WriteBatch::approximate_size() const { return rep_.size(); }

uint32_t WriteBatch::count() const { return WriteBatchInternal::count(*this); }

void WriteBatch::put(std::string_view key, std::string_view value) {
  WriteBatchInternal::set_count(this, WriteBatchInternal::count(*this) + 1);
  rep_.push_back(static_cast<char>(ValueType::kValue));
  put_length_prefixed(&rep_, key);
  put_length_prefixed(&rep_, value);
}

void WriteBatch::del(std::string_view key) {
  WriteBatchInternal::set_count(this, WriteBatchInternal::count(*this) + 1);
  rep_.push_back(static_cast<char>(ValueType::kDeletion));
  put_length_prefixed(&rep_, key);
}

Status WriteBatch::iterate(Handler* handler) const {
  if (rep_.size() < WriteBatchInternal::kHeader) {
    return Status::corruption("write batch is smaller than its header");
  }

  std::string_view input(rep_);
  input.remove_prefix(WriteBatchInternal::kHeader);

  // The declared count is checked against what is actually there, rather than
  // trusted.  A truncated record that still passed the log checksum is
  // impossible, but a batch handed to set_contents by a future caller is not,
  // and a mismatch here is far cheaper to find than a silent short replay.
  uint32_t found = 0;
  std::string_view key;
  std::string_view value;

  while (!input.empty()) {
    const char tag = input.front();
    input.remove_prefix(1);
    ++found;

    switch (static_cast<ValueType>(tag)) {
      case ValueType::kValue:
        if (!get_length_prefixed(&input, &key) ||
            !get_length_prefixed(&input, &value)) {
          return Status::corruption("write batch: truncated put record");
        }
        handler->put(key, value);
        break;

      case ValueType::kDeletion:
        if (!get_length_prefixed(&input, &key)) {
          return Status::corruption("write batch: truncated delete record");
        }
        handler->del(key);
        break;

      default:
        return Status::corruption("write batch: unknown record tag");
    }
  }

  if (found != WriteBatchInternal::count(*this)) {
    return Status::corruption("write batch: record count does not match header");
  }
  return Status::ok();
}

// -- WriteBatchInternal -----------------------------------------------------

uint32_t WriteBatchInternal::count(const WriteBatch& b) {
  return decode_fixed32(b.rep_.data() + 8);
}

void WriteBatchInternal::set_count(WriteBatch* b, uint32_t n) {
  encode_fixed32(&b->rep_[8], n);
}

SequenceNumber WriteBatchInternal::sequence(const WriteBatch& b) {
  return SequenceNumber{decode_fixed64(b.rep_.data())};
}

void WriteBatchInternal::set_sequence(WriteBatch* b, SequenceNumber seq) {
  encode_fixed64(&b->rep_[0], seq);
}

Status WriteBatchInternal::set_contents(WriteBatch* b,
                                        std::string_view contents) {
  if (contents.size() < kHeader) {
    return Status::corruption("write batch: log record smaller than header");
  }
  b->rep_.assign(contents.data(), contents.size());
  return Status::ok();
}

void WriteBatchInternal::append(WriteBatch* dst, const WriteBatch& src) {
  set_count(dst, count(*dst) + count(src));
  assert(src.rep_.size() >= kHeader);
  dst->rep_.append(src.rep_.data() + kHeader, src.rep_.size() - kHeader);
}

namespace {

// Walks a batch and inserts each operation into a memtable, handing out
// consecutive sequence numbers.
class MemTableInserter final : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber first, MemTable* memtable)
      : sequence_(first), memtable_(memtable) {}

  void put(std::string_view key, std::string_view value) override {
    memtable_->add(sequence_, ValueType::kValue, key, value);
    ++sequence_;
  }

  void del(std::string_view key) override {
    memtable_->add(sequence_, ValueType::kDeletion, key, std::string_view());
    ++sequence_;
  }

  SequenceNumber next_sequence() const { return sequence_; }

 private:
  SequenceNumber sequence_;
  MemTable* memtable_;
};

}  // namespace

Status WriteBatchInternal::insert_into(const WriteBatch& b,
                                       MemTable* memtable) {
  MemTableInserter inserter(sequence(b), memtable);
  return b.iterate(&inserter);
}

}  // namespace ambar
