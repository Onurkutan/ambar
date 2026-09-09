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
    truncated_ = true;
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
        // ends here.
        truncated_ = true;
        failure_reason_ = "partial record header at end of log";
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
      // Zero padding: skip to the next block.
      if (block_.size() < kBlockSize) {
        eof_ = true;
        return false;
      }
      if (!load_next_block()) return false;
      continue;
    }

    if (block_offset_ + kHeaderSize + length > block_.size()) {
      truncated_ = true;
      failure_reason_ = "record length runs past the end of the log";
      eof_ = true;
      return false;
    }

    const char* data = header + kHeaderSize;
    uint32_t actual_crc = crc32c_extend(0, std::string_view(header + 6, 1));
    actual_crc = crc32c_extend(actual_crc, std::string_view(data, length));
    if (actual_crc != expected_crc) {
      // The signature of a torn write, or of a damaged sector.  Either way the
      // durable prefix ends immediately before this record.
      truncated_ = true;
      failure_reason_ = "checksum mismatch";
      eof_ = true;
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
    truncated_ = true;
    failure_reason_ = "log begins with a continuation fragment";
    eof_ = true;
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
    truncated_ = true;
    failure_reason_ = "unexpected fragment type inside a record";
    eof_ = true;
    return false;
  }
}

}  // namespace ambar
