// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// A disk that loses what was not synced.
//
// Every other test in this project runs against a disk that keeps
// everything, because that is what a disk does until the power goes.  The
// durability contract in docs/DESIGN.md is a claim about the other case: if
// write() returned with sync set, the batch is there after a power cut.  A
// process kill cannot check it -- the kernel keeps whatever the process had
// handed over, so a database that never calls fsync survives a kill exactly
// as well as one that does.  This filesystem can: it is the engine's whole
// view of the disk, it knows which bytes were synced and which were not,
// and at a chosen instant it cuts the power and keeps only what a real disk
// might have kept.
//
// The model, stated so a reader can judge what a green run proves.  Where
// it has a choice it takes the more adversarial one, so that the states it
// produces are a superset of what the filesystems named below produce; a
// pass here is then a pass there, and a failure here may be a state one of
// them cannot reach, which is said where it applies.
//
//   Data.    A file is a durable prefix -- what fsync has covered -- and an
//            unsynced tail the kernel has.  A cut keeps the prefix and a
//            random length of the tail: the kernel writes a file back in
//            order, and each file's tail is independent of every other's,
//            since nothing orders writeback across files.  With
//            Tails::kOrdered the size never runs ahead of the data, which
//            is what ext4's classic ordered mode gave.  With Tails::kHoles
//            it may, and each 4 KiB page of the kept tail is independently
//            kept or read back as zeros: ext4 since Linux 5.6 and xfs since
//            5.8 extend the size once the data is submitted and mark the
//            extent written only when it completes, and an unwritten
//            extent reads as zeros, so this is what a stock Linux
//            filesystem leaves today; NTFS reads zeros past its valid-data
//            length the same way.  With Tails::kGarbage the unlanded pages
//            hold whatever the blocks held before -- ext4 in writeback
//            mode, ext2, the older configurations -- which is random bytes
//            here rather than a deleted file's, see below.  Bytes still in
//            the process's own buffer never land, as with stdio, which
//            hands them to the kernel in 4 KiB pieces.  The durable prefix
//            is never damaged: writing a range of bytes changes no byte
//            outside it, which SQLite calls powersafe overwrite and every
//            disk this decade provides.
//
//   Names.   Creating, renaming and deleting a file, and creating a
//            directory, are metadata.  They land in order -- a journal
//            commits transactions one after another, so a later one is
//            never durable while an earlier one is not -- and a cut keeps a
//            random-length prefix of those not yet committed.
//            sync_directory commits them all.  With Dirents::kFile, syncing
//            a file also commits that file's own name, and nothing else:
//            what btrfs does, and less than ext4 and xfs do, which commit
//            everything pending, so the states are a superset of both.
//            With Dirents::kPosix nothing but sync_directory commits, which
//            is all POSIX promises and stricter than any filesystem in use;
//            it is here because the difference is exactly the directory
//            syncs the engine does after creating a file, and a mutation
//            that removes one should have a test that goes red.
//            Truncating a file that exists is a change to the file, not to
//            a name: it lands with that file's next sync and with nothing
//            else, and a cut before one may show the old contents in full.
//            (What it held before the truncation, that is; two truncations
//            with no sync between them offer the original or the latest,
//            never the one in the middle.  The engine never truncates a
//            file that exists.)
//
//   Not modelled: a disk that lies about fsync; damage to synced bytes; a
//   file overwritten in place, which the engine never does; and the
//   particular garbage that is a deleted file's bytes -- a writeback-mode
//   filesystem can extend a log over blocks that still hold intact records
//   of the log it replaced, which is the one state that could make the
//   engine replay old records as new rather than refuse.
//
// After the cut every operation fails with "power is off" until power_on(),
// which makes the surviving state the live state -- as a reboot would --
// and releases the lock the dead process held.  tests/powercut_driver.hpp
// drives it.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "file.hpp"

namespace ambar {

class SimFileSystem final : public FileSystem {
 public:
  enum class Dirents { kFile, kPosix };
  enum class Tails { kOrdered, kHoles, kGarbage };

  struct Model {
    Dirents dirents = Dirents::kFile;
    Tails tails = Tails::kOrdered;
  };

  SimFileSystem(Model model, uint64_t seed);
  ~SimFileSystem() override;

