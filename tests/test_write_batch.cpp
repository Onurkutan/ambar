// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// What these tests are for.
//
// A write batch claims two things.  First, that what comes out of it is
// exactly what went in -- same operations, same order, arbitrary bytes
// intact.  Second, and the reason the class exists at all, that a batch
// reaches the log whole or not at all, so a crash can never leave half a
// multi-key update behind.
//
// The first claim is checked by round-tripping through a recording handler.
// The second cannot be checked by inspecting the class: atomicity is a
// property of the batch *plus* the log, and the only honest test is to write
// a batch, damage the file the way a crash would, and confirm that what
// survives is a whole batch or nothing.  That is what the last group does.

#include "harness.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "testutil.hpp"

#include "../include/ambar/write_batch.hpp"
#include "../src/memtable.hpp"
#include "../src/wal.hpp"
#include "../src/write_batch_internal.hpp"

using namespace ambar;

namespace {

using ambar_test::TempDir;

// Records what a batch actually applies, rather than what it was asked to
// apply.  Rendering the operations as text makes a wrong order or a lost NUL
// visible in the failure message instead of only in a length mismatch.
class Recorder final : public WriteBatch::Handler {
 public:
  void put(std::string_view key, std::string_view value) override {
    ops.push_back("put(" + escape(key) + ", " + escape(value) + ")");
  }
  void del(std::string_view key) override {
    ops.push_back("del(" + escape(key) + ")");
  }

  std::string joined() const {
    std::string out;
    for (const auto& op : ops) {
      if (!out.empty()) out += " ";
      out += op;
    }
    return out;
  }

  std::vector<std::string> ops;

 private:
  static std::string escape(std::string_view s) {
    std::string out;
    for (const char c : s) {
      if (c == '\0') {
        out += "\\0";
      } else if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\x%02x",
                      static_cast<unsigned char>(c));
        out += buf;
      } else {
        out += c;
      }
    }
    return out;
  }
};

std::string apply(const WriteBatch& batch) {
  Recorder recorder;
  const Status s = batch.iterate(&recorder);
  if (!s.is_ok()) return "ERROR: " + s.to_string();
  return recorder.joined();
}

}  // namespace

// --------------------------------------------------------------- contents ---

TEST(write_batch, empty_batch_holds_nothing) {
  WriteBatch batch;
  CHECK_EQ(batch.count(), 0u);
  CHECK_EQ(batch.approximate_size(), WriteBatchInternal::kHeader);
  CHECK_EQ(apply(batch), "");
}

TEST(write_batch, operations_replay_in_order) {
  WriteBatch batch;
  batch.put("a", "1");
  batch.del("b");
  batch.put("c", "3");
  CHECK_EQ(batch.count(), 3u);
  CHECK_EQ(apply(batch), "put(a, 1) del(b) put(c, 3)");
}

TEST(write_batch, clear_returns_to_empty) {
  WriteBatch batch;
  batch.put("a", "1");
  batch.clear();
  CHECK_EQ(batch.count(), 0u);
  CHECK_EQ(batch.approximate_size(), WriteBatchInternal::kHeader);
  CHECK_EQ(apply(batch), "");
}

// An empty key and an empty value are legal, and they are exactly the inputs a
// length-prefixed format gets wrong if it confuses "empty" with "absent".
TEST(write_batch, empty_key_and_value_survive) {
  WriteBatch batch;
  batch.put("", "");
  batch.put("k", "");
  batch.del("");
  CHECK_EQ(apply(batch), "put(, ) put(k, ) del()");
}

// Keys are byte strings, not C strings.  A NUL in the middle must not end one.
TEST(write_batch, embedded_nul_bytes_survive) {
  const std::string key("a\0b", 3);
  // Split so the octal escape ends at the literal boundary: "\0002" reads as a
  // three-digit escape followed by '2' to a person and to MSVC's C4125, which
  // is a warning this project's CI turns into an error.
  const std::string value("1\000" "2", 3);
  WriteBatch batch;
  batch.put(key, value);

  Recorder recorder;
  CHECK_OK(batch.iterate(&recorder));
  CHECK_EQ(recorder.ops.size(), 1u);
  CHECK_EQ(recorder.ops[0], "put(a\\0b, 1\\02)");
}

