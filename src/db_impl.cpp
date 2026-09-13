// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "db_impl.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "builder.hpp"
#include "db_iter.hpp"
#include "filename.hpp"
#include "merger.hpp"
#include "table_builder.hpp"
#include "write_batch_internal.hpp"

namespace ambar {

Snapshot::~Snapshot() = default;
DB::~DB() = default;

namespace {

// A sequence number, wearing the opaque type the public API hands out.
struct SnapshotImpl final : public Snapshot {
  explicit SnapshotImpl(SequenceNumber s) : sequence(s) {}
  SequenceNumber sequence;
};

Options sanitize_options(const std::string& dbname, const Options& source,
                         const FilterPolicy* internal_policy,
                         Cache* fallback_block_cache) {
  Options result = source;
  result.filter_policy = internal_policy;
  if (result.block_cache == nullptr) result.block_cache = fallback_block_cache;

  // Clamped rather than trusted.  A caller who passes zero for the write
  // buffer gets a database that flushes on every key and appears to have
  // stopped working; a caller who passes four gigabytes gets one that loses
  // four gigabytes of work to a crash.  Both are more likely to be a mistake
  // than an intention.
  auto clamp = [](size_t value, size_t low, size_t high) {
    return value < low ? low : (value > high ? high : value);
  };
  result.write_buffer_size =
      clamp(result.write_buffer_size, 64 << 10, 1u << 30);
  result.max_file_size = clamp(result.max_file_size, 1 << 20, 1u << 30);
  result.block_size = clamp(result.block_size, 1 << 10, 4 << 20);
  if (result.max_open_files < 20) result.max_open_files = 20;
  (void)dbname;
  return result;
}

// How many table and log files a directory holds: the files that carry data,
// as opposed to the manifest, lock and info log that only describe it.  A
// directory that cannot be listed is an error, not an empty one: the caller
// is deciding whether to create a database over what is there.
Status count_data_files(const std::string& dbname, int* count) {
  *count = 0;
  std::vector<std::string> names;
  Status status = list_directory(dbname, &names);
  if (!status.is_ok()) return status;
  for (const std::string& name : names) {
    uint64_t number = 0;
    FileType type;
    if (!parse_file_name(name, &number, &type)) continue;
    if (type == FileType::kTable || type == FileType::kLog) ++*count;
  }
  return Status::ok();
}

}  // namespace

// One caller's pending write, and the machinery for handing the work to
// whichever writer reaches the front of the queue.
struct DBImpl::Writer {
  explicit Writer(std::mutex* m) : mutex(m) {}

  Status status;
  WriteBatch* batch = nullptr;
  bool sync = false;
  bool done = false;
  std::condition_variable condition;
  std::mutex* mutex;
};

struct DBImpl::CompactionState {
  struct Output {
    uint64_t number = 0;
    uint64_t file_size = 0;
    std::string smallest;
    std::string largest;
  };

  explicit CompactionState(Compaction* c) : compaction(c) {}

  Output* current_output() { return &outputs.back(); }

  Compaction* const compaction;

  // Versions newer than this may not be dropped, because a live snapshot can
  // still see them.  Without it, a compaction would collapse a key to its
  // newest version and a snapshot older than that would start returning the
  // wrong value -- silently, and only for readers that had been open a while.
  SequenceNumber smallest_snapshot = 0;

  std::vector<Output> outputs;
  std::unique_ptr<WritableFile> outfile;
  std::unique_ptr<TableBuilder> builder;
  uint64_t total_bytes = 0;
};

DBImpl::DBImpl(const Options& options, std::string dbname)
    : dbname_(std::move(dbname)),
      internal_comparator_(internal_key_comparator()),
      internal_filter_policy_(
          options.filter_policy != nullptr
              ? new_internal_filter_policy(options.filter_policy)
              : nullptr),
      owned_block_cache_(options.block_cache == nullptr
                             ? new_lru_cache(8 << 20)
                             : nullptr),
      options_(sanitize_options(dbname_, options,
                                internal_filter_policy_.get(),
                                owned_block_cache_.get())),
      table_cache_(std::make_unique<TableCache>(
          dbname_, options_, internal_comparator_,
          options_.max_open_files - 10)),
      versions_(std::make_unique<VersionSet>(dbname_, options_,
                                             table_cache_.get(),
                                             internal_comparator_)) {}

DBImpl::~DBImpl() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    shutting_down_.store(true, std::memory_order_release);
    // Waking the worker so it can notice and leave, then waiting for it to
    // finish whatever it was doing.  Destroying the database while a
    // compaction is writing a file would leave a half-written table and a
    // manifest that may or may not mention it.
    background_work_available_.notify_all();
    while (background_compaction_scheduled_) {
      background_work_finished_.wait(lock);
    }
  }
  if (background_thread_.joinable()) {
    background_thread_.join();
  }

  if (mem_ != nullptr) mem_->unref();
  if (imm_ != nullptr) imm_->unref();
}

// ------------------------------------------------------------- open --------

Status DBImpl::new_db() {
  VersionEdit edit;
  edit.set_comparator_name(internal_comparator_->name);
  edit.set_log_number(0);
  edit.set_next_file(2);
  edit.set_last_sequence(0);

  const std::string manifest = descriptor_file_name(dbname_, 1);
  std::unique_ptr<WritableFile> file;
  Status status = WritableFile::open(manifest, /*append=*/false, &file);
  if (!status.is_ok()) return status;

  {
    LogWriter log(std::move(file));
    std::string record;
    edit.encode_to(&record);
    status = log.add_record(record);
    if (status.is_ok()) status = log.sync();
    if (status.is_ok()) status = log.close();
  }

  if (status.is_ok()) {
    // CURRENT last, so a crash before this point leaves a directory with a
    // manifest nothing points at -- which reads as "no database here" rather
    // than as a broken one.
    status = set_current_file(dbname_, 1);
  } else {
    remove_file(manifest);
  }
  return status;
}

