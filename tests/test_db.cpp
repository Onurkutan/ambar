// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// End to end, against a std::map.
//
// Every component below this has its own tests, and passing all of them is not
// evidence that the engine is correct: the bugs that survive component tests
// are the ones that live between components -- a flush that drops a key the
// log already forgot, a compaction that resurrects a delete, a snapshot that
// starts seeing writes it should not.
//
// So these tests run real workloads against a real database and compare every
// answer with a std::map that received the same operations.  A disagreement is
// a bug, and the test says which key and what each side thinks.

#include "harness.hpp"

#include <filesystem>
#include <fstream>
#include <cstdio>
#include <map>
#include <memory>
#include <atomic>
#include <random>
#include <thread>

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <string>
#include <vector>

#include "testutil.hpp"

#include "../include/ambar/db.hpp"
#include "../include/ambar/filter_policy.hpp"

using namespace ambar;

namespace {

using ambar_test::TempDir;

Options test_options() {
  Options options;
  options.create_if_missing = true;
  // Small, so that a few thousand keys produce real flushes and real
  // compactions rather than sitting in one memtable and testing nothing.
  options.write_buffer_size = 64 << 10;
  options.max_file_size = 1 << 20;
  options.block_size = 1024;
  return options;
}

std::string key_of(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%08d", i);
  return buf;
}

std::string value_of(int i, size_t length = 40) {
  std::string base = "value_" + std::to_string(i) + "_";
  while (base.size() < length) base += "xyzzy";
  return base.substr(0, length);
}

// Asks the database for every key the model holds, and for a spread of keys it
// does not.  Reports the first disagreement in full.
bool agrees_with(DB* db, const std::map<std::string, std::string>& model,
                 const char* where) {
  std::string value;
  for (const auto& [key, expected] : model) {
    const Status status = db->get(ReadOptions(), key, &value);
    if (!status.is_ok()) {
      std::printf("    [%s] get(%s) -> %s, expected the value\n", where,
                  key.c_str(), status.to_string().c_str());
      return false;
    }
    if (value != expected) {
      std::printf("    [%s] get(%s) -> \"%s\", expected \"%s\"\n", where,
                  key.c_str(), value.c_str(), expected.c_str());
      return false;
    }
  }

  for (int i = 0; i < 500; ++i) {
    const std::string absent = key_of(900000 + i);
    if (model.count(absent) > 0) continue;
    const Status status = db->get(ReadOptions(), absent, &value);
    if (!status.is_not_found()) {
      std::printf("    [%s] get(%s) -> %s, expected not-found\n", where,
                  absent.c_str(), status.to_string().c_str());
      return false;
    }
  }
  return true;
}

bool scan_agrees_with(DB* db, const std::map<std::string, std::string>& model,
                      const char* where) {
  std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));

  auto expected = model.begin();
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    if (expected == model.end()) {
      std::printf("    [%s] scan returned extra key \"%s\"\n", where,
                  std::string(iter->key()).c_str());
      return false;
    }
    if (iter->key() != std::string_view(expected->first) ||
        iter->value() != std::string_view(expected->second)) {
      std::printf("    [%s] scan: got %s=%s, expected %s=%s\n", where,
                  std::string(iter->key()).c_str(),
                  std::string(iter->value()).c_str(), expected->first.c_str(),
                  expected->second.c_str());
      return false;
    }
    ++expected;
  }
  if (expected != model.end()) {
    std::printf("    [%s] scan stopped early; %s was missing\n", where,
                expected->first.c_str());
    return false;
  }
  if (!iter->status().is_ok()) {
    std::printf("    [%s] scan status: %s\n", where,
                iter->status().to_string().c_str());
    return false;
  }
  return true;
}

// Opens, or reports why and gives the caller a way to stop.  A test that keeps
// going after a failed open dereferences a null pointer, and the crash hides
// the status that explained everything.
bool open_db(const Options& options, const std::string& path,
             std::unique_ptr<DB>* db, const char* where) {
  const Status status = DB::open(options, path, db);
  if (!status.is_ok()) {
    std::printf("    [%s] open failed: %s\n", where,
                status.to_string().c_str());
    return false;
  }
  return true;
}

}  // namespace

