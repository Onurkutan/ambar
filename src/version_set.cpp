// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "version_set.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>

#include "filename.hpp"
#include "merger.hpp"
#include "two_level_iterator.hpp"

namespace ambar {
namespace {

// The size at which a level is considered full.
//
// Ten megabytes at level 1, ten times that at each level below.  The first
// level is kept small on purpose: it is compacted often, and a small level 1
// makes each of those compactions cheap.
double max_bytes_for_level(int level) {
  double result = 10. * 1048576.0;
  while (level > 1) {
    result *= 10;
    --level;
  }
  return result;
}

uint64_t max_file_size_for_level(const Options& options, int) {
  return options.max_file_size;
}

int64_t total_file_size(const std::vector<FileMetaData*>& files) {
  int64_t sum = 0;
  for (const FileMetaData* file : files) {
    sum += static_cast<int64_t>(file->file_size);
  }
  return sum;
}

// Both take the *user* comparator: their arguments are user keys, and the
// internal comparator would strip eight bytes off each of them.
bool after_file(const Comparator* user_comparator, const std::string* user_key,
                const FileMetaData* file) {
  return user_key != nullptr &&
         user_comparator->compare(*user_key,
                                  extract_user_key(file->largest)) > 0;
}

bool before_file(const Comparator* user_comparator, const std::string* user_key,
                 const FileMetaData* file) {
  return user_key != nullptr &&
         user_comparator->compare(*user_key,
                                  extract_user_key(file->smallest)) < 0;
}

// Index of the first file whose largest key is at or after `key`.
size_t find_file(const Comparator* comparator,
                 const std::vector<FileMetaData*>& files,
                 std::string_view key) {
  size_t left = 0;
  size_t right = files.size();
  while (left < right) {
    const size_t mid = (left + right) / 2;
    if (comparator->compare(files[mid]->largest, key) < 0) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  return right;
}

bool some_file_overlaps_range(const Comparator* comparator,
                              const Comparator* user_comparator,
                              bool disjoint_sorted_files,
                              const std::vector<FileMetaData*>& files,
                              const std::string* smallest_user_key,
                              const std::string* largest_user_key) {
  if (!disjoint_sorted_files) {
    // Level 0: the files overlap each other, so there is nothing to binary
    // search and every file has to be checked.
    for (const FileMetaData* file : files) {
      if (!after_file(user_comparator, smallest_user_key, file) &&
          !before_file(user_comparator, largest_user_key, file)) {
        return true;
      }
    }
    return false;
  }

  size_t index = 0;
  if (smallest_user_key != nullptr) {
    const std::string probe =
        make_internal_key(*smallest_user_key, kMaxSequenceNumber,
                          kValueTypeForSeek);
    index = find_file(comparator, files, probe);
  }
  if (index >= files.size()) return false;
  return !before_file(user_comparator, largest_user_key, files[index]);
}

// An iterator over the file *list* of a level: key is each file's largest key,
// value is the (number, size) pair a two-level iterator turns into a table.
class LevelFileNumIterator final : public Iterator {
 public:
  LevelFileNumIterator(const Comparator* comparator,
                       const std::vector<FileMetaData*>* files)
      : comparator_(comparator), files_(files), index_(files->size()) {}

  bool valid() const override { return index_ < files_->size(); }
  void seek(std::string_view target) override {
    index_ = find_file(comparator_, *files_, target);
  }
  void seek_to_first() override { index_ = 0; }
  void seek_to_last() override {
    index_ = files_->empty() ? 0 : files_->size() - 1;
  }
  void next() override { ++index_; }
  void prev() override {
    index_ = (index_ == 0) ? files_->size() : index_ - 1;
  }

  std::string_view key() const override { return (*files_)[index_]->largest; }

  std::string_view value() const override {
    encode_fixed64(value_buf_, (*files_)[index_]->number);
    encode_fixed64(value_buf_ + 8, (*files_)[index_]->file_size);
    return std::string_view(value_buf_, sizeof(value_buf_));
  }

  Status status() const override { return Status::ok(); }

 private:
  const Comparator* const comparator_;
  const std::vector<FileMetaData*>* const files_;
  size_t index_;
  mutable char value_buf_[16] = {};
};

Iterator* get_file_iterator(void* arg, const ReadOptions& options,
                            std::string_view file_value) {
  auto* cache = reinterpret_cast<TableCache*>(arg);
  if (file_value.size() != 16) {
    return new_error_iterator(
        Status::corruption("file entry in a level index is the wrong size"));
  }
  return cache->new_iterator(options, decode_fixed64(file_value.data()),
                             decode_fixed64(file_value.data() + 8));
}

}  // namespace

// ------------------------------------------------------------- Version -----

Version::~Version() {
  assert(refs_ == 0);
  prev_->next_ = next_;
  next_->prev_ = prev_;
  for (auto& level : files_) {
    for (FileMetaData* file : level) {
      assert(file->refs > 0);
      if (--file->refs == 0) delete file;
    }
  }
}

void Version::ref() { ++refs_; }

void Version::unref() {
  assert(this != &vset_->dummy_versions_);
  assert(refs_ >= 1);
  if (--refs_ == 0) delete this;
}

void Version::add_iterators(const ReadOptions& options,
                            std::vector<Iterator*>* iters) {
  // Level 0's files overlap, so each contributes its own iterator and the
  // merge sorts between them.
  for (FileMetaData* file : files_[0]) {
    iters->push_back(vset_->table_cache_->new_iterator(options, file->number,
                                                       file->file_size));
  }
  // Deeper levels are disjoint and sorted, so one two-level iterator covers a
  // whole level and opens only the files a scan actually reaches.
  for (int level = 1; level < kNumLevels; ++level) {
    if (files_[level].empty()) continue;
    iters->push_back(new_two_level_iterator(
        new LevelFileNumIterator(vset_->comparator_, &files_[level]),
        &get_file_iterator, vset_->table_cache_, options));
  }
}

namespace {

// What a table lookup found, gathered by the callback.
struct Saver {
  enum class State { kNotFound, kFound, kDeleted, kCorrupt };
  State state = State::kNotFound;
  const Comparator* comparator = nullptr;
  std::string_view user_key;
  std::string* value = nullptr;
};

void save_value(void* arg, std::string_view key, std::string_view value) {
  auto* saver = reinterpret_cast<Saver*>(arg);
  ParsedInternalKey parsed;
  if (!parse_internal_key(key, &parsed)) {
    saver->state = Saver::State::kCorrupt;
    return;
  }
  // The table returns the first entry at or after the lookup key, which may
  // belong to the *next* user key when this one is absent from the file.
  if (parsed.user_key != saver->user_key) return;

  saver->state = (parsed.type == ValueType::kValue) ? Saver::State::kFound
                                                    : Saver::State::kDeleted;
  if (saver->state == Saver::State::kFound) {
    saver->value->assign(value.data(), value.size());
  }
}

}  // namespace

Status Version::get(const ReadOptions& options, std::string_view user_key,
                    SequenceNumber snapshot, std::string* value,
                    GetStats* stats) {
  stats->seek_file = nullptr;
  stats->seek_file_level = -1;

  const std::string lookup = make_lookup_key(user_key, snapshot);
  FileMetaData* last_file_read = nullptr;
  int last_file_read_level = -1;

  std::vector<FileMetaData*> candidates;
  for (int level = 0; level < kNumLevels; ++level) {
    candidates.clear();

    if (level == 0) {
      // Overlapping files, so collect every one whose range covers the key and
      // search them newest first -- the newest write wins, and stopping at the
      // first answer is only correct in that order.
      for (FileMetaData* file : files_[0]) {
        if (vset_->user_comparator_->compare(
                user_key, extract_user_key(file->smallest)) >= 0 &&
            vset_->user_comparator_->compare(
                user_key, extract_user_key(file->largest)) <= 0) {
          candidates.push_back(file);
        }
      }
      if (candidates.empty()) continue;
      std::sort(candidates.begin(), candidates.end(),
                [](const FileMetaData* a, const FileMetaData* b) {
                  return a->number > b->number;  // newest first
                });
    } else {
      if (files_[level].empty()) continue;
      const size_t index = find_file(vset_->comparator_, files_[level], lookup);
      if (index >= files_[level].size()) continue;
      FileMetaData* file = files_[level][index];
      // find_file only guarantees largest >= key; the key may still fall in
      // the gap before this file.
      if (vset_->user_comparator_->compare(
              user_key, extract_user_key(file->smallest)) < 0) {
        continue;
      }
      candidates.push_back(file);
    }

    for (FileMetaData* file : candidates) {
      // The first file searched without success is charged, not the last: it
      // is the shallowest one, and compacting it is what removes the wasted
      // read from every future lookup of this key.
      if (last_file_read != nullptr && stats->seek_file == nullptr) {
        stats->seek_file = last_file_read;
        stats->seek_file_level = last_file_read_level;
      }
      last_file_read = file;
      last_file_read_level = level;

      Saver saver;
      saver.comparator = vset_->comparator_;
      saver.user_key = user_key;
      saver.value = value;

      const Status status = vset_->table_cache_->get(
          options, file->number, file->file_size, lookup, &saver, save_value);
      if (!status.is_ok()) return status;

      switch (saver.state) {
        case Saver::State::kNotFound:
          break;  // keep looking, in this level and then deeper
        case Saver::State::kFound:
          return Status::ok();
        case Saver::State::kDeleted:
          // A tombstone is a definitive answer: it shadows every older version
          // in every deeper level, and continuing would resurrect one.
          return Status::not_found("deleted");
        case Saver::State::kCorrupt:
          return Status::corruption("table entry has a malformed internal key");
      }
    }
  }
  return Status::not_found("not present");
}

bool Version::update_stats(const GetStats& stats) {
  FileMetaData* file = stats.seek_file;
  if (file == nullptr) return false;
  if (--file->allowed_seeks <= 0 && file_to_compact_ == nullptr) {
    file_to_compact_ = file;
    file_to_compact_level_ = stats.seek_file_level;
    return true;
  }
  return false;
}

void Version::get_overlapping_inputs(int level, const std::string* begin,
                                     const std::string* end,
                                     std::vector<FileMetaData*>* inputs) {
  assert(level >= 0 && level < kNumLevels);
  inputs->clear();

  std::string user_begin;
  std::string user_end;
  if (begin != nullptr) user_begin = std::string(extract_user_key(*begin));
  if (end != nullptr) user_end = std::string(extract_user_key(*end));

  for (size_t i = 0; i < files_[level].size();) {
    FileMetaData* file = files_[level][i++];
    const std::string_view file_start = extract_user_key(file->smallest);
    const std::string_view file_limit = extract_user_key(file->largest);

    if (begin != nullptr &&
        vset_->user_comparator_->compare(file_limit, user_begin) < 0) {
      continue;
    }
    if (end != nullptr &&
        vset_->user_comparator_->compare(file_start, user_end) > 0) {
      continue;
    }

    inputs->push_back(file);
    if (level == 0) {
      // Level 0's files overlap, so pulling one in can widen the range, which
      // can pull in others.  Restarting is the simplest correct response; the
      // alternative is a fixed point computed by hand, and this loop runs at
      // most a few times because level 0 holds at most a dozen files.
      if (begin != nullptr &&
          vset_->user_comparator_->compare(file_start, user_begin) < 0) {
        user_begin = std::string(file_start);
        inputs->clear();
        i = 0;
      } else if (end != nullptr &&
                 vset_->user_comparator_->compare(file_limit, user_end) > 0) {
        user_end = std::string(file_limit);
        inputs->clear();
        i = 0;
      }
    }
  }
}

bool Version::overlaps_in_level(int level, const std::string* smallest_user_key,
                                const std::string* largest_user_key) {
  return some_file_overlaps_range(vset_->comparator_, vset_->user_comparator_,
                                  level > 0, files_[level], smallest_user_key,
                                  largest_user_key);
}

int Version::pick_level_for_memtable_output(std::string_view smallest_user_key,
                                            std::string_view largest_user_key) {
  int level = 0;
  const std::string smallest(smallest_user_key);
  const std::string largest(largest_user_key);
  if (overlaps_in_level(0, &smallest, &largest)) return 0;

  const std::string start =
      make_internal_key(smallest_user_key, kMaxSequenceNumber,
                        kValueTypeForSeek);
  const std::string limit = make_internal_key(largest_user_key, 0,
                                              static_cast<ValueType>(0));
  std::vector<FileMetaData*> overlaps;

  while (level < kMaxMemCompactLevel) {
    if (overlaps_in_level(level + 1, &smallest, &largest)) break;
    if (level + 2 < kNumLevels) {
      // Going deeper is only worth it if the *grandparent* level does not
      // overlap much: a file pushed to level 2 that spans a huge slice of
      // level 3 turns the next compaction into an enormous one.
      get_overlapping_inputs(level + 2, &start, &limit, &overlaps);
      if (total_file_size(overlaps) >
          10 * static_cast<int64_t>(max_file_size_for_level(vset_->options_, 0))) {
        break;
      }
    }
    ++level;
  }
  return level;
}

std::string Version::debug_string() const {
  std::string out;
  for (int level = 0; level < kNumLevels; ++level) {
    out += "--- level " + std::to_string(level) + " ---\n";
    for (const FileMetaData* file : files_[level]) {
      out += "  " + std::to_string(file->number) + ": " +
             std::to_string(file->file_size) + " bytes\n";
    }
  }
  return out;
}

// ---------------------------------------------------- VersionSet::Builder ---

// Applies a sequence of edits to a version to produce the next one.
//
// The awkward requirement is that the result must be *sorted* and must contain
// no file that a later edit deleted, even when the delete and the add appear in
// different edits.  Sorting after the fact would be enough for correctness and
// would cost an O(n log n) pass over every file at every install; accumulating
// into ordered sets instead makes the final pass a merge.
class VersionSet::Builder {
 public:
  Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
    base_->ref();
    BySmallestKey cmp;
    cmp.comparator = vset_->comparator_;
    for (auto& level : levels_) {
      level.added_files = new FileSet(cmp);
    }
  }

