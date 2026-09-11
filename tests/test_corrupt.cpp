// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// What happens when the files are not what the engine wrote.
//
// A storage engine reads files, and a file is not a trusted input even when
// this process created it: a disk can flip a bit, a backup can truncate, and
// somebody can hand the engine a directory they built themselves.  Every one of
// those has to end in an error, and none of them may end in a crash, a hang, or
// an allocation of whatever size the file happened to ask for.
//
// These tests do not check that damage is detected -- test_table.cpp does that
// with single-bit flips.  They check the weaker and more important property
// that damage is *survived*: whatever the bytes say, the engine returns a
// status.  They are written to be run under AddressSanitizer, which is what
// turns a read past the end of a buffer from something that usually works into
// a failure.

#include "harness.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "testutil.hpp"

#include "../include/ambar/db.hpp"
#include "../include/ambar/filter_policy.hpp"
#include "../src/comparator.hpp"
#include "../src/block.hpp"
#include "../src/filename.hpp"
#include "../src/filter_block.hpp"
#include "../src/format.hpp"
#include "../src/table.hpp"
#include "../src/version_edit.hpp"
#include "../src/wal.hpp"

using namespace ambar;

namespace {

using ambar_test::TempDir;

// Builds a small, valid database and returns its directory.
void build_database(const std::string& path) {
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 64 << 10;

  std::unique_ptr<DB> db;
  if (!DB::open(options, path, &db).is_ok()) return;
  for (int i = 0; i < 4000; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "key_%06d", i);
    db->put(WriteOptions(), key, std::string(80, static_cast<char>('a' + i % 26)));
  }
  db->compact_range(nullptr, nullptr);
}

std::vector<std::string> files_in(const std::string& path) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
    if (entry.is_regular_file(ec)) out.push_back(entry.path().string());
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

// Opens and exercises a database, reporting only whether it survived.  Any
// status is acceptable; a crash, a hang or a sanitizer report is not.
bool survives_open(const std::string& path) {
  Options options;
  options.create_if_missing = false;

  std::unique_ptr<DB> db;
  const Status status = DB::open(options, path, &db);
  if (!status.is_ok()) return true;  // refused: the correct outcome

  // It opened.  Whatever it now holds, reading it must not crash.
  std::string value;
  for (int i = 0; i < 4000; i += 7) {
    char key[32];
    std::snprintf(key, sizeof(key), "key_%06d", i);
    db->get(ReadOptions(), key, &value);
  }
  std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
  int seen = 0;
  for (iter->seek_to_first(); iter->valid() && seen < 100000; iter->next()) {
    ++seen;
  }
  return true;
}

}  // namespace

// Every byte of every file, corrupted one at a time in a sample, must produce
// a status rather than a crash.
TEST(corrupt, damaged_files_produce_errors_not_crashes) {
  TempDir source;
  const std::string original_path = source.file("db");
  build_database(original_path);

  const auto paths = files_in(original_path);
  CHECK(!paths.empty());

  std::mt19937 rng(4242);
  int trials = 0;

  for (const std::string& file : paths) {
    const std::string bytes = read_file(file);
    if (bytes.empty()) continue;

    for (int attempt = 0; attempt < 12; ++attempt) {
      TempDir work;
      const std::string copy_path = work.file("db");
      std::error_code ec;
      std::filesystem::copy(original_path, copy_path,
                            std::filesystem::copy_options::recursive, ec);
      if (ec) continue;

      const std::string target =
          copy_path + "/" +
          std::filesystem::path(file).filename().string();

      std::string damaged = read_file(target);
      if (damaged.empty()) continue;

      const size_t offset = rng() % damaged.size();
      switch (attempt % 3) {
        case 0:  // a flipped bit
          damaged[offset] = static_cast<char>(damaged[offset] ^ 0x40);
          break;
        case 1:  // a truncation
          damaged.resize(offset);
          break;
        default:  // a run of garbage
          for (size_t i = offset; i < damaged.size() && i < offset + 64; ++i) {
            damaged[i] = static_cast<char>(rng() & 0xff);
          }
          break;
      }
      write_file(target, damaged);

      ++trials;
      CHECK(survives_open(copy_path));
    }
  }
  std::printf("    %d damaged copies opened without crashing\n", trials);
  CHECK(trials > 50);
}

