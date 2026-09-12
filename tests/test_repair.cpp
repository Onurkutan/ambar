// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Rebuilding a manifest from the files that survive.
//
// Every test builds a database against a std::map, breaks it in a way that
// open() refuses, repairs it, and asks the same map whether what came back is
// what went in.  Repair is the one operation that runs when the database is
// already in trouble, which is exactly when a quiet wrong answer costs most.

#include "harness.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "testutil.hpp"

#include "../include/ambar/db.hpp"
#include "../src/comparator.hpp"
#include "../src/dbformat.hpp"
#include "../src/filename.hpp"
#include "../src/table_builder.hpp"
#include "../src/version_set.hpp"
#include "../src/wal.hpp"

using namespace ambar;

namespace {

using ambar_test::TempDir;
using Model = std::map<std::string, std::string>;

std::string key_of(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%06d", i);
  return buf;
}

std::string value_of(int i) {
  return "value_" + std::to_string(i) + "_" +
         std::string(static_cast<size_t>(300 + i % 100),
                     static_cast<char>('a' + i % 26));
}

Options small_buffers() {
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 64 << 10;  // several flushes, real compactions
  options.max_file_size = 1 << 20;       // and more than one output table
  return options;
}

// Writes `keys` keys, deletes every fifth, compacts so that most of it lives
// in tables, then writes a final tail with sync so the log holds something
// recovery has to replay.  Closes the database.  The model is what a reader
// should see afterwards.
Model build(const std::string& path, int keys) {
  Model model;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(small_buffers(), path, &db));

  for (int i = 0; i < keys; ++i) {
    CHECK_OK(db->put(WriteOptions(), key_of(i), value_of(i)));
    model[key_of(i)] = value_of(i);
  }
  for (int i = 0; i < keys; i += 5) {
    CHECK_OK(db->del(WriteOptions(), key_of(i)));
    model.erase(key_of(i));
  }
  db->compact_range(nullptr, nullptr);

  WriteOptions durable;
  durable.sync = true;
  for (int i = keys; i < keys + 40; ++i) {
    CHECK_OK(db->put(durable, key_of(i), value_of(i)));
    model[key_of(i)] = value_of(i);
  }
  return model;
}

// Every key in the model reads back with its value; every key deleted from
// the model is absent.  Reports the first disagreement rather than the count.
bool agrees(DB* db, const Model& model, int keys) {
  std::string value;
  for (int i = 0; i < keys + 40; ++i) {
    const std::string key = key_of(i);
    const Status status = db->get(ReadOptions(), key, &value);
    const auto it = model.find(key);
    if (it == model.end()) {
      if (!status.is_not_found()) {
        std::printf("    %s should be absent, got %s\n", key.c_str(),
                    status.to_string().c_str());
        return false;
      }
    } else if (!status.is_ok() || value != it->second) {
      std::printf("    %s: expected the model's value, got %s\n", key.c_str(),
                  status.is_ok() ? "a different value"
                                 : status.to_string().c_str());
      return false;
    }
  }
  return true;
}

std::vector<std::string> files_of_type(const std::string& path, FileType want) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
    uint64_t number = 0;
    FileType type;
    const std::string name = entry.path().filename().string();
    if (parse_file_name(name, &number, &type) && type == want) {
      out.push_back(entry.path().string());
    }
  }
  return out;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool refuses_to_open(const std::string& path) {
  Options options;
  options.create_if_missing = true;  // the setting that used to make it worse
  std::unique_ptr<DB> db;
  return !DB::open(options, path, &db).is_ok();
}

uint64_t last_sequence_of(DB* db) {
  std::string text;
  CHECK(db->get_property("ambar.last-sequence", &text));
  return std::stoull(text);
}

int files_at_level(DB* db, int level) {
  std::string text;
  CHECK(db->get_property("ambar.num-files-at-level" + std::to_string(level),
                         &text));
  return std::stoi(text);
}

