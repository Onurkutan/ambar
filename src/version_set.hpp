// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Which files exist, at which level, and how they change.
//
// A Version is one immutable answer to "what does the database consist of".
// Readers hold a reference to one and are unaffected by anything a compaction
// does: the compaction builds a *new* Version and installs it, and the old one
// survives until its last reader lets go.  That is what lets compaction run
// concurrently with reads without a lock around the data, and it is why
// nothing here is mutated in place.
//
// Levels.  Level 0 is special: its files come straight from memtable flushes,
// so they overlap each other and a lookup must search all of them, newest
// first.  Every level below is a set of files with disjoint key ranges, sorted,
// so a lookup binary-searches for the one file that could hold the key.  Each
// level is an order of magnitude larger than the one above, so the number of
// levels a key can hide in stays small.
//
// The size of that difference is what makes the whole structure work, and what
// it costs: a key written once is eventually rewritten into every level below
// the one it landed in, so writing a byte costs some multiple of a byte in
// eventual disk traffic.  That is the trade an LSM tree makes -- sequential
// writes now, amplified writes later.
//
// The engine counts that multiple as it happens -- DBImpl keeps the bytes
// each level's compactions read and wrote, and each flush, and this class
// keeps the manifest's -- and reports it through get_property; tools/bench
// divides it by the bytes handed in.  It is kept apart from *space*
// amplification, bytes on disk against bytes of data, which is a different
// and much easier quantity.  docs/BENCHMARKS.md reports both and says which
// is which.

#ifndef AMBAR_VERSION_SET_HPP_
#define AMBAR_VERSION_SET_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ambar/iterator.hpp"
#include "ambar/options.hpp"
#include "ambar/status.hpp"
#include "comparator.hpp"
#include "dbformat.hpp"
#include "file.hpp"
#include "table_cache.hpp"
#include "version_edit.hpp"
#include "wal.hpp"

namespace ambar {

constexpr int kNumLevels = 7;

// Level 0 gets special treatment throughout, because its files overlap.
//
// Compaction starts at four files: enough that a flush is not immediately
// followed by a compaction, few enough that a lookup does not have to search
// many overlapping files.
constexpr int kL0CompactionTrigger = 4;

// Writes slow down at eight and stop at twelve.  The pause is deliberate and
// gradual: a hard stop with no warning turns a gentle backlog into a stall
// that the application sees as a multi-second hang, whereas slowing each write
// by a millisecond at eight files lets compaction catch up while the
// application keeps making progress.
constexpr int kL0SlowdownWritesTrigger = 8;
constexpr int kL0StopWritesTrigger = 12;

// A memtable flush goes as deep as it can without overlapping much, so that
// data which is never read again does not have to be compacted down level by
// level.  Two is the limit: pushing further would make later compactions with
// that level unnecessarily large.
constexpr int kMaxMemCompactLevel = 2;

class Compaction;
class VersionSet;

class Version {
 public:
  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  // Appends iterators over everything in this version, for a compaction or a
  // full scan.
  void add_iterators(const ReadOptions& options,
                     std::vector<Iterator*>* iters);

  // Which file a lookup should charge a fruitless seek to, so that a file
  // repeatedly searched in vain eventually gets compacted away.
  struct GetStats {
    FileMetaData* seek_file = nullptr;
    int seek_file_level = -1;
  };

  Status get(const ReadOptions& options, const LookupKey& key,
             std::string* value, GetStats* stats);

  // Returns true when the charge suggests a compaction is now worth starting.
  bool update_stats(const GetStats& stats);

