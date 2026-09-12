// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Rebuilding a database from the files that survive.
//
// A manifest is a log of edits that, replayed, says which table sits at which
// level covering which keys.  Everything it says can also be found out the
// slow way: a table carries its own keys, so its range and its newest
// sequence number are a matter of reading it.  Repair reads every file,
// keeps what reads back, and writes what it kept out again -- merged, so
// that each user key appears once, at its newest version, in tables that
// do not overlap -- and then a manifest that names those.
//
// Merging rather than pointing the manifest at the surviving files is the
// part that matters.  The survivors overlap, and the engine resolves overlap
// among level-0 files by file number, which stands in for age only among
// files that were all flushed from memtables; a compaction output carries a
// newer number and older data than a flush that came before it.  Naming the
// survivors at level 0 would have a lookup answer with the overwritten value.
// The survivors can also hold the same entry twice -- a compaction output and
// the input it was made from, both still on disk -- which the engine's own
// merge is entitled to assume never happens.  A merge that tolerates both is
// a few dozen lines here and a precondition everywhere else.
//
// The merged tables go to the last level, where nothing lies beneath them:
// tombstones can be dropped outright, and the level is never scored for
// compaction, so the first open after a repair does not set off a rewrite of
// everything it just wrote.
//
// The order of operations is arranged so that nothing the old database owned
// is lost until the new one is known to read.  Files that could not be read
// are moved aside rather than deleted, so are the old manifests, the merged
// tables are read back before the manifest names them, and the files they
// replace are left for the next open's cleanup, which removes only what the
// new manifest does not name.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ambar/db.hpp"
#include "builder.hpp"
#include "comparator.hpp"
#include "dbformat.hpp"
#include "file.hpp"
#include "filename.hpp"
#include "internal_filter_policy.hpp"
#include "memtable.hpp"
#include "table_builder.hpp"
#include "table_cache.hpp"
#include "version_edit.hpp"
#include "version_set.hpp"
#include "wal.hpp"
#include "write_batch_internal.hpp"

