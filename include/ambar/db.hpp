// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The public interface: an ordered, persistent map from byte strings to byte
// strings.
//
// What it promises, precisely, because a storage engine that is vague about
// this is not usable:
//
//   * A write that returns ok with WriteOptions::sync survives a power cut, on
//     hardware that does not lie about flushing.
//   * A write that returns ok without sync survives this process dying, but
//     not the machine losing power.
//   * A WriteBatch is applied entirely or not at all, whatever happens.
//   * A read sees every write that returned before it, and nothing that has
//     not been written -- there is no window in which a completed write is
//     invisible.
//   * A snapshot sees the database exactly as it was when the snapshot was
//     taken, for as long as it is held.
//
// What it does not promise is in docs/DESIGN.md, and is worth reading before
// trusting any of the above.

#ifndef AMBAR_DB_HPP_
#define AMBAR_DB_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ambar/iterator.hpp"
#include "ambar/options.hpp"
#include "ambar/status.hpp"
#include "ambar/write_batch.hpp"

namespace ambar {

// The engine's version, as the tag it was released from.  Before 1.0 the
// on-disk format may change between minor versions, and a database written
// by one is not promised to open under another; a change to the format is a
// change to this number, and docs/DESIGN.md says what changed.
constexpr int kMajorVersion = 0;
constexpr int kMinorVersion = 3;
constexpr int kPatchVersion = 0;
constexpr std::string_view kVersion = "0.3.0";

// A point in the database's history, held open.
//
// Opaque on purpose: it is a sequence number, but a caller who knew that could
// construct one for a point the engine has already compacted away, and would
// get an answer that is quietly wrong rather than an error.
struct Snapshot {
  virtual ~Snapshot();
};

class DB {
 public:
  // Opens or creates the database in the directory `name`.
  //
  // Fails with kIoError when another process already has this directory open.
  // The exclusion is a lock file in the directory and it is not advisory in
  // practice: two processes writing to one database destroy each other's data
  // with no error reported to either, so this is a refusal rather than a
  // warning.  It does not stop a second DB object inside *this* process --
  // see src/file.hpp for why that is not achievable with the same mechanism.
  static Status open(const Options& options, const std::string& name,
                     std::unique_ptr<DB>* db);

  DB() = default;
  virtual ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual Status put(const WriteOptions& options, std::string_view key,
                     std::string_view value) = 0;

  // Deleting a key that is not present is not an error.  The engine cannot
  // know, at write time, whether the key exists in some file it has not read.
  virtual Status del(const WriteOptions& options, std::string_view key) = 0;

  virtual Status write(const WriteOptions& options, WriteBatch* updates) = 0;

  // Returns kNotFound when the key is absent, which is an ordinary result and
  // not an error.
  virtual Status get(const ReadOptions& options, std::string_view key,
                     std::string* value) = 0;

  // Over the whole database, in key order.  The caller owns it, and it must be
  // destroyed before the database is.
  //
  // An iterator holds a snapshot: it sees the database as of the moment it was
  // created, and holding one for a long time keeps the files it references
  // from being deleted.
  virtual Iterator* new_iterator(const ReadOptions& options) = 0;

  virtual const Snapshot* get_snapshot() = 0;
  virtual void release_snapshot(const Snapshot* snapshot) = 0;