// A file of pure noise, presented as a table.  Nothing in it is a valid handle,
// so every length it claims is one the reader must refuse rather than allocate.
TEST(corrupt, a_table_of_random_bytes_is_refused_not_allocated) {
  TempDir dir;
  std::mt19937 rng(99);

  for (int attempt = 0; attempt < 40; ++attempt) {
    const size_t size = 64 + rng() % 8192;
    std::string bytes(size, '\0');
    for (char& c : bytes) c = static_cast<char>(rng() & 0xff);

    const std::string path = dir.file("noise.sst");
    write_file(path, bytes);

    std::unique_ptr<RandomAccessFile> file;
    if (!RandomAccessFile::open(path, &file).is_ok()) continue;

    std::unique_ptr<Table> table;
    const Status status = Table::open(Options(), internal_key_comparator(),
                                      file.get(), size, &table);
    if (status.is_ok()) {
      // Astronomically unlikely -- the magic number would have to match -- but
      // if it happens the table must still be walkable without crashing.
      std::unique_ptr<Iterator> iter(table->new_iterator(ReadOptions()));
      for (iter->seek_to_first(); iter->valid(); iter->next()) {
      }
    }
  }
}

// A directory that is not a database at all, and one that is missing pieces.
TEST(corrupt, a_directory_that_is_not_a_database_is_refused) {
  TempDir dir;
  const std::string path = dir.file("notadb");
  std::filesystem::create_directories(path);
  write_file(path + "/CURRENT", "MANIFEST-999999\n");

  Options options;
  options.create_if_missing = false;
  std::unique_ptr<DB> db;
  CHECK(!DB::open(options, path, &db).is_ok());

  // CURRENT with no newline, which is the shape a partial write leaves.
  write_file(path + "/CURRENT", "MANIFEST-000001");
  CHECK(!DB::open(options, path, &db).is_ok());

  // CURRENT naming something outside the database directory.  The engine must
  // refuse rather than follow it: a file inside the directory should not be
  // able to send the engine somewhere else on the machine, even though what it
  // would find there produces nothing but a parse error.
  for (const char* escape : {"../../../etc/passwd\n", "/etc/passwd\n",
                             "..\\..\\windows\\system32\\config\\sam\n",
                             "subdir/MANIFEST-000001\n", "MANIFEST-\n",
                             "MANIFEST-abc\n", "000001.sst\n", "\n"}) {
    write_file(path + "/CURRENT", escape);
    const Status status = DB::open(options, path, &db);
    if (status.is_ok()) {
      std::printf("    CURRENT containing %s was accepted\n", escape);
      CHECK(false);
      return;
    }
  }
}

// A manifest that references a table which is not there.  Opening anyway would
// serve a database with a hole in it, reporting keys absent that are not.
TEST(corrupt, a_missing_table_file_is_reported_rather_than_ignored) {
  TempDir dir;
  const std::string path = dir.file("db");
  build_database(path);

  bool removed = false;
  for (const std::string& file : files_in(path)) {
    if (file.size() > 4 && file.substr(file.size() - 4) == ".sst") {
      std::error_code ec;
      std::filesystem::remove(file, ec);
      removed = !ec;
      break;
    }
  }
  CHECK(removed);

  Options options;
  options.create_if_missing = false;
  std::unique_ptr<DB> db;
  const Status status = DB::open(options, path, &db);
  CHECK(status.is_corruption());
}

// ------------------------------------------------- hostile block contents ---
//
// The tests above damage files that this engine wrote.  These ones build the
// bytes by hand, because the failures they cover are not reachable by flipping
// a bit at random — they need a specific, valid-looking, deliberately wrong
// field.  Each was found by an adversarial review and each crashed the engine
// before the fix.

namespace {

void put_varint32_into(std::string* dst, uint32_t value) {
  while (value >= 0x80) {
    dst->push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  dst->push_back(static_cast<char>(value));
}

void put_fixed32_into(std::string* dst, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    dst->push_back(static_cast<char>((value >> (8 * i)) & 0xff));
  }
}

// Walks a hand-built block both ways.  Returns the iterator's status; the point
// is that it returns at all.
Status walk_block(const std::string& bytes, const Comparator* comparator,
                  std::string_view seek_target) {
  BlockContents contents;
  contents.data = bytes;
  contents.heap_allocated = false;
  contents.cachable = false;

  Block block(contents);
  std::unique_ptr<Iterator> iter(block.new_iterator(comparator));

  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    (void)iter->key();
    (void)iter->value();
  }
  Status status = iter->status();

