// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Scaffolding shared between test files.

#ifndef AMBAR_TESTS_TESTUTIL_HPP_
#define AMBAR_TESTS_TESTUTIL_HPP_

#include <filesystem>
#include <random>
#include <string>

#include "../src/memtable.hpp"

namespace ambar_test {

// A scratch directory that cleans up after itself, so a failing test cannot
// leave state that makes the next run pass or fail for the wrong reason.
class TempDir {
 public:
  TempDir() {
    // std::random_device rather than the process id, because getpid() is POSIX
    // and this has to build on Windows too.
    static std::random_device seed;
    static const auto run_id = static_cast<unsigned>(seed());
    static int counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("ambar_test_" + std::to_string(run_id) + "_" +
             std::to_string(counter++));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string file(const std::string& name) const {
    return (path_ / name).string();
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// Memtables are reference counted and delete themselves, so tests hold them
// through a guard rather than a raw pointer that is easy to leak under ASan.
class MemTableRef {
 public:
  MemTableRef() : table_(new ambar::MemTable()) { table_->ref(); }
  ~MemTableRef() { table_->unref(); }

  MemTableRef(const MemTableRef&) = delete;
  MemTableRef& operator=(const MemTableRef&) = delete;

  ambar::MemTable* operator->() const { return table_; }
  ambar::MemTable& operator*() const { return *table_; }
  ambar::MemTable* get() const { return table_; }

 private:
  ambar::MemTable* table_;
};

}  // namespace ambar_test

#endif  // AMBAR_TESTS_TESTUTIL_HPP_