  // ref() and unref() REQUIRE the database mutex.  release() does not.
  //
  // The count is atomic so that a lookup can let go of its version without
  // taking the database mutex a second time for exactly that: tools/bench's
  // read-scaling phase, on a database held entirely in memory, put a good
  // part of the gap between one thread and eight on that second
  // acquisition.  What the mutex still guards is the part an atomic counter
  // cannot make safe: dropping the last reference unlinks the version from
  // the set's list and releases every file it holds.  So release() drops a
  // reference without the mutex only while it is not the last one, and
  // takes the mutex to drop the last, which means the count reaches zero
  // and the destructor runs under the mutex on every path -- and a thread
  // that takes a new reference under the mutex, which is the only place
  // ref() may be called, either sees the version still alive or never sees
  // it at all.  A version that is current holds a reference of its own, so
  // a lookup's release is never the last while the version is current.
  void ref();
  void unref();
  void release(std::mutex* mutex);

  void get_overlapping_inputs(int level, const std::string* begin,
                              const std::string* end,
                              std::vector<FileMetaData*>* inputs);

  bool overlaps_in_level(int level, const std::string* smallest_user_key,
                         const std::string* largest_user_key);

  // The deepest level a memtable's output can go without overlapping too much.
  int pick_level_for_memtable_output(std::string_view smallest_user_key,
                                     std::string_view largest_user_key);

  int num_files(int level) const {
    return static_cast<int>(files_[level].size());
  }

  std::string debug_string() const;

 private:
  friend class Compaction;
  friend class VersionSet;

  explicit Version(VersionSet* vset) : vset_(vset) {}
  ~Version();

  VersionSet* const vset_;
  Version* next_ = this;  // circular list of live versions, newest last
  Version* prev_ = this;
  std::atomic<int> refs_{0};

  std::vector<FileMetaData*> files_[kNumLevels];

  // Chosen when the version is installed, so that picking a compaction is a
  // constant-time decision rather than a scan of every level.
  FileMetaData* file_to_compact_ = nullptr;
  int file_to_compact_level_ = -1;
  double compaction_score_ = -1;
  int compaction_level_ = -1;
};

class VersionSet {
 public:
  VersionSet(std::string dbname, const Options& options, TableCache* cache,
             const Comparator* comparator);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // Applies `edit` to the current version and writes it to the manifest.
  //
  // `mutex` is held on entry and is released while the manifest is written,
  // because that write includes an fsync and holding a lock across an fsync
  // stalls every writer.  Only one thread may be inside this at a time, which
  // the caller guarantees by holding the write lock.
  Status log_and_apply(VersionEdit* edit, std::mutex* mutex);

  // Replays the manifest CURRENT names.  The first log_and_apply after this
  // writes a fresh manifest and points CURRENT at it: the old one is never
  // appended to (see the comment in the implementation for why), so the
  // caller always has an edit to apply.
  Status recover();

  Version* current() const { return current_; }

  uint64_t manifest_file_number() const { return manifest_file_number_; }

  // Bytes appended to manifests since this set was created: the snapshot
  // each open writes, every edit after it, and the first manifest of a new
  // database, which DBImpl::new_db writes and reports here.  Moved only
  // under the database mutex, after the write has finished, so that a
  // get_property under the mutex never reads a number in motion.
  uint64_t manifest_bytes_written() const { return manifest_bytes_; }
  void add_manifest_bytes(uint64_t bytes) { manifest_bytes_ += bytes; }
  uint64_t new_file_number() { return next_file_number_++; }
  void reuse_file_number(uint64_t number) {
    if (next_file_number_ == number + 1) next_file_number_ = number;
  }

  uint64_t last_sequence() const { return last_sequence_; }
  void set_last_sequence(uint64_t sequence) { last_sequence_ = sequence; }

  uint64_t log_number() const { return log_number_; }
  uint64_t prev_log_number() const { return prev_log_number_; }

  void mark_file_number_used(uint64_t number) {
    if (next_file_number_ <= number) next_file_number_ = number + 1;
  }

  int num_level_files(int level) const;
  int64_t num_level_bytes(int level) const;

  // Picks the compaction most worth doing, or null.
  Compaction* pick_compaction();

  // A compaction covering [begin, end) at `level`, for an explicit request.
  Compaction* compact_range(int level, const std::string* begin,
                            const std::string* end);

  Iterator* make_input_iterator(Compaction* compaction);