  // Every operation that changes the disk or syncs it -- append, flush,
  // sync, close, create, truncate, rename, remove, mkdir, rmdir, directory
  // sync -- is counted from the start of the simulation; reads and opens
  // for reading are not, since a cut between two reads is the cut before
  // the next write.
  uint64_t operations() const;

  // How many times an operation has been applied to files of a kind --
  // "create .log", "sync .sst", "remove .sst", "rename CURRENT", "create
  // MANIFEST" -- or, for the three that take a directory, just "mkdir",
  // "rmdir", "sync directory".  A test can ask whether the workload reached
  // a rotation or a compaction rather than assume it did.
  uint64_t count(const std::string& what) const;

  // Cuts the power just before operation `index` happens -- when the
  // counter equals it exactly, so an index already passed never fires --
  // and the operation itself does not.  kNever cancels.
  static constexpr uint64_t kNever = ~uint64_t{0};
  void crash_at(uint64_t index);

  // Cuts the power now.
  void crash();

  bool powered() const;

  // Restores power.  What survived the cut is the whole disk from here on.
  void power_on();

  // Writes the live tree into a real directory, so that the state a failure
  // left can be looked at with ambar_repair and a hex editor.  Not through
  // this filesystem, obviously.
  Status dump_to(const std::string& directory) const;

  // One line per file -- path, size, how much of it was synced -- and,
  // after a cut, what each file had at the instant of the cut and how much
  // of its unsynced tail was kept, which is what a failure needs.
  std::vector<std::string> describe() const;

  // FileSystem.
  Status new_writable_file(const std::string& path, bool append,
                           std::unique_ptr<WritableFile>* out) override;
  Status new_sequential_file(const std::string& path,
                             std::unique_ptr<SequentialFile>* out) override;
  Status new_random_access_file(
      const std::string& path, std::unique_ptr<RandomAccessFile>* out) override;
  Status lock_file(const std::string& path,
                   std::unique_ptr<FileLock>* out) override;
  Status file_size(const std::string& path, uint64_t* size) override;
  Status remove_file(const std::string& path) override;
  Status rename_file(const std::string& from, const std::string& to) override;
  PathType path_type(const std::string& path) override;
  Status create_directory(const std::string& path) override;
  Status remove_directory(const std::string& path) override;
  Status list_directory(const std::string& path,
                        std::vector<std::string>* names) override;
  Status sync_directory(const std::string& path) override;

 private:
  friend class SimWritableFile;
  friend class SimSequentialFile;
  friend class SimRandomAccessFile;
  friend class SimFileLock;

  // An inode: the same object follows a rename, and outlives an unlink for
  // as long as a handle or the committed tree still refers to it.
  struct File {
    std::string kernel;    // what the kernel has, and what reads return
    uint64_t durable = 0;  // the prefix fsync has covered
    // Set by a truncating open of a file that existed: what the file held
    // before, until the truncation lands with the next sync.
    struct Before {
      std::string kernel;
      uint64_t durable = 0;
    };
    std::optional<Before> before;
  };
  using FilePtr = std::shared_ptr<File>;

  struct Tree {
    std::map<std::string, FilePtr> files;
    std::set<std::string> directories;
  };

  struct Event {
    enum Kind { kCreate, kRename, kDelete, kMkdir } kind;
    std::string path;
    std::string to;  // kRename only
    FilePtr file;    // kCreate, kRename
  };

  // Called with mutex_ held.  Returns the error every operation returns
  // once the power is off, or ok and counts the operation.
  Status begin_operation(const char* what, const std::string& path,
                         bool directory = false);
  void commit_all();
  void commit_names_of(const FilePtr& file);
  void cut();
  static void replay(Tree* tree, const Event& event);
  static std::string directory_of(const std::string& path);
  static std::string kind_of(const std::string& path);

  const Model model_;
  const uint64_t seed_;
  mutable std::mutex mutex_;
  Tree live_;
  Tree committed_;
  std::vector<Event> pending_;
  std::set<std::string> locked_;
  std::map<std::string, uint64_t> counts_;
  uint64_t operations_ = 0;
  uint64_t crash_at_ = kNever;
  uint64_t crashes_ = 0;
  uint64_t generation_ = 0;  // bumped by power_on, so stale handles stay dead
  bool powered_ = true;
  std::string cut_at_;  // the operation the cut landed on
  std::vector<std::string> cut_report_;  // what each file kept at the cut
  Tree survived_;       // the durable tree, computed at the cut
};

}  // namespace ambar
