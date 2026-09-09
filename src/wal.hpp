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
  // `dest` is taken over.  `initial_offset` lets an existing log be reopened
  // and appended to without breaking its block alignment.
  LogWriter(std::unique_ptr<WritableFile> dest, uint64_t initial_offset = 0);

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

 private:
  Status emit_physical_record(RecordType type, const char* data, size_t length);

  std::unique_ptr<WritableFile> dest_;
  uint64_t offset_ = 0;  // byte position within the current block
};

class LogReader {
 public:
  explicit LogReader(std::unique_ptr<SequentialFile> source);

  // Reads the next logical record into *scratch, pointing *record at it.
  // Returns false at a clean end of file, at a truncated tail, or at the first
  // corruption -- the caller cannot distinguish them, and does not need to:
  // everything returned so far is durable, and everything after is not.
  //
  // `truncated` is set when the stop was caused by damage rather than a tidy
  // end, so callers that care (recovery reporting) can say so.
  bool read_record(std::string_view* record, std::string* scratch);

  bool truncated() const { return truncated_; }
  const std::string& failure_reason() const { return failure_reason_; }

 private:
  bool read_physical_record(RecordType* type, std::string_view* payload);
  bool load_next_block();

  std::unique_ptr<SequentialFile> source_;
  std::string block_;          // the 32 KiB currently being parsed
  size_t block_offset_ = 0;    // read position within block_
  bool eof_ = false;
  bool truncated_ = false;
  std::string failure_reason_;
};

}  // namespace ambar