TEST(db, opens_writes_and_reads_back) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));

  CHECK_OK(db->put(WriteOptions(), "hello", "world"));

  std::string value;
  CHECK_OK(db->get(ReadOptions(), "hello", &value));
  CHECK_EQ(value, "world");

  CHECK(db->get(ReadOptions(), "absent", &value).is_not_found());
}

TEST(db, refuses_to_create_when_not_asked) {
  TempDir dir;
  Options options;
  options.create_if_missing = false;
  std::unique_ptr<DB> db;
  CHECK(!DB::open(options, dir.file("missing"), &db).is_ok());
}

TEST(db, a_delete_hides_the_value_underneath) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));

  CHECK_OK(db->put(WriteOptions(), "k", "v1"));
  CHECK_OK(db->del(WriteOptions(), "k"));

  std::string value;
  CHECK(db->get(ReadOptions(), "k", &value).is_not_found());

  // And writing it again brings it back.
  CHECK_OK(db->put(WriteOptions(), "k", "v2"));
  CHECK_OK(db->get(ReadOptions(), "k", &value));
  CHECK_EQ(value, "v2");
}

TEST(db, deleting_a_key_that_was_never_written_is_not_an_error) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));
  CHECK_OK(db->del(WriteOptions(), "never_written"));
}

TEST(db, a_batch_applies_every_operation) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));

  CHECK_OK(db->put(WriteOptions(), "a", "1"));
  WriteBatch batch;
  batch.put("b", "2");
  batch.del("a");
  batch.put("c", "3");
  CHECK_OK(db->write(WriteOptions(), &batch));

  std::string value;
  CHECK(db->get(ReadOptions(), "a", &value).is_not_found());
  CHECK_OK(db->get(ReadOptions(), "b", &value));
  CHECK_EQ(value, "2");
  CHECK_OK(db->get(ReadOptions(), "c", &value));
  CHECK_EQ(value, "3");
}

TEST(db, empty_keys_and_values_and_binary_data_survive) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));

  const std::string binary_key("a\0b\xff", 4);
  const std::string binary_value("1\000" "2\xfe", 4);  // split: see C4125

  CHECK_OK(db->put(WriteOptions(), "", "empty key"));
  CHECK_OK(db->put(WriteOptions(), "empty value", ""));
  CHECK_OK(db->put(WriteOptions(), binary_key, binary_value));

  std::string value;
  CHECK_OK(db->get(ReadOptions(), "", &value));
  CHECK_EQ(value, "empty key");
  CHECK_OK(db->get(ReadOptions(), "empty value", &value));
  CHECK_EQ(value, "");
  CHECK_OK(db->get(ReadOptions(), binary_key, &value));
  CHECK(value == binary_value);
}

// ----------------------------------------------------- against a model -----

// Enough writes to force flushes and compactions, then every key checked.
TEST(db, agrees_with_a_map_across_flushes_and_compactions) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));

  std::map<std::string, std::string> model;
  std::mt19937 rng(20260907);

  for (int i = 0; i < 30000; ++i) {
    const int which = static_cast<int>(rng() % 100);
    const std::string key = key_of(static_cast<int>(rng() % 8000));

    if (which < 70) {
      const std::string value = value_of(i);
      CHECK_OK(db->put(WriteOptions(), key, value));
      model[key] = value;
    } else if (which < 90) {
      CHECK_OK(db->del(WriteOptions(), key));
      model.erase(key);
    } else {
      // A batch, so the group-commit path is exercised alongside the rest.
      WriteBatch batch;
      for (int j = 0; j < 5; ++j) {
        const std::string k = key_of(static_cast<int>(rng() % 8000));
        const std::string v = value_of(i * 10 + j);
        batch.put(k, v);
        model[k] = v;
      }
      CHECK_OK(db->write(WriteOptions(), &batch));
    }
  }

  std::string files;
  db->get_property("ambar.stats", &files);
  std::printf("    after %zu live keys:\n%s", model.size(), files.c_str());

  CHECK(agrees_with(db.get(), model, "after writes"));
  CHECK(scan_agrees_with(db.get(), model, "after writes"));
}

