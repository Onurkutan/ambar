// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "harness.hpp"

#include <cstdio>
#include <filesystem>
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