Status DBImpl::recover(VersionEdit* edit) {
  Status status = create_directory(dbname_);
  if (!status.is_ok()) return status;

  // Taken before anything is read or written.  A second process that got as
  // far as replaying the log would already have handed out file numbers the
  // first one is using, and neither would ever find out.
  status = FileLock::acquire(lock_file_name(dbname_), &db_lock_);
  if (!status.is_ok()) return status;

  if (!file_exists(current_file_name(dbname_))) {
    if (!options_.create_if_missing) {
      return Status::invalid_argument(
          "database does not exist in '" + dbname_ +
          "' and create_if_missing is false");
    }
    // No CURRENT, but tables or logs: not an empty directory but a database
    // that has lost the one file naming its manifest -- a crash on a
    // filesystem that did not make the rename durable, a copy that missed a
    // file.  Creating a fresh database here would be the last thing that
    // ever happened to the data, because the cleanup that follows a
    // successful open deletes every table the new manifest does not name,
    // which is all of them.  So it is refused.
    //
    // A manifest with nothing pointing at it and no data beside it is a
    // different shape: what new_db leaves when it dies before writing
    // CURRENT.  That directory is still empty in every way that matters, and
    // is still created over.
    int data_files = 0;
    status = count_data_files(dbname_, &data_files);
    if (!status.is_ok()) return status;
    if (data_files > 0) {
      return Status::corruption(
          "'" + dbname_ + "' has no CURRENT file but holds " +
          std::to_string(data_files) +
          " table or log file(s): this is a database whose CURRENT was lost, "
          "not an empty directory, and creating a new database over it would "
          "delete them.  To start over, remove the directory.");
    }
    status = new_db();
    if (!status.is_ok()) return status;
  } else if (options_.error_if_exists) {
    return Status::invalid_argument("database already exists in '" + dbname_ +
                                    "' and error_if_exists is true");
  }

  status = versions_->recover();
  if (!status.is_ok()) return status;

  // Every log file newer than the one the manifest names is replayed.  There
  // can be more than one: a flush creates a new log before the old one's
  // memtable has been written out, and a crash in between leaves both.
  const uint64_t min_log = versions_->log_number();
  const uint64_t prev_log = versions_->prev_log_number();

  std::set<uint64_t> expected;
  versions_->add_live_files(&expected);

  std::vector<uint64_t> logs;
  std::vector<std::string> names;
  status = list_directory(dbname_, &names);
  if (!status.is_ok()) return status;
  for (const std::string& name : names) {
    uint64_t number = 0;
    FileType type;
    if (!parse_file_name(name, &number, &type)) continue;
    expected.erase(number);
    if (type == FileType::kLog && (number >= min_log || number == prev_log)) {
      logs.push_back(number);
    }
  }

  if (!expected.empty()) {
    // The manifest references a table that is not on disk.  Continuing would
    // mean serving reads from a database that is missing part of itself, and
    // reporting keys absent that are not.
    return Status::corruption(
        "the manifest references " + std::to_string(expected.size()) +
        " missing table file(s), the first being " +
        std::to_string(*expected.begin()) + ".sst");
  }

  std::sort(logs.begin(), logs.end());
  SequenceNumber max_sequence = 0;
  for (size_t i = 0; i < logs.size(); ++i) {
    status = recover_log_file(logs[i], edit, &max_sequence);
    if (!status.is_ok()) return status;
    versions_->mark_file_number_used(logs[i]);
  }

  if (versions_->last_sequence() < max_sequence) {
    versions_->set_last_sequence(max_sequence);
  }
  return Status::ok();
}

Status DBImpl::recover_log_file(uint64_t log_number, VersionEdit* edit,
                                SequenceNumber* max_sequence) {
  const std::string path = log_file_name(dbname_, log_number);
  std::unique_ptr<SequentialFile> file;
  Status status = SequentialFile::open(path, &file);
  if (!status.is_ok()) {
    // A log named by the manifest that will not open is a real loss, but the
    // engine cannot tell it apart from one that was deleted after its contents
    // were flushed.  Reporting it is the honest response.
    return status;
  }

  LogReader reader(std::move(file));
  MemTable* mem = nullptr;

  std::string_view record;
  std::string scratch;
  while (reader.read_record(&record, &scratch) && status.is_ok()) {
    if (record.size() < 12) {
      status = Status::corruption("log record is too short to be a batch");
      break;
    }

    WriteBatch batch;
    status = WriteBatchInternal::set_contents(&batch, record);
    if (!status.is_ok()) break;

    if (mem == nullptr) {
      mem = new MemTable();
      mem->ref();
    }
    status = WriteBatchInternal::insert_into(batch, mem);
    if (!status.is_ok()) break;

    // count == 0 is a legal, empty batch -- header only, no records -- that a
    // hand-written or damaged log can contain.  count(batch) - 1 would wrap a
    // uint32_t to 0xffffffff and corrupt max_sequence with it; skipping the
    // update is correct, since an empty batch has no sequence number to
    // account for.
    if (const uint32_t count = WriteBatchInternal::count(batch); count > 0) {
      const SequenceNumber last = WriteBatchInternal::sequence(batch) + count - 1;
      if (last > *max_sequence) *max_sequence = last;
    }

    if (mem->approximate_memory_usage() > options_.write_buffer_size) {
      // The log held more than one memtable's worth, so it becomes more than
      // one table file.  Writing them out as we go keeps recovery's memory
      // bounded by write_buffer_size rather than by the size of the log.
      status = write_level0_table(mem, edit, nullptr);
      mem->unref();
      mem = nullptr;
      if (!status.is_ok()) break;
    }
  }

  if (status.is_ok() && reader.damaged()) {
    // Intact records follow the damage: this is a hole in the middle of the
    // log, not the torn tail a crash leaves, and the batches after it were
    // acknowledged -- with sync, promised durable.  Replaying up to the hole
    // and calling that recovery would drop them without a word.
    status = Status::corruption(
        "log " + std::to_string(log_number) + " stops before its end (" +
        reader.failure_reason() +
        ") and not at a torn tail: records or unread bytes follow, and "
        "replaying up to the stop would silently drop the writes they hold");
  }

  if (status.is_ok() && reader.truncated()) {
    // Expected, not exceptional: the tail of the last log is whatever the
    // process was in the middle of writing when it died.  Everything before it
    // is intact and has been replayed; the incomplete record was never
    // acknowledged to anyone.
  }

  if (status.is_ok() && mem != nullptr) {
    // Every recovered log becomes a table file, including the newest one.
    //
    // Keeping the last log's memtable live and appending to that log would
    // save one flush per open.  It also means the recovered memtable has no
    // log writer attached until one is built for it, and the version edit that
    // records the open then carries a log number of zero -- which is not
    // merely wrong, it is *below* the log number already in the manifest, so a
    // later recovery would replay logs that have already been folded in.
    // Flushing is one write at open and no special case anywhere else.
    status = write_level0_table(mem, edit, nullptr);
  }

  if (mem != nullptr) mem->unref();
  return status;
}