TEST(write_batch, large_values_round_trip) {
  const std::string value(200000, 'x');  // past every varint boundary
  WriteBatch batch;
  batch.put("big", value);

  Recorder recorder;
  CHECK_OK(batch.iterate(&recorder));
  CHECK_EQ(recorder.ops.size(), 1u);
  CHECK(batch.approximate_size() > value.size());
}

// -------------------------------------------------------------- sequence ---

TEST(write_batch, sequence_is_settable_and_readable) {
  WriteBatch batch;
  batch.put("a", "1");
  WriteBatchInternal::set_sequence(&batch, 42);
  CHECK_EQ(WriteBatchInternal::sequence(batch), 42u);

  // Patching the header must not disturb the records after it.
  CHECK_EQ(apply(batch), "put(a, 1)");
}

TEST(write_batch, sequence_survives_the_top_of_its_range) {
  WriteBatch batch;
  batch.put("a", "1");
  WriteBatchInternal::set_sequence(&batch, kMaxSequenceNumber);
  CHECK_EQ(WriteBatchInternal::sequence(batch), kMaxSequenceNumber);
}

// ---------------------------------------------------------------- append ---

TEST(write_batch, append_concatenates_and_sums_counts) {
  WriteBatch first;
  first.put("a", "1");
  first.del("b");

  WriteBatch second;
  second.put("c", "3");

  WriteBatchInternal::append(&first, second);
  CHECK_EQ(first.count(), 3u);
  CHECK_EQ(apply(first), "put(a, 1) del(b) put(c, 3)");

  // The source is not consumed; a merged writer may still need to report on it.
  CHECK_EQ(second.count(), 1u);
  CHECK_EQ(apply(second), "put(c, 3)");
}

TEST(write_batch, append_of_empty_changes_nothing) {
  WriteBatch first;
  first.put("a", "1");
  const size_t before = first.approximate_size();

  WriteBatch empty;
  WriteBatchInternal::append(&first, empty);
  CHECK_EQ(first.count(), 1u);
  CHECK_EQ(first.approximate_size(), before);

  WriteBatchInternal::append(&empty, first);
  CHECK_EQ(empty.count(), 1u);
  CHECK_EQ(apply(empty), "put(a, 1)");
}

// ------------------------------------------------------------- memtable ----

TEST(write_batch, insert_into_assigns_consecutive_sequences) {
  ambar_test::MemTableRef mem;
  WriteBatch batch;
  batch.put("a", "1");
  batch.put("b", "2");
  batch.put("c", "3");
  WriteBatchInternal::set_sequence(&batch, 100);
  CHECK_OK(WriteBatchInternal::insert_into(batch, &*mem));

  // Operation i took sequence 100 + i, so a snapshot taken between them sees a
  // prefix of the batch.  Nothing outside the engine may observe such a
  // snapshot -- the point of checking it here is that the numbering is
  // positional and dense, which is what later recovery arithmetic assumes.
  std::string value;
  Status status;
  CHECK(mem->get("a", 100, &value, &status));
  CHECK_EQ(value, "1");
  CHECK(!mem->get("b", 100, &value, &status));
  CHECK(mem->get("b", 101, &value, &status));
  CHECK_EQ(value, "2");
  CHECK(mem->get("c", 102, &value, &status));
  CHECK_EQ(value, "3");
}

TEST(write_batch, later_write_of_a_key_wins_within_one_batch) {
  ambar_test::MemTableRef mem;
  WriteBatch batch;
  batch.put("k", "old");
  batch.put("k", "new");
  WriteBatchInternal::set_sequence(&batch, 1);
  CHECK_OK(WriteBatchInternal::insert_into(batch, &*mem));

  std::string value;
  Status status;
  CHECK(mem->get("k", kMaxSequenceNumber, &value, &status));
  CHECK_EQ(value, "new");
}