namespace ambar {
namespace {

// How many tables one merge pass reads at once.  Each is an open file for
// the whole pass, and a default Linux process may hold 1024; a database of
// a few gigabytes has more tables than that, and is merged in rounds.
constexpr size_t kMergeFanIn = 256;

std::string base_name(const std::string& path) {
  return std::filesystem::path(path).filename().string();
}

// Moves a file into <name>/lost/, which must be a real directory -- a link
// planted there would send the file wherever the planter chose -- and never
// over a file already there, which an earlier repair may have set aside.
Status set_aside(const std::string& name, const std::string& path,
                 const std::string& why, RepairReport* report) {
  const std::string lost = name + "/lost";
  std::error_code ec;
  const auto existing = std::filesystem::symlink_status(lost, ec);
  if (!ec && std::filesystem::exists(existing) &&
      !std::filesystem::is_directory(existing)) {
    return Status::io_error("'" + lost + "' exists and is not a directory");
  }
  Status status = create_directory(lost);
  if (!status.is_ok()) return status;

  std::string target = lost + "/" + base_name(path);
  for (int n = 1; file_exists(target) && n < 1000; ++n) {
    target = lost + "/" + base_name(path) + "." + std::to_string(n);
  }
  if (file_exists(target)) {
    return Status::io_error("'" + target + "' already exists");
  }
  status = rename_file(path, target);
  if (!status.is_ok()) return status;
  report->notes.push_back(base_name(path) + ": " + why + "; moved to lost/" +
                          base_name(target));
  return Status::ok();
}

// Reads a table from beginning to end.  Bounds come from the first and last
// key rather than from the index, whose keys are shortened separators and
// not keys the file holds; the newest sequence number comes from every key;
// and the keys have to be in order, because everything after this assumes
// so and a table that passed its checksums can still have been written
// wrong.
Status read_table(TableCache* cache, const Comparator* comparator,
                  uint64_t number, uint64_t size, FileMetaData* meta,
                  uint64_t* newest, uint64_t* entries) {
  std::unique_ptr<Iterator> iter(
      cache->new_iterator(ReadOptions(), number, size));
  *entries = 0;
  std::string previous;
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    ParsedInternalKey parsed;
    if (!parse_internal_key(iter->key(), &parsed)) {
      return Status::corruption("holds a key that is not an internal key");
    }
    if (*entries == 0) {
      meta->smallest.assign(iter->key().data(), iter->key().size());
    } else if (comparator->compare(previous, iter->key()) >= 0) {
      return Status::corruption("holds keys out of order");
    }
    previous.assign(iter->key().data(), iter->key().size());
    meta->largest = previous;
    *newest = std::max(*newest, parsed.sequence);
    ++*entries;
  }
  Status status = iter->status();
  if (status.is_ok() && *entries == 0) {
    status = Status::corruption("holds no entries");
  }
  return status;
}

class Discard final : public WriteBatch::Handler {
 public:
  void put(std::string_view, std::string_view) override {}
  void del(std::string_view) override {}
};

struct LogOutcome {
  bool opened = false;      // the file could be opened at all
  bool early = false;       // reading ended before the end of the file
  uint64_t batches = 0;     // batches replayed
  std::string how_it_ended;
};

// Replays a log into tables the way recovery would, as far as it can be
// read.  A batch is checked before it is applied -- that it parses, that
// its sequence numbers follow the previous batch's and fit -- because
// applying one that turns out to be malformed leaves the part that parsed
// in the memtable with sequence numbers nobody accounts for, and a header
// that lies about its sequence would set last_sequence to a number every
// later write loses to.
Status replay_log(const std::string& name, const Options& options,
                  TableCache* cache, const Comparator* comparator,
                  const std::string& path, uint64_t* next_number,
                  std::vector<FileMetaData>* tables, uint64_t* newest,
                  LogOutcome* outcome) {
  *outcome = LogOutcome();

  std::unique_ptr<SequentialFile> source;
  const Status opened = SequentialFile::open(path, &source);
  if (!opened.is_ok()) {
    outcome->early = true;
    outcome->how_it_ended = opened.to_string();
    return Status::ok();  // the log's problem, not repair's
  }
  outcome->opened = true;

  LogReader reader(std::move(source));
  MemTable* mem = new MemTable();
  mem->ref();

  Status status;
  auto flush = [&]() {
    FileMetaData meta;
    meta.number = (*next_number)++;
    Status built;
    {
      std::unique_ptr<Iterator> iter(mem->new_iterator());
      built = build_table(name, options, cache, comparator, iter.get(), &meta);
    }
    if (built.is_ok() && meta.file_size > 0) tables->push_back(meta);
    cache->evict(meta.number);
    mem->unref();
    mem = new MemTable();
    mem->ref();
    return built;
  };

  uint64_t last_sequence_in_log = 0;
  bool batch_failed = false;
  std::string_view record;
  std::string scratch;
  Discard discard;
  while (reader.read_record(&record, &scratch)) {
    if (record.size() < WriteBatchInternal::kHeader) {
      outcome->how_it_ended = "a record too short to be a batch";
      batch_failed = true;
      break;
    }
    WriteBatch batch;
    Status parsed = WriteBatchInternal::set_contents(&batch, record);
    if (parsed.is_ok()) parsed = batch.iterate(&discard);
    if (parsed.is_ok()) {
      const uint64_t first = WriteBatchInternal::sequence(batch);
      const uint64_t count = WriteBatchInternal::count(batch);
      if (count > 0 && (first <= last_sequence_in_log ||
                        first > kMaxSequenceNumber - count + 1)) {
        parsed = Status::corruption(
            "batch sequence " + std::to_string(first) +
            " does not follow the previous batch or does not fit");
      }
    }
    if (!parsed.is_ok()) {
      outcome->how_it_ended = parsed.to_string();
      batch_failed = true;
      break;
    }
    status = WriteBatchInternal::insert_into(batch, mem);
    if (!status.is_ok()) break;  // cannot happen for a batch that iterated
    if (const uint64_t count = WriteBatchInternal::count(batch); count > 0) {
      last_sequence_in_log = WriteBatchInternal::sequence(batch) + count - 1;
      *newest = std::max(*newest, last_sequence_in_log);
    }
    ++outcome->batches;
    if (mem->approximate_memory_usage() > options.write_buffer_size) {
      status = flush();
      if (!status.is_ok()) break;
    }
  }
  if (status.is_ok()) status = flush();
  mem->unref();
  if (!status.is_ok()) return status;  // repair's own writes failed

  // What comes after the stop decides how it is described.  A stop the
  // reader made says so itself; a stop on a batch the reader read fine is
  // followed by whatever the reader would read next.
  bool more_follows = false;
  if (batch_failed) {
    outcome->early = true;
    std::string_view next;
    more_follows = reader.read_record(&next, &scratch) || reader.damaged();
  } else if (reader.truncated()) {
    outcome->early = true;
    outcome->how_it_ended = reader.failure_reason();
    more_follows = reader.damaged();
  }
  if (more_follows) {
    outcome->how_it_ended +=
        "; readable records follow the stop and were not replayed, so the "
        "copy in lost/ holds acknowledged writes";
  }
  if (!outcome->early) outcome->how_it_ended = "read to its end";
  return Status::ok();
}

// One pass of the k-way merge described at the top: every user key once, at
// its newest version, in order, into tables of at most max_file_size bytes.
// Equal internal keys -- the same entry held by two inputs -- collapse to
// one.  A newest version that is a deletion is dropped: the output is the
// whole database, so there is nothing beneath it for a tombstone to hide.
Status merge_pass(const std::string& name, const Options& options,
                  TableCache* cache, const Comparator* comparator,
                  const std::vector<FileMetaData>& inputs, bool drop_deletions,
                  uint64_t* next_number, std::vector<FileMetaData>* outputs,
                  uint64_t* newest, RepairReport* report) {
  std::vector<std::unique_ptr<Iterator>> children;
  children.reserve(inputs.size());
  for (const FileMetaData& input : inputs) {
    children.emplace_back(
        cache->new_iterator(ReadOptions(), input.number, input.file_size));
    children.back()->seek_to_first();
    if (!children.back()->valid() && !children.back()->status().is_ok()) {
      return children.back()->status();  // before anything is written
    }
  }

  std::unique_ptr<WritableFile> file;
  std::unique_ptr<TableBuilder> builder;
  FileMetaData meta;
  std::string last_emitted;  // internal key
  std::string last_user_key;
  std::string last_value;
  bool has_last = false;

  auto finish_output = [&]() {
    if (builder == nullptr) return Status::ok();
    Status status = builder->finish();
    if (status.is_ok()) {
      meta.file_size = builder->file_size();
      status = file->sync();
    }
    if (status.is_ok()) status = file->close();
    builder.reset();
    file.reset();
    if (status.is_ok()) {
      outputs->push_back(meta);
    } else {
      remove_file(table_file_name(name, meta.number));
    }
    return status;
  };
  // Undoes the pass: the half-written output and every finished one.  The
  // inputs were never touched, so nothing is lost, and a rerun does not have
  // to read the outputs of a pass that did not complete.
  auto abandon = [&](const Status& why) {
    if (builder != nullptr) {
      builder->abandon();
      builder.reset();
      file.reset();
      remove_file(table_file_name(name, meta.number));
    }
    for (const FileMetaData& output : *outputs) {
      remove_file(table_file_name(name, output.number));
    }
    outputs->clear();
    return why;
  };

  while (true) {
    Iterator* smallest = nullptr;
    for (auto& child : children) {
      if (!child->valid()) continue;
      if (smallest == nullptr ||
          comparator->compare(child->key(), smallest->key()) < 0) {
        smallest = child.get();
      }
    }
    if (smallest == nullptr) break;

    ParsedInternalKey parsed;
    if (!parse_internal_key(smallest->key(), &parsed)) {
      return abandon(
          Status::corruption("a key that read once would not read twice"));
    }
    *newest = std::max(*newest, parsed.sequence);

    // Internal keys order by user key, then by sequence descending, so the
    // first entry seen for a user key is its newest version and every later
    // one -- older versions, and the same entry from another input -- is not.
    if (has_last) {
      const int order = comparator->compare(last_emitted, smallest->key());
      if (order > 0) {
        return abandon(Status::corruption("inputs are not in order"));
      }
      if (order == 0 && smallest->value() != last_value) {
        report->notes.push_back(
            "two copies of the same entry disagree; kept the one from the "
            "lower-numbered file (user key " +
            std::string(parsed.user_key.substr(0, 40)) + ")");
      }
    }
    const bool newest_version = !has_last || parsed.user_key != last_user_key;
    if (newest_version) {
      last_user_key.assign(parsed.user_key.data(), parsed.user_key.size());
      has_last = true;
      const bool keep = !(drop_deletions && parsed.type == ValueType::kDeletion);
      if (keep) {
        if (builder == nullptr) {
          meta = FileMetaData();
          meta.number = (*next_number)++;
          const Status opened = WritableFile::open(
              table_file_name(name, meta.number), /*append=*/false, &file);
          if (!opened.is_ok()) return abandon(opened);
          builder = std::make_unique<TableBuilder>(options, file.get(),
                                                   comparator);
          meta.smallest.assign(smallest->key().data(), smallest->key().size());
        }
        builder->add(smallest->key(), smallest->value());
        meta.largest.assign(smallest->key().data(), smallest->key().size());
        if (builder->file_size() >= options.max_file_size) {
          const Status finished = finish_output();
          if (!finished.is_ok()) return abandon(finished);
        }
      }
    }
    last_emitted.assign(smallest->key().data(), smallest->key().size());
    last_value.assign(smallest->value().data(), smallest->value().size());
    smallest->next();
  }

  for (auto& child : children) {
    if (!child->status().is_ok()) return abandon(child->status());
  }
  const Status finished = finish_output();
  if (!finished.is_ok()) return abandon(finished);
  return Status::ok();
}

// The whole merge: rounds of at most kMergeFanIn inputs until one pass
// covers everything.  Newest-wins and equal-keys-collapse hold across
// rounds, so the result does not depend on how the inputs were grouped.
// Deletions are dropped only in the final pass, where the output is the
// whole database.
Status merge(const std::string& name, const Options& options,
             TableCache* cache, const Comparator* comparator,
             std::vector<FileMetaData> inputs, uint64_t* next_number,
             std::vector<FileMetaData>* outputs, uint64_t* newest,
             RepairReport* report) {
  while (inputs.size() > kMergeFanIn) {
    std::vector<FileMetaData> round;
    for (size_t at = 0; at < inputs.size(); at += kMergeFanIn) {
      const size_t end = std::min(at + kMergeFanIn, inputs.size());
      const std::vector<FileMetaData> group(
          inputs.begin() + static_cast<std::ptrdiff_t>(at),
          inputs.begin() + static_cast<std::ptrdiff_t>(end));
      std::vector<FileMetaData> merged;
      const Status status =
          merge_pass(name, options, cache, comparator, group,
                     /*drop_deletions=*/false, next_number, &merged, newest,
                     report);
      for (const FileMetaData& input : group) cache->evict(input.number);
      if (!status.is_ok()) return status;
      round.insert(round.end(), merged.begin(), merged.end());
    }
    report->notes.push_back("merged " + std::to_string(inputs.size()) +
                            " tables into " + std::to_string(round.size()) +
                            " in a round; more rounds follow");
    inputs = std::move(round);
  }
  const Status status =
      merge_pass(name, options, cache, comparator, inputs,
                 /*drop_deletions=*/true, next_number, outputs, newest, report);
  for (const FileMetaData& input : inputs) cache->evict(input.number);
  return status;
}

}  // namespace

