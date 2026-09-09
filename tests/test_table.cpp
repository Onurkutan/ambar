// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Tables are where a bug stops being a crash and starts being a wrong answer.
//
// A memtable that loses a key fails a test immediately.  A table can be
// written, checksummed, opened and iterated without complaint while a lookup
// for a key that is in the file returns not-found, because the index sent the
// search to the neighbouring block.  Nothing in the file is corrupt; the file
// simply says something different from what it was asked to say.
//
// So these tests do not check that a table can be written and read.  They
// check the specific places where the index and the data can drift apart:
// block boundaries, shortened separators, the first and last key of every
// block, and lookups for keys that fall between two blocks.

#include "harness.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "testutil.hpp"

#include "../include/ambar/filter_policy.hpp"
#include "../src/comparator.hpp"
#include "../src/dbformat.hpp"
#include "../src/internal_filter_policy.hpp"
#include "../src/table.hpp"
#include "../src/table_builder.hpp"

using namespace ambar;

namespace {

using ambar_test::TempDir;

// Wraps a real file and counts reads, so a test can assert what a lookup costs
// rather than assuming.
class CountingFile final : public RandomAccessFile {
 public:
  explicit CountingFile(std::unique_ptr<RandomAccessFile> inner)
      : inner_(std::move(inner)) {}

  Status read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override {
    ++reads;
    bytes += n;
    return inner_->read(offset, n, result, scratch);
  }

  mutable int reads = 0;
  mutable uint64_t bytes = 0;

 private:
  std::unique_ptr<RandomAccessFile> inner_;
};

std::string key_of(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%08d", i);
  return std::string(buf);
}

// A table on disk, plus the handle needed to read it back.
struct BuiltTable {
  TempDir dir;
  std::string path;
  std::unique_ptr<CountingFile> file;
  std::unique_ptr<Table> table;
  uint64_t size = 0;
};

// Writes `entries` (which must be sorted) into a table and opens it.
Status build(BuiltTable* out, const std::vector<std::pair<std::string, std::string>>& entries,
             const Options& options, const Comparator* comparator) {
  out->path = out->dir.file("table.sst");

  {
    std::unique_ptr<WritableFile> file;
    Status status = WritableFile::open(out->path, /*append=*/false, &file);
    if (!status.is_ok()) return status;

    TableBuilder builder(options, file.get(), comparator);
    for (const auto& [key, value] : entries) {
      builder.add(key, value);
    }
    status = builder.finish();
    if (!status.is_ok()) return status;
    status = file->close();
    if (!status.is_ok()) return status;
  }

  Status status = file_size(out->path, &out->size);
  if (!status.is_ok()) return status;

  std::unique_ptr<RandomAccessFile> raw;
  status = RandomAccessFile::open(out->path, &raw);
  if (!status.is_ok()) return status;
  out->file = std::make_unique<CountingFile>(std::move(raw));

  return Table::open(options, comparator, out->file.get(), out->size,
                     &out->table);
}

// Internal-key entries, which is what the engine actually writes.  Sequence
// numbers descend as i ascends so that the internal ordering matches the user
// ordering -- the case that would hide an ordering bug if it did not.
std::vector<std::pair<std::string, std::string>> internal_entries(int n) {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    out.emplace_back(
        make_internal_key(key_of(i), static_cast<SequenceNumber>(1000 + i),
                          ValueType::kValue),
        "value_" + std::to_string(i));
  }
  return out;
}

std::string found_value;
bool found = false;

void save_result(void*, std::string_view, std::string_view value) {
  found_value.assign(value.data(), value.size());
  found = true;
}

// Looks up an internal key at the newest snapshot and reports what came back.
bool lookup(const Table& table, std::string_view user_key, std::string* value) {
  found = false;
  found_value.clear();
  const std::string target = make_lookup_key(user_key, kMaxSequenceNumber);
  const Status status =
      table.internal_get(ReadOptions(), target, nullptr, save_result);
  if (!status.is_ok() || !found) return false;

  // internal_get returns the first entry at or after the target, which may
  // belong to a different user key when the wanted one is absent.  Filtering
  // that out is the caller's job in the real engine too.
  *value = found_value;
  return true;
}

}  // namespace

// ------------------------------------------------------------ round trip ---