  bool needs_compaction() const {
    Version* v = current_;
    return v->compaction_score_ >= 1 || v->file_to_compact_ != nullptr;
  }

  // Every file referenced by any live version, so the rest can be deleted.
  void add_live_files(std::set<uint64_t>* live);

  std::string level_summary() const;

 private:
  friend class Compaction;
  friend class Version;

  class Builder;

  void finalize(Version* v);
  void append_version(Version* v);
  Status write_snapshot(LogWriter* log);
  void get_range(const std::vector<FileMetaData*>& inputs, std::string* smallest,
                 std::string* largest);
  void get_range2(const std::vector<FileMetaData*>& a,
                  const std::vector<FileMetaData*>& b, std::string* smallest,
                  std::string* largest);
  void setup_other_inputs(Compaction* compaction);

  const std::string dbname_;
  const Options options_;
  TableCache* const table_cache_;

  // Two comparators, and mixing them up is a real bug rather than a style
  // question.  `comparator_` orders internal keys, whose last eight bytes are a
  // sequence number; handing it a bare user key makes it treat the key's own
  // last eight bytes as that trailer, so "key_00000123" is compared as "key_"
  // and every range check quietly returns the wrong answer.  User keys go
  // through `user_comparator_`.
  const Comparator* const comparator_;
  const Comparator* const user_comparator_;

  uint64_t next_file_number_ = 2;
  uint64_t manifest_file_number_ = 0;
  uint64_t last_sequence_ = 0;
  uint64_t log_number_ = 0;
  uint64_t prev_log_number_ = 0;

  // The log writer owns the file, so there is one owner rather than two that
  // could disagree about when it closes.
  std::unique_ptr<LogWriter> descriptor_log_;
  uint64_t manifest_bytes_ = 0;

  Version dummy_versions_;  // head of the circular list
  Version* current_ = nullptr;

  // Where the next compaction of each level should begin, so successive
  // compactions sweep the key space instead of revisiting one busy range.
  std::string compact_pointer_[kNumLevels];
};

class Compaction {
 public:
  ~Compaction();

  int level() const { return level_; }
  VersionEdit* edit() { return &edit_; }

  int num_input_files(int which) const {
    return static_cast<int>(inputs_[which].size());
  }
  FileMetaData* input(int which, int i) const {
    return inputs_[which][static_cast<size_t>(i)];
  }

  uint64_t max_output_file_size() const { return max_output_file_size_; }

  // True when the compaction can be done by moving one file down a level
  // without reading it, because nothing below overlaps it.  The cheapest
  // possible compaction, and common enough to be worth detecting.
  bool is_trivial_move() const;

  void add_input_deletions(VersionEdit* edit);

  // True when `user_key` exists in no level deeper than the ones being
  // compacted, so a tombstone for it can be dropped rather than carried down.
  bool is_base_level_for_key(std::string_view user_key);

  // True when the output should be cut here to avoid creating a file that
  // overlaps too much of the grandparent level -- which would make the *next*
  // compaction enormous.
  bool should_stop_before(std::string_view internal_key);

  void release_inputs();

 private:
  friend class Version;
  friend class VersionSet;

  Compaction(const Options& options, int level);

  int level_;
  uint64_t max_output_file_size_;
  Version* input_version_ = nullptr;
  VersionEdit edit_;

  // inputs_[0] is the level being compacted, inputs_[1] the level below.
  std::vector<FileMetaData*> inputs_[2];

  // Files in level + 2 that overlap the output range, used by
  // should_stop_before.
  std::vector<FileMetaData*> grandparents_;
  size_t grandparent_index_ = 0;
  bool seen_key_ = false;
  int64_t overlapped_bytes_ = 0;

  // Where is_base_level_for_key has got to in each deeper level.  The keys it
  // is asked about arrive in order, so each level's search resumes rather than
  // restarting.
  size_t level_pointers_[kNumLevels] = {};
};

}  // namespace ambar

#endif  // AMBAR_VERSION_SET_HPP_