// Writes a table file by hand, the way the engine would, holding exactly
// `entries` in the order given -- which must be in order under `comparator`.
void write_table(const std::string& path, uint64_t number,
                 const std::vector<std::pair<std::string, std::string>>& entries,
                 const Comparator* comparator = internal_key_comparator()) {
  std::unique_ptr<WritableFile> file;
  CHECK_OK(WritableFile::open(table_file_name(path, number), /*append=*/false,
                              &file));
  TableBuilder builder(Options(), file.get(), comparator);
  for (const auto& [key, value] : entries) builder.add(key, value);
  CHECK_OK(builder.finish());
  CHECK_OK(file->sync());
  CHECK_OK(file->close());
}

}  // namespace

// The manifest and CURRENT are gone.  Everything the database held is still
// in its tables and its log, and repair has to find all of it.
TEST(repair, rebuilds_a_database_whose_manifest_is_gone) {
  TempDir dir;
  const std::string path = dir.file("db");
  const int keys = 3000;
  const Model model = build(path, keys);

  uint64_t sequence_before = 0;
  {
    std::unique_ptr<DB> db;
    Options options;
    CHECK_OK(DB::open(options, path, &db));
    sequence_before = last_sequence_of(db.get());
  }

  CHECK_OK(remove_file(current_file_name(path)));
  for (const std::string& manifest : files_of_type(path, FileType::kDescriptor)) {
    CHECK_OK(remove_file(manifest));
  }
  CHECK(refuses_to_open(path));

  RepairReport report;
  CHECK_OK(repair_db(path, Options(), &report));
  CHECK(report.tables_kept > 0);
  CHECK_EQ(report.tables_set_aside, 0);
  CHECK_EQ(report.logs_set_aside, 0);
  CHECK(report.tables_written > 0);
  CHECK(report.opened);

  std::unique_ptr<DB> db;
  Options options;
  CHECK_OK(DB::open(options, path, &db));
  CHECK(agrees(db.get(), model, keys));

  // The merged tables sit at the last level, where they cannot overlap and
  // nothing is beneath them, and nothing is at level 0 to be resolved by
  // file number.
  CHECK_EQ(files_at_level(db.get(), 0), 0);
  CHECK_EQ(files_at_level(db.get(), kNumLevels - 1), report.tables_written);

  // Sequence numbers carry on from where they were, not from below: a write
  // that got a number older than an existing entry would lose to it.
  CHECK(last_sequence_of(db.get()) >= sequence_before);
  CHECK_OK(db->put(WriteOptions(), key_of(0), "back"));
  std::string value;
  CHECK_OK(db->get(ReadOptions(), key_of(0), &value));
  CHECK_EQ(value, std::string("back"));
}

// A bit flipped in the middle of the manifest: open refuses, as it should,
// and this is what a person does next.
TEST(repair, repairs_a_manifest_damaged_in_the_middle) {
  TempDir dir;
  const std::string path = dir.file("db");
  const int keys = 2000;
  const Model model = build(path, keys);

  std::string current = read_file(current_file_name(path));
  while (!current.empty() && current.back() == '\n') current.pop_back();
  const std::string manifest = path + "/" + current;
  std::string bytes = read_file(manifest);
  const size_t first_length = static_cast<size_t>(
      static_cast<unsigned char>(bytes[4]) |
      (static_cast<unsigned char>(bytes[5]) << 8));
  const size_t offset = kHeaderSize + first_length + kHeaderSize + 1;
  CHECK(offset + 64 < bytes.size());
  bytes[offset] = static_cast<char>(bytes[offset] ^ 0x40);
  write_file(manifest, bytes);
  CHECK(refuses_to_open(path));

  RepairReport report;
  CHECK_OK(repair_db(path, Options(), &report));

  std::unique_ptr<DB> db;
  Options options;
  CHECK_OK(DB::open(options, path, &db));
  CHECK(agrees(db.get(), model, keys));
}