TEST(table, an_empty_table_opens_and_iterates_to_nothing) {
  BuiltTable built;
  Options options;
  CHECK_OK(build(&built, {}, options, bytewise_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));
  iter->seek_to_first();
  CHECK(!iter->valid());
  iter->seek_to_last();
  CHECK(!iter->valid());
  iter->seek("anything");
  CHECK(!iter->valid());
  CHECK_OK(iter->status());
}

TEST(table, a_single_entry_round_trips) {
  BuiltTable built;
  Options options;
  CHECK_OK(build(&built, {{"k", "v"}}, options, bytewise_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));
  iter->seek_to_first();
  CHECK(iter->valid());
  CHECK_EQ(iter->key(), std::string_view("k"));
  CHECK_EQ(iter->value(), std::string_view("v"));
  iter->next();
  CHECK(!iter->valid());
}

TEST(table, forward_iteration_returns_every_entry_in_order) {
  BuiltTable built;
  Options options;
  options.block_size = 256;  // many blocks, so boundaries are exercised
  const auto entries = internal_entries(5000);
  CHECK_OK(build(&built, entries, options, internal_key_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));
  size_t i = 0;
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    CHECK(i < entries.size());
    CHECK_EQ(iter->key(), std::string_view(entries[i].first));
    CHECK_EQ(iter->value(), std::string_view(entries[i].second));
    ++i;
  }
  CHECK_OK(iter->status());
  CHECK_EQ(i, entries.size());
}

TEST(table, backward_iteration_returns_every_entry_in_reverse) {
  BuiltTable built;
  Options options;
  options.block_size = 256;
  const auto entries = internal_entries(3000);
  CHECK_OK(build(&built, entries, options, internal_key_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));
  size_t i = entries.size();
  for (iter->seek_to_last(); iter->valid(); iter->prev()) {
    CHECK(i > 0);
    --i;
    CHECK_EQ(iter->key(), std::string_view(entries[i].first));
    CHECK_EQ(iter->value(), std::string_view(entries[i].second));
  }
  CHECK_OK(iter->status());
  CHECK_EQ(i, 0u);
}

// ------------------------------------------------------------------ seek ---

// The test that catches an index off by one block.  Seeking to a key that
// exists must land on it, from any starting position and at every block
// boundary.
TEST(table, seek_finds_every_key_that_exists) {
  BuiltTable built;
  Options options;
  options.block_size = 200;
  const auto entries = internal_entries(4000);
  CHECK_OK(build(&built, entries, options, internal_key_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));
  for (size_t i = 0; i < entries.size(); ++i) {
    iter->seek(entries[i].first);
    if (!iter->valid() || iter->key() != std::string_view(entries[i].first)) {
      std::printf("    seek to entry %zu landed %s\n", i,
                  iter->valid() ? std::string(iter->key()).c_str() : "nowhere");
      CHECK(false);
      return;
    }
    CHECK_EQ(iter->value(), std::string_view(entries[i].second));
  }
}

// A key that is not in the table must land on the next one that is -- not on
// the previous, and not nowhere.  This is where a separator that sorts on the
// wrong side of a block boundary shows up.
TEST(table, seek_to_an_absent_key_lands_on_the_next_present_one) {
  BuiltTable built;
  Options options;
  options.block_size = 200;

  // Even indices only, so every odd key is a gap.
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 4000; i += 2) {
    entries.emplace_back(key_of(i), "value_" + std::to_string(i));
  }
  CHECK_OK(build(&built, entries, options, bytewise_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));
  for (int i = 1; i < 3999; i += 2) {
    iter->seek(key_of(i));
    CHECK(iter->valid());
    if (iter->key() != std::string_view(key_of(i + 1))) {
      std::printf("    seek(%s) landed on %s, expected %s\n",
                  key_of(i).c_str(), std::string(iter->key()).c_str(),
                  key_of(i + 1).c_str());
      CHECK(false);
      return;
    }
  }

  // Past the end.
  iter->seek(key_of(999999));
  CHECK(!iter->valid());
  CHECK_OK(iter->status());
}

TEST(table, seek_before_the_first_key_lands_on_the_first) {
  BuiltTable built;
  Options options;
  CHECK_OK(build(&built, {{"b", "1"}, {"d", "2"}}, options,
                 bytewise_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));
  iter->seek("a");
  CHECK(iter->valid());
  CHECK_EQ(iter->key(), std::string_view("b"));
}