  // Introspection.  Understood names:
  //
  //   ambar.num-files-at-level<N>   files at that level
  //   ambar.stats                   files and bytes per level, and what
  //                                 producing each has cost since open:
  //                                 seconds, bytes read, bytes written
  //   ambar.bytes-written           bytes appended since open, one
  //                                 "kind bytes" line each: log (records,
  //                                 headers, padding), flush (tables from
  //                                 memtables), compaction (tables from
  //                                 compactions), manifest.  Their sum over
  //                                 the bytes of keys and values handed in
  //                                 is the write amplification.
  //   ambar.log-writes              group commit since open, as "records N",
  //                                 "batches N" and "parked N" lines: records
  //                                 appended to the log, the writers' batches
  //                                 they carried, and the writers that slept
  //                                 for their turn rather than being answered
  //                                 while they watched.  Batches over records
  //                                 is how many writes each log write and its
  //                                 fsync served; parked over batches, how
  //                                 many paid a context switch to wait.
  //   ambar.table-reads             reads of table files since open, as
  //                                 "reads N" and "bytes N" lines: every
  //                                 data block the block cache did not
  //                                 answer, and the footer, index and,
  //                                 with a filter configured, metaindex
  //                                 and filter read when a table is
  //                                 opened.  Over
  //                                 the lookups that caused them, the read
  //                                 amplification.
  //   ambar.sstables                every file, with its key range
  //   ambar.approximate-memory-usage bytes held in memtables
  //   ambar.last-sequence           the newest sequence number assigned, which
  //                                 is what a snapshot taken now would see
  virtual bool get_property(std::string_view property, std::string* value) = 0;

  // Compacts the given range, or the whole database when both are null.  Used
  // by benchmarks and by an application that knows it has just deleted a great
  // deal and would like the space back now.
  virtual void compact_range(const std::string_view* begin,
                             const std::string_view* end) = 0;
};

// Removes every file the database owns.  Files in the directory that this
// engine did not write are left alone, and the directory itself is not removed
// if anything remains in it.
//
// Takes the same lock DB::open does, and fails the same way if another process
// holds it.  Deleting a database out from under a running process is the one
// thing the lock exists to prevent, and it would be absurd for the destroy
// path to be the hole in it.
Status destroy_db(const std::string& name, const Options& options);

// What repair_db found, and what it did about it.
struct RepairReport {
  int tables_kept = 0;       // table files that opened and read to their end
  int tables_set_aside = 0;  // moved to <name>/lost/: would not open or read
  int logs_converted = 0;    // log files replayed, as recovery would have
  int logs_set_aside = 0;    // of those, ones that stopped early: replayed as
                             // far as they went, then moved to <name>/lost/
  int tables_written = 0;    // the merged tables the new manifest names
  uint64_t last_sequence = 0;
  bool opened = false;       // the repaired database was opened and closed
  std::vector<std::string> notes;  // one line per file, and per surprise
};

// Rebuilds a database from the files that survive in its directory, so that
// one open() refuses -- CURRENT lost, a manifest damaged in the middle, a
// table the manifest names that is not there -- can be opened again.
//
// Every table that opens and reads to its end, in order, is kept; every log
// is replayed as far as it can be read, each batch checked before it is
// applied; what will not read is moved to <name>/lost/ rather than deleted,
// and so are the old manifests, which are the record of what went wrong --
// but only once the new manifest is in place, so that a power cut during
// the repair leaves either the directory as it was or the repaired one.
// What was kept is merged -- each user key once, at its newest version,
// deletions dropped, in tables that do not overlap -- read back, and named
// by a fresh manifest at the last level, where nothing lies beneath them
// and nothing schedules a compaction over them.  Finally the database is
// opened, which is what says the repair worked, and whose cleanup removes
// the files the merge replaced.  The report says what was kept, what was
// set aside, and why.
//
// What it cannot restore is the history that produced the files.  Two
// consequences, both rare, both stated because the report is read under
// pressure:
//
//   * A tombstone that a compaction already dropped no longer shadows an
//     older value that survives in a stale input file -- one that cleanup
//     had not yet removed when the manifest was lost.  That key comes back.
//   * A table set aside takes its deletions with it as well as its values,
//     so a key it deleted may reappear from an older table.
//
// Takes the lock open() takes, with the same limit: it keeps out another
// process, and on POSIX not a second caller in this one.  A repair that
// fails before the manifest is written leaves the directory as it found it;
// one that fails at the final open has already pointed CURRENT at the
// merged tables and left every other file in place or in lost/, and says
// so.  Running it again reads the survivors and any merged tables
// alike, and the merge collapses the overlap.  Not for a database that
// opens: repair rewrites every table, and an intact database gains nothing
// from that.
Status repair_db(const std::string& name, const Options& options,
                 RepairReport* report);

}  // namespace ambar

#endif  // AMBAR_DB_HPP_
