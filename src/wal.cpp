// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "wal.hpp"

#include <cassert>
#include <cstring>

#include "encoding.hpp"

namespace ambar {

LogWriter::LogWriter(std::unique_ptr<WritableFile> dest, uint64_t initial_offset)
    : dest_(std::move(dest)), offset_(initial_offset % kBlockSize) {}

Status LogWriter::add_record(std::string_view payload) {
  const char* data = payload.data();
  size_t left = payload.size();

  // A zero-length record is legal and must round-trip, so the loop runs at
  // least once regardless of `left`.
  bool first_fragment = true;
  Status status;
  do {
    const size_t block_left = kBlockSize - offset_;

    // A header cannot straddle a boundary.  If fewer than seven bytes remain,
    // pad them with zeros and start a new block; a reader recognises the zeros
    // as trailer rather than as a record.
    if (block_left < kHeaderSize) {
      if (block_left > 0) {
        static const char kPadding[kHeaderSize] = {};
        status = dest_->append(std::string_view(kPadding, block_left));
        if (!status.is_ok()) return status;
      }
      offset_ = 0;
    }

    const size_t available = kBlockSize - offset_ - kHeaderSize;
    const size_t fragment = left < available ? left : available;
    const bool last_fragment = (fragment == left);

    RecordType type;
    if (first_fragment && last_fragment) {
      type = RecordType::kFull;
    } else if (first_fragment) {
      type = RecordType::kFirst;
    } else if (last_fragment) {
      type = RecordType::kLast;
    } else {
      type = RecordType::kMiddle;
    }

    status = emit_physical_record(type, data, fragment);
    if (!status.is_ok()) return status;

    data += fragment;
    left -= fragment;
    first_fragment = false;
  } while (left > 0);

  return Status::ok();
}

Status LogWriter::emit_physical_record(RecordType type, const char* data,
                                       size_t length) {
  assert(length <= 0xffff);
  assert(offset_ + kHeaderSize + length <= kBlockSize);

  // The checksum covers the type byte as well as the payload, so a corrupted
  // type -- which would change how the record is reassembled -- is caught too.
  std::string header;
  header.reserve(kHeaderSize);
  const char type_byte = static_cast<char>(type);
  uint32_t crc = crc32c_extend(0, std::string_view(&type_byte, 1));
  crc = crc32c_extend(crc, std::string_view(data, length));

  put_fixed32(&header, crc);
  header.push_back(static_cast<char>(length & 0xff));
  header.push_back(static_cast<char>((length >> 8) & 0xff));
  header.push_back(type_byte);

  if (Status s = dest_->append(header); !s.is_ok()) return s;
  if (Status s = dest_->append(std::string_view(data, length)); !s.is_ok()) {
    return s;
  }
  offset_ += kHeaderSize + length;
  return Status::ok();
}

Status LogWriter::flush() { return dest_->flush(); }
Status LogWriter::sync() { return dest_->sync(); }
Status LogWriter::close() { return dest_->close(); }

// ------------------------------------------------------------- LogReader ---
LogReader::LogReader(std::unique_ptr<SequentialFile> source)
    : source_(std::move(source)) {}

bool LogReader::load_next_block() {
  std::string_view chunk;
  const Status status = source_->read(kBlockSize, &chunk, &block_);
  if (!status.is_ok()) {
    // Not a tail of any kind: bytes that could not be read, whatever they
    // hold.  Reported as damage so nobody builds on the part that was read.
    truncated_ = true;
    read_failed_ = true;
    failure_reason_ = status.to_string();
    eof_ = true;
    return false;
  }
  block_.resize(chunk.size());
  block_offset_ = 0;
  if (chunk.empty()) {
    eof_ = true;
    return false;
  }
  return true;
}

namespace {

struct Header {
  uint32_t crc;
  size_t length;
  RecordType type;
};

Header parse_header(const char* p) {
  return {decode_fixed32(p),
          static_cast<size_t>(static_cast<unsigned char>(p[4]) |
                              (static_cast<unsigned char>(p[5]) << 8)),
          static_cast<RecordType>(p[6])};
}

// Seven zero bytes.  The writer pads only the last six bytes of a block, so
// a whole header of zeros is never something it wrote: it is a span the
// filesystem zeroed, or a hole.
bool is_zeroed(const Header& h) {
  return h.type == RecordType::kZero && h.length == 0 && h.crc == 0;
}

bool known_type(RecordType t) {
  return t == RecordType::kFull || t == RecordType::kFirst ||
         t == RecordType::kMiddle || t == RecordType::kLast;
}

// Whether the record whose header is at `p` carries the checksum it claims.
bool verifies(const char* p, const Header& h) {
  uint32_t crc = crc32c_extend(0, std::string_view(p + 6, 1));
  crc = crc32c_extend(crc, std::string_view(p + kHeaderSize, h.length));
  return crc == h.crc && known_type(h.type);
}

}  // namespace

void LogReader::stop(const char* reason, size_t resume_offset) {
  truncated_ = true;
  eof_ = true;
  failure_reason_ = reason;
  // The byte-by-byte search starts just past the failed header rather than
  // at `resume_offset`: the checksum does not cover the length field, so a
  // damaged length leaves `resume_offset` pointing anywhere at all.
  const size_t scan_from =
      resume_offset < block_offset_ + 1 ? resume_offset : block_offset_ + 1;
  damaged_ = readable_record_follows(resume_offset, scan_from);
}

// Whether records verify back to back from `start` until the block ends --
// at its padding, or at the end of the file.  A single record can be a copy
// of one that happens to sit inside a payload; a chain of them that runs
// exactly to the end of the block cannot, in practice.
bool LogReader::verifies_to_block_end(size_t start) const {
  size_t offset = start;
  bool any = false;
  while (offset + kHeaderSize <= block_.size()) {
    const char* p = block_.data() + offset;
    const Header h = parse_header(p);
    if (is_zeroed(h)) break;
    if (offset + kHeaderSize + h.length > block_.size()) return false;
    if (!verifies(p, h)) return false;
    any = true;
    offset += kHeaderSize + h.length;
  }
  for (size_t i = offset; i < block_.size(); ++i) {
    if (block_[i] != '\0') return false;
  }
  return any;
}

// Whether a record that verifies exists anywhere after `offset` in the
// current block, or in any later block.  Runs once, after the reader has
// already stopped, so consuming the rest of the file is fine.
//
// A false positive -- calling a torn tail damage -- would refuse to open a
// database that a crash left in a perfectly ordinary state, so the search is
// arranged to make one all but impossible: a record found by walking the
// length fields is accepted on its own checksum, since that walk lands on
// real record boundaries whenever the damage was in a payload; a record
// found by trying every byte offset, which is what a damaged length field
// calls for, is accepted only when the records after it verify all the way
// to the end of the block.  The one shape that rule misses is a damaged
// length field *and* a torn tail in the same final block: the chain cannot
// reach the end, and the intact records between the two read as a tear.
bool LogReader::readable_record_follows(size_t walk_from,
                                        size_t scan_from) {
  while (true) {
    size_t walk = walk_from;
    while (walk + kHeaderSize <= block_.size()) {
      const char* p = block_.data() + walk;
      const Header h = parse_header(p);
      if (is_zeroed(h)) {
        ++walk;  // a zeroed span: look past it, a byte at a time
        continue;
      }
      if (walk + kHeaderSize + h.length > block_.size()) break;
      if (verifies(p, h)) return true;
      walk += kHeaderSize + h.length;
    }

    for (size_t at = scan_from; at + kHeaderSize <= block_.size(); ++at) {
      const Header h = parse_header(block_.data() + at);
      if (!known_type(h.type) || at + kHeaderSize + h.length > block_.size()) {
        continue;
      }
      if (verifies_to_block_end(at)) return true;
    }

    if (block_.size() < kBlockSize) return false;  // that was the last block

    std::string_view chunk;
    if (!source_->read(kBlockSize, &chunk, &block_).is_ok()) {
      read_failed_ = true;  // unread bytes: not a tail, whatever else it is
      return false;
    }
    block_.resize(chunk.size());
    if (chunk.empty()) return false;
    walk_from = 0;
    scan_from = 0;
  }
}

bool LogReader::read_physical_record(RecordType* type,
                                     std::string_view* payload) {
  while (true) {
    if (block_offset_ + kHeaderSize > block_.size()) {
      // Not enough left in this block for a header: either padding at the end
      // of a full block, or the file simply stopped here.
      if (block_.size() == kBlockSize) {
        if (!load_next_block()) return false;
        continue;
      }
      if (block_offset_ < block_.size()) {
        // A few stray bytes that are too short to be a header -- a write that
        // did not finish.  Not corruption in any alarming sense, but the log
        // ends here.  (This is the last block, so nothing can follow.)
        stop("partial record header at end of log", block_.size());
        return false;
      }
      eof_ = true;
      return false;
    }

    const char* header = block_.data() + block_offset_;
    const uint32_t expected_crc = decode_fixed32(header);
    const size_t length = static_cast<size_t>(
        static_cast<unsigned char>(header[4]) |
        (static_cast<unsigned char>(header[5]) << 8));
    const auto record_type = static_cast<RecordType>(header[6]);

    if (record_type == RecordType::kZero && length == 0 &&
        expected_crc == 0) {
      // Seven zero bytes where a header should be.  The writer pads only the
      // last six bytes of a block, so this is a zeroed span -- what a power
      // cut leaves on a filesystem that extends a file before its data lands.
      // Whatever follows decides whether the file is torn here or damaged.
      stop("zeroed span where a record header should be", block_offset_);
      return false;
    }

    if (block_offset_ + kHeaderSize + length > block_.size()) {
      // The length cannot be trusted, so the rest of this block cannot be
      // walked; any later block still can be.
      stop("record length runs past the end of the log", block_.size());
      return false;
    }

    const char* data = header + kHeaderSize;
    uint32_t actual_crc = crc32c_extend(0, std::string_view(header + 6, 1));
    actual_crc = crc32c_extend(actual_crc, std::string_view(data, length));
    if (actual_crc != expected_crc) {
      // The signature of a torn write, or of a damaged sector.  The durable
      // prefix ends immediately before this record either way; which of the
      // two it was is decided by what lies after it.
      stop("checksum mismatch", block_offset_ + kHeaderSize + length);
      return false;
    }

    block_offset_ += kHeaderSize + length;
    *type = record_type;
    *payload = std::string_view(data, length);
    return true;
  }
}

bool LogReader::read_record(std::string_view* record, std::string* scratch) {
  if (eof_) return false;
  if (block_.empty() && !load_next_block()) return false;

  RecordType type;
  std::string_view fragment;
  if (!read_physical_record(&type, &fragment)) return false;

  if (type == RecordType::kFull) {
    // The common case: no copy, the caller reads straight out of the block.
    *record = fragment;
    return true;
  }

  if (type != RecordType::kFirst) {
    // The fragment verified, so it counts as a readable record after the
    // failure: resume at its start, not past it.
    stop("log begins with a continuation fragment",
         block_offset_ - kHeaderSize - fragment.size());
    return false;
  }

  // A fragmented record has to be reassembled, so here we do copy.
  scratch->assign(fragment.data(), fragment.size());
  while (true) {
    if (!read_physical_record(&type, &fragment)) {
      // Ran out mid-record: the write never completed.
      truncated_ = true;
      if (failure_reason_.empty()) {
        failure_reason_ = "log ends in the middle of a fragmented record";
      }
      return false;
    }
    if (type == RecordType::kMiddle) {
      scratch->append(fragment.data(), fragment.size());
      continue;
    }
    if (type == RecordType::kLast) {
      scratch->append(fragment.data(), fragment.size());
      *record = *scratch;
      return true;
    }
    stop("unexpected fragment type inside a record",
         block_offset_ - kHeaderSize - fragment.size());
    return false;
  }
}

}  // namespace ambar
