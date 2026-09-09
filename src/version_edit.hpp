// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// A change to the set of files that make up the database.
//
// The database's structure -- which table file sits at which level, covering
// which key range -- is not stored as a snapshot that gets rewritten.  It is
// stored as a log of edits: this file was added at level 2, that one was
// removed.  Opening replays the log to rebuild the current picture.
//
// The reason is atomicity, and it is the same reason the write path uses a log.
// A compaction produces new files and retires old ones, and there is no instant
// at which a rewritten snapshot could be half-updated without the database
// being wrong.  An edit, by contrast, is one record in a log file: it is either
// entirely there after a crash or entirely absent, and the log's own checksum
// decides which.  Nothing has to be undone.
//
// The log is called the manifest.  It holds the same kind of records as the
// write-ahead log, and uses the same reader and writer.

#ifndef AMBAR_VERSION_EDIT_HPP_
#define AMBAR_VERSION_EDIT_HPP_

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ambar/status.hpp"
#include "dbformat.hpp"

namespace ambar {

// What is known about one table file without opening it.
//
// The key range is here, rather than being read from the file, because that is
// what makes a lookup cheap: a level's files are disjoint and sorted, so a
// binary search over these ranges finds the one file that could hold a key,
// and the other files are never touched.
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // smallest internal key in the file
  std::string largest;   // largest internal key in the file

  // How many reads may still miss this file before it is worth compacting.
  //
  // A file that is repeatedly searched and repeatedly does not have the key is
  // costing every one of those reads; folding it into the level below removes
  // the cost.  Counted at runtime and not persisted, because it describes the
  // current workload rather than the data.
  int allowed_seeks = 1 << 30;
  int refs = 0;
};

class VersionEdit {
 public:
  void clear();

  void set_comparator_name(std::string_view name) {
    comparator_ = std::string(name);
    has_comparator_ = true;
  }
  void set_log_number(uint64_t number) {
    log_number_ = number;
    has_log_number_ = true;
  }
  void set_prev_log_number(uint64_t number) {
    prev_log_number_ = number;
    has_prev_log_number_ = true;
  }
  void set_next_file(uint64_t number) {
    next_file_number_ = number;
    has_next_file_number_ = true;
  }
  void set_last_sequence(SequenceNumber sequence) {
    last_sequence_ = sequence;
    has_last_sequence_ = true;
  }

  // Where the next compaction of `level` should start, so that successive
  // compactions sweep across the key space instead of returning to the same
  // busy range.
  void set_compact_pointer(int level, std::string_view key) {
    compact_pointers_.emplace_back(level, std::string(key));
  }

  void add_file(int level, uint64_t file, uint64_t file_size,
                std::string_view smallest, std::string_view largest);

  void remove_file(int level, uint64_t file) {
    deleted_files_.insert({level, file});
  }

  void encode_to(std::string* dst) const;
  Status decode_from(std::string_view src);

  std::string debug_string() const;

  // Accessors used by VersionSet during recovery and installation.
  bool has_comparator() const { return has_comparator_; }
  const std::string& comparator() const { return comparator_; }
  bool has_log_number() const { return has_log_number_; }
  uint64_t log_number() const { return log_number_; }
  bool has_prev_log_number() const { return has_prev_log_number_; }
  uint64_t prev_log_number() const { return prev_log_number_; }
  bool has_next_file_number() const { return has_next_file_number_; }
  uint64_t next_file_number() const { return next_file_number_; }
  bool has_last_sequence() const { return has_last_sequence_; }
  SequenceNumber last_sequence() const { return last_sequence_; }

  const std::vector<std::pair<int, std::string>>& compact_pointers() const {
    return compact_pointers_;
  }
  const std::set<std::pair<int, uint64_t>>& deleted_files() const {
    return deleted_files_;
  }
  const std::vector<std::pair<int, FileMetaData>>& new_files() const {
    return new_files_;
  }

 private:
  std::string comparator_;
  uint64_t log_number_ = 0;
  uint64_t prev_log_number_ = 0;
  uint64_t next_file_number_ = 0;
  SequenceNumber last_sequence_ = 0;

  bool has_comparator_ = false;
  bool has_log_number_ = false;
  bool has_prev_log_number_ = false;
  bool has_next_file_number_ = false;
  bool has_last_sequence_ = false;

  std::vector<std::pair<int, std::string>> compact_pointers_;
  std::set<std::pair<int, uint64_t>> deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace ambar

#endif  // AMBAR_VERSION_EDIT_HPP_