// Seeking forwards then backwards through the same table, alternating, is what
// a merge does when several sources interleave.  Doing it across block
// boundaries is where a stale cached block shows up.
TEST(table, alternating_seek_and_step_stays_consistent) {
  BuiltTable built;
  Options options;
  options.block_size = 128;
  const auto entries = internal_entries(1000);
  CHECK_OK(build(&built, entries, options, internal_key_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));
  std::mt19937 rng(7);
  for (int trial = 0; trial < 2000; ++trial) {
    const size_t i = rng() % entries.size();
    iter->seek(entries[i].first);
    CHECK(iter->valid());
    CHECK_EQ(iter->key(), std::string_view(entries[i].first));

    if (i + 1 < entries.size()) {
      iter->next();
      CHECK(iter->valid());
      CHECK_EQ(iter->key(), std::string_view(entries[i + 1].first));
      iter->prev();
      CHECK(iter->valid());
      CHECK_EQ(iter->key(), std::string_view(entries[i].first));
    }
  }
}

// ------------------------------------------------------------ odd inputs ---

TEST(table, empty_keys_and_values_round_trip) {
  BuiltTable built;
  Options options;
  CHECK_OK(build(&built, {{"", ""}, {"a", ""}, {"b", "v"}}, options,
                 bytewise_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));
  iter->seek_to_first();
  CHECK(iter->valid());
  CHECK_EQ(iter->key(), std::string_view(""));
  CHECK_EQ(iter->value(), std::string_view(""));
  iter->next();
  CHECK_EQ(iter->key(), std::string_view("a"));
  CHECK_EQ(iter->value(), std::string_view(""));
}

TEST(table, keys_and_values_with_embedded_nul_bytes_round_trip) {
  const std::string k1("a\0b", 3);
  const std::string k2("a\0c", 3);
  const std::string v("1\000" "2", 3);  // split: see C4125

  BuiltTable built;
  Options options;
  CHECK_OK(build(&built, {{k1, v}, {k2, v}}, options, bytewise_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));
  iter->seek(k2);
  CHECK(iter->valid());
  CHECK_EQ(iter->key(), std::string_view(k2));
  CHECK_EQ(iter->value(), std::string_view(v));
}

// A value larger than a block cannot be split, so the block holding it exceeds
// block_size.  The reader must handle a block that is not the size it expects.
TEST(table, a_value_larger_than_a_block_round_trips) {
  BuiltTable built;
  Options options;
  options.block_size = 512;

  const std::string big(200000, 'x');
  CHECK_OK(build(&built, {{"a", "1"}, {"b", big}, {"c", "3"}}, options,
                 bytewise_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));
  iter->seek("b");
  CHECK(iter->valid());
  CHECK_EQ(iter->value().size(), big.size());
  CHECK(iter->value() == std::string_view(big));

  iter->next();
  CHECK(iter->valid());
  CHECK_EQ(iter->key(), std::string_view("c"));
}

// ----------------------------------------------------------- the filter ---

// The claim a bloom filter makes is about disk reads, so the test counts disk
// reads.  Anything less measures that the filter exists, not that it works.
TEST(table, the_filter_prevents_reads_for_absent_keys) {
  const auto user_policy = new_bloom_filter_policy(10);
  // Wrapped, because the table stores internal keys and a lookup asks about a
  // user key at a different sequence number.  Passing the unwrapped policy
  // here is not a slow configuration -- it is a filter that hides keys that
  // are present.  See internal_filter_policy.hpp.
  const auto policy = new_internal_filter_policy(user_policy.get());

  Options options;
  options.block_size = 1024;
  options.filter_policy = policy.get();

  BuiltTable built;
  const auto entries = internal_entries(20000);
  CHECK_OK(build(&built, entries, options, internal_key_comparator()));

  // Present keys: every lookup must read its data block.
  built.file->reads = 0;
  std::string value;
  for (int i = 0; i < 1000; ++i) {
    CHECK(lookup(*built.table, key_of(i * 19), &value));
  }
  const int reads_present = built.file->reads;

  // Absent keys: almost none should read anything.
  built.file->reads = 0;
  for (int i = 0; i < 1000; ++i) {
    lookup(*built.table, key_of(500000 + i), &value);
  }
  const int reads_absent = built.file->reads;

  std::printf("    block reads: %d for 1000 present keys, %d for 1000 absent\n",
              reads_present, reads_absent);

  CHECK(reads_present >= 1000);   // one block each, at least
  CHECK(reads_absent < 100);      // the filter's ~1 % false positive rate
}