// The same, but reopened first: everything must survive a close and an open,
// which means the log replayed correctly and the manifest named every file.
TEST(db, everything_survives_a_reopen) {
  TempDir dir;
  const std::string path = dir.file("db");
  std::map<std::string, std::string> model;
  std::mt19937 rng(11);

  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::open(test_options(), path, &db));
    for (int i = 0; i < 20000; ++i) {
      const std::string key = key_of(static_cast<int>(rng() % 5000));
      if (rng() % 5 == 0) {
        CHECK_OK(db->del(WriteOptions(), key));
        model.erase(key);
      } else {
        const std::string value = value_of(i);
        CHECK_OK(db->put(WriteOptions(), key, value));
        model[key] = value;
      }
    }
  }

  {
    std::unique_ptr<DB> db;
    if (!open_db(test_options(), path, &db, "reopen")) {
      CHECK(false);
      return;
    }
    CHECK(agrees_with(db.get(), model, "after reopen"));
    CHECK(scan_agrees_with(db.get(), model, "after reopen"));
  }

  // And again, to catch anything that only breaks on the second recovery --
  // a log that is replayed but not retired, say.
  {
    std::unique_ptr<DB> db;
    if (!open_db(test_options(), path, &db, "second reopen")) {
      CHECK(false);
      return;
    }
    CHECK(agrees_with(db.get(), model, "after second reopen"));
  }
}

// Reopening many times with a few writes each time is the shape that finds
// recovery bugs: each open leaves a log, a manifest edit, and possibly a
// flush, and the mistakes compound.
TEST(db, survives_many_small_sessions) {
  TempDir dir;
  const std::string path = dir.file("db");
  std::map<std::string, std::string> model;
  std::mt19937 rng(77);

  for (int session = 0; session < 30; ++session) {
    std::unique_ptr<DB> db;
    if (!open_db(test_options(), path, &db, "session")) {
      std::printf("    session %d could not open\n", session);
      CHECK(false);
      return;
    }

    if (!agrees_with(db.get(), model, "session start")) {
      std::printf("    disagreement at the start of session %d\n", session);
      CHECK(false);
      return;
    }

    for (int i = 0; i < 400; ++i) {
      const std::string key = key_of(static_cast<int>(rng() % 1500));
      if (rng() % 4 == 0) {
        CHECK_OK(db->del(WriteOptions(), key));
        model.erase(key);
      } else {
        const std::string value = value_of(session * 1000 + i);
        CHECK_OK(db->put(WriteOptions(), key, value));
        model[key] = value;
      }
    }
  }
}

// --------------------------------------------------------- snapshots -------

TEST(db, a_snapshot_does_not_see_later_writes) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));

  CHECK_OK(db->put(WriteOptions(), "k", "before"));
  const Snapshot* snapshot = db->get_snapshot();
  CHECK_OK(db->put(WriteOptions(), "k", "after"));
  CHECK_OK(db->put(WriteOptions(), "new", "value"));
  CHECK_OK(db->del(WriteOptions(), "k"));

  ReadOptions at_snapshot;
  at_snapshot.snapshot = snapshot;

  std::string value;
  CHECK_OK(db->get(at_snapshot, "k", &value));
  CHECK_EQ(value, "before");
  CHECK(db->get(at_snapshot, "new", &value).is_not_found());

  // And the current state is unaffected by the snapshot existing.
  CHECK(db->get(ReadOptions(), "k", &value).is_not_found());
  CHECK_OK(db->get(ReadOptions(), "new", &value));

  db->release_snapshot(snapshot);
}

// The test that proves compaction respects snapshots.  Enough writing happens
// after the snapshot to force the old versions through a compaction, which
// would drop them if the smallest-snapshot check were missing.
TEST(db, a_snapshot_survives_compaction_of_the_versions_it_sees) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));

  std::map<std::string, std::string> at_snapshot_model;
  for (int i = 0; i < 2000; ++i) {
    const std::string key = key_of(i);
    const std::string value = value_of(i);
    CHECK_OK(db->put(WriteOptions(), key, value));
    at_snapshot_model[key] = value;
  }

  const Snapshot* snapshot = db->get_snapshot();

  // Overwrite every key, delete half of them, and write enough to compact.
  for (int i = 0; i < 2000; ++i) {
    CHECK_OK(db->put(WriteOptions(), key_of(i), "overwritten"));
  }
  for (int i = 0; i < 2000; i += 2) {
    CHECK_OK(db->del(WriteOptions(), key_of(i)));
  }
  for (int i = 0; i < 20000; ++i) {
    CHECK_OK(db->put(WriteOptions(), key_of(100000 + i), value_of(i)));
  }
  db->compact_range(nullptr, nullptr);

  ReadOptions at_snapshot;
  at_snapshot.snapshot = snapshot;

  std::string value;
  for (const auto& [key, expected] : at_snapshot_model) {
    const Status status = db->get(at_snapshot, key, &value);
    if (!status.is_ok() || value != expected) {
      std::printf("    snapshot lost %s: %s (\"%s\", expected \"%s\")\n",
                  key.c_str(), status.to_string().c_str(), value.c_str(),
                  expected.c_str());
      CHECK(false);
      db->release_snapshot(snapshot);
      return;
    }
  }
  db->release_snapshot(snapshot);
}

