// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "harness.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "testutil.hpp"

#include "../src/encoding.hpp"
#include "../src/wal.hpp"

using namespace ambar;

namespace {

using ambar_test::TempDir;

std::vector<std::string> replay(const std::string& path, bool* truncated,
                                std::string* reason) {
  std::unique_ptr<SequentialFile> source;
  if (!SequentialFile::open(path, &source).is_ok()) return {};
  LogReader reader(std::move(source));
  std::vector<std::string> records;
  std::string_view record;
  std::string scratch;
  while (reader.read_record(&record, &scratch)) {
    records.emplace_back(record);
  }
  if (truncated != nullptr) *truncated = reader.truncated();
  if (reason != nullptr) *reason = reader.failure_reason();
  return records;
}

// Writes `records` and returns the log's path.
std::string write_log(const TempDir& dir, const std::vector<std::string>& records) {
  const std::string path = dir.file("000001.log");
  std::unique_ptr<WritableFile> file;
  CHECK_OK(WritableFile::open(path, /*append=*/false, &file));
  LogWriter writer(std::move(file));
  for (const auto& record : records) {
    CHECK_OK(writer.add_record(record));
  }
  CHECK_OK(writer.sync());
  CHECK_OK(writer.close());
  return path;
}

// Cuts a file down to `bytes`, the way an interrupted write leaves it.
void truncate_to(const std::string& path, uint64_t bytes) {
  std::filesystem::resize_file(path, bytes);
}

// Flips one bit of one byte, the way a damaged sector leaves it.
void flip_byte(const std::string& path, size_t offset) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  f.seekg(static_cast<std::streamoff>(offset));
  char c = 0;
  f.get(c);
  f.seekp(static_cast<std::streamoff>(offset));
  f.put(static_cast<char>(c ^ 0x40));
}

// Reads to the end, then reports whether the reader classed the stop as
// damage in the middle of the file rather than a torn tail.
bool replay_saw_damage(const std::string& path) {
  std::unique_ptr<SequentialFile> source;
  if (!SequentialFile::open(path, &source).is_ok()) return false;
  LogReader reader(std::move(source));
  std::string_view record;
  std::string scratch;
  while (reader.read_record(&record, &scratch)) {
  }
  return reader.damaged();
}

}  // namespace

TEST(Wal, round_trips_small_records) {
  TempDir dir;
  const std::vector<std::string> written = {"alpha", "", "beta", "gamma"};
  const std::string path = write_log(dir, written);
  const auto read = replay(path, nullptr, nullptr);
  CHECK_EQ(read.size(), written.size());
  for (size_t i = 0; i < read.size() && i < written.size(); ++i) {
    CHECK_EQ(read[i], written[i]);
  }
}

TEST(Wal, round_trips_a_record_that_spans_several_blocks) {
  // Fragmentation is the part of the format most likely to be wrong, and it
  // only happens for payloads larger than 32 KiB.
  TempDir dir;
  std::mt19937 rng(7);
  std::string big(kBlockSize * 3 + 1234, '\0');
  for (char& c : big) c = static_cast<char>('a' + (rng() % 26));

  const std::string path = write_log(dir, {"before", big, "after"});
  const auto read = replay(path, nullptr, nullptr);
  CHECK_EQ(read.size(), size_t{3});
  if (read.size() == 3) {
    CHECK_EQ(read[0], std::string("before"));
    CHECK(read[1] == big);
    CHECK_EQ(read[2], std::string("after"));
  }
}

TEST(Wal, records_survive_landing_exactly_on_a_block_boundary) {
  // The header cannot straddle a boundary, so payload sizes near
  // kBlockSize - kHeaderSize exercise the padding path.  Off-by-one bugs live
  // here and nowhere else.
  for (int delta = -2; delta <= 2; ++delta) {
    TempDir dir;
    const size_t size = kBlockSize - kHeaderSize + static_cast<size_t>(delta);
    const std::string payload(size, 'x');
    const std::string path = write_log(dir, {payload, "next"});
    const auto read = replay(path, nullptr, nullptr);
    CHECK_EQ(read.size(), size_t{2});
    if (read.size() == 2) {
      CHECK(read[0] == payload);
      CHECK_EQ(read[1], std::string("next"));
    }
  }
}