Status DB::open(const Options& options, const std::string& name,
                std::unique_ptr<DB>* db) {
  db->reset();

  auto impl = std::make_unique<DBImpl>(options, name);
  VersionEdit edit;

  std::unique_lock<std::mutex> lock(impl->mutex_);
  Status status = impl->recover(&edit);

  if (status.is_ok() && impl->mem_ == nullptr) {
    const uint64_t new_log_number = impl->versions_->new_file_number();
    std::unique_ptr<WritableFile> file;
    status = WritableFile::open(log_file_name(name, new_log_number),
                                /*append=*/false, &file);
    // The log's name reaches the disk with the directory sync that installs
    // the fresh manifest below, before any write can be acknowledged into
    // it; a rotation, which installs nothing, syncs the directory itself.
    if (status.is_ok()) {
      edit.set_log_number(new_log_number);
      impl->logfile_number_ = new_log_number;
      impl->log_ = std::make_unique<LogWriter>(std::move(file));
      impl->mem_ = new MemTable();
      impl->mem_->ref();
    }
  }

  // The edit that records the open: the tables recovery wrote from the logs
  // and the new log's number.  It is always written, into a fresh manifest
  // (see VersionSet::recover).  When the manifest used to be reused, it was
  // once skipped if nothing had been replayed, which left the log number at
  // its old value: every previous log was kept forever and re-read at every
  // open -- one leaked file per open, and a replay that grew without bound.
  // Recovery stayed correct throughout, because a stale-low log number
  // over-replays rather than under-replays, which is why nothing else
  // noticed.
  if (status.is_ok()) {
    edit.set_prev_log_number(0);
    edit.set_log_number(impl->logfile_number_);
    status = impl->versions_->log_and_apply(&edit, &impl->mutex_);
  }

  if (status.is_ok()) {
    impl->remove_obsolete_files();
    impl->maybe_schedule_compaction();
  }

  lock.unlock();
  if (status.is_ok()) {
    *db = std::move(impl);
  }
  return status;
}

Status destroy_db(const std::string& name, const Options&) {
  if (!file_exists(name)) return Status::ok();

  // The lock is taken first, and held for the whole operation.
  //
  // Deleting a database that another process has open is the one thing the
  // lock exists to prevent, and it would be absurd for the destroy path to be
  // the hole in it: the other process would carry on writing into files whose
  // directory entries had been removed, and lose everything at its next open.
  std::unique_ptr<FileLock> lock;
  Status status = FileLock::acquire(lock_file_name(name), &lock);
  if (!status.is_ok()) return status;

  std::vector<std::string> names;
  Status result = list_directory(name, &names);
  for (const std::string& base : names) {
    uint64_t number = 0;
    FileType type;
    // Only files this engine recognises are removed.  The directory may not be
    // exclusively ours, and deleting something unrecognised on the strength of
    // a guess is not recoverable.
    if (!parse_file_name(base, &number, &type)) continue;
    if (type == FileType::kLock) continue;

    const Status removed = remove_file(name + "/" + base);
    if (result.is_ok() && !removed.is_ok()) result = removed;
  }

  // The lock file goes last, after the lock itself is released, so that no
  // other process can create and lock a fresh one while this one is still
  // deleting.
  lock.reset();
  remove_file(lock_file_name(name));

  remove_directory(name);  // succeeds only if it is now empty
  return result;
}

// ------------------------------------------------------------- writes ------

Status DBImpl::put(const WriteOptions& options, std::string_view key,
                   std::string_view value) {
  WriteBatch batch;
  batch.put(key, value);
  return write(options, &batch);
}

Status DBImpl::del(const WriteOptions& options, std::string_view key) {
  WriteBatch batch;
  batch.del(key);
  return write(options, &batch);
}

Status DBImpl::write(const WriteOptions& options, WriteBatch* updates) {
  Writer writer(&mutex_);
  writer.batch = updates;
  writer.sync = options.sync;

  std::unique_lock<std::mutex> lock(mutex_);
  writers_.push_back(&writer);
  while (!writer.done && &writer != writers_.front()) {
    writer.condition.wait(lock);
  }
  if (writer.done) {
    // Someone else's group included this batch and reported its result.
    return writer.status;
  }

  Status status = make_room_for_write(updates == nullptr);
  uint64_t last_sequence = versions_->last_sequence();
  Writer* last_writer = &writer;

  if (status.is_ok() && updates != nullptr) {
    WriteBatch* batch = build_batch_group(&last_writer);
    WriteBatchInternal::set_sequence(batch, last_sequence + 1);
    last_sequence += WriteBatchInternal::count(*batch);

    {
      // The log write and the memtable insert happen with the lock released.
      //
      // That is safe for a reason worth stating: this thread is at the front
      // of the writer queue, so no other thread can be writing, and the queue
      // is what serialises writes rather than the mutex.  Readers may run
      // concurrently -- they see either the entry or not, never a half-written
      // one, because the skip list publishes each node with a release store.
      lock.unlock();

      status = log_->add_record(WriteBatchInternal::contents(*batch));
      if (status.is_ok()) {
        // Every write reaches the kernel here; only a sync write waits for the
        // device.  Both halves of that matter.
        //
        // Without the flush, an unsynced record sits in this process's own
        // stdio buffer while the memtable insert below makes it readable.  A
        // reader can then be handed a value that a SIGKILL destroys, because
        // the buffer dies with the process -- which contradicts the contract
        // in db.hpp, where an unsynced write survives the process dying and
        // only a power cut may lose it.
        //
        // This was not caught by reasoning about the code.  It was caught by
        // tools/crash_test running a reader alongside the writer: the reader
        // journalled four keys it had been given, the kill landed, and they
        // were gone on reopen.
        status = options.sync ? log_->sync() : log_->flush();
      }
      if (status.is_ok()) {
        status = WriteBatchInternal::insert_into(*batch, mem_);
      }

      lock.lock();
    }

    if (batch == &tmp_batch_) tmp_batch_.clear();

    // This line is the publication point, and it is worth being precise about
    // that, because the obvious belief is that the memtable insert above is.
    //
    // It is not.  A reader takes its snapshot from last_sequence, and every
    // entry this batch put in the memtable carries a sequence above the old
    // value -- so until this store, those entries exist and are invisible.
    // Which means the order of the log write and the memtable insert cannot be
    // observed by anything: swapping them changes when a key is *in memory*,
    // not when it can be read.
    //
    // Confirmed by mutation rather than by argument.  Writing the memtable
    // first, then the log, survives the crash test with a reader hammering the
    // in-flight batch -- correctly, because it is an equivalent change.  The
    // ordering that does matter is that both finish before this store, and
    // that the log record has reached the kernel before it (see the flush
    // above); mutating either of those turns the crash test red.
    versions_->set_last_sequence(last_sequence);

    if (!status.is_ok()) {
      // A failed log write leaves the memtable and the log disagreeing, and
      // there is no way back: the memtable may hold entries the log does not,
      // so continuing would serve reads that a restart would lose.  Every
      // later write fails, and a reopen replays the log, which is consistent.
      record_background_error(status);
    }
  }

  // Report to everyone whose batch was included.
  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &writer) {
      ready->status = status;
      ready->done = true;
      ready->condition.notify_one();
    }
    if (ready == last_writer) break;
  }
  if (!writers_.empty()) {
    writers_.front()->condition.notify_one();
  }
  return status;
}