// --------------------------------------------------------- iteration -------

TEST(db, iteration_skips_deleted_keys_and_shows_the_newest_value) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));

  std::map<std::string, std::string> model;
  for (int i = 0; i < 3000; ++i) {
    CHECK_OK(db->put(WriteOptions(), key_of(i), value_of(i)));
    model[key_of(i)] = value_of(i);
  }
  for (int i = 0; i < 3000; i += 3) {
    CHECK_OK(db->del(WriteOptions(), key_of(i)));
    model.erase(key_of(i));
  }
  for (int i = 1; i < 3000; i += 7) {
    CHECK_OK(db->put(WriteOptions(), key_of(i), "rewritten"));
    model[key_of(i)] = "rewritten";
  }

  CHECK(scan_agrees_with(db.get(), model, "mixed"));
}

TEST(db, reverse_iteration_matches_forward_iteration_reversed) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));

  std::map<std::string, std::string> model;
  std::mt19937 rng(5);
  for (int i = 0; i < 5000; ++i) {
    const std::string key = key_of(static_cast<int>(rng() % 3000));
    if (rng() % 4 == 0) {
      CHECK_OK(db->del(WriteOptions(), key));
      model.erase(key);
    } else {
      CHECK_OK(db->put(WriteOptions(), key, value_of(i)));
      model[key] = value_of(i);
    }
  }

  std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
  auto expected = model.rbegin();
  for (iter->seek_to_last(); iter->valid(); iter->prev()) {
    if (expected == model.rend()) {
      std::printf("    reverse scan returned extra key %s\n",
                  std::string(iter->key()).c_str());
      CHECK(false);
      return;
    }
    if (iter->key() != std::string_view(expected->first) ||
        iter->value() != std::string_view(expected->second)) {
      std::printf("    reverse scan: got %s=%s, expected %s=%s\n",
                  std::string(iter->key()).c_str(),
                  std::string(iter->value()).c_str(), expected->first.c_str(),
                  expected->second.c_str());
      CHECK(false);
      return;
    }
    ++expected;
  }
  CHECK(expected == model.rend());
}

TEST(db, seek_finds_the_first_key_at_or_after_the_target) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));

  std::map<std::string, std::string> model;
  for (int i = 0; i < 4000; i += 2) {
    CHECK_OK(db->put(WriteOptions(), key_of(i), value_of(i)));
    model[key_of(i)] = value_of(i);
  }

  std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
  for (int i = 0; i < 4000; ++i) {
    const std::string probe = key_of(i);
    iter->seek(probe);
    const auto expected = model.lower_bound(probe);
    if (expected == model.end()) {
      CHECK(!iter->valid());
    } else {
      CHECK(iter->valid());
      if (iter->key() != std::string_view(expected->first)) {
        std::printf("    seek(%s) -> %s, expected %s\n", probe.c_str(),
                    std::string(iter->key()).c_str(), expected->first.c_str());
        CHECK(false);
        return;
      }
    }
  }
}

// ------------------------------------------------------- with a filter -----

TEST(db, works_the_same_with_a_bloom_filter_configured) {
  const auto policy = new_bloom_filter_policy(10);
  Options options = test_options();
  options.filter_policy = policy.get();

  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(options, dir.file("db"), &db));

  std::map<std::string, std::string> model;
  std::mt19937 rng(13);
  for (int i = 0; i < 20000; ++i) {
    const std::string key = key_of(static_cast<int>(rng() % 6000));
    if (rng() % 5 == 0) {
      CHECK_OK(db->del(WriteOptions(), key));
      model.erase(key);
    } else {
      CHECK_OK(db->put(WriteOptions(), key, value_of(i)));
      model[key] = value_of(i);
    }
  }
  db->compact_range(nullptr, nullptr);

  CHECK(agrees_with(db.get(), model, "with a filter"));
  CHECK(scan_agrees_with(db.get(), model, "with a filter"));
}