TEST(Wal, a_torn_tail_costs_only_the_torn_record) {
  // The durability promise in docs/DESIGN.md: recovery yields a prefix of the
  // acknowledged writes, never a partial value.
  TempDir dir;
  const std::vector<std::string> written = {"one", "two", "three", "four"};
  const std::string path = write_log(dir, written);

  uint64_t size = 0;
  CHECK_OK(file_size(path, &size));

  // Chop off progressively more of the tail and check the surviving prefix is
  // always a prefix of what was written -- never a mangled record.
  for (uint64_t cut = 1; cut < size && cut < 40; ++cut) {
    TempDir attempt;
    const std::string copy = attempt.file("copy.log");
    std::filesystem::copy_file(path, copy);
    truncate_to(copy, size - cut);

    const auto read = replay(copy, nullptr, nullptr);
    CHECK(read.size() <= written.size());
    for (size_t i = 0; i < read.size(); ++i) {
      CHECK_EQ(read[i], written[i]);
    }
  }
}

TEST(Wal, a_flipped_bit_ends_the_log_instead_of_returning_bad_data) {
  TempDir dir;
  const std::string path = write_log(dir, {"first", "second", "third"});

  // Corrupt a byte inside the second record's payload.
  std::string contents;
  {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    char buf[4096];
    const size_t n = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    contents.assign(buf, n);
  }
  const size_t second_record_payload = kHeaderSize + 5 + kHeaderSize + 2;
  CHECK(second_record_payload < contents.size());
  contents[second_record_payload] =
      static_cast<char>(contents[second_record_payload] ^ 0x40);
  {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    std::fwrite(contents.data(), 1, contents.size(), f);
    std::fclose(f);
  }

  bool truncated = false;
  std::string reason;
  const auto read = replay(path, &truncated, &reason);
  CHECK_EQ(read.size(), size_t{1});          // only the record before the damage
  if (!read.empty()) CHECK_EQ(read[0], std::string("first"));
  CHECK(truncated);                           // and it says so
  CHECK_EQ(reason, std::string("checksum mismatch"));
}

TEST(Wal, an_empty_log_replays_as_nothing_not_as_an_error) {
  TempDir dir;
  const std::string path = write_log(dir, {});
  bool truncated = true;
  const auto read = replay(path, &truncated, nullptr);
  CHECK_EQ(read.size(), size_t{0});
  CHECK(!truncated);
}

TEST(Wal, many_random_records_round_trip) {
  // A blunt instrument, but it covers the interaction between sizes that the
  // hand-written cases above each isolate.
  TempDir dir;
  std::mt19937 rng(99);
  std::vector<std::string> written;
  for (int i = 0; i < 400; ++i) {
    const size_t size = rng() % (kBlockSize / 4);
    std::string payload(size, '\0');
    for (char& c : payload) c = static_cast<char>(rng() & 0xff);
    written.push_back(std::move(payload));
  }
  const std::string path = write_log(dir, written);
  const auto read = replay(path, nullptr, nullptr);
  CHECK_EQ(read.size(), written.size());
  for (size_t i = 0; i < read.size() && i < written.size(); ++i) {
    CHECK(read[i] == written[i]);
  }
}

// A flipped bit in the middle of the log and a write that never finished stop
// the reader at the same place and mean opposite things: after the one, the
// records on disk are intact and the file's real state is past the stop;
// after the other, nothing was ever written past it.  A caller that treats
// the two alike acts on the wrong state.  The reader tells them apart by
// looking for a record it can verify beyond the failure.
TEST(Wal, damage_in_the_middle_is_told_apart_from_a_torn_tail) {
  TempDir dir;
  const std::vector<std::string> written = {"first", "second", "third"};

  // A flipped bit in the second record: "third" is intact after it.
  {
    const std::string path = write_log(dir, written);
    flip_byte(path, kHeaderSize + 5 + kHeaderSize + 2);
    bool truncated = false;
    std::string reason;
    const auto read = replay(path, &truncated, &reason);
    CHECK_EQ(read.size(), size_t{1});
    CHECK(truncated);
    CHECK(replay_saw_damage(path));
  }

  // The same flip in the last record: nothing follows, so it reads as a torn
  // tail -- which, from the outside, is what it is.
  {
    const std::string path = write_log(dir, written);
    flip_byte(path, kHeaderSize + 5 + kHeaderSize + 6 + kHeaderSize + 1);
    const auto read = replay(path, nullptr, nullptr);
    CHECK_EQ(read.size(), size_t{2});
    CHECK(!replay_saw_damage(path));
  }

  // An actual torn tail: the file ends inside the last record.
  {
    const std::string path = write_log(dir, written);
    uint64_t size = 0;
    CHECK_OK(file_size(path, &size));
    truncate_to(path, size - 2);
    const auto read = replay(path, nullptr, nullptr);
    CHECK_EQ(read.size(), size_t{2});
    CHECK(!replay_saw_damage(path));
  }
}