// One table file is noise.  Repair keeps the others, moves that one aside
// rather than deleting it, and the database opens with its keys gone and
// every other key right.
TEST(repair, sets_aside_a_table_that_will_not_read_and_keeps_the_rest) {
  TempDir dir;
  const std::string path = dir.file("db");
  const int keys = 5000;  // over a megabyte of values: at least two tables
  const Model model = build(path, keys);

  const std::vector<std::string> tables = files_of_type(path, FileType::kTable);
  CHECK(tables.size() >= 2);
  const std::string victim = tables[tables.size() / 2];
  write_file(victim, std::string(4096, '\x5a'));
  CHECK_OK(remove_file(current_file_name(path)));

  RepairReport report;
  CHECK_OK(repair_db(path, Options(), &report));
  CHECK_EQ(report.tables_set_aside, 1);
  CHECK(std::filesystem::exists(path + "/lost/" +
                                std::filesystem::path(victim).filename().string()));
  CHECK(!std::filesystem::exists(victim));

  std::unique_ptr<DB> db;
  Options options;
  CHECK_OK(DB::open(options, path, &db));

  // Nothing wrong comes back: every key present has the model's value.
  int missing = 0;
  std::string value;
  for (const auto& [key, expected] : model) {
    const Status status = db->get(ReadOptions(), key, &value);
    if (status.is_not_found()) {
      ++missing;
      continue;
    }
    CHECK_OK(status);
    CHECK(value == expected);
  }
  std::printf("    %d of %zu keys were in the table that was set aside\n",
              missing, model.size());
  CHECK(missing > 0);
}

// The log is what holds the newest writes, and a torn one -- the shape a crash
// leaves -- is replayed as far as it goes and then set aside, not skipped.
TEST(repair, replays_the_log_and_sets_a_torn_one_aside) {
  TempDir dir;
  const std::string path = dir.file("db");
  const int keys = 500;
  const Model model = build(path, keys);

  const std::vector<std::string> logs = files_of_type(path, FileType::kLog);
  CHECK_EQ(logs.size(), size_t{1});
  if (logs.empty()) return;
  const std::string log = logs[0];
  const std::string bytes = read_file(log);
  CHECK(bytes.size() > 100);
  // Cut inside the last record: a torn tail.
  write_file(log, bytes.substr(0, bytes.size() - 3));
  CHECK_OK(remove_file(current_file_name(path)));

  RepairReport report;
  CHECK_OK(repair_db(path, Options(), &report));
  CHECK_EQ(report.logs_converted, 1);
  CHECK_EQ(report.logs_set_aside, 1);
  CHECK(std::filesystem::exists(path + "/lost/" +
                                std::filesystem::path(log).filename().string()));

  std::unique_ptr<DB> db;
  Options options;
  CHECK_OK(DB::open(options, path, &db));

  // Everything but the torn record: the last key written is the one missing.
  std::string value;
  CHECK_OK(db->get(ReadOptions(), key_of(keys), &value));
  CHECK(value == model.at(key_of(keys)));
  CHECK_OK(db->get(ReadOptions(), key_of(keys + 38), &value));
  CHECK(db->get(ReadOptions(), key_of(keys + 39), &value).is_not_found());
}

// Repair takes the database lock, so it cannot run under an open database.
//
// On POSIX that needs a second process: record locks are held per process,
// so a second lock taken from this one succeeds by design (file.hpp says so,
// and test_db.cpp forks for the same reason).  Windows locks the file
// exclusively, so there the check holds in-process.
TEST(repair, refuses_while_the_database_is_open) {
  TempDir dir;
  const std::string path = dir.file("db");
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(small_buffers(), path, &db));
  CHECK_OK(db->put(WriteOptions(), "k", "v"));

#if defined(_WIN32)
  RepairReport report;
  CHECK(!repair_db(path, Options(), &report).is_ok());
#else
  const pid_t child = ::fork();
  if (child < 0) {
    std::printf("    fork failed; the test cannot run here\n");
    return;
  }
  if (child == 0) {
    RepairReport report;
    const Status status = repair_db(path, Options(), &report);
    ::_exit(status.is_ok() ? 1 : 0);  // 1: it got in, which is the failure
  }
  int wait_status = 0;
  ::waitpid(child, &wait_status, 0);
  CHECK(WIFEXITED(wait_status));
  CHECK_EQ(WEXITSTATUS(wait_status), 0);
#endif
}