TEST(db, large_values_survive_flush_and_compaction) {
  TempDir dir;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(test_options(), dir.file("db"), &db));

  std::map<std::string, std::string> model;
  for (int i = 0; i < 200; ++i) {
    // Each value is larger than a block and several are larger than the write
    // buffer, so a single value spans blocks and forces its own flush.
    const std::string value(static_cast<size_t>(100000 + i),
                            static_cast<char>('a' + i % 26));
    CHECK_OK(db->put(WriteOptions(), key_of(i), value));
    model[key_of(i)] = value;
  }
  db->compact_range(nullptr, nullptr);

  std::string value;
  for (const auto& [key, expected] : model) {
    CHECK_OK(db->get(ReadOptions(), key, &value));
    if (value != expected) {
      std::printf("    %s came back %zu bytes, expected %zu\n", key.c_str(),
                  value.size(), expected.size());
      CHECK(false);
      return;
    }
  }
}

TEST(db, destroy_removes_the_database) {
  TempDir dir;
  const std::string path = dir.file("db");
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::open(test_options(), path, &db));
    CHECK_OK(db->put(WriteOptions(), "k", "v"));
  }
  CHECK_OK(destroy_db(path, Options()));

  Options options;
  options.create_if_missing = false;
  std::unique_ptr<DB> db;
  CHECK(!DB::open(options, path, &db).is_ok());
}

// --------------------------------------------------------- concurrency -----
//
// Everything above runs the database from one thread, with the background
// compaction thread as the only other participant.  That is not the claim the
// engine makes: it says many threads may read and write at once, and a claim
// nobody exercises is a claim nobody has checked.
//
// These tests are written to be run under ThreadSanitizer as well as normally.
// TSan is what makes them worth having -- a race that corrupts a reference
// count shows up as a crash once in a thousand runs and as a report every time.
// Two real races were found this way: an iterator releasing its version
// reference without the database lock, and the version list being unlinked
// while a compaction walked it.

TEST(db, many_writers_and_readers_at_once) {
  TempDir dir;
  std::unique_ptr<DB> db;
  if (!open_db(test_options(), dir.file("db"), &db, "concurrent")) {
    CHECK(false);
    return;
  }

  constexpr int kWriters = 4;
  constexpr int kReaders = 3;
  constexpr int kPerWriter = 4000;

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};

  std::vector<std::thread> threads;

  // Each writer owns a disjoint slice of the key space, so what the readers
  // check is unambiguous: a key that exists must hold exactly the value its
  // writer wrote, whatever order things happened in.
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        const int id = w * 1000000 + i;
        if (!db->put(WriteOptions(), key_of(id), value_of(id)).is_ok()) {
          ++failures;
          return;
        }
        if (i % 500 == 499) {
          // A batch as well, so the group-commit path runs with several
          // threads queued behind each other -- which is the point of it.
          WriteBatch batch;
          for (int j = 0; j < 10; ++j) {
            const int bid = w * 1000000 + 500000 + i + j;
            batch.put(key_of(bid), value_of(bid));
          }
          if (!db->write(WriteOptions(), &batch).is_ok()) {
            ++failures;
            return;
          }
        }
      }
    });
  }

  // Readers do point lookups and full scans while all of that is going on.  A
  // scan holds a snapshot and a set of file references, so creating and
  // destroying iterators under a running compaction is exactly the situation
  // the reference counting exists for.
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r] {
      std::mt19937 rng(static_cast<unsigned>(100 + r));
      std::string value;
      while (!stop.load(std::memory_order_acquire)) {
        const int w = static_cast<int>(rng() % kWriters);
        const int i = static_cast<int>(rng() % kPerWriter);
        const int id = w * 1000000 + i;

        const Status status = db->get(ReadOptions(), key_of(id), &value);
        if (status.is_ok()) {
          if (value != value_of(id)) {
            std::printf("    reader saw a value that was never written for %s\n",
                        key_of(id).c_str());
            ++failures;
            return;
          }
        } else if (!status.is_not_found()) {
          std::printf("    reader failed: %s\n", status.to_string().c_str());
          ++failures;
          return;
        }

        if (rng() % 64 == 0) {
          std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
          int seen = 0;
          for (iter->seek_to_first(); iter->valid() && seen < 500;
               iter->next()) {
            ++seen;
          }
          if (!iter->status().is_ok()) {
            std::printf("    scan failed: %s\n",
                        iter->status().to_string().c_str());
            ++failures;
            return;
          }
        }
      }
    });
  }

  for (int i = 0; i < kWriters; ++i) threads[static_cast<size_t>(i)].join();
  stop.store(true, std::memory_order_release);
  for (size_t i = kWriters; i < threads.size(); ++i) threads[i].join();

  CHECK_EQ(failures.load(), 0);

  // And everything each writer wrote is there afterwards.
  std::string value;
  for (int w = 0; w < kWriters; ++w) {
    for (int i = 0; i < kPerWriter; i += 37) {
      const int id = w * 1000000 + i;
      const Status status = db->get(ReadOptions(), key_of(id), &value);
      if (!status.is_ok() || value != value_of(id)) {
        std::printf("    %s missing or wrong after the run: %s\n",
                    key_of(id).c_str(), status.to_string().c_str());
        CHECK(false);
        return;
      }
    }
  }
}