// The look past the failure has to cross block boundaries: a record spanning
// two blocks, damaged in the first, has its continuation and everything after
// it sitting intact in the second.
TEST(Wal, damage_in_an_earlier_block_is_seen_past_the_block_boundary) {
  TempDir dir;
  const std::string big(kBlockSize + kBlockSize / 2, 'b');
  const std::vector<std::string> written = {big, "after"};

  {
    const std::string path = write_log(dir, written);
    flip_byte(path, kHeaderSize + 100);  // inside the first fragment
    const auto read = replay(path, nullptr, nullptr);
    CHECK_EQ(read.size(), size_t{0});
    CHECK(replay_saw_damage(path));
  }

  // Cut inside the second block instead: a torn tail, and nothing after it.
  {
    const std::string path = write_log(dir, written);
    truncate_to(path, kBlockSize + 100);
    const auto read = replay(path, nullptr, nullptr);
    CHECK_EQ(read.size(), size_t{0});
    CHECK(!replay_saw_damage(path));
  }
}

// Seven zero bytes where a header should be.  The writer pads only the last
// six bytes of a block, so this is a span the filesystem zeroed -- what a
// power cut leaves where a file was extended before its data landed.  At the
// tail it is a tear; in the middle, with records after it, it is damage.
TEST(Wal, a_zeroed_span_is_a_tear_at_the_tail_and_damage_in_the_middle) {
  TempDir dir;
  const std::vector<std::string> written = {"first", "second", "third"};

  auto zero_out = [](const std::string& path, size_t offset, size_t count) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(offset));
    for (size_t i = 0; i < count; ++i) f.put('\0');
  };

  // The whole of the last record zeroed: "first" and "second" survive, the
  // stop is a tear, and it is reported as one.
  {
    const std::string path = write_log(dir, written);
    zero_out(path, kHeaderSize + 5 + kHeaderSize + 6, kHeaderSize + 5);
    bool truncated = false;
    const auto read = replay(path, &truncated, nullptr);
    CHECK_EQ(read.size(), size_t{2});
    CHECK(truncated);
    CHECK(!replay_saw_damage(path));
  }

  // The whole of the middle record zeroed, "third" intact after it.
  {
    const std::string path = write_log(dir, written);
    zero_out(path, kHeaderSize + 5, kHeaderSize + 6);
    const auto read = replay(path, nullptr, nullptr);
    CHECK_EQ(read.size(), size_t{1});
    CHECK(replay_saw_damage(path));
  }
}

// The checksum covers the type byte and the payload, not the length field.
// A flipped bit in the length sends the reader a wrong distance -- past the
// end of the block, or into the middle of the next record -- and walking by
// lengths from there finds nothing.  The byte-by-byte search does.
TEST(Wal, damage_to_a_length_field_is_still_told_from_a_torn_tail) {
  TempDir dir;
  const std::vector<std::string> written = {"first", "second", "third"};
  const size_t second_length_byte = kHeaderSize + 5 + kHeaderSize - 3;

  // Length 6 becomes 70: runs past the end of the block.
  {
    const std::string path = write_log(dir, written);
    flip_byte(path, second_length_byte);
    const auto read = replay(path, nullptr, nullptr);
    CHECK_EQ(read.size(), size_t{1});
    CHECK(replay_saw_damage(path));
  }

  // Length 6 becomes 2: the record verifies against the wrong bytes and
  // fails, and the walk resumes inside the real payload.
  {
    const std::string path = write_log(dir, written);
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(second_length_byte));
    f.put(static_cast<char>(2));
    f.close();
    const auto read = replay(path, nullptr, nullptr);
    CHECK_EQ(read.size(), size_t{1});
    CHECK(replay_saw_damage(path));
  }

  // And the same flip in the last record is a tear: nothing follows it.
  {
    const std::string path = write_log(dir, written);
    flip_byte(path, kHeaderSize + 5 + kHeaderSize + 6 + kHeaderSize - 3);
    const auto read = replay(path, nullptr, nullptr);
    CHECK_EQ(read.size(), size_t{2});
    CHECK(!replay_saw_damage(path));
  }
}
