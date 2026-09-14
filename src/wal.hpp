// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The write-ahead log: the file that makes a crash survivable.
//
// Format, one 32 KiB block at a time:
//
//     block := record*  trailer?
//     record := crc32c(4) | length(2) | type(1) | payload(length)
//     type   := FULL | FIRST | MIDDLE | LAST
//
// Two decisions worth explaining, both borrowed from LevelDB because they solve
// real problems rather than imaginary ones:
//
// * **Fixed blocks.** A record never straddles a block boundary without saying
//   so.  After a corrupt region, a reader can skip to the next block boundary
//   and resynchronise instead of giving up on the whole file.
//
// * **A checksum per record, not per file.** A crash mid-write leaves a partial
//   record at the tail.  Its checksum will not match, so replay stops there and
//   treats everything before it as good -- which is exactly the "recover to a
//   prefix of the acknowledged writes" promise in docs/DESIGN.md.  A whole-file
//   checksum could only say "this file is broken", losing all of it.
//
// The seven bytes of header per record are the cost of being able to tell a
// truncated log from a complete one.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ambar/status.hpp"
#include "file.hpp"

namespace ambar {

constexpr size_t kBlockSize = 32768;
constexpr size_t kHeaderSize = 4 + 2 + 1;  // crc + length + type

enum class RecordType : uint8_t {
  kZero = 0,   // never written; a zeroed region reads as this and ends replay
  kFull = 1,
  kFirst = 2,
  kMiddle = 3,
  kLast = 4,
};

class LogWriter {
 public:
  // `dest` is taken over, and is written from its beginning: nothing in the
  // engine appends to a log or a manifest that already exists.
  explicit LogWriter(std::unique_ptr<WritableFile> dest);

  // Appends one logical record, fragmenting it across blocks if needed.
  //
  // The bytes go no further than this process's own buffer.  Nothing here
  // reaches the kernel, so a record that has only been added is lost if the
  // process dies -- the caller decides when to flush, because it knows whether
  // a group of records is still being assembled and one syscall can serve all
  // of them.
  Status add_record(std::string_view payload);

  // Pushes to the kernel; survives a process crash.
  Status flush();

  // Waits for the device; survives a power cut.  The expensive one.
  Status sync();

  Status close();

  uint64_t offset() const { return offset_; }

  // Bytes appended to the file so far -- headers, payloads and the padding
  // at block ends -- which is what the file costs the disk, as opposed to
  // what its records carried.  Write amplification is measured in the
  // former.
  uint64_t bytes_written() const { return bytes_written_; }

 private:
  Status emit_physical_record(RecordType type, const char* data, size_t length);

  std::unique_ptr<WritableFile> dest_;
  uint64_t offset_ = 0;  // byte position within the current block
  uint64_t bytes_written_ = 0;
};

class LogReader {
 public:
  explicit LogReader(std::unique_ptr<SequentialFile> source);

  // Reads the next logical record into *scratch, pointing *record at it.
  // Returns false at a clean end of file, at a torn tail, or at the first
  // damage.  Everything returned so far is durable; whether the stop is the
  // end of what was ever written, or a hole with more after it, is what
  // truncated() and damaged() below say.
  //
  // `truncated` is set when the stop was caused by damage rather than a tidy
  // end, so callers that care (recovery reporting) can say so.
  bool read_record(std::string_view* record, std::string* scratch);

  bool truncated() const { return truncated_; }
  const std::string& failure_reason() const { return failure_reason_; }

  // True when the stop was damage with readable records after it -- a flipped
  // bit in the middle of the file -- rather than a write that never finished.
  //
  // The two look alike at the point of failure and mean opposite things.  A
  // torn tail is what a crash leaves: the record being written when the
  // process died, and nothing after it, because nothing was written after
  // it.  Stopping there yields exactly the durable prefix.  Damage in the
  // middle has intact records after it, whose contents *did* become durable;
  // stopping there yields a state the file had moved past, and a caller who
  // treated it as the durable prefix would act on it -- a manifest reader
  // would serve an older version of the database and then delete every
  // table the later records name, as unreferenced.  So the reader looks past
  // the failure for a record it can verify, and says which of the two it saw.
  //
  // Also true when the file could not be read to its end: unread bytes are
  // not a tail, whatever they hold.
  bool damaged() const { return damaged_ || read_failed_; }

 private:
  bool read_physical_record(RecordType* type, std::string_view* payload);
  bool load_next_block();

  // Ends the read: records the reason, and decides whether the stop is a
  // torn tail or damage by looking for a verifiable record from
  // `resume_offset` onward.
  void stop(const char* reason, size_t resume_offset);
  bool readable_record_follows(size_t walk_from, size_t scan_from);
  bool verifies_to_block_end(size_t start) const;

  std::unique_ptr<SequentialFile> source_;
  std::string block_;          // the 32 KiB currently being parsed
  size_t block_offset_ = 0;    // read position within block_
  bool eof_ = false;
  bool truncated_ = false;
  bool damaged_ = false;
  bool read_failed_ = false;
  std::string failure_reason_;
};

}  // namespace ambar