// Snapshots taken and released from several threads while compaction runs.
// The oldest live snapshot is what stops a compaction dropping versions, and
// it is shared mutable state that every one of these threads touches.
TEST(db, snapshots_taken_concurrently_stay_consistent) {
  TempDir dir;
  std::unique_ptr<DB> db;
  if (!open_db(test_options(), dir.file("db"), &db, "snapshots")) {
    CHECK(false);
    return;
  }

  for (int i = 0; i < 3000; ++i) {
    CHECK_OK(db->put(WriteOptions(), key_of(i), value_of(i)));
  }

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;

  // A writer that keeps overwriting, so compaction has versions to drop.
  threads.emplace_back([&] {
    for (int round = 0; round < 8 && !stop.load(); ++round) {
      for (int i = 0; i < 3000; ++i) {
        db->put(WriteOptions(), key_of(i), "round" + std::to_string(round));
      }
    }
    stop.store(true, std::memory_order_release);
  });

  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t));
      std::string value;
      while (!stop.load(std::memory_order_acquire)) {
        const Snapshot* snapshot = db->get_snapshot();

        // Whatever the value is at this instant, it must not change while the
        // snapshot is held -- that is the entire promise.
        const int id = static_cast<int>(rng() % 3000);
        ReadOptions options;
        options.snapshot = snapshot;

        std::string first;
        const Status status = db->get(options, key_of(id), &first);
        if (status.is_ok()) {
          for (int check = 0; check < 20; ++check) {
            if (!db->get(options, key_of(id), &value).is_ok() ||
                value != first) {
              std::printf("    a snapshot's view of %s changed under it\n",
                          key_of(id).c_str());
              ++failures;
              db->release_snapshot(snapshot);
              return;
            }
          }
        }
        db->release_snapshot(snapshot);
      }
    });
  }

  for (auto& thread : threads) thread.join();
  CHECK_EQ(failures.load(), 0);
}

// ------------------------------------------------------------- the lock ----

#if !defined(_WIN32)

// Two processes must not be able to open the same directory.
//
// This one needs a real second process, and there is no way around it: POSIX
// record locks are held per process, so a second lock taken from *this* process
// succeeds by design. The engine says as much in file.hpp — the lock stops
// another process, not another DB object inside this one.
//
// Before the lock existed, two processes each writing 6,000 keys destroyed
// 11,032 of the 12,000 acknowledged writes between them, and both reported
// success throughout. That is the failure this test exists to prevent coming
// back.
TEST(db, a_second_process_cannot_open_the_same_database) {
  TempDir dir;
  const std::string path = dir.file("db");

  std::unique_ptr<DB> db;
  if (!open_db(test_options(), path, &db, "first")) {
    CHECK(false);
    return;
  }
  CHECK_OK(db->put(WriteOptions(), "k", "v"));

  // The child tries to open the same directory and reports what happened
  // through its exit status: 0 if it was refused, 1 if it got in.
  const pid_t child = ::fork();
  if (child < 0) {
    std::printf("    fork failed; the test cannot run here\n");
    return;
  }
  if (child == 0) {
    Options options = test_options();
    options.create_if_missing = false;
    std::unique_ptr<DB> second;
    const Status status = DB::open(options, path, &second);
    ::_exit(status.is_ok() ? 1 : 0);
  }

  int wait_status = 0;
  ::waitpid(child, &wait_status, 0);
  CHECK(WIFEXITED(wait_status));
  if (WEXITSTATUS(wait_status) != 0) {
    std::printf("    a second process opened a database that was already open\n");
    CHECK(false);
    return;
  }

  // And the lock is released when the database is closed, so the directory can
  // be reopened afterwards -- a lock that outlived the process would be worse
  // than none.
  db.reset();

  std::unique_ptr<DB> reopened;
  Options options = test_options();
  options.create_if_missing = false;
  CHECK_OK(DB::open(options, path, &reopened));

  std::string value;
  CHECK_OK(reopened->get(ReadOptions(), "k", &value));
  CHECK_EQ(value, "v");
}