TEST(write_batch, delete_after_put_in_one_batch_leaves_the_key_deleted) {
  ambar_test::MemTableRef mem;
  WriteBatch batch;
  batch.put("k", "v");
  batch.del("k");
  WriteBatchInternal::set_sequence(&batch, 1);
  CHECK_OK(WriteBatchInternal::insert_into(batch, &*mem));

  std::string value;
  Status status;
  // True: the memtable answers the question.  Not-found: the answer is that
  // the key is gone.  Collapsing the two would resurrect deleted keys from
  // older tables.
  CHECK(mem->get("k", kMaxSequenceNumber, &value, &status));
  CHECK(status.is_not_found());
}

// ------------------------------------------------------------ corruption ---

TEST(write_batch, set_contents_rejects_a_short_buffer) {
  WriteBatch batch;
  CHECK(WriteBatchInternal::set_contents(&batch, "short").is_corruption());
}

TEST(write_batch, unknown_tag_is_corruption) {
  WriteBatch batch;
  batch.put("a", "1");
  std::string bytes(WriteBatchInternal::contents(batch));
  bytes[WriteBatchInternal::kHeader] = static_cast<char>(0x7f);

  WriteBatch damaged;
  CHECK_OK(WriteBatchInternal::set_contents(&damaged, bytes));
  Recorder recorder;
  CHECK(damaged.iterate(&recorder).is_corruption());
}

TEST(write_batch, truncated_record_is_corruption) {
  WriteBatch batch;
  batch.put("hello", "world");
  std::string bytes(WriteBatchInternal::contents(batch));
  bytes.resize(bytes.size() - 3);

  WriteBatch damaged;
  CHECK_OK(WriteBatchInternal::set_contents(&damaged, bytes));
  Recorder recorder;
  CHECK(damaged.iterate(&recorder).is_corruption());
}

// A count that disagrees with the records is the failure a plain "walk until
// the bytes run out" loop would miss entirely, replaying a short batch and
// calling it success.
TEST(write_batch, count_mismatch_is_corruption) {
  WriteBatch batch;
  batch.put("a", "1");
  batch.put("b", "2");
  WriteBatchInternal::set_count(&batch, 5);

  Recorder recorder;
  CHECK(batch.iterate(&recorder).is_corruption());
}

// ------------------------------------------------- atomicity, end to end ---

namespace {

constexpr size_t kNoTruncation = static_cast<size_t>(-1);

// Writes one batch as one log record, then cuts the file down to `cut_to` bytes
// -- the shape a crash leaves behind, since a partial write reaches the disk
// as a prefix.  Returns what recovery would replay.
std::vector<std::string> replay_after_truncation(const std::string& path,
                                                 const WriteBatch& batch,
                                                 size_t cut_to) {
  {
    std::unique_ptr<WritableFile> file;
    if (!WritableFile::open(path, /*append=*/false, &file).is_ok()) return {"OPEN FAILED"};
    LogWriter writer(std::move(file));
    if (!writer.add_record(WriteBatchInternal::contents(batch)).is_ok()) {
      return {"WRITE FAILED"};
    }
    if (!writer.close().is_ok()) return {"CLOSE FAILED"};
  }

  if (cut_to != kNoTruncation) {
    std::error_code ec;
    std::filesystem::resize_file(path, cut_to, ec);
    if (ec) return {"TRUNCATE FAILED"};
  }

  std::unique_ptr<SequentialFile> source;
  if (!SequentialFile::open(path, &source).is_ok()) return {"REOPEN FAILED"};
  LogReader reader(std::move(source));

  std::vector<std::string> applied;
  std::string_view record;
  std::string scratch;
  while (reader.read_record(&record, &scratch)) {
    WriteBatch recovered;
    if (!WriteBatchInternal::set_contents(&recovered, record).is_ok()) {
      applied.push_back("CORRUPT HEADER");
      continue;
    }
    Recorder recorder;
    if (!recovered.iterate(&recorder).is_ok()) {
      applied.push_back("CORRUPT BODY");
      continue;
    }
    applied.push_back(recorder.joined());
  }
  return applied;
}

}  // namespace

