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

#include "ambar/iterator.hpp"
#include "ambar/options.hpp"
#include "ambar/status.hpp"
#include "ambar/write_batch.hpp"

namespace ambar {

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
  //   ambar.stats                   files and bytes per level
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

}  // namespace ambar

#endif  // AMBAR_DB_HPP_
