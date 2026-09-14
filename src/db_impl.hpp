// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The engine, assembled.
//
// Concurrency, in one paragraph, because everything else here follows from it.
// One mutex protects the metadata: the memtable pointers, the version set, the
// list of writers.  It is *not* held while doing IO -- not for the log write,
// not for a table read, not for a compaction -- because those take
// milliseconds and the lock protects operations that take nanoseconds.  What
// makes that safe is that the objects handed out under the lock are either
// immutable (a Version, a Table) or reference counted (a MemTable), so a
// reader that took one keeps it alive without holding anything.
//
// Writes are serialised through a queue rather than a lock.  A writer joins
// the queue, and whoever is at the front writes not only its own batch but
// every batch queued behind it, in one log record with one fsync.  Ten threads
// committing concurrently therefore cost one fsync between them rather than
// ten, and each still learns whether its own write succeeded.

#ifndef AMBAR_DB_IMPL_HPP_
#define AMBAR_DB_IMPL_HPP_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "ambar/cache.hpp"
#include "ambar/db.hpp"
#include "comparator.hpp"
#include "internal_filter_policy.hpp"
#include "memtable.hpp"
#include "table_cache.hpp"
#include "version_set.hpp"
#include "wal.hpp"

namespace ambar {

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string dbname);
  ~DBImpl() override;

  Status put(const WriteOptions&, std::string_view key,
             std::string_view value) override;
  Status del(const WriteOptions&, std::string_view key) override;
  Status write(const WriteOptions& options, WriteBatch* updates) override;
  Status get(const ReadOptions& options, std::string_view key,
             std::string* value) override;
  Iterator* new_iterator(const ReadOptions& options) override;
  const Snapshot* get_snapshot() override;
  void release_snapshot(const Snapshot* snapshot) override;
  bool get_property(std::string_view property, std::string* value) override;
  void compact_range(const std::string_view* begin,
                     const std::string_view* end) override;

  // Called by DB::open.
  Status recover(VersionEdit* edit);

  // Waits for any in-flight compaction to finish.  Used by tests, and by
  // compact_range.
  void wait_for_background_work();

 private:
  friend class DB;

  struct Writer;
  struct CompactionState;

  Status new_db();
  Status recover_log_file(uint64_t log_number, VersionEdit* edit,
                          SequenceNumber* max_sequence);

  // Makes room for a write, flushing or waiting as needed.  Called with the
  // lock held; may release and reacquire it.
  Status make_room_for_write(bool force);

  // Collects the batches queued behind `first` into one, so a group of writers
  // shares a single log record and a single fsync.
  WriteBatch* build_batch_group(Writer** last_writer);

  void maybe_schedule_compaction();
  void background_loop();
  void background_compaction();

  Status compact_memtable();
  Status write_level0_table(MemTable* mem, VersionEdit* edit, Version* base);

  Status do_compaction_work(CompactionState* compact);
  Status open_compaction_output_file(CompactionState* compact);
  Status finish_compaction_output_file(CompactionState* compact,
                                       Iterator* input);
  Status install_compaction_results(CompactionState* compact);

  void remove_obsolete_files();
  void record_background_error(const Status& status);

  const std::string dbname_;
  const Comparator* const internal_comparator_;
  const std::unique_ptr<const FilterPolicy> internal_filter_policy_;

  // Declared before options_, because options_ is initialised from it and
  // member initialisation follows declaration order regardless of what the
  // constructor's list says.  Owned only when the caller did not supply a
  // cache; options_.block_cache points at it either way.
  const std::unique_ptr<Cache> owned_block_cache_;

  const Options options_;  // filter_policy and block_cache are ours

  // Held for the life of the database, and released only when it is destroyed.
  std::unique_ptr<FileLock> db_lock_;

  std::unique_ptr<TableCache> table_cache_;

  std::mutex mutex_;

  // Two signals, in opposite directions.  `background_work_available_` wakes
  // the worker when there is something to do; `background_work_finished_`
  // wakes the writers and closers waiting for it to stop.  One condition
  // variable for both would wake every waiter on every event.
  std::condition_variable background_work_available_;
  std::condition_variable background_work_finished_;

  std::atomic<bool> shutting_down_{false};

  MemTable* mem_ = nullptr;
  MemTable* imm_ = nullptr;             // being flushed
  std::atomic<bool> has_imm_{false};    // read without the lock by the writer

  std::unique_ptr<LogWriter> log_;
  uint64_t logfile_number_ = 0;

  std::deque<Writer*> writers_;
  WriteBatch tmp_batch_;

  // Snapshots, oldest first.  A compaction may not drop any version newer than
  // the oldest live snapshot.
  std::set<SequenceNumber> snapshots_;
  std::vector<std::unique_ptr<Snapshot>> snapshot_objects_;

  std::set<uint64_t> pending_outputs_;  // files a compaction is writing

  // What producing each level has cost since the database was opened: the
  // bytes its compactions read and wrote and the time they took, with a
  // flush charged to the level its table landed in.  Added to only with
  // mutex_ held, after the I/O has finished, because the I/O itself runs
  // without it.  These three totals, with the manifest's kept by
  // VersionSet, are what get_property("ambar.bytes-written") reports;
  // tests/test_stats.cpp checks them against the simulated disk's own
  // count of what was appended.  Error paths are not exact: a flush whose
  // table could not be completed is deleted uncounted, and an append that
  // failed partway leaves its first piece uncounted, while a compaction
  // output abandoned partway is counted.
  struct LevelStats {
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    int64_t micros = 0;
  };
  LevelStats level_stats_[kNumLevels];
  uint64_t log_bytes_ = 0;         // log records, headers and padding included
  uint64_t flush_bytes_ = 0;       // tables written from memtables
  uint64_t compaction_bytes_ = 0;  // tables written by compactions

  // One worker thread for the life of the database, rather than one per
  // compaction.  The first version created a thread inside
  // maybe_schedule_compaction and joined the previous one there -- which
  // deadlocked the moment a compaction scheduled the next one, because that
  // call runs on the very thread being joined.
  bool background_compaction_scheduled_ = false;
  bool background_thread_started_ = false;
  std::thread background_thread_;

  Status background_error_;

  // A manual compaction requested through compact_range.
  struct ManualCompaction {
    int level = 0;
    bool done = false;
    const std::string* begin = nullptr;
    const std::string* end = nullptr;
    std::string tmp_storage;
  };
  ManualCompaction* manual_compaction_ = nullptr;

  std::unique_ptr<VersionSet> versions_;
};

}  // namespace ambar

#endif  // AMBAR_DB_IMPL_HPP_