TEST(write_batch, an_intact_log_replays_the_whole_batch) {
  TempDir dir;
  WriteBatch batch;
  batch.put("a", "1");
  batch.del("b");
  batch.put("c", "3");

  const auto applied =
      replay_after_truncation(dir.file("full.log"), batch, kNoTruncation);
  CHECK_EQ(applied.size(), 1u);
  CHECK_EQ(applied[0], "put(a, 1) del(b) put(c, 3)");
}

// The claim under test: after a crash mid-write there is no such thing as
// half a batch.  Cutting the record at every possible length must yield
// either the complete batch or nothing -- never "put(a, 1)" on its own.
//
// This is the test that would fail if atomicity were implemented by applying
// operations one at a time as they were parsed out of a partially written
// file, which is the obvious wrong design.
//
// Verified by mutation rather than asserted.  Changing LogReader so that a
// record ending mid-fragment returns the prefix assembled so far, instead of
// returning false -- exactly the bug this test exists to catch -- turns the
// suite red here and in the multi-block case below.  Restoring it turns it
// green again.  A reader who doubts the test can repeat that in a minute.
TEST(write_batch, a_partial_log_record_replays_nothing) {
  TempDir dir;
  WriteBatch batch;
  batch.put("a", "1");
  batch.del("b");
  batch.put("c", "3");

  // Establish the intact length once.
  const std::string full_path = dir.file("probe.log");
  {
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open(full_path, /*append=*/false, &file));
    LogWriter writer(std::move(file));
    CHECK_OK(writer.add_record(WriteBatchInternal::contents(batch)));
    CHECK_OK(writer.close());
  }
  const auto full_size =
      static_cast<size_t>(std::filesystem::file_size(full_path));
  CHECK(full_size > 0);

  for (size_t cut = 0; cut < full_size; ++cut) {
    const std::string path = dir.file("cut_" + std::to_string(cut) + ".log");
    const auto applied = replay_after_truncation(path, batch, cut);
    if (!applied.empty()) {
      // A short read must not produce a short batch.
      CHECK_EQ(applied.size(), 1u);
      CHECK_EQ(applied[0], "put(a, 1) del(b) put(c, 3)");
    }
  }
}

// The same claim for a batch big enough to span several 32 KiB blocks, where
// the record is fragmented and the reader has to reassemble it.  Fragmentation
// is where a "checksum each piece" design silently degrades into per-fragment
// atomicity.
TEST(write_batch, a_multi_block_batch_is_still_all_or_nothing) {
  TempDir dir;
  WriteBatch batch;
  for (int i = 0; i < 400; ++i) {
    batch.put("key_" + std::to_string(i), std::string(300, 'v'));
  }
  const std::string expected = apply(batch);
  CHECK(batch.approximate_size() > 3 * 32768);

  const std::string full_path = dir.file("big.log");
  {
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open(full_path, /*append=*/false, &file));
    LogWriter writer(std::move(file));
    CHECK_OK(writer.add_record(WriteBatchInternal::contents(batch)));
    CHECK_OK(writer.close());
  }
  const auto full_size =
      static_cast<size_t>(std::filesystem::file_size(full_path));

  // Every 997 bytes rather than every byte: the same property, at a test
  // runtime that will not discourage anyone from running the suite.
  for (size_t cut = 0; cut < full_size; cut += 997) {
    const std::string path = dir.file("bigcut_" + std::to_string(cut) + ".log");
    const auto applied = replay_after_truncation(path, batch, cut);
    if (!applied.empty()) {
      CHECK_EQ(applied.size(), 1u);
      CHECK_EQ(applied[0], expected);
    }
  }
}