WriteBatch* DBImpl::build_batch_group(Writer** last_writer) {
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;

  size_t size = WriteBatchInternal::byte_size(*first->batch);

  // A small write should not be delayed behind a large group it happens to
  // join, so the group is capped -- and capped lower when the leading batch is
  // itself small, since that writer is the one whose latency would suffer.
  size_t max_size = 1 << 20;
  if (size <= (128 << 10)) {
    max_size = size + (128 << 10);
  }

  *last_writer = first;
  for (auto it = std::next(writers_.begin()); it != writers_.end(); ++it) {
    Writer* w = *it;
    if (w->sync && !first->sync) {
      // A writer that asked for sync must not be answered by a group that did
      // not sync.  Including it would report success for a write that is only
      // in the page cache.
      break;
    }
    if (w->batch == nullptr) break;

    size += WriteBatchInternal::byte_size(*w->batch);
    if (size > max_size) break;

    if (result == first->batch) {
      // Copy on first append, so a group of one never pays for a copy.
      result = &tmp_batch_;
      WriteBatchInternal::append(result, *first->batch);
    }
    WriteBatchInternal::append(result, *w->batch);
    *last_writer = w;
  }
  return result;
}

Status DBImpl::make_room_for_write(bool force) {
  bool allow_delay = !force;
  Status status;

  std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
  while (true) {
    if (!background_error_.is_ok()) {
      status = background_error_;
      break;
    }

    if (allow_delay &&
        versions_->num_level_files(0) >= kL0SlowdownWritesTrigger) {
      // A millisecond of delay per write, once level 0 is getting full.
      //
      // Not a stall: the point is to hand some of this thread's time to the
      // compaction rather than to stop it.  Waiting until the hard limit and
      // then blocking for seconds converts a gentle backlog into a latency
      // spike the application sees as a hang.  This happens once per write.
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      allow_delay = false;  // only once, so a slow compaction cannot starve us
      lock.lock();
      continue;
    }

    if (!force && mem_->approximate_memory_usage() <= options_.write_buffer_size) {
      break;  // there is room
    }

    if (imm_ != nullptr) {
      // The previous memtable is still being written out; nothing to do but
      // wait for it.
      background_work_finished_.wait(lock);
      continue;
    }

    if (versions_->num_level_files(0) >= kL0StopWritesTrigger) {
      background_work_finished_.wait(lock);
      continue;
    }

    // Rotate: a new log, a new memtable, and the old one queued for flushing.
    const uint64_t new_log_number = versions_->new_file_number();
    std::unique_ptr<WritableFile> file;
    status = WritableFile::open(log_file_name(dbname_, new_log_number),
                                /*append=*/false, &file);
    if (status.is_ok()) {
      // Two syncs before the new log takes over, with the lock released
      // because each waits on the device: this thread is at the front of
      // the writer queue, so no other write can slip in between.
      //
      // The old log is synced whether or not anything asked for it.  The
      // contract for unsynced writes is a prefix: a later batch is never
      // there when an earlier one is missing.  A sync in the new log covers
      // only the new log, so without this the old log's unsynced tail could
      // vanish in a power cut while the new log's synced batches survived
      // -- later writes present, earlier ones gone.  The power-cut
      // simulation found it in its first minute.  Once per rotation, so
      // once per write_buffer_size of writes, and not on the path of any
      // single write.
      //
      // The directory is synced so the new log's name is on the disk
      // before the first synced write into it is acknowledged.
      lock.unlock();
      status = log_->sync();
      if (status.is_ok()) status = sync_directory(dbname_);
      lock.lock();
      if (!status.is_ok()) {
        // The same treatment as a failed sync in write(): the old log may
        // now have a hole that a retried fsync would not report, so the
        // memtable and the log disagree and every later write must fail
        // with the reason.  The new log holds nothing and is removed.
        record_background_error(status);
        file.reset();
        remove_file(log_file_name(dbname_, new_log_number));
      }
    }
    if (!status.is_ok()) {
      versions_->reuse_file_number(new_log_number);
      break;
    }

    log_.reset();
    logfile_number_ = new_log_number;
    log_ = std::make_unique<LogWriter>(std::move(file));

    imm_ = mem_;
    has_imm_.store(true, std::memory_order_release);
    mem_ = new MemTable();
    mem_->ref();
    force = false;
    maybe_schedule_compaction();
  }

  lock.release();  // the caller still holds the mutex
  return status;
}

// ------------------------------------------------------------- reads -------