  ~Builder() {
    for (auto& level : levels_) {
      // Copied out before the set is destroyed, because unref may delete the
      // file and the set's comparator would then read freed memory.
      const std::vector<FileMetaData*> to_unref(level.added_files->begin(),
                                                level.added_files->end());
      delete level.added_files;
      for (FileMetaData* file : to_unref) {
        if (--file->refs == 0) delete file;
      }
    }
    base_->unref();
  }

  void apply(const VersionEdit* edit) {
    for (const auto& [level, key] : edit->compact_pointers()) {
      vset_->compact_pointer_[level] = key;
    }
    for (const auto& [level, number] : edit->deleted_files()) {
      levels_[level].deleted_files.insert(number);
    }
    for (const auto& [level, meta] : edit->new_files()) {
      auto* file = new FileMetaData(meta);
      file->refs = 1;

      // How many fruitless seeks this file may absorb before it is worth
      // compacting.  The unit is derived rather than picked: a compaction of
      // one byte costs roughly 25 bytes of IO once the level below is counted,
      // and a seek costs about a 16 KB read, so a seek is worth about 40 KB of
      // compaction.  Charging one seek per 16 KB of file is a conservative
      // tenth of that, and the floor keeps tiny files from being compacted on
      // their first miss.
      file->allowed_seeks = static_cast<int>(file->file_size / 16384);
      if (file->allowed_seeks < 100) file->allowed_seeks = 100;

      levels_[level].deleted_files.erase(file->number);
      levels_[level].added_files->insert(file);
    }
  }

