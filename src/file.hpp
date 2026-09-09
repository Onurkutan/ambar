// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The thin layer between Ambar and the operating system.
//
// Kept deliberately small: three file abstractions and one honest fsync.  The
// point of isolating them is that durability lives or dies on exactly one
// question -- has this byte reached stable storage? -- and that question should
// have exactly one answer in the codebase.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#include "ambar/status.hpp"

namespace ambar {

// Append-only, buffered, with an explicit sync.  Writes go to a userspace
// buffer, then to the kernel on flush, then to the disk on sync -- three
// distinct states that are easy to confuse and expensive to confuse.
class WritableFile {
 public:
  static Status open(const std::string& path, bool append,
                     std::unique_ptr<WritableFile>* out);
  ~WritableFile();

  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  Status append(std::string_view data);

  // Hands the buffer to the kernel.  Survives a process crash, not a power cut.
  Status flush();

  // Waits for the kernel to hand the data to the device.  This is the call the
  // durability contract is written in terms of, and the one that costs
  // milliseconds.
  Status sync();

  Status close();

  uint64_t bytes_written() const { return bytes_written_; }
  const std::string& path() const { return path_; }

 private:
  WritableFile(std::FILE* file, std::string path)
      : file_(file), path_(std::move(path)) {}

  std::FILE* file_ = nullptr;
  std::string path_;
  uint64_t bytes_written_ = 0;
};

// Read the whole file, once, forwards.  What log replay and table loading need.
class SequentialFile {
 public:
  static Status open(const std::string& path,
                     std::unique_ptr<SequentialFile>* out);
  ~SequentialFile();

  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;

  // Reads up to n bytes into *scratch and points *result at them.  A short read
  // means end of file, which for a log is a normal and expected way to finish.
  Status read(size_t n, std::string_view* result, std::string* scratch);

 private:
  explicit SequentialFile(std::FILE* file, std::string path)
      : file_(file), path_(std::move(path)) {}

  std::FILE* file_ = nullptr;
  std::string path_;
};

// Read any part of the file, from any thread, at any time.  What table files
// need: one open file is shared by every reader in the process, and two
// threads reading different blocks must not interfere.
//
// This is a separate class from SequentialFile rather than a mode on it,
// because the difference is not the direction of travel -- it is that this one
// carries no file position.  A shared FILE* with fseek+fread would compile and
// pass a single-threaded test, and then return one thread's block to another
// under load, because the seek and the read are two operations on one piece of
// mutable state.  The call below takes the offset as an argument, so there is
// no such state to race on.
//
// read() is virtual, which is a deliberate cost: one indirect call per *block*
// -- not per key -- next to a syscall that costs thousands of times more.  It
// buys two things.  A test can wrap a real file and count how many blocks a
// lookup actually reads, which is the only way to demonstrate that the bloom
// filter prevents reads rather than merely claiming to; and a future
// memory-mapped implementation slots in without the table code changing.
class RandomAccessFile {
 public:
  static Status open(const std::string& path,
                     std::unique_ptr<RandomAccessFile>* out);

  RandomAccessFile() = default;
  virtual ~RandomAccessFile();

  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;

  // Reads n bytes at `offset` into `scratch` (which must have room for n) and
  // points *result at them.  Const and thread-safe: callers may hold one of
  // these behind a shared pointer and read from any thread.
  //
  // A short read is an error here, unlike in SequentialFile.  A caller asking
  // for a block knows exactly how long it is, so getting less back means the
  // file is truncated, not that it has finished.
  virtual Status read(uint64_t offset, size_t n, std::string_view* result,
                      char* scratch) const = 0;
};

// An exclusive claim on a database directory, held for as long as it is open.
//
// Two processes opening the same directory is not a race that resolves badly
// sometimes; it is guaranteed destruction. Both replay each other's in-flight
// logs, both hand out the same file numbers, both append to the same manifest,
// and each one's cleanup deletes files the other is using — with no error
// reported to either. Measured on this engine before the lock existed: two
// processes writing 6,000 keys each destroyed 11,032 of the 12,000
// acknowledged writes between them, and both reported success throughout.
//
// The lock is advisory and process-scoped, which is what the platforms offer.
// It stops a second process; it does not stop a second DB object inside one
// process, and on POSIX the record lock is released by closing *any* file
// descriptor for the file, so nothing else may open it.
class FileLock {
 public:
  // Returns kIoError when the file is already locked by another process, with
  // a message that says so rather than reporting a generic failure.
  static Status acquire(const std::string& path,
                        std::unique_ptr<FileLock>* out);
  ~FileLock();

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

 private:
#if defined(_WIN32)
  FileLock(void* handle, std::string path)
      : handle_(handle), path_(std::move(path)) {}
  void* handle_ = nullptr;
#else
  FileLock(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  int fd_ = -1;
#endif
  std::string path_;
};

// Filesystem helpers, each returning a Status rather than throwing, so callers
// can decide what a missing file means in their context.
Status file_size(const std::string& path, uint64_t* size);
Status remove_file(const std::string& path);
Status rename_file(const std::string& from, const std::string& to);
bool file_exists(const std::string& path);
Status create_directory(const std::string& path);

// Fsyncs a directory, so that a file created inside it is guaranteed to be
// findable after a crash.  A no-op on Windows, where directories are not
// openable as files -- documented rather than silently skipped.
Status sync_directory(const std::string& path);

}  // namespace ambar