Status DBImpl::get(const ReadOptions& options, std::string_view key,
                   std::string* value) {
  std::unique_lock<std::mutex> lock(mutex_);

  SequenceNumber snapshot;
  if (options.snapshot != nullptr) {
    snapshot = static_cast<const SnapshotImpl*>(options.snapshot)->sequence;
  } else {
    snapshot = versions_->last_sequence();
  }

  // References taken under the lock and released after; the lookup itself runs
  // without it.  A memtable or version that a compaction retires while this
  // read is in progress stays alive because of these.
  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = versions_->current();
  mem->ref();
  if (imm != nullptr) imm->ref();
  current->ref();

  Status status;
  Version::GetStats stats;
  bool have_stat_update = false;

  {
    lock.unlock();

    // Newest source first, stopping at the first definite answer -- including
    // a tombstone, which is definite.
    if (mem->get(key, snapshot, value, &status)) {
      // answered
    } else if (imm != nullptr && imm->get(key, snapshot, value, &status)) {
      // answered
    } else {
      status = current->get(options, key, snapshot, value, &stats);
      have_stat_update = true;
    }

    lock.lock();
  }

  if (have_stat_update && current->update_stats(stats)) {
    maybe_schedule_compaction();
  }

  mem->unref();
  if (imm != nullptr) imm->unref();
  current->unref();
  return status;
}

Iterator* DBImpl::new_iterator(const ReadOptions& options) {
  std::unique_lock<std::mutex> lock(mutex_);

  const SequenceNumber sequence =
      options.snapshot != nullptr
          ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence
          : versions_->last_sequence();

  std::vector<Iterator*> list;
  list.push_back(mem_->new_iterator());
  mem_->ref();
  if (imm_ != nullptr) {
    list.push_back(imm_->new_iterator());
    imm_->ref();
  }
  Version* current = versions_->current();
  current->add_iterators(options, &list);
  current->ref();

  Iterator* internal = new_merging_iterator(
      internal_comparator_, list.data(), static_cast<int>(list.size()));

  MemTable* mem = mem_;
  MemTable* imm = imm_;
  lock.unlock();

  // The references are dropped when the iterator is destroyed, which is what
  // keeps the memtables and the version alive for as long as it is walked.
  //
  // The destructor takes the database mutex, because Version::unref requires
  // it: dropping the last reference unlinks the version from the set's list,
  // and a compaction may be walking that list at the same time.  Without the
  // lock this is a data race that ThreadSanitizer reports and that in practice
  // corrupts the list only under load -- an iterator destroyed on one thread
  // while a compaction installs a version on another.
  class Registered final : public Iterator {
   public:
    Registered(Iterator* inner, std::mutex* mutex, MemTable* mem,
               MemTable* imm, Version* version)
        : inner_(inner),
          mutex_(mutex),
          mem_(mem),
          imm_(imm),
          version_(version) {}

    ~Registered() override {
      // The inner iterator is destroyed first and outside the lock: it may
      // hold cache handles and open blocks, and releasing those has nothing to
      // do with the database's metadata.
      inner_.reset();

      std::lock_guard<std::mutex> lock(*mutex_);
      mem_->unref();
      if (imm_ != nullptr) imm_->unref();
      version_->unref();
    }
    bool valid() const override { return inner_->valid(); }
    void seek_to_first() override { inner_->seek_to_first(); }
    void seek_to_last() override { inner_->seek_to_last(); }
    void seek(std::string_view t) override { inner_->seek(t); }
    void next() override { inner_->next(); }
    void prev() override { inner_->prev(); }
    std::string_view key() const override { return inner_->key(); }
    std::string_view value() const override { return inner_->value(); }
    Status status() const override { return inner_->status(); }

   private:
    std::unique_ptr<Iterator> inner_;
    std::mutex* mutex_;
    MemTable* mem_;
    MemTable* imm_;
    Version* version_;
  };

  Iterator* registered = new Registered(internal, &mutex_, mem, imm, current);
  return new_db_iterator(this, bytewise_comparator(), registered, sequence, 0);
}

const Snapshot* DBImpl::get_snapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto snapshot = std::make_unique<SnapshotImpl>(versions_->last_sequence());
  const Snapshot* result = snapshot.get();
  snapshots_.insert(snapshot->sequence);
  snapshot_objects_.push_back(std::move(snapshot));
  return result;
}

void DBImpl::release_snapshot(const Snapshot* snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto* impl = static_cast<const SnapshotImpl*>(snapshot);

  // Erase one occurrence, not all: two snapshots taken without an intervening
  // write share a sequence number, and releasing one must not free the other.
  const auto it = snapshots_.find(impl->sequence);
  if (it != snapshots_.end()) {
    bool another_holds_it = false;
    for (const auto& held : snapshot_objects_) {
      if (held.get() != snapshot &&
          static_cast<const SnapshotImpl*>(held.get())->sequence ==
              impl->sequence) {
        another_holds_it = true;
        break;
      }
    }
    if (!another_holds_it) snapshots_.erase(it);
  }

  snapshot_objects_.erase(
      std::remove_if(snapshot_objects_.begin(), snapshot_objects_.end(),
                     [snapshot](const std::unique_ptr<Snapshot>& held) {
                       return held.get() == snapshot;
                     }),
      snapshot_objects_.end());
}

// --------------------------------------------------------- compaction ------

void DBImpl::record_background_error(const Status& status) {
  if (background_error_.is_ok()) {
    background_error_ = status;
    background_work_finished_.notify_all();
  }
}

void DBImpl::maybe_schedule_compaction() {
  if (background_compaction_scheduled_) return;
  if (shutting_down_.load(std::memory_order_acquire)) return;
  if (!background_error_.is_ok()) return;

  const bool work_to_do = imm_ != nullptr || manual_compaction_ != nullptr ||
                          versions_->needs_compaction();
  if (!work_to_do) return;

  background_compaction_scheduled_ = true;

  // One background thread, started on first need and reused.  Compaction is IO
  // bound and its inputs and outputs are ordered with respect to each other,
  // so a second thread would mostly contend for the same lock; parallel
  // compaction needs per-level scheduling, which is a design change rather
  // than a thread count.
  if (!background_thread_started_) {
    background_thread_started_ = true;
    background_thread_ = std::thread([this] { background_loop(); });
  }
  background_work_available_.notify_one();
}

void DBImpl::background_loop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    while (!background_compaction_scheduled_ &&
           !shutting_down_.load(std::memory_order_acquire)) {
      background_work_available_.wait(lock);
    }
    if (shutting_down_.load(std::memory_order_acquire)) {
      background_compaction_scheduled_ = false;
      background_work_finished_.notify_all();
      return;
    }

    if (background_error_.is_ok()) {
      lock.unlock();
      background_compaction();
      lock.lock();
    }

    background_compaction_scheduled_ = false;
    background_work_finished_.notify_all();

    // Another round may now be worthwhile: one compaction can push a level
    // over its limit.
    maybe_schedule_compaction();
  }
}