  void save_to(Version* v) {
    BySmallestKey cmp;
    cmp.comparator = vset_->comparator_;

    for (int level = 0; level < kNumLevels; ++level) {
      // Merge the base level with the files this builder added, keeping the
      // result sorted by smallest key.
      const std::vector<FileMetaData*>& base_files = base_->files_[level];
      auto base_iter = base_files.begin();
      const auto base_end = base_files.end();
      const FileSet* added = levels_[level].added_files;
      v->files_[level].reserve(base_files.size() + added->size());

      for (FileMetaData* added_file : *added) {
        for (auto bpos = std::upper_bound(base_iter, base_end, added_file, cmp);
             base_iter != bpos; ++base_iter) {
          maybe_add_file(v, level, *base_iter);
        }
        maybe_add_file(v, level, added_file);
      }
      for (; base_iter != base_end; ++base_iter) {
        maybe_add_file(v, level, *base_iter);
      }

#ifndef NDEBUG
      // Files below level 0 must not overlap.  A violation makes lookups miss
      // keys rather than crash, so it is checked where it is cheap to check.
      if (level > 0) {
        for (size_t i = 1; i < v->files_[level].size(); ++i) {
          const std::string& previous = v->files_[level][i - 1]->largest;
          const std::string& current = v->files_[level][i]->smallest;
          assert(vset_->comparator_->compare(previous, current) < 0 &&
                 "files in a level below zero must have disjoint ranges");
        }
      }
#endif
    }
  }