Status repair_db(const std::string& name, const Options& options,
                 RepairReport* report) {
  *report = RepairReport();

  std::unique_ptr<FileLock> lock;
  Status status = FileLock::acquire(lock_file_name(name), &lock);
  if (!status.is_ok()) return status;

  // What is here.  Numbers are the only clue to age, so tables are taken in
  // number order, and everything repair writes is numbered past everything
  // it found -- of any kind, so nothing can collide.
  std::vector<std::pair<uint64_t, std::string>> tables;
  std::vector<std::pair<uint64_t, std::string>> logs;
  std::vector<std::string> manifests;
  uint64_t highest = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(name, ec)) {
    uint64_t number = 0;
    FileType type;
    if (!parse_file_name(base_name(entry.path().string()), &number, &type)) {
      continue;
    }
    highest = std::max(highest, number);
    const std::string path = entry.path().string();
    if (type == FileType::kTable) tables.emplace_back(number, path);
    if (type == FileType::kLog) logs.emplace_back(number, path);
    if (type == FileType::kDescriptor) manifests.push_back(path);
  }
  if (ec) {
    return Status::io_error("cannot list '" + name + "': " + ec.message());
  }
  if (tables.empty() && logs.empty()) {
    return Status::invalid_argument(
        "'" + name + "' holds no table or log files: nothing to repair");
  }
  if (highest > (uint64_t{1} << 62)) {
    return Status::corruption("'" + name + "' holds a file numbered " +
                              std::to_string(highest) +
                              ", which is not a number this engine hands out");
  }
  std::sort(tables.begin(), tables.end());
  std::sort(logs.begin(), logs.end());
  uint64_t next_number = highest + 1;

  // Tables are read and written the way the engine reads and writes them:
  // with the filter wrapped so that it sees user keys, which is the shape a
  // filter block in an existing table has.
  const Comparator* const comparator = internal_key_comparator();
  const std::unique_ptr<const FilterPolicy> filter =
      options.filter_policy != nullptr
          ? new_internal_filter_policy(options.filter_policy)
          : nullptr;
  Options local = options;
  local.filter_policy = filter.get();
  if (local.write_buffer_size < (64 << 10)) local.write_buffer_size = 64 << 10;
  if (local.block_size < 1024) local.block_size = 1024;
  if (local.max_file_size < (1 << 20)) local.max_file_size = 1 << 20;

  std::vector<FileMetaData> merged;
  uint64_t newest = 0;
  {
    TableCache cache(name, local, comparator, 1000);
    std::vector<FileMetaData> survivors;

    for (const auto& [number, path] : tables) {
      uint64_t size = 0;
      status = file_size(path, &size);
      FileMetaData meta;
      meta.number = number;
      meta.file_size = size;
      uint64_t entries = 0;
      if (status.is_ok()) {
        status = read_table(&cache, comparator, number, size, &meta, &newest,
                            &entries);
      }
      cache.evict(number);  // release the file before anything moves it
      if (status.is_ok()) {
        survivors.push_back(meta);
        ++report->tables_kept;
        report->notes.push_back(base_name(path) + ": " +
                                std::to_string(entries) + " entries");
      } else {
        status = set_aside(name, path, status.to_string(), report);
        if (!status.is_ok()) return status;
        ++report->tables_set_aside;
      }
    }

    for (const auto& log : logs) {
      const std::string& path = log.second;
      LogOutcome outcome;
      status = replay_log(name, local, &cache, comparator, path, &next_number,
                          &survivors, &newest, &outcome);
      if (!status.is_ok()) return status;  // repair's own writes failed
      if (outcome.opened) ++report->logs_converted;
      if (outcome.early) {
        status = set_aside(name, path,
                           std::to_string(outcome.batches) +
                               " batches replayed, then stopped early: " +
                               outcome.how_it_ended,
                           report);
        if (!status.is_ok()) return status;
        ++report->logs_set_aside;
      } else {
        report->notes.push_back(base_name(path) + ": " +
                                std::to_string(outcome.batches) +
                                " batches, read to its end");
      }
    }

    if (survivors.empty()) {
      return Status::corruption("'" + name +
                                "' holds no table or log that reads: "
                                "nothing survived to repair");
    }

    status = merge(name, local, &cache, comparator, survivors, &next_number,
                   &merged, &newest, report);
    if (!status.is_ok()) return status;

    // Read back before naming: the manifest must not point at a table that
    // does not read, and the files these replace are not touched until it
    // has.
    for (const FileMetaData& meta : merged) {
      FileMetaData again;
      uint64_t seen = 0;
      uint64_t entries = 0;
      status = read_table(&cache, comparator, meta.number, meta.file_size,
                          &again, &seen, &entries);
      cache.evict(meta.number);
      if (!status.is_ok()) {
        for (const FileMetaData& output : merged) {
          remove_file(table_file_name(name, output.number));
        }
        return Status::corruption(
            "a table repair just wrote does not read back (" +
            status.to_string() + "); the merged tables were removed and "
            "nothing else has been changed");
      }
    }
  }

  // The old manifests and CURRENT are the record of what went wrong.  They
  // are set aside, not left for cleanup to delete.
  for (const std::string& manifest : manifests) {
    status = set_aside(name, manifest, "superseded", report);
    if (!status.is_ok()) return status;
  }
  if (file_exists(current_file_name(name))) {
    status = set_aside(name, current_file_name(name), "superseded", report);
    if (!status.is_ok()) return status;
  }

  // The manifest: the merged tables at the last level, where files must not
  // overlap and do not; the newest sequence number seen anywhere; and a log
  // number past every log, so that none is replayed again -- they have
  // become tables.
  const uint64_t manifest_number = next_number++;
  const uint64_t log_number = next_number++;
  VersionEdit edit;
  edit.set_comparator_name(comparator->name);
  edit.set_log_number(log_number);
  edit.set_next_file(next_number);
  edit.set_last_sequence(newest);
  for (const FileMetaData& meta : merged) {
    edit.add_file(kNumLevels - 1, meta.number, meta.file_size, meta.smallest,
                  meta.largest);
  }
  report->last_sequence = newest;
  report->tables_written = static_cast<int>(merged.size());

  const std::string manifest = descriptor_file_name(name, manifest_number);
  {
    std::unique_ptr<WritableFile> file;
    status = WritableFile::open(manifest, /*append=*/false, &file);
    if (!status.is_ok()) return status;
    LogWriter log(std::move(file));
    std::string record;
    edit.encode_to(&record);
    status = log.add_record(record);
    if (status.is_ok()) status = log.sync();
    if (status.is_ok()) status = log.close();
  }
  if (!status.is_ok()) {
    remove_file(manifest);
    return status;
  }
  status = set_current_file(name, manifest_number);
  if (!status.is_ok()) return status;

  // Then open it, as the caller is about to.  This is what says the repair
  // worked, and its cleanup is what removes the files the merge replaced.
  lock.reset();
  Options check = options;
  check.create_if_missing = false;
  check.error_if_exists = false;
  std::unique_ptr<DB> db;
  status = DB::open(check, name, &db);
  if (!status.is_ok()) {
    return Status::corruption(
        "the repaired database does not open (" + status.to_string() +
        "); CURRENT now names " + base_name(manifest) + ", which lists the " +
        std::to_string(merged.size()) +
        " merged tables, and every file that was there before is still there "
        "or in lost/; running repair again reads the merged tables too");
  }
  report->opened = true;
  return Status::ok();
}

}  // namespace ambar