void DBImpl::background_compaction() {
  std::unique_lock<std::mutex> lock(mutex_);

  if (imm_ != nullptr) {
    // Flushing the immutable memtable comes before anything else: until it is
    // done, writers may be blocked waiting for room.
    lock.unlock();
    const Status status = compact_memtable();
    lock.lock();
    if (!status.is_ok()) record_background_error(status);
    return;
  }

  std::unique_ptr<Compaction> compaction;
  ManualCompaction* manual = manual_compaction_;
  std::string manual_end;

  if (manual != nullptr) {
    compaction.reset(versions_->compact_range(manual->level, manual->begin,
                                              manual->end));
    manual->done = (compaction == nullptr);
    if (compaction != nullptr) {
      manual_end = compaction->input(0, compaction->num_input_files(0) - 1)
                       ->largest;
    }
  } else {
    compaction.reset(versions_->pick_compaction());
  }

  if (compaction == nullptr) {
    if (manual != nullptr) manual->done = true;
    return;
  }

  Status status;
  if (manual == nullptr && compaction->is_trivial_move()) {
    // Nothing overlaps below, so the file moves down a level by editing the
    // manifest.  No bytes are read or written.
    FileMetaData* file = compaction->input(0, 0);
    compaction->edit()->remove_file(compaction->level(), file->number);
    compaction->edit()->add_file(compaction->level() + 1, file->number,
                                 file->file_size, file->smallest,
                                 file->largest);
    status = versions_->log_and_apply(compaction->edit(), &mutex_);
  } else {
    auto compact = std::make_unique<CompactionState>(compaction.get());
    compact->smallest_snapshot = snapshots_.empty()
                                     ? versions_->last_sequence()
                                     : *snapshots_.begin();
    lock.unlock();
    status = do_compaction_work(compact.get());
    lock.lock();

    if (status.is_ok()) {
      status = install_compaction_results(compact.get());
    }

    // The outputs are deliberately NOT deleted when the install fails.
    //
    // install_compaction_results appends the edit to the manifest and then
    // syncs it, and the append happens first — so an fsync that fails leaves a
    // record naming these files already in the file. Deleting them then
    // produces a manifest that permanently references a table that no longer
    // exists, which recover() correctly refuses to open. One transient device
    // error and the database is unopenable with every byte of its data still on
    // disk.
    //
    // Leaving them costs disk space until the next successful cleanup. That is
    // the right side to err on, and it is also why remove_obsolete_files
    // declines to run at all while a background error is outstanding: with the
    // manifest in an unknown state, a file that looks unreferenced may be the
    // only copy of data a reopen will name.
    //
    // Found by injecting one EIO into fsync across every manifest write in a
    // workload: 17 of 85 injection points left the database permanently
    // unopenable.  tools/fault_sweep.sh is that harness, kept so the claim can
    // be re-run rather than believed; it reports every injection clean now.
    for (const auto& output : compact->outputs) {
      pending_outputs_.erase(output.number);
    }
    compaction->release_inputs();
  }

  if (!status.is_ok() && !shutting_down_.load(std::memory_order_acquire)) {
    record_background_error(status);
  }
  remove_obsolete_files();

  if (manual != nullptr) {
    if (!status.is_ok()) manual->done = true;
    if (!manual->done) {
      // Continue from where this compaction stopped.
      manual->tmp_storage = manual_end;
      manual->begin = &manual->tmp_storage;
    }
    // Cleared so the requester can arm the next round.  Leaving it set makes
    // compact_range wait forever after the first round: it only schedules work
    // when the slot is free, and nothing else ever frees it.
    manual_compaction_ = nullptr;
  }
}

Status DBImpl::compact_memtable() {
  // The lock is taken here and held across write_level0_table, which releases
  // it around the IO itself.  See that function for why the discipline is
  // "the caller holds it" rather than "the callee takes it".
  std::unique_lock<std::mutex> lock(mutex_);
  assert(imm_ != nullptr);

  VersionEdit edit;
  Version* base = versions_->current();
  base->ref();

  Status status = write_level0_table(imm_, &edit, base);
  base->unref();

  if (status.is_ok() && shutting_down_.load(std::memory_order_acquire)) {
    status = Status::io_error("database is shutting down");
  }

  if (status.is_ok()) {
    // The log that fed this memtable is now redundant, and saying so in the
    // same edit that adds the table is what makes the flush atomic: a crash
    // either leaves the log (and replays it) or leaves the table.
    edit.set_prev_log_number(0);
    edit.set_log_number(logfile_number_);
    status = versions_->log_and_apply(&edit, &mutex_);
  }

  if (status.is_ok()) {
    imm_->unref();
    imm_ = nullptr;
    has_imm_.store(false, std::memory_order_release);
    remove_obsolete_files();
  }
  return status;
}

// REQUIRES: mutex_ is held on entry, and is held again on return.
//
// The lock is released around build_table and reacquired afterwards, rather
// than being taken here.  Taking it here is the obvious shape and it
// deadlocks: recovery calls this from inside DB::open, which already holds
// the lock, and std::mutex is not recursive.  That deadlock does not appear in
// a fresh database -- only in one whose log is long enough to need flushing
// during recovery -- so it survives every quick test and hangs the first real
// reopen.
Status DBImpl::write_level0_table(MemTable* mem, VersionEdit* edit,
                                  Version* base) {
  FileMetaData meta;
  meta.number = versions_->new_file_number();
  pending_outputs_.insert(meta.number);

  Status status;
  {
    mutex_.unlock();
    std::unique_ptr<Iterator> iter(mem->new_iterator());
    status = build_table(dbname_, options_, table_cache_.get(),
                         internal_comparator_, iter.get(), &meta);
    mutex_.lock();
  }
  pending_outputs_.erase(meta.number);

  if (status.is_ok() && meta.file_size > 0) {
    int level = 0;
    if (base != nullptr) {
      // Pushed as deep as it can go without overlapping, so data that is never
      // read again is not compacted down one level at a time.
      level = base->pick_level_for_memtable_output(
          extract_user_key(meta.smallest), extract_user_key(meta.largest));
    }
    edit->add_file(level, meta.number, meta.file_size, meta.smallest,
                   meta.largest);
  }
  return status;
}