 private:
  struct BySmallestKey {
    const Comparator* comparator;
    bool operator()(const FileMetaData* a, const FileMetaData* b) const {
      const int r = comparator->compare(a->smallest, b->smallest);
      if (r != 0) return r < 0;
      // Ties broken by file number so the order is total, which a std::set
      // requires and which also makes the newer file sort later.
      return a->number < b->number;
    }
  };

  using FileSet = std::set<FileMetaData*, BySmallestKey>;

  struct LevelState {
    std::set<uint64_t> deleted_files;
    FileSet* added_files = nullptr;
  };

  void maybe_add_file(Version* v, int level, FileMetaData* file) {
    if (levels_[level].deleted_files.count(file->number) > 0) return;
    ++file->refs;
    v->files_[level].push_back(file);
  }

  VersionSet* const vset_;
  Version* const base_;
  LevelState levels_[kNumLevels];
};

// ---------------------------------------------------------- VersionSet -----

VersionSet::VersionSet(std::string dbname, const Options& options,
                       TableCache* cache, const Comparator* comparator)
    : dbname_(std::move(dbname)),
      options_(options),
      table_cache_(cache),
      comparator_(comparator),
      user_comparator_(bytewise_comparator()),
      dummy_versions_(this) {
  append_version(new Version(this));
}

VersionSet::~VersionSet() {
  current_->unref();
  assert(dummy_versions_.next_ == &dummy_versions_);
}

void VersionSet::append_version(Version* v) {
  assert(v->refs_ == 0);
  assert(v != current_);
  if (current_ != nullptr) current_->unref();
  current_ = v;
  v->ref();

  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

Status VersionSet::log_and_apply(VersionEdit* edit, std::mutex* mutex) {
  if (edit->has_log_number()) {
    assert(edit->log_number() >= log_number_);
  } else {
    edit->set_log_number(log_number_);
  }
  if (!edit->has_prev_log_number()) {
    edit->set_prev_log_number(prev_log_number_);
  }
  edit->set_next_file(next_file_number_);
  edit->set_last_sequence(last_sequence_);

  auto* version = new Version(this);
  {
    Builder builder(this, current_);
    builder.apply(edit);
    builder.save_to(version);
  }
  finalize(version);

  // A brand new manifest, if this is the first edit since the database opened.
  // Writing a fresh one and pointing CURRENT at it is how the manifest is kept
  // from growing without bound.
  std::string new_manifest;
  Status status;
  if (descriptor_log_ == nullptr) {
    new_manifest = descriptor_file_name(dbname_, manifest_file_number_);
    std::unique_ptr<WritableFile> file;
    status = WritableFile::open(new_manifest, /*append=*/false, &file);
    if (status.is_ok()) {
      descriptor_log_ = std::make_unique<LogWriter>(std::move(file));
      status = write_snapshot(descriptor_log_.get());
    }
  }

  // The manifest write, including its fsync, happens with the lock released.
  // A version install is rare and an fsync is slow; holding the write lock
  // across it would stall every writer for milliseconds.  Only one thread can
  // be here at a time -- the caller guarantees it -- so the descriptor log
  // needs no lock of its own.
  {
    mutex->unlock();

    if (status.is_ok()) {
      std::string record;
      edit->encode_to(&record);
      status = descriptor_log_->add_record(record);
      if (status.is_ok()) status = descriptor_log_->sync();
    }

    // CURRENT is written only after the manifest is durable, and is itself
    // written by rename, so it either names the old manifest or the new one
    // and never a partial name.
    if (status.is_ok() && !new_manifest.empty()) {
      status = set_current_file(dbname_, manifest_file_number_);
    }

    mutex->lock();
  }

  if (status.is_ok()) {
    append_version(version);
    log_number_ = edit->log_number();
    prev_log_number_ = edit->prev_log_number();
  } else {
    delete version;
    if (!new_manifest.empty()) {
      // The half-written manifest is removed rather than left: CURRENT still
      // names the old one, so this file is unreferenced, and leaving it would
      // accumulate a manifest per failed install.
      descriptor_log_.reset();
      remove_file(new_manifest);
    }
  }
  return status;
}

void VersionSet::finalize(Version* v) {
  // Which level most needs compacting, decided once here so that the check on
  // every write is a comparison rather than a scan.
  int best_level = -1;
  double best_score = -1;

  for (int level = 0; level < kNumLevels - 1; ++level) {
    double score;
    if (level == 0) {
      // Level 0 is scored by file count, not bytes.  Its files overlap, so
      // every one of them is searched by every lookup that reaches level 0 --
      // the cost is per file, and bytes are beside the point.  Scoring by
      // bytes would also compact too often when write_buffer_size is small and
      // too rarely when it is large.
      score = static_cast<double>(v->files_[0].size()) / kL0CompactionTrigger;
    } else {
      score = static_cast<double>(total_file_size(v->files_[level])) /
              max_bytes_for_level(level);
    }
    if (score > best_score) {
      best_score = score;
      best_level = level;
    }
  }

  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}

Status VersionSet::write_snapshot(LogWriter* log) {
  // The manifest starts with the whole current state, so replaying it never
  // has to reach further back than the newest manifest.
  VersionEdit edit;
  edit.set_comparator_name(comparator_->name);

  for (int level = 0; level < kNumLevels; ++level) {
    if (!compact_pointer_[level].empty()) {
      edit.set_compact_pointer(level, compact_pointer_[level]);
    }
    for (const FileMetaData* file : current_->files_[level]) {
      edit.add_file(level, file->number, file->file_size, file->smallest,
                    file->largest);
    }
  }

  std::string record;
  edit.encode_to(&record);
  return log->add_record(record);
}

int VersionSet::num_level_files(int level) const {
  assert(level >= 0 && level < kNumLevels);
  return static_cast<int>(current_->files_[level].size());
}

int64_t VersionSet::num_level_bytes(int level) const {
  assert(level >= 0 && level < kNumLevels);
  return total_file_size(current_->files_[level]);
}

void VersionSet::add_live_files(std::set<uint64_t>* live) {
  for (Version* v = dummy_versions_.next_; v != &dummy_versions_; v = v->next_) {
    for (const auto& level : v->files_) {
      for (const FileMetaData* file : level) {
        live->insert(file->number);
      }
    }
  }
}

std::string VersionSet::level_summary() const {
  std::string out = "files[";
  for (int level = 0; level < kNumLevels; ++level) {
    if (level > 0) out += " ";
    out += std::to_string(current_->files_[level].size());
  }
  out += "]";
  return out;
}

void VersionSet::get_range(const std::vector<FileMetaData*>& inputs,
                           std::string* smallest, std::string* largest) {
  assert(!inputs.empty());
  smallest->clear();
  largest->clear();
  for (size_t i = 0; i < inputs.size(); ++i) {
    const FileMetaData* file = inputs[i];
    if (i == 0) {
      *smallest = file->smallest;
      *largest = file->largest;
    } else {
      if (comparator_->compare(file->smallest, *smallest) < 0) {
        *smallest = file->smallest;
      }
      if (comparator_->compare(file->largest, *largest) > 0) {
        *largest = file->largest;
      }
    }
  }
}

void VersionSet::get_range2(const std::vector<FileMetaData*>& a,
                            const std::vector<FileMetaData*>& b,
                            std::string* smallest, std::string* largest) {
  std::vector<FileMetaData*> all = a;
  all.insert(all.end(), b.begin(), b.end());
  get_range(all, smallest, largest);
}

// ------------------------------------------------------------- recovery ---

namespace {

// Reads CURRENT, which names the manifest to replay.
Status read_current(const std::string& dbname, std::string* manifest) {
  std::unique_ptr<SequentialFile> file;
  Status status = SequentialFile::open(current_file_name(dbname), &file);
  if (!status.is_ok()) return status;

  std::string scratch;
  std::string_view contents;
  status = file->read(4096, &contents, &scratch);
  if (!status.is_ok()) return status;

  if (contents.empty() || contents.back() != '\n') {
    return Status::corruption("CURRENT does not end in a newline");
  }
  const std::string_view name(contents.data(), contents.size() - 1);

  // CURRENT names a file, not a path.
  //
  // The name is joined to the database directory, so anything that escapes it
  // -- a separator, a parent reference -- makes the engine open a file
  // somewhere else on the machine and try to read it as a manifest.  Nothing
  // is disclosed by that, since the contents only produce a parse error, but
  // an engine that follows a path out of its own directory because a file in
  // that directory told it to is doing something it was never asked to do.
  //
  // Only the shape this engine writes is accepted, which also rejects a
  // directory that is not a database at all before anything is opened.
  uint64_t number = 0;
  FileType type;
  if (name.find('/') != std::string_view::npos ||
      name.find('\\') != std::string_view::npos ||
      !parse_file_name(name, &number, &type) ||
      type != FileType::kDescriptor) {
    return Status::corruption(
        "CURRENT does not name a manifest in this directory");
  }

  manifest->assign(name.data(), name.size());
  return Status::ok();
}

}  // namespace

Status VersionSet::recover(bool* save_manifest) {
  std::string manifest_base;
  Status status = read_current(dbname_, &manifest_base);
  if (!status.is_ok()) return status;

  const std::string manifest_path = dbname_ + "/" + manifest_base;
  std::unique_ptr<SequentialFile> file;
  status = SequentialFile::open(manifest_path, &file);
  if (!status.is_ok()) {
    return Status::corruption("CURRENT names a manifest that will not open: " +
                              manifest_base);
  }

  bool have_log_number = false;
  bool have_prev_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;
  uint64_t prev_log_number = 0;

  Builder builder(this, current_);
  int records = 0;

  {
    LogReader reader(std::move(file));
    std::string_view record;
    std::string scratch;
    while (reader.read_record(&record, &scratch) && status.is_ok()) {
      ++records;
      VersionEdit edit;
      status = edit.decode_from(record);
      if (!status.is_ok()) break;

      if (edit.has_comparator() && edit.comparator() != comparator_->name) {
        // Refusing rather than reading: a database written under a different
        // ordering is not corrupt, it is sorted differently, and reading it
        // with this comparator would return wrong answers from intact files.
        status = Status::invalid_argument(
            "database was written with comparator '" + edit.comparator() +
            "' but is being opened with '" + std::string(comparator_->name) +
            "'");
        break;
      }

      builder.apply(&edit);

      if (edit.has_log_number()) {
        log_number = edit.log_number();
        have_log_number = true;
      }
      if (edit.has_prev_log_number()) {
        prev_log_number = edit.prev_log_number();
        have_prev_log_number = true;
      }
      if (edit.has_next_file_number()) {
        next_file = edit.next_file_number();
        have_next_file = true;
      }
      if (edit.has_last_sequence()) {
        last_sequence = edit.last_sequence();
        have_last_sequence = true;
      }
    }

    if (status.is_ok() && reader.truncated()) {
      // A manifest whose tail was lost is not fatal.  Every record before the
      // damage is a complete edit, and the state they describe is a state the
      // database really was in; the edits after it never became durable, so
      // nothing referenced them.  Recovery stops there and carries on.
      //
      // What *is* fatal is a manifest with no usable records at all, which the
      // checks below catch.
    }
  }

  if (status.is_ok()) {
    if (!have_next_file) {
      status = Status::corruption("manifest holds no next-file-number record");
    } else if (!have_log_number) {
      status = Status::corruption("manifest holds no log-number record");
    } else if (!have_last_sequence) {
      status = Status::corruption("manifest holds no last-sequence record");
    }
    if (!have_prev_log_number) prev_log_number = 0;

    mark_file_number_used(prev_log_number);
    mark_file_number_used(log_number);
  }
  if (!status.is_ok()) return status;

  auto* version = new Version(this);
  builder.save_to(version);
  finalize(version);
  append_version(version);

  next_file_number_ = next_file + 1;
  last_sequence_ = last_sequence;
  log_number_ = log_number;
  prev_log_number_ = prev_log_number;

  // A manifest that is still small is appended to rather than replaced, so an
  // open does not rewrite the whole state every time.
  //
  // manifest_file_number_ is what remove_obsolete_files compares against when
  // deciding which manifests are dead, so it must name the file CURRENT points
  // at.  Setting it to the *next* file number here -- which is what a new
  // manifest would take -- makes the live manifest look obsolete, and the
  // first cleanup after open deletes the file the database needs to reopen.
  // The database survives until the process exits and then will not open at
  // all, with every table file intact.
  uint64_t reused_number = 0;
  FileType reused_type;
  const bool parsed =
      parse_file_name(manifest_base, &reused_number, &reused_type) &&
      reused_type == FileType::kDescriptor;

  uint64_t size = 0;
  if (parsed && file_size(manifest_path, &size).is_ok() &&
      size <= options_.max_file_size) {
    std::unique_ptr<WritableFile> appendable;
    if (WritableFile::open(manifest_path, /*append=*/true, &appendable).is_ok()) {
      descriptor_log_ = std::make_unique<LogWriter>(std::move(appendable), size);
      manifest_file_number_ = reused_number;
      mark_file_number_used(reused_number);
      *save_manifest = false;
      (void)records;
      return Status::ok();
    }
  }

  // Not reusing: a new manifest will be written, and it takes the number
  // reserved above.  The old one stays live until CURRENT has been repointed.
  manifest_file_number_ = next_file;
  *save_manifest = true;
  (void)records;
  return Status::ok();
}

// ------------------------------------------------------------ compaction ---

Compaction* VersionSet::pick_compaction() {
  // Two reasons to compact, and the size-based one wins when both apply.
  //
  // Too much data in a level is a structural problem that gets worse; too many
  // fruitless seeks on one file is a workload problem that costs reads now.
  // The first is unbounded and the second is not, so it goes first.
  const bool by_size = current_->compaction_score_ >= 1;
  const bool by_seeks = current_->file_to_compact_ != nullptr;

  int level;
  Compaction* compaction;

  if (by_size) {
    level = current_->compaction_level_;
    assert(level >= 0 && level + 1 < kNumLevels);
    compaction = new Compaction(options_, level);

    // Start where the last compaction of this level ended, so successive
    // compactions sweep across the key space instead of repeatedly rewriting
    // whichever range happens to sort first.
    for (FileMetaData* file : current_->files_[level]) {
      if (compact_pointer_[level].empty() ||
          comparator_->compare(file->largest, compact_pointer_[level]) > 0) {
        compaction->inputs_[0].push_back(file);
        break;
      }
    }
    if (compaction->inputs_[0].empty()) {
      compaction->inputs_[0].push_back(current_->files_[level][0]);  // wrapped
    }
  } else if (by_seeks) {
    level = current_->file_to_compact_level_;
    compaction = new Compaction(options_, level);
    compaction->inputs_[0].push_back(current_->file_to_compact_);
  } else {
    return nullptr;
  }

  compaction->input_version_ = current_;
  compaction->input_version_->ref();

  if (level == 0) {
    // Level 0's files overlap, so compacting one means compacting everything
    // that overlaps it -- otherwise an older version of a key could be left
    // above a newer one.
    std::string smallest;
    std::string largest;
    get_range(compaction->inputs_[0], &smallest, &largest);
    current_->get_overlapping_inputs(0, &smallest, &largest,
                                     &compaction->inputs_[0]);
    assert(!compaction->inputs_[0].empty());
  }

  setup_other_inputs(compaction);
  return compaction;
}

namespace {

// Adds the file just after `largest` in `level`, when that file's smallest key
// has the same user key.
//
// Without this, a compaction can end at a boundary that splits the versions of
// one user key across two files -- the newer version compacted down, the older
// left behind and now shadowing it.  The result is a read returning a value
// that was overwritten, from files that are individually intact.
void add_boundary_inputs(const Comparator* comparator,
                         const std::vector<FileMetaData*>& level_files,
                         std::vector<FileMetaData*>* compaction_files) {
  if (compaction_files->empty()) return;

  std::string largest;
  bool found = false;
  for (const FileMetaData* file : *compaction_files) {
    if (!found || comparator->compare(file->largest, largest) > 0) {
      largest = file->largest;
      found = true;
    }
  }

  bool continue_searching = true;
  while (continue_searching) {
    continue_searching = false;
    for (FileMetaData* file : level_files) {
      if (comparator->compare(file->smallest, largest) > 0 &&
          extract_user_key(file->smallest) == extract_user_key(largest)) {
        largest = file->largest;
        compaction_files->push_back(file);
        continue_searching = true;
        break;
      }
    }
  }
}

}  // namespace

void VersionSet::setup_other_inputs(Compaction* compaction) {
  const int level = compaction->level();

  add_boundary_inputs(comparator_, current_->files_[level],
                      &compaction->inputs_[0]);

  std::string smallest;
  std::string largest;
  get_range(compaction->inputs_[0], &smallest, &largest);

  current_->get_overlapping_inputs(level + 1, &smallest, &largest,
                                   &compaction->inputs_[1]);
  add_boundary_inputs(comparator_, current_->files_[level + 1],
                      &compaction->inputs_[1]);

  std::string all_start;
  std::string all_limit;
  get_range2(compaction->inputs_[0], compaction->inputs_[1], &all_start,
             &all_limit);

  // Having fixed the range, see whether more of the upper level can be pulled
  // in for free -- that is, without making the lower level's input any larger.
  // A wider upper input means fewer compactions later at no extra cost now.
  if (!compaction->inputs_[1].empty()) {
    std::vector<FileMetaData*> expanded0;
    current_->get_overlapping_inputs(level, &all_start, &all_limit, &expanded0);
    add_boundary_inputs(comparator_, current_->files_[level], &expanded0);

    const int64_t inputs0_size = total_file_size(compaction->inputs_[0]);
    const int64_t inputs1_size = total_file_size(compaction->inputs_[1]);
    const int64_t expanded0_size = total_file_size(expanded0);

    if (expanded0.size() > compaction->inputs_[0].size() &&
        inputs1_size + expanded0_size <
            50 * static_cast<int64_t>(options_.max_file_size)) {
      std::string new_start;
      std::string new_limit;
      get_range(expanded0, &new_start, &new_limit);

      std::vector<FileMetaData*> expanded1;
      current_->get_overlapping_inputs(level + 1, &new_start, &new_limit,
                                       &expanded1);
      add_boundary_inputs(comparator_, current_->files_[level + 1], &expanded1);

      if (expanded1.size() == compaction->inputs_[1].size()) {
        smallest = new_start;
        largest = new_limit;
        compaction->inputs_[0] = expanded0;
        compaction->inputs_[1] = expanded1;
        get_range2(compaction->inputs_[0], compaction->inputs_[1], &all_start,
                   &all_limit);
      }
      (void)inputs0_size;
    }
  }

  if (level + 2 < kNumLevels) {
    current_->get_overlapping_inputs(level + 2, &all_start, &all_limit,
                                     &compaction->grandparents_);
  }

  // Where the next compaction of this level should start.  Recorded on the
  // edit as well as here, so it survives a restart -- otherwise every open
  // would begin sweeping from the same place and the same range would be
  // rewritten repeatedly.
  compact_pointer_[level] = largest;
  compaction->edit_.set_compact_pointer(level, largest);
}

Compaction* VersionSet::compact_range(int level, const std::string* begin,
                                      const std::string* end) {
  std::vector<FileMetaData*> inputs;
  current_->get_overlapping_inputs(level, begin, end, &inputs);
  if (inputs.empty()) return nullptr;

  // Below level 0, a request covering a huge range is cut down to one
  // compaction's worth, so an explicit compact_range does not produce a single
  // enormous unit of work that blocks everything else.
  if (level > 0) {
    const uint64_t limit = max_file_size_for_level(options_, level);
    uint64_t total = 0;
    for (size_t i = 0; i < inputs.size(); ++i) {
      total += inputs[i]->file_size;
      if (total >= limit) {
        inputs.resize(i + 1);
        break;
      }
    }
  }

  auto* compaction = new Compaction(options_, level);
  compaction->input_version_ = current_;
  compaction->input_version_->ref();
  compaction->inputs_[0] = inputs;
  setup_other_inputs(compaction);
  return compaction;
}

Iterator* VersionSet::make_input_iterator(Compaction* compaction) {
  ReadOptions options;
  // A compaction reads every block exactly once and will never want them
  // again, so filling the cache with them would evict the working set of
  // everything else.
  options.fill_cache = false;

  const size_t space = (compaction->level() == 0)
                           ? compaction->inputs_[0].size() + 1
                           : 2;
  std::vector<Iterator*> list;
  list.reserve(space);

  for (int which = 0; which < 2; ++which) {
    if (compaction->inputs_[which].empty()) continue;
    if (compaction->level() + which == 0) {
      for (FileMetaData* file : compaction->inputs_[which]) {
        list.push_back(table_cache_->new_iterator(options, file->number,
                                                  file->file_size));
      }
    } else {
      list.push_back(new_two_level_iterator(
          new LevelFileNumIterator(comparator_, &compaction->inputs_[which]),
          &get_file_iterator, table_cache_, options));
    }
  }

  return new_merging_iterator(comparator_, list.data(),
                              static_cast<int>(list.size()));
}

// ------------------------------------------------------------ Compaction ---

Compaction::Compaction(const Options& options, int level)
    : level_(level),
      max_output_file_size_(max_file_size_for_level(options, level)) {}

Compaction::~Compaction() {
  if (input_version_ != nullptr) input_version_->unref();
}

bool Compaction::is_trivial_move() const {
  // One file, nothing below it overlaps, and not much in the grandparent
  // level: the file can be moved down by editing the version, without reading
  // or writing a byte.
  return num_input_files(0) == 1 && num_input_files(1) == 0 &&
         total_file_size(grandparents_) <=
             10 * static_cast<int64_t>(max_output_file_size_);
}

void Compaction::add_input_deletions(VersionEdit* edit) {
  for (int which = 0; which < 2; ++which) {
    for (const FileMetaData* file : inputs_[which]) {
      edit->remove_file(level_ + which, file->number);
    }
  }
}

bool Compaction::is_base_level_for_key(std::string_view user_key) {
  // User keys, so the user comparator.
  const Comparator* comparator = input_version_->vset_->user_comparator_;
  for (int level = level_ + 2; level < kNumLevels; ++level) {
    const std::vector<FileMetaData*>& files = input_version_->files_[level];
    // The keys arrive in order, so each level's scan resumes where it stopped
    // rather than restarting -- which turns an O(levels * files) check per key
    // into an amortised constant one.
    while (level_pointers_[level] < files.size()) {
      const FileMetaData* file = files[level_pointers_[level]];
      if (comparator->compare(user_key, extract_user_key(file->largest)) <= 0) {
        if (comparator->compare(user_key,
                                extract_user_key(file->smallest)) >= 0) {
          return false;  // the key exists deeper, so a tombstone must be kept
        }
        break;
      }
      ++level_pointers_[level];
    }
  }
  return true;
}

bool Compaction::should_stop_before(std::string_view internal_key) {
  const Comparator* comparator = input_version_->vset_->comparator_;

  while (grandparent_index_ < grandparents_.size() &&
         comparator->compare(internal_key,
                             grandparents_[grandparent_index_]->largest) > 0) {
    if (seen_key_) {
      overlapped_bytes_ +=
          static_cast<int64_t>(grandparents_[grandparent_index_]->file_size);
    }
    ++grandparent_index_;
  }
  seen_key_ = true;

  // Ten grandparent files' worth is the limit.  An output file that overlaps
  // more than that turns the *next* compaction of this range into one that
  // reads ten times as much as it writes, and the cost of cutting the file
  // early is one extra file.
  if (overlapped_bytes_ >
      10 * static_cast<int64_t>(max_output_file_size_)) {
    overlapped_bytes_ = 0;
    return true;
  }
  return false;
}

void Compaction::release_inputs() {
  if (input_version_ != nullptr) {
    input_version_->unref();
    input_version_ = nullptr;
  }
}

}  // namespace ambar
