// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "version_edit.hpp"

#include "encoding.hpp"

namespace ambar {
namespace {

// Each field is preceded by a tag, so a record written by a later version --
// one that knows fields this build does not -- is rejected explicitly instead
// of being misread as the fields that follow.
enum class Tag : uint32_t {
  kComparator = 1,
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kCompactPointer = 5,
  kDeletedFile = 6,
  kNewFile = 7,
  // 8 was a field that never shipped; the number is retired rather than
  // reused, so an old file cannot be misread by a new build.
  kPrevLogNumber = 9,
};

bool get_level(std::string_view* input, int* level) {
  uint32_t value = 0;
  if (!get_varint32(input, &value)) return false;
  // A level out of range would index past the end of the version's arrays.
  if (value >= 7) return false;
  *level = static_cast<int>(value);
  return true;
}

}  // namespace

void VersionEdit::clear() {
  *this = VersionEdit();
}

void VersionEdit::add_file(int level, uint64_t file, uint64_t file_size,
                           std::string_view smallest,
                           std::string_view largest) {
  FileMetaData meta;
  meta.number = file;
  meta.file_size = file_size;
  meta.smallest.assign(smallest.data(), smallest.size());
  meta.largest.assign(largest.data(), largest.size());
  new_files_.emplace_back(level, std::move(meta));
}

void VersionEdit::encode_to(std::string* dst) const {
  if (has_comparator_) {
    put_varint32(dst, static_cast<uint32_t>(Tag::kComparator));
    put_length_prefixed(dst, comparator_);
  }
  if (has_log_number_) {
    put_varint32(dst, static_cast<uint32_t>(Tag::kLogNumber));
    put_varint64(dst, log_number_);
  }
  if (has_prev_log_number_) {
    put_varint32(dst, static_cast<uint32_t>(Tag::kPrevLogNumber));
    put_varint64(dst, prev_log_number_);
  }
  if (has_next_file_number_) {
    put_varint32(dst, static_cast<uint32_t>(Tag::kNextFileNumber));
    put_varint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    put_varint32(dst, static_cast<uint32_t>(Tag::kLastSequence));
    put_varint64(dst, last_sequence_);
  }

  for (const auto& [level, key] : compact_pointers_) {
    put_varint32(dst, static_cast<uint32_t>(Tag::kCompactPointer));
    put_varint32(dst, static_cast<uint32_t>(level));
    put_length_prefixed(dst, key);
  }
  for (const auto& [level, number] : deleted_files_) {
    put_varint32(dst, static_cast<uint32_t>(Tag::kDeletedFile));
    put_varint32(dst, static_cast<uint32_t>(level));
    put_varint64(dst, number);
  }
  for (const auto& [level, meta] : new_files_) {
    put_varint32(dst, static_cast<uint32_t>(Tag::kNewFile));
    put_varint32(dst, static_cast<uint32_t>(level));
    put_varint64(dst, meta.number);
    put_varint64(dst, meta.file_size);
    put_length_prefixed(dst, meta.smallest);
    put_length_prefixed(dst, meta.largest);
  }
}

Status VersionEdit::decode_from(std::string_view src) {
  clear();
  std::string_view input = src;
  const char* failure = nullptr;

  uint32_t tag = 0;
  int level = 0;
  uint64_t number = 0;
  std::string_view field;
  FileMetaData meta;

  while (failure == nullptr && get_varint32(&input, &tag)) {
    switch (static_cast<Tag>(tag)) {
      case Tag::kComparator:
        if (get_length_prefixed(&input, &field)) {
          set_comparator_name(field);
        } else {
          failure = "comparator name";
        }
        break;

      case Tag::kLogNumber:
        if (get_varint64(&input, &log_number_)) {
          has_log_number_ = true;
        } else {
          failure = "log number";
        }
        break;

      case Tag::kPrevLogNumber:
        if (get_varint64(&input, &prev_log_number_)) {
          has_prev_log_number_ = true;
        } else {
          failure = "previous log number";
        }
        break;

      case Tag::kNextFileNumber:
        if (get_varint64(&input, &next_file_number_)) {
          has_next_file_number_ = true;
        } else {
          failure = "next file number";
        }
        break;

      case Tag::kLastSequence:
        if (get_varint64(&input, &last_sequence_)) {
          has_last_sequence_ = true;
        } else {
          failure = "last sequence";
        }
        break;

      case Tag::kCompactPointer:
        if (get_level(&input, &level) && get_length_prefixed(&input, &field)) {
          // Compared against a file's largest key by compare_internal_keys,
          // which reads an eight-byte trailer from the end of *this* string --
          // the same underflow the kNewFile check below guards against, for
          // the same reason.  Rejected here rather than where it is compared,
          // which is the one place manifest bytes become a compact pointer.
          if (field.size() < 8) {
            failure = "compaction pointer: a key too short to be an internal key";
          } else {
            compact_pointers_.emplace_back(level, std::string(field));
          }
        } else {
          failure = "compaction pointer";
        }
        break;

      case Tag::kDeletedFile:
        if (get_level(&input, &level) && get_varint64(&input, &number)) {
          deleted_files_.insert({level, number});
        } else {
          failure = "deleted file";
        }
        break;

      case Tag::kNewFile: {
        std::string_view smallest;
        std::string_view largest;
        if (get_level(&input, &level) && get_varint64(&input, &meta.number) &&
            get_varint64(&input, &meta.file_size) &&
            get_length_prefixed(&input, &smallest) &&
            get_length_prefixed(&input, &largest)) {
          // These two are internal keys, and every comparison the version set
          // makes on them reads an eight-byte trailer from the end.  A shorter
          // one — which a damaged or hand-written manifest can contain — makes
          // `size() - 8` underflow, `substr` clamp, and the trailer read land
          // before the start of the string.  Rejected here, at the one place
          // where manifest bytes become keys.
          if (smallest.size() < 8 || largest.size() < 8) {
            failure = "new file: a key too short to be an internal key";
          } else {
            meta.smallest.assign(smallest.data(), smallest.size());
            meta.largest.assign(largest.data(), largest.size());
            new_files_.emplace_back(level, meta);
          }
        } else {
          failure = "new file";
        }
        break;
      }

      default:
        failure = "unknown tag";
        break;
    }
  }

  if (failure == nullptr && !input.empty()) {
    failure = "trailing bytes after the last field";
  }
  if (failure != nullptr) {
    return Status::corruption(std::string("version edit: ") + failure);
  }
  return Status::ok();
}

std::string VersionEdit::debug_string() const {
  std::string out = "VersionEdit {";
  if (has_comparator_) out += "\n  comparator: " + comparator_;
  if (has_log_number_) out += "\n  log: " + std::to_string(log_number_);
  if (has_prev_log_number_) {
    out += "\n  prev log: " + std::to_string(prev_log_number_);
  }
  if (has_next_file_number_) {
    out += "\n  next file: " + std::to_string(next_file_number_);
  }
  if (has_last_sequence_) {
    out += "\n  last sequence: " + std::to_string(last_sequence_);
  }
  for (const auto& [level, key] : compact_pointers_) {
    out += "\n  compact pointer: level " + std::to_string(level);
    (void)key;
  }
  for (const auto& [level, number] : deleted_files_) {
    out += "\n  remove: level " + std::to_string(level) + " file " +
           std::to_string(number);
  }
  for (const auto& [level, meta] : new_files_) {
    out += "\n  add: level " + std::to_string(level) + " file " +
           std::to_string(meta.number) + " (" +
           std::to_string(meta.file_size) + " bytes)";
  }
  out += "\n}";
  return out;
}

}  // namespace ambar