Status DBImpl::open_compaction_output_file(CompactionState* compact) {
  uint64_t number;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    number = versions_->new_file_number();
    // Recorded as pending so that remove_obsolete_files, which runs on this
    // same thread between compactions, does not delete a file that is being
    // written and is not yet in any version.
    pending_outputs_.insert(number);
  }

  CompactionState::Output output;
  output.number = number;
  compact->outputs.push_back(output);

  std::unique_ptr<WritableFile> file;
  Status status = WritableFile::open(table_file_name(dbname_, number),
                                     /*append=*/false, &file);
  if (status.is_ok()) {
    compact->outfile = std::move(file);
    compact->builder = std::make_unique<TableBuilder>(
        options_, compact->outfile.get(), internal_comparator_);
  }
  return status;
}

Status DBImpl::finish_compaction_output_file(CompactionState* compact,
                                             Iterator* input) {
  Status status = input->status();
  const uint64_t entries = compact->builder->num_entries();

  if (status.is_ok()) {
    status = compact->builder->finish();
  } else {
    compact->builder->abandon();
  }

  const uint64_t bytes = compact->builder->file_size();
  compact->current_output()->file_size = bytes;
  compact->total_bytes += bytes;
  compact->builder.reset();

  // Synced before it is installed.  A compaction output that is only in the
  // page cache would take the data with it if the machine lost power after the
  // manifest recorded it and before the file reached the disk -- and the
  // inputs would already have been deleted.
  if (status.is_ok()) status = compact->outfile->sync();
  if (status.is_ok()) status = compact->outfile->close();
  compact->outfile.reset();
  // And its directory entry, which fsync of the file does not promise to
  // cover: a manifest that names a table whose name never reached the disk
  // is a database that refuses to open.
  if (status.is_ok()) status = sync_directory(dbname_);

  if (status.is_ok() && entries > 0) {
    std::unique_ptr<Iterator> check(table_cache_->new_iterator(
        ReadOptions(), compact->current_output()->number, bytes));
    status = check->status();
  }
  return status;
}

Status DBImpl::install_compaction_results(CompactionState* compact) {
  compact->compaction->add_input_deletions(compact->compaction->edit());
  const int level = compact->compaction->level();
  for (const auto& output : compact->outputs) {
    compact->compaction->edit()->add_file(level + 1, output.number,
                                          output.file_size, output.smallest,
                                          output.largest);
  }
  return versions_->log_and_apply(compact->compaction->edit(), &mutex_);
}

Status DBImpl::do_compaction_work(CompactionState* compact) {
  std::unique_ptr<Iterator> input(
      versions_->make_input_iterator(compact->compaction));
  input->seek_to_first();

  Status status;
  ParsedInternalKey key;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;

  for (; input->valid() && !shutting_down_.load(std::memory_order_acquire);) {
    // Flushing the memtable takes priority: writers may be blocked on it.
    if (has_imm_.load(std::memory_order_acquire)) {
      const Status flush = compact_memtable();
      background_work_finished_.notify_all();

      if (!flush.is_ok()) {
        // Stop, rather than carry on and try again on the next key.
        //
        // compact_memtable only clears imm_ on success, so a failure leaves it
        // set and this loop retries it — but by then a table file has been
        // written and its ADD record has reached the manifest. The retry
        // writes a *second* file for the same memtable and appends a second
        // ADD, and the manifest ends up naming two files with identical key
        // ranges at the same level. That breaks the disjointness the level
        // below zero is required to have, which every lookup's binary search
        // depends on, and it repeats once per input key for as long as the
        // failure lasts.
        {
          std::lock_guard<std::mutex> lock(mutex_);
          record_background_error(flush);
        }
        // `break`, not `return`: the code after the loop abandons the output
        // file that is still open, and returning here would leave a
        // TableBuilder to be destroyed unfinished.
        status = flush;
        break;
      }
    }

    const std::string_view internal_key = input->key();

    // Cut the output file before it starts overlapping too much of the
    // grandparent level, which would make the next compaction of this range
    // read far more than it writes.
    if (compact->builder != nullptr &&
        compact->compaction->should_stop_before(internal_key)) {
      status = finish_compaction_output_file(compact, input.get());
      if (!status.is_ok()) break;
    }

    bool drop = false;
    if (!parse_internal_key(internal_key, &key)) {
      // A key that will not parse is kept verbatim rather than dropped: it is
      // already damage, and discarding it would turn "this file has a bad
      // record" into "this key never existed".
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key || key.user_key != current_user_key) {
        current_user_key.assign(key.user_key.data(), key.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }

      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // A newer version of this key is already visible to every live
        // snapshot, so this one can never be read again.
        drop = true;
      } else if (key.type == ValueType::kDeletion &&
                 key.sequence <= compact->smallest_snapshot &&
                 compact->compaction->is_base_level_for_key(key.user_key)) {
        // A tombstone can only be dropped once there is nothing left below for
        // it to hide.  Dropping it earlier would let an older version in a
        // deeper level reappear -- a deleted key coming back to life, which is
        // the worst failure this code can produce and the one that makes the
        // is_base_level_for_key check non-negotiable.
        drop = true;
      }
      last_sequence_for_key = key.sequence;
    }

    if (!drop) {
      if (compact->builder == nullptr) {
        status = open_compaction_output_file(compact);
        if (!status.is_ok()) break;
      }
      if (compact->builder->num_entries() == 0) {
        compact->current_output()->smallest.assign(internal_key.data(),
                                                   internal_key.size());
      }
      compact->current_output()->largest.assign(internal_key.data(),
                                                internal_key.size());
      compact->builder->add(internal_key, input->value());

      if (compact->builder->file_size() >=
          compact->compaction->max_output_file_size()) {
        status = finish_compaction_output_file(compact, input.get());
        if (!status.is_ok()) break;
      }
    }
    input->next();
  }

  if (status.is_ok() && shutting_down_.load(std::memory_order_acquire)) {
    status = Status::io_error("database is shutting down during compaction");
  }

  if (status.is_ok() && compact->builder != nullptr) {
    status = finish_compaction_output_file(compact, input.get());
  }

  // An output still open here means the compaction is being abandoned -- an
  // error, or a shutdown partway through a file.  It is abandoned explicitly
  // rather than destroyed: a builder that is simply dropped leaves a file with
  // no footer, which is unopenable and therefore harmless, but it also leaves
  // no record that this code took that path.  The caller deletes the file.
  if (compact->builder != nullptr) {
    compact->builder->abandon();
    compact->builder.reset();
    compact->outfile.reset();
  }

  if (status.is_ok()) status = input->status();
  return status;
}