  iter->seek(seek_target);
  if (iter->valid()) {
    (void)iter->key();
  }
  if (status.is_ok()) status = iter->status();

  iter->seek_to_last();
  while (iter->valid()) {
    (void)iter->key();
    iter->prev();
  }
  if (status.is_ok()) status = iter->status();
  return status;
}

}  // namespace

// The entry declares a key of 0xffffffff bytes and a value of one.  Summed as
// uint32 that is zero, so a bounds check written in 32 bits passes and the
// engine then copies four gigabytes out of a fifteen-byte block.
TEST(corrupt, an_entry_whose_lengths_overflow_is_refused) {
  std::string block;
  put_varint32_into(&block, 0);           // shared
  put_varint32_into(&block, 0xffffffffu); // non_shared
  put_varint32_into(&block, 1);           // value_length
  block += "k";

  const auto restart_offset = static_cast<uint32_t>(block.size());
  put_fixed32_into(&block, 0);
  put_fixed32_into(&block, 1);
  (void)restart_offset;

  const Status status = walk_block(block, bytewise_comparator(), "k");
  CHECK(status.is_corruption());
}

// A three-byte key handed to the internal comparator, which reads an eight-byte
// trailer from the end of it — five bytes before the key begins.
TEST(corrupt, a_key_too_short_for_the_comparator_is_refused) {
  std::string block;
  put_varint32_into(&block, 0);
  put_varint32_into(&block, 3);
  put_varint32_into(&block, 0);
  block += "abc";

  put_fixed32_into(&block, 0);
  put_fixed32_into(&block, 1);

  const std::string target =
      make_internal_key("abc", kMaxSequenceNumber, kValueTypeForSeek);
  const Status status = walk_block(block, internal_key_comparator(), target);
  CHECK(status.is_corruption());

  // The same key is perfectly legal under the bytewise comparator, which reads
  // no trailer.  The rejection is about the comparator, not the key.
  CHECK_OK(walk_block(block, bytewise_comparator(), "abc"));
}

// A restart point that lies past the end of the entry region.
TEST(corrupt, a_restart_point_outside_the_entries_is_refused) {
  std::string block;
  put_varint32_into(&block, 0);
  put_varint32_into(&block, 1);
  put_varint32_into(&block, 0);
  block += "a";

  put_fixed32_into(&block, 0);
  put_fixed32_into(&block, 0x7fffffffu);  // nowhere near the block
  put_fixed32_into(&block, 2);

  const Status status = walk_block(block, bytewise_comparator(), "a");
  CHECK(!status.is_ok());
}

// A block handle whose size or offset falls outside the file it came from.
// Before the fix, the size was capped at a gigabyte and nothing else, so a
// forty-eight byte file could ask for a gigabyte of memory.
TEST(corrupt, a_block_handle_pointing_outside_the_file_is_refused) {
  TempDir dir;
  const std::string path = dir.file("small.sst");
  write_file(path, std::string(64, 'x'));

  std::unique_ptr<RandomAccessFile> file;
  CHECK_OK(RandomAccessFile::open(path, &file));

  struct Case { uint64_t offset; uint64_t size; };
  for (const Case& c : {Case{0, 1ull << 30},        // a gigabyte from a 64-byte file
                        Case{0, 64},                // no room for the trailer
                        Case{60, 10},               // runs off the end
                        Case{1ull << 40, 8},        // an offset past everything
                        Case{0, ~0ull}}) {          // a size that overflows
    BlockHandle handle;
    handle.set_offset(c.offset);
    handle.set_size(c.size);

    BlockContents contents;
    const Status status = read_block(file.get(), 64, handle, &contents);
    if (status.is_ok()) {
      std::printf("    handle offset=%llu size=%llu was accepted\n",
                  static_cast<unsigned long long>(c.offset),
                  static_cast<unsigned long long>(c.size));
      CHECK(false);
      return;
    }
  }
}

// The filter block's last byte is a shift amount taken straight from the file.
// A value of 200 makes `offset >> base_lg_` undefined.
TEST(corrupt, a_filter_with_an_impossible_region_size_answers_may_match) {
  const auto policy = new_bloom_filter_policy(10);

  std::vector<std::string_view> keys = {"a", "b", "c"};
  std::string filter;
  FilterBlockBuilder builder(policy.get());
  builder.start_block(0);
  for (const std::string_view key : keys) builder.add_key(key);
  filter.assign(builder.finish());

  for (const int base : {64, 65, 200, 255}) {
    std::string damaged = filter;
    damaged.back() = static_cast<char>(base);
    FilterBlockReader reader(policy.get(), damaged);
    // "May match" is the only safe answer for a filter that cannot be read.
    CHECK(reader.key_may_match(0, "a"));
    CHECK(reader.key_may_match(0, "definitely absent"));
  }
}

