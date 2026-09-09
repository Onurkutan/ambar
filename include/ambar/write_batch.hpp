// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// A batch of updates applied to the database as a single unit.
//
// The batch is the engine's unit of atomicity, and it gets that property for
// free rather than by implementing it.  A batch serialises into one byte
// string; that string is written to the log as one record; a log record is
// either wholly present after a crash or wholly absent, because the reader
// verifies a checksum over the whole thing.  So "all of the batch survives, or
// none of it does" is not a guarantee this class enforces -- it is a
// restatement of the log's guarantee.  Nothing here needs to undo anything.
//
// The same string is also what a replica or a backup would ship, which is why
// the format is documented in docs/DESIGN.md rather than left implicit.

#ifndef AMBAR_WRITE_BATCH_HPP_
#define AMBAR_WRITE_BATCH_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ambar/status.hpp"

namespace ambar {

class WriteBatch {
 public:
  WriteBatch();
  ~WriteBatch();

  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;

  // Stores key -> value.  Both may contain any bytes, including embedded NULs
  // and the empty string.
  void put(std::string_view key, std::string_view value);

  // Records that key is deleted.  Deleting a key that was never present is not
  // an error and still costs a record: the engine cannot know, at write time,
  // whether the key exists somewhere down the level structure.
  void del(std::string_view key);

  // Empties the batch back to the state of a freshly constructed one.
  void clear();

  // How many operations the batch holds.  Two puts of the same key count as
  // two; the later one wins on replay because it takes the higher sequence
  // number.
  uint32_t count() const;

  // Bytes this batch will add to the log, header included.  Callers use it to
  // decide when to stop accumulating; it is exact, not an estimate, but the
  // name matches the convention callers expect.
  size_t approximate_size() const;

  // Applies every operation in the batch, in order, to a receiver.  This is
  // how recovery replays a batch into a fresh memtable, and how a test can
  // assert what a batch actually contains rather than what it was asked to
  // contain.
  class Handler {
   public:
    virtual ~Handler();
    virtual void put(std::string_view key, std::string_view value) = 0;
    virtual void del(std::string_view key) = 0;
  };

  // Returns kCorruption if the serialised form is malformed, which for a batch
  // built through this class cannot happen -- the check exists because after a
  // crash the bytes come from a file, and a file is not a trusted input even
  // when this process wrote it.
  Status iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  // header: fixed64 sequence, fixed32 count, then the records.
  std::string rep_;
};

}  // namespace ambar

#endif  // AMBAR_WRITE_BATCH_HPP_