// A compaction output and the input it was made from, both on disk: the same
// entries twice.  The engine's own merge assumes that cannot happen; repair's
// must not, and must not write a key twice either -- the table builder
// refuses a key that does not strictly follow the last.
TEST(repair, collapses_a_duplicated_table) {
  TempDir dir;
  const std::string path = dir.file("db");
  const int keys = 2000;
  const Model model = build(path, keys);

  const std::vector<std::string> tables = files_of_type(path, FileType::kTable);
  CHECK(!tables.empty());
  std::error_code ec;
  std::filesystem::copy_file(tables[0], table_file_name(path, 900000), ec);
  CHECK(!ec);
  CHECK_OK(remove_file(current_file_name(path)));

  RepairReport report;
  CHECK_OK(repair_db(path, Options(), &report));

  std::unique_ptr<DB> db;
  Options options;
  CHECK_OK(DB::open(options, path, &db));
  CHECK(agrees(db.get(), model, keys));

  size_t seen = 0;
  std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
  for (iter->seek_to_first(); iter->valid(); iter->next()) ++seen;
  CHECK_OK(iter->status());
  CHECK_EQ(seen, model.size());
}

// The reason repair merges rather than naming the survivors at level 0.  A
// lookup among overlapping level-0 files trusts the file number as age, and
// a compaction output carries a newer number than a flush that came before
// it while holding older versions of the same keys.  Two tables built by
// hand: the lower number holds the newer versions.
TEST(repair, takes_the_newest_version_regardless_of_file_number) {
  TempDir dir;
  const std::string path = dir.file("db");
  std::filesystem::create_directories(path);

  write_table(path, 10, {
      {make_internal_key("alpha", 50, ValueType::kValue), "newest"},
      {make_internal_key("beta", 60, ValueType::kDeletion), ""},
  });
  write_table(path, 20, {
      {make_internal_key("alpha", 30, ValueType::kValue), "older"},
      {make_internal_key("beta", 40, ValueType::kValue), "back from the dead"},
      {make_internal_key("gamma", 45, ValueType::kValue), "only here"},
  });

  RepairReport report;
  CHECK_OK(repair_db(path, Options(), &report));
  CHECK_EQ(report.tables_kept, 2);
  CHECK_EQ(report.last_sequence, uint64_t{60});

  std::unique_ptr<DB> db;
  Options options;
  CHECK_OK(DB::open(options, path, &db));
  std::string value;
  CHECK_OK(db->get(ReadOptions(), "alpha", &value));
  CHECK_EQ(value, std::string("newest"));
  CHECK(db->get(ReadOptions(), "beta", &value).is_not_found());
  CHECK_OK(db->get(ReadOptions(), "gamma", &value));
  CHECK_EQ(value, std::string("only here"));

  // And a write made now is newer than anything that was there.
  CHECK_OK(db->put(WriteOptions(), "alpha", "newer still"));
  CHECK_OK(db->get(ReadOptions(), "alpha", &value));
  CHECK_EQ(value, std::string("newer still"));
}

// A table whose blocks pass their checksums but whose keys are out of order
// under the internal comparator -- written under a bug, or by hand with the
// wrong comparator, as here -- would put the merge's output out of order.
// It is set aside like any other table that does not read.
TEST(repair, sets_aside_a_table_whose_keys_are_out_of_order) {
  TempDir dir;
  const std::string path = dir.file("db");
  std::filesystem::create_directories(path);

  write_table(path, 10, {
      {make_internal_key("k", 7, ValueType::kValue), "good"},
  });
  // Under the bytewise comparator the older version sorts first; under the
  // internal one it must come second.
  write_table(path, 20, {
      {make_internal_key("m", 5, ValueType::kValue), "older"},
      {make_internal_key("m", 9, ValueType::kValue), "newer"},
  }, bytewise_comparator());

  RepairReport report;
  CHECK_OK(repair_db(path, Options(), &report));
  CHECK_EQ(report.tables_kept, 1);
  CHECK_EQ(report.tables_set_aside, 1);
  CHECK(std::filesystem::exists(path + "/lost/000020.sst"));

  std::unique_ptr<DB> db;
  Options options;
  CHECK_OK(DB::open(options, path, &db));
  std::string value;
  CHECK_OK(db->get(ReadOptions(), "k", &value));
  CHECK_EQ(value, std::string("good"));
  CHECK(db->get(ReadOptions(), "m", &value).is_not_found());
}

// Nothing to repair is an error, not an empty database.
TEST(repair, refuses_a_directory_with_nothing_in_it) {
  TempDir dir;
  const std::string path = dir.file("empty");
  std::filesystem::create_directories(path);
  RepairReport report;
  CHECK(repair_db(path, Options(), &report).is_invalid_argument());
  CHECK(!file_exists(current_file_name(path)));
}