// A manifest naming a file whose key is too short to be an internal key.  Every
// comparison the version set makes on it would read before the string.
TEST(corrupt, a_manifest_with_a_short_internal_key_is_refused) {
  VersionEdit edit;
  edit.set_comparator_name(internal_key_comparator()->name);
  edit.set_log_number(1);
  edit.set_next_file(2);
  edit.set_last_sequence(0);
  edit.add_file(1, /*file=*/5, /*file_size=*/100, "short", "alsoshort");

  std::string encoded;
  edit.encode_to(&encoded);

  VersionEdit decoded;
  CHECK(decoded.decode_from(encoded).is_corruption());
}

// A manifest naming a compaction pointer whose key is too short to be an
// internal key.  Unlike a file's smallest/largest bounds, a compaction
// pointer is compared directly by compare_internal_keys wherever a compaction
// picks up where the last one left off -- so the same eight-byte trailer read
// applies here, five bytes before the string for a key this short.
TEST(corrupt, a_manifest_with_a_short_compaction_pointer_is_refused) {
  VersionEdit edit;
  edit.set_comparator_name(internal_key_comparator()->name);
  edit.set_log_number(1);
  edit.set_next_file(2);
  edit.set_last_sequence(0);
  edit.set_compact_pointer(1, "short");

  std::string encoded;
  edit.encode_to(&encoded);

  VersionEdit decoded;
  CHECK(decoded.decode_from(encoded).is_corruption());
}

// A log record that is a complete, empty batch: the twelve-byte header --
// sequence 0, count 0 -- and nothing after it.  Legal, and cheap to forge.
// Recovery used to compute the batch's last sequence as sequence + count - 1,
// and count - 1 on a uint32_t of zero is 4294967295: the database opened,
// replayed nothing, and then handed out sequence numbers from four billion.
// Nothing a read could see went wrong, which is why it needs a test that asks
// the engine directly.
TEST(corrupt, an_empty_batch_in_the_log_does_not_skip_four_billion_sequences) {
  TempDir dir;
  const std::string path = dir.file("db");
  {
    Options options;
    options.create_if_missing = true;
    std::unique_ptr<DB> db;
    CHECK_OK(DB::open(options, path, &db));
  }

  // The live log, which the next open replays.
  std::string log_path;
  for (const std::string& file : files_in(path)) {
    uint64_t number = 0;
    FileType type;
    const std::string name = std::filesystem::path(file).filename().string();
    if (parse_file_name(name, &number, &type) && type == FileType::kLog) {
      log_path = file;
    }
  }
  CHECK(!log_path.empty());
  if (log_path.empty()) return;

  {
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open(log_path, /*append=*/false, &file));
    LogWriter writer(std::move(file));
    CHECK_OK(writer.add_record(std::string(12, '\0')));
    CHECK_OK(writer.sync());
    CHECK_OK(writer.close());
  }

  Options options;
  options.create_if_missing = false;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(options, path, &db));

  std::string sequence;
  CHECK(db->get_property("ambar.last-sequence", &sequence));
  CHECK_EQ(sequence, std::string("0"));

  // And the numbering carries on from there, not from four billion.
  CHECK_OK(db->put(WriteOptions(), "k", "v"));
  CHECK(db->get_property("ambar.last-sequence", &sequence));
  CHECK_EQ(sequence, std::string("1"));
}

// A file name whose number does not fit in 64 bits must not wrap around into a
// number the engine is using.
TEST(corrupt, an_absurd_file_number_is_not_parsed) {
  uint64_t number = 0;
  FileType type;
  CHECK(!parse_file_name("18446744073709551617.log", &number, &type));
  CHECK(!parse_file_name("99999999999999999999999999.sst", &number, &type));
  CHECK(!parse_file_name("MANIFEST-18446744073709551617", &number, &type));

  // And the ordinary cases still work.
  CHECK(parse_file_name("000123.log", &number, &type));
  CHECK_EQ(number, 123u);
  CHECK(parse_file_name("MANIFEST-000007", &number, &type));
  CHECK_EQ(number, 7u);
}