void DBImpl::remove_obsolete_files() {
  // Called with the lock held.
  if (!background_error_.is_ok()) {
    // The set of live files cannot be trusted while an error is outstanding:
    // an edit may have failed to reach the manifest, so a file that looks
    // unreferenced may be the only copy of data the manifest will name after a
    // reopen.  Leaving files behind wastes space; deleting them loses data.
    return;
  }

  std::set<uint64_t> live = pending_outputs_;
  versions_->add_live_files(&live);

  std::vector<std::string> names;
  if (!list_directory(dbname_, &names).is_ok()) return;
  std::vector<std::string> to_delete;
  for (const std::string& base : names) {
    uint64_t number = 0;
    FileType type;
    if (!parse_file_name(base, &number, &type)) continue;

    bool keep = true;
    switch (type) {
      case FileType::kLog:
        keep = (number >= versions_->log_number()) ||
               (number == versions_->prev_log_number());
        break;
      case FileType::kDescriptor:
        keep = (number >= versions_->manifest_file_number());
        break;
      case FileType::kTable:
        keep = live.count(number) > 0;
        break;
      case FileType::kTemp:
        keep = live.count(number) > 0;
        break;
      case FileType::kCurrent:
      case FileType::kLock:
      case FileType::kInfoLog:
        keep = true;
        break;
    }
    if (!keep) {
      if (type == FileType::kTable) table_cache_->evict(number);
      to_delete.push_back(dbname_ + "/" + base);
    }
  }

  // Deleted with the lock released: unlinking is a syscall per file and the
  // lock protects in-memory state that none of this touches.
  mutex_.unlock();
  for (const std::string& path : to_delete) {
    remove_file(path);
  }
  mutex_.lock();
}

void DBImpl::wait_for_background_work() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (background_compaction_scheduled_) {
    background_work_finished_.wait(lock);
  }
}

void DBImpl::compact_range(const std::string_view* begin,
                           const std::string_view* end) {
  // Find the deepest level that holds anything, so the whole tree is swept.
  int max_level_with_files = 1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Version* base = versions_->current();
    for (int level = 1; level < kNumLevels; ++level) {
      if (base->num_files(level) > 0) max_level_with_files = level;
    }
  }

  // The memtable first, so that its contents take part in the compaction
  // rather than being left behind in memory.  As a write with nothing in
  // it, through the writer queue: the rotation releases the mutex around
  // its syncs on the strength of being the only writer, and only the front
  // of the queue is that.  Calling make_room_for_write from here with the
  // mutex alone used to be a race against a writer using the log with the
  // mutex released; with the syncs it became two rotations at once.
  bool has_data = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    has_data = mem_ != nullptr && mem_->approximate_memory_usage() > 0;
  }
  if (has_data) {
    const Status status = write(WriteOptions(), nullptr);
    if (!status.is_ok()) {
      // compact_range returns void, so there is nowhere to report this to
      // the caller.  Recording it makes every subsequent write fail with the
      // real reason instead of the failure being swallowed entirely and the
      // compaction proceeding over a memtable that was never flushed.
      std::lock_guard<std::mutex> lock(mutex_);
      record_background_error(status);
      return;
    }
  }
  wait_for_background_work();

  for (int level = 0; level < max_level_with_files; ++level) {
    ManualCompaction manual;
    manual.level = level;

    std::string begin_storage;
    std::string end_storage;
    if (begin != nullptr) {
      begin_storage = make_internal_key(*begin, kMaxSequenceNumber,
                                        kValueTypeForSeek);
      manual.begin = &begin_storage;
    }
    if (end != nullptr) {
      end_storage = make_internal_key(*end, 0, static_cast<ValueType>(0));
      manual.end = &end_storage;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    while (!manual.done && !shutting_down_.load(std::memory_order_acquire) &&
           background_error_.is_ok()) {
      if (manual_compaction_ == nullptr) {
        // Idle: claim the slot and start a round.
        manual_compaction_ = &manual;
        maybe_schedule_compaction();
      } else {
        // A compaction is running -- this one or an automatic one.  Waiting is
        // the only correct move; claiming the slot again would lose whichever
        // request is already in it.
        background_work_finished_.wait(lock);
      }
    }
    if (manual_compaction_ == &manual) manual_compaction_ = nullptr;
  }
}

bool DBImpl::get_property(std::string_view property, std::string* value) {
  value->clear();
  constexpr std::string_view kPrefix = "ambar.";
  if (property.substr(0, kPrefix.size()) != kPrefix) return false;
  const std::string_view name = property.substr(kPrefix.size());

  std::lock_guard<std::mutex> lock(mutex_);

  constexpr std::string_view kNumFiles = "num-files-at-level";
  if (name.substr(0, kNumFiles.size()) == kNumFiles) {
    const std::string_view suffix = name.substr(kNumFiles.size());
    int level = 0;
    for (const char c : suffix) {
      if (c < '0' || c > '9') return false;
      level = level * 10 + (c - '0');
    }
    if (suffix.empty() || level >= kNumLevels) return false;
    *value = std::to_string(versions_->num_level_files(level));
    return true;
  }

  if (name == "stats") {
    *value = "level  files      size(MB)\n"
             "--------------------------\n";
    for (int level = 0; level < kNumLevels; ++level) {
      const int files = versions_->num_level_files(level);
      if (files == 0) continue;
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%5d %6d %13.1f\n", level, files,
                    static_cast<double>(versions_->num_level_bytes(level)) /
                        1048576.0);
      *value += buf;
    }
    return true;
  }

  if (name == "sstables") {
    *value = versions_->current()->debug_string();
    return true;
  }

  if (name == "approximate-memory-usage") {
    size_t total = mem_ != nullptr ? mem_->approximate_memory_usage() : 0;
    if (imm_ != nullptr) total += imm_->approximate_memory_usage();
    *value = std::to_string(total);
    return true;
  }

  if (name == "last-sequence") {
    *value = std::to_string(versions_->last_sequence());
    return true;
  }

  return false;
}

}  // namespace ambar
