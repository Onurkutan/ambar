// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The parts of WriteBatch the database needs and callers do not.
//
// A batch is built before the engine knows what sequence number it will get:
// the number is assigned under the writer lock, at the moment the batch joins
// the log, so that the order of numbers matches the order of records in the
// file.  That means someone has to patch the header after the fact, and that
// someone should not be the public API -- a caller who could set the sequence
// number could silently make one batch overwrite another's newer values.

#ifndef AMBAR_WRITE_BATCH_INTERNAL_HPP_
#define AMBAR_WRITE_BATCH_INTERNAL_HPP_

#include <cstdint>
#include <string>
#include <string_view>

#include "ambar/status.hpp"
#include "ambar/write_batch.hpp"
#include "dbformat.hpp"
#include "memtable.hpp"

namespace ambar {

class WriteBatchInternal {
 public:
  // fixed64 sequence + fixed32 count.
  static constexpr size_t kHeader = 12;

  static uint32_t count(const WriteBatch& b);
  static void set_count(WriteBatch* b, uint32_t n);

  // The sequence number of the batch's *first* operation.  The i-th operation
  // takes sequence + i, which is what makes the operations inside one batch
  // ordered with respect to each other.
  static SequenceNumber sequence(const WriteBatch& b);
  static void set_sequence(WriteBatch* b, SequenceNumber seq);

  static std::string_view contents(const WriteBatch& b) { return b.rep_; }
  static size_t byte_size(const WriteBatch& b) { return b.rep_.size(); }

  // Replaces the whole batch with bytes read back from a log record.
  static Status set_contents(WriteBatch* b, std::string_view contents);

  // Applies the batch to a memtable, assigning sequence numbers as it goes.
  static Status insert_into(const WriteBatch& b, MemTable* memtable);

  // Concatenates src onto dst.  Used when several waiting writers are merged
  // into one log record, which is what turns many small fsyncs into one.
  static void append(WriteBatch* dst, const WriteBatch& src);
};

}  // namespace ambar

#endif  // AMBAR_WRITE_BATCH_INTERNAL_HPP_