#endif  // !_WIN32

// ------------------------------------------------- what cleanup may delete ---
//
// Two bugs lived here, both of the same shape: a file the database still needs
// looked obsolete to the cleanup that runs on every open. Neither showed up in
// a single session -- the database worked perfectly until the process ended --
// and one of them made it never open again.

// The manifest CURRENT points at must survive every open.
//
// The bug: when recovery reused an existing manifest, it set the "next
// manifest number" to a *new* number rather than the one it had reused. The
// cleanup deletes any manifest below that number, so it deleted the live one.
// The database ran fine for as long as the process lived, and then refused to
// open, with every table file intact.
TEST(db, cleanup_never_deletes_the_manifest_current_names) {
  TempDir dir;
  const std::string path = dir.file("db");

  // Several sessions, because the bug needed a *reuse* of an existing
  // manifest, which only happens on the second and later opens.
  for (int session = 0; session < 6; ++session) {
    std::unique_ptr<DB> db;
    if (!open_db(test_options(), path, &db, "session")) {
      std::printf("    session %d could not open\n", session);
      CHECK(false);
      return;
    }
    for (int i = 0; i < 300; ++i) {
      CHECK_OK(db->put(WriteOptions(), key_of(session * 1000 + i),
                       value_of(i)));
    }
    db.reset();

    // Whatever CURRENT names has to be on disk.  Reading it directly rather
    // than asking the engine: the engine is the thing under test.
    std::string current;
    {
      std::ifstream in(path + "/CURRENT", std::ios::binary);
      std::getline(in, current);
    }
    if (current.empty()) {
      std::printf("    session %d left CURRENT empty or missing\n", session);
      CHECK(false);
      return;
    }
    if (!std::filesystem::exists(path + "/" + current)) {
      std::printf("    after session %d, CURRENT names %s which is gone\n",
                  session, current.c_str());
      CHECK(false);
      return;
    }
  }
}

// Opening and closing without writing anything must not leak a log file.
//
// The bug: a new log is created on every open, but the version edit recording
// it was only written when recovery had decided the manifest needed rewriting.
// With nothing to replay it had not, so the manifest's log number stayed at its
// old value and every previous log looked live forever -- one leaked file per
// open, and a growing replay at every open after that.
//
// Recovery stayed correct throughout, which is why nothing else noticed: a
// stale-low log number over-replays rather than under-replays.
TEST(db, opening_without_writing_does_not_leak_log_files) {
  TempDir dir;
  const std::string path = dir.file("db");

  auto count_logs = [&path] {
    int logs = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.size() > 4 && name.substr(name.size() - 4) == ".log") ++logs;
    }
    return logs;
  };

  {
    std::unique_ptr<DB> db;
    if (!open_db(test_options(), path, &db, "initial")) {
      CHECK(false);
      return;
    }
    CHECK_OK(db->put(WriteOptions(), "k", "v"));
  }

  for (int i = 0; i < 8; ++i) {
    std::unique_ptr<DB> db;
    if (!open_db(test_options(), path, &db, "reopen")) {
      CHECK(false);
      return;
    }
    db.reset();

    const int logs = count_logs();
    if (logs > 2) {
      std::printf("    %d log files after %d write-free opens\n", logs, i + 1);
      CHECK(false);
      return;
    }
  }

  // And the data is still there, which is the half that matters more than the
  // file count.
  std::unique_ptr<DB> db;
  if (!open_db(test_options(), path, &db, "final")) {
    CHECK(false);
    return;
  }
  std::string value;
  CHECK_OK(db->get(ReadOptions(), "k", &value));
  CHECK_EQ(value, "v");
}