// The bug the wrapper exists to prevent, pinned so that removing the wrapper
// fails here loudly instead of silently losing lookups.
//
// A filter built over raw internal keys answers a question nobody asked: it
// knows about "key_7 at sequence 1007" and is queried for "key_7 at the newest
// snapshot".  Those differ in their last eight bytes, so it reports the key
// absent and the table never reads the block holding it.
TEST(table, an_unwrapped_filter_over_internal_keys_hides_present_keys) {
  const auto raw_policy = new_bloom_filter_policy(10);

  Options options;
  options.block_size = 1024;
  options.filter_policy = raw_policy.get();  // deliberately not wrapped

  BuiltTable built;
  CHECK_OK(build(&built, internal_entries(2000), options,
                 internal_key_comparator()));

  int found_count = 0;
  std::string value;
  for (int i = 0; i < 2000; ++i) {
    if (lookup(*built.table, key_of(i), &value)) ++found_count;
  }
  std::printf("    unwrapped filter: %d of 2000 present keys found\n",
              found_count);

  // The point is not the exact number, it is that it is nowhere near 2000.
  // If this ever passes 2000, the wrapper has become unnecessary and both this
  // test and internal_filter_policy.hpp should go.
  CHECK(found_count < 100);
}

// The same table without a filter, to show the difference is the filter and
// not something else about the workload.
TEST(table, without_a_filter_absent_keys_cost_a_read_each) {
  Options options;
  options.block_size = 1024;
  options.filter_policy = nullptr;

  BuiltTable built;
  const auto entries = internal_entries(20000);
  CHECK_OK(build(&built, entries, options, internal_key_comparator()));

  built.file->reads = 0;
  std::string value;
  for (int i = 0; i < 1000; ++i) {
    lookup(*built.table, key_of(500000 + i), &value);
  }
  std::printf("    block reads without a filter, 1000 absent keys: %d\n",
              built.file->reads);
  CHECK(built.file->reads >= 1000);
}

// A table written with a filter must still be readable by a build that does
// not use one, and vice versa.  Both are ordinary version-skew situations and
// neither may lose a key.
TEST(table, a_table_reads_correctly_with_the_filter_option_flipped) {
  const auto user_policy = new_bloom_filter_policy(10);
  const auto policy = new_internal_filter_policy(user_policy.get());

  Options write_options;
  write_options.filter_policy = policy.get();
  write_options.block_size = 512;

  BuiltTable built;
  const auto entries = internal_entries(2000);
  CHECK_OK(build(&built, entries, write_options, internal_key_comparator()));

  // Reopen the same file with filters switched off.
  Options read_options;
  read_options.filter_policy = nullptr;
  std::unique_ptr<RandomAccessFile> raw;
  CHECK_OK(RandomAccessFile::open(built.path, &raw));
  std::unique_ptr<Table> table;
  CHECK_OK(Table::open(read_options, internal_key_comparator(), raw.get(),
                       built.size, &table));

  std::string value;
  for (int i = 0; i < 2000; i += 7) {
    CHECK(lookup(*table, key_of(i), &value));
    CHECK_EQ(value, "value_" + std::to_string(i));
  }
}

// -------------------------------------------------------------- damage ----

TEST(table, a_file_that_is_not_a_table_is_rejected) {
  TempDir dir;
  const std::string path = dir.file("random.bin");
  {
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open(path, false, &file));
    CHECK_OK(file->append(std::string(4096, 'z')));
    CHECK_OK(file->close());
  }

  uint64_t size = 0;
  CHECK_OK(file_size(path, &size));
  std::unique_ptr<RandomAccessFile> raw;
  CHECK_OK(RandomAccessFile::open(path, &raw));
  std::unique_ptr<Table> table;
  const Status status = Table::open(Options(), bytewise_comparator(),
                                    raw.get(), size, &table);
  CHECK(status.is_corruption());
}

TEST(table, a_truncated_table_is_rejected) {
  BuiltTable built;
  Options options;
  CHECK_OK(build(&built, internal_entries(500), options,
                 internal_key_comparator()));

  for (const uint64_t keep : {built.size / 2, built.size - 1, uint64_t{4}}) {
    std::error_code ec;
    const std::string path = built.dir.file("cut.sst");
    std::filesystem::copy_file(built.path, path,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    std::filesystem::resize_file(path, keep, ec);
    CHECK(!ec);

    std::unique_ptr<RandomAccessFile> raw;
    CHECK_OK(RandomAccessFile::open(path, &raw));
    std::unique_ptr<Table> table;
    const Status status = Table::open(options, internal_key_comparator(),
                                      raw.get(), keep, &table);
    CHECK(!status.is_ok());
    std::filesystem::remove(path, ec);
  }
}

// Every byte of a data block, flipped one at a time, must be caught by the
// checksum rather than producing a wrong answer.  Only the first few hundred
// bytes are tested, because the point is the mechanism, not the coverage.
TEST(table, a_flipped_bit_in_a_data_block_is_caught) {
  BuiltTable built;
  Options options;
  options.block_size = 4096;
  CHECK_OK(build(&built, internal_entries(200), options,
                 internal_key_comparator()));

  std::string bytes;
  {
    std::unique_ptr<SequentialFile> file;
    CHECK_OK(SequentialFile::open(built.path, &file));
    std::string_view chunk;
    std::string scratch;
    CHECK_OK(file->read(static_cast<size_t>(built.size), &chunk, &scratch));
    bytes.assign(chunk.data(), chunk.size());
  }

  int caught = 0;
  int missed = 0;
  for (size_t offset = 0; offset < 400 && offset < bytes.size(); ++offset) {
    std::string damaged = bytes;
    damaged[offset] = static_cast<char>(damaged[offset] ^ 0x01);

    const std::string path = built.dir.file("bad.sst");
    {
      std::unique_ptr<WritableFile> file;
      CHECK_OK(WritableFile::open(path, false, &file));
      CHECK_OK(file->append(damaged));
      CHECK_OK(file->close());
    }

    std::unique_ptr<RandomAccessFile> raw;
    CHECK_OK(RandomAccessFile::open(path, &raw));
    std::unique_ptr<Table> table;
    Status status = Table::open(options, internal_key_comparator(), raw.get(),
                                built.size, &table);
    if (!status.is_ok()) {
      ++caught;
    } else {
      std::unique_ptr<Iterator> iter(table->new_iterator(ReadOptions()));
      bool saw_error = false;
      for (iter->seek_to_first(); iter->valid(); iter->next()) {
      }
      if (!iter->status().is_ok()) saw_error = true;
      if (saw_error) {
        ++caught;
      } else {
        ++missed;
      }
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  std::printf("    single-bit flips in the first 400 bytes: %d caught, %d not\n",
              caught, missed);
  // Every flip lands inside a data block here, and every data block is
  // covered by a CRC, so none may pass unnoticed.
  CHECK_EQ(missed, 0);
}

// ------------------------------------------------------- index shortening ---

// What shortening is worth, measured, and when it is worth nothing.
//
// The optimisation looks obviously good until it is measured on the key shape
// a database usually has.  Between "prefix_00000041" and "prefix_00000042" the
// first differing byte is '1' against '2', and a separator has to sort
// strictly below the second key, so there is nothing to put between them:
// dense sequential keys admit no shortening at all.  The saving arrives when
// consecutive index keys diverge early, which is what sparse keys give.
//
// Both shapes are measured here rather than the flattering one being reported
// alone.  A comparator whose shortening functions do nothing supplies the
// baseline, so the number compares two real files instead of estimating one
// that was never written.
TEST(table, index_separators_are_shortened_without_changing_answers) {
  Options options;
  options.block_size = 128;  // small, so the index is a real share of the file

  static const Comparator kNoShortening = {
      "test.NoShortening",
      0,  // bytewise keys have no minimum length
      bytewise_comparator()->compare,
      [](std::string*, std::string_view) {},
      [](std::string*) {},
  };

  struct Case {
    const char* name;
    bool expect_saving;
    std::vector<std::pair<std::string, std::string>> entries;
  };
  std::vector<Case> cases;

  {
    Case dense{"dense sequential", false, {}};
    for (int i = 0; i < 3000; ++i) {
      dense.entries.emplace_back(
          "a_very_long_common_prefix_for_these_keys_" + key_of(i), "v");
    }
    cases.push_back(std::move(dense));
  }
  {
    Case sparse{"sparse random", true, {}};
    std::mt19937 rng(11);
    std::map<std::string, std::string> sorted;
    for (int i = 0; i < 3000; ++i) {
      std::string key;
      for (int j = 0; j < 40; ++j) {
        key.push_back(static_cast<char>('a' + rng() % 26));
      }
      sorted[key] = "v";
    }
    sparse.entries.assign(sorted.begin(), sorted.end());
    cases.push_back(std::move(sparse));
  }

  std::printf("    %-18s %10s %10s %8s\n", "key shape", "shortened", "full",
              "saving");
  for (const Case& c : cases) {
    BuiltTable shortened;
    BuiltTable full;
    CHECK_OK(build(&shortened, c.entries, options, bytewise_comparator()));
    CHECK_OK(build(&full, c.entries, options, &kNoShortening));

    const double saving =
        100.0 * (1.0 - static_cast<double>(shortened.size) /
                           static_cast<double>(full.size));
    std::printf("    %-18s %10llu %10llu %7.1f%%\n", c.name,
                static_cast<unsigned long long>(shortened.size),
                static_cast<unsigned long long>(full.size), saving);

    CHECK(shortened.size <= full.size);
    if (c.expect_saving) {
      // If this ever falls to nothing, the shortening has stopped working and
      // this is the only case that would notice.
      CHECK(saving > 2.0);
    }

    // The half that matters: both files must answer identically.
    for (BuiltTable* built : {&shortened, &full}) {
      std::unique_ptr<Iterator> iter(built->table->new_iterator(ReadOptions()));
      for (const auto& [key, value] : c.entries) {
        iter->seek(key);
        CHECK(iter->valid());
        CHECK_EQ(iter->key(), std::string_view(key));
        CHECK_EQ(iter->value(), std::string_view(value));
      }
      CHECK_OK(iter->status());
    }
  }
}

// ------------------------------------------------------------- vs a map ----

// The model test.  A random workload of writes is applied to both a table and
// a std::map, and every key -- present and absent -- is asked of both.  A
// disagreement is a bug regardless of which component caused it.
TEST(table, agrees_with_a_std_map_on_a_random_workload) {
  std::mt19937 rng(20260906);
  std::map<std::string, std::string> model;

  for (int i = 0; i < 20000; ++i) {
    // Varied key lengths and shapes, because a uniform workload only exercises
    // one path through the prefix compression.
    const int shape = static_cast<int>(rng() % 4);
    std::string key;
    switch (shape) {
      case 0: key = key_of(static_cast<int>(rng() % 100000)); break;
      case 1: key = std::to_string(rng() % 1000); break;
      case 2: key = std::string(rng() % 40, 'p') + std::to_string(rng() % 100);
              break;
      default: key = std::string(1 + rng() % 3, static_cast<char>('a' + rng() % 26));
               break;
    }
    model[key] = std::string(rng() % 30, static_cast<char>('A' + rng() % 26));
  }

  std::vector<std::pair<std::string, std::string>> entries(model.begin(),
                                                           model.end());
  Options options;
  options.block_size = 300;
  BuiltTable built;
  CHECK_OK(build(&built, entries, options, bytewise_comparator()));

  std::unique_ptr<Iterator> iter(built.table->new_iterator(ReadOptions()));

  // Every present key.
  for (const auto& [key, value] : model) {
    iter->seek(key);
    CHECK(iter->valid());
    CHECK_EQ(iter->key(), std::string_view(key));
    CHECK_EQ(iter->value(), std::string_view(value));
  }

  // And a sweep of absent ones, checked against what the map says comes next.
  for (int i = 0; i < 20000; ++i) {
    const std::string probe = key_of(static_cast<int>(rng() % 200000)) + "#";
    iter->seek(probe);
    const auto it = model.lower_bound(probe);
    if (it == model.end()) {
      CHECK(!iter->valid());
    } else {
      CHECK(iter->valid());
      CHECK_EQ(iter->key(), std::string_view(it->first));
    }
  }
  CHECK_OK(iter->status());
}
