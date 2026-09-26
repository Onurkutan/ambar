// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Compression in the table format.  tests/test_compress.cpp checks the
// codec; this checks what the engine does with it: that a table written
// with compression on reads back the same as one written with it off, by
// lookup and by scan in both directions, and across a reopen and a
// compaction; that the trailer byte records the choice per block, so a
// block that did not shrink by an eighth is stored raw and a database
// written with compression on is read by a build with it off; that the
// index, metaindex and filter are never compressed; that the block cache
// is charged the decoded size; and that a compressed block that is
// damaged, or forged
// with a valid checksum over a stream the decoder must refuse, or labelled
// with a type this build does not know, is an error and not a crash.

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "ambar/cache.hpp"
#include "ambar/db.hpp"
#include "ambar/filter_policy.hpp"
#include "block.hpp"
#include "compress.hpp"
#include "encoding.hpp"
#include "comparator.hpp"
#include "file.hpp"
#include "format.hpp"
#include "harness.hpp"
#include "table_builder.hpp"
#include "testutil.hpp"

using namespace ambar;

namespace {

using ambar_test::TempDir;

std::string key_of(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%08d", i);
  return buf;
}

// Values with structure: a prefix the neighbours share and a run, which is
// what compression is for.
std::string structured_value(int i) {
  return "value_" + std::to_string(i) + "_" +
         std::string(60, static_cast<char>('a' + i % 26));
}

std::string random_value(int i, size_t n = 80) {
  std::mt19937 rng(static_cast<uint32_t>(i));
  std::string out(n, '\0');
  for (char& c : out) c = static_cast<char>(rng() & 0xff);
  return out;
}

Options options_with(Options::Compression compression) {
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 64 << 10;
  options.max_file_size = 1 << 20;
  options.block_size = 4096;
  options.compression = compression;
  return options;
}

uint64_t directory_bytes(const std::string& path) {
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
    if (entry.is_regular_file(ec) &&
        entry.path().extension() == ".sst") {
      total += entry.file_size(ec);
    }
  }
  return total;
}

bool agrees(DB* db, const std::map<std::string, std::string>& model,
            const char* where) {
  std::string value;
  for (const auto& [key, expected] : model) {
    const Status status = db->get(ReadOptions(), key, &value);
    if (!status.is_ok() || value != expected) {
      std::printf("    [%s] get(%s): %s\n", where, key.c_str(),
                  status.to_string().c_str());
      return false;
    }
  }
  std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
  auto it = model.begin();
  for (iter->seek_to_first(); iter->valid(); iter->next(), ++it) {
    if (it == model.end() || iter->key() != it->first ||
        iter->value() != it->second) {
      std::printf("    [%s] forward scan diverged at %s\n", where,
                  std::string(iter->key()).c_str());
      return false;
    }
  }
  if (it != model.end()) {
    std::printf("    [%s] forward scan ended early\n", where);
    return false;
  }
  auto rit = model.rbegin();
  for (iter->seek_to_last(); iter->valid(); iter->prev(), ++rit) {
    if (rit == model.rend() || iter->key() != rit->first) {
      std::printf("    [%s] reverse scan diverged\n", where);
      return false;
    }
  }
  return iter->status().is_ok();
}

std::string read_file(const std::string& path) {
  std::string out;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

void write_file(const std::string& path, const std::string& bytes) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return;
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
}

std::vector<std::string> tables_in(const std::string& path) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
    if (entry.path().extension() == ".sst") out.push_back(entry.path().string());
  }
  return out;
}

// Where the first data block ends: the first offset L at which the five
// bytes after the block are a trailer whose checksum covers bytes 0..L.
// A data block is written first, at offset 0, so the first such L is its
// length.  Returns 0 if none is found.
size_t first_block_length(const std::string& bytes) {
  for (size_t length = 1; length + kBlockTrailerSize <= bytes.size();
       ++length) {
    const uint32_t stored = decode_fixed32(bytes.data() + length + 1);
    const uint32_t actual = crc32c(std::string_view(bytes.data(), length + 1));
    if (stored == actual) return length;
  }
  return 0;
}

}  // namespace

TEST(compressed_tables, reads_back_what_was_written_with_compression_on) {
  TempDir dir;
  std::map<std::string, std::string> model;
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::open(options_with(Options::Compression::kLz), dir.file("db"),
                      &db));
    for (int i = 0; i < 6000; ++i) {
      CHECK_OK(db->put(WriteOptions(), key_of(i), structured_value(i)));
      model[key_of(i)] = structured_value(i);
    }
    // Some overwritten and some deleted, so compaction has versions to
    // resolve inside compressed blocks.
    for (int i = 0; i < 6000; i += 3) {
      CHECK_OK(db->put(WriteOptions(), key_of(i), structured_value(i + 1)));
      model[key_of(i)] = structured_value(i + 1);
    }
    for (int i = 0; i < 6000; i += 7) {
      CHECK_OK(db->del(WriteOptions(), key_of(i)));
      model.erase(key_of(i));
    }
    CHECK(agrees(db.get(), model, "before compaction"));
    db->compact_range(nullptr, nullptr);
    CHECK(agrees(db.get(), model, "after compaction"));
  }
  // Reopened with compression on, and with it off: the trailer byte says
  // what each block is, and a build that has the codec reads either.
  for (const auto compression :
       {Options::Compression::kLz, Options::Compression::kNone}) {
    Options options = options_with(compression);
    options.create_if_missing = false;
    std::unique_ptr<DB> db;
    CHECK_OK(DB::open(options, dir.file("db"), &db));
    CHECK(agrees(db.get(), model,
                 compression == Options::Compression::kLz ? "reopen, on"
                                                          : "reopen, off"));
  }
}

TEST(compressed_tables, structured_data_shrinks_and_random_data_barely_does) {
  // The same keys written with structured values and with random ones,
  // with compression on and off.  A database's blocks always have some
  // structure -- the keys, the internal-key trailers -- so even random
  // values compress a little; what is checked is the shape, and the
  // raw-storage rule is checked below on a block with no structure at all.
  uint64_t structured_on = 0;
  uint64_t structured_off = 0;
  uint64_t random_on = 0;
  uint64_t random_off = 0;
  for (const bool structured : {true, false}) {
    for (const auto compression :
         {Options::Compression::kLz, Options::Compression::kNone}) {
      TempDir dir;
      {
        std::unique_ptr<DB> db;
        CHECK_OK(DB::open(options_with(compression), dir.file("db"), &db));
        for (int i = 0; i < 4000; ++i) {
          CHECK_OK(db->put(WriteOptions(), key_of(i),
                           structured ? structured_value(i) : random_value(i)));
        }
        db->compact_range(nullptr, nullptr);
      }
      const uint64_t bytes = directory_bytes(dir.file("db"));
      const bool on = compression == Options::Compression::kLz;
      if (structured && on) structured_on = bytes;
      if (structured && !on) structured_off = bytes;
      if (!structured && on) random_on = bytes;
      if (!structured && !on) random_off = bytes;
    }
  }
  std::printf("    tables: structured %llu -> %llu with compression, random "
              "values %llu -> %llu\n",
              static_cast<unsigned long long>(structured_off),
              static_cast<unsigned long long>(structured_on),
              static_cast<unsigned long long>(random_off),
              static_cast<unsigned long long>(random_on));
  CHECK(structured_on < structured_off / 2);
  // Random values: the keys still compress, the values do not, and no
  // block saves the eighth that is worth a decode on every read, so the
  // tables are stored as they would have been without compression -- and
  // never larger, since a block that would grow is stored as it was too.
  CHECK(random_on <= random_off);
  CHECK(random_on > random_off * 9 / 10);
}

TEST(compressed_tables, a_block_that_saves_less_than_an_eighth_is_stored_raw) {
  // A database of random values: the keys and the internal-key trailers
  // compress a little and the values not at all, so a block comes out a
  // few percent smaller -- which is not worth a decode on every read of
  // it.  The builder stores it raw.  The proof that the rule made that
  // choice, and not the data, is that the block's own bytes, handed to
  // the coder here, do come out smaller.
  TempDir dir;
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::open(options_with(Options::Compression::kLz), dir.file("db"),
                      &db));
    for (int i = 0; i < 4000; ++i) {
      CHECK_OK(db->put(WriteOptions(), key_of(i), random_value(i)));
    }
    db->compact_range(nullptr, nullptr);
  }
  const auto tables = tables_in(dir.file("db"));
  CHECK(!tables.empty());
  if (tables.empty()) return;
  const std::string bytes = read_file(tables[0]);
  const size_t block = first_block_length(bytes);
  CHECK(block > 0);
  if (block == 0) return;
  CHECK_EQ(static_cast<int>(static_cast<unsigned char>(bytes[block])), 0);

  std::string compressed;
  compress_block(std::string_view(bytes.data(), block), &compressed);
  std::printf("    first block: %zu bytes, %zu compressed, %.1f%% saved\n",
              block, compressed.size(),
              100.0 * (1.0 - static_cast<double>(compressed.size()) /
                                 static_cast<double>(block)));
  CHECK(compressed.size() < block);               // it would have shrunk,
  CHECK(compressed.size() >= block - block / 8);  // by less than an eighth
}

TEST(compressed_tables, a_block_that_would_not_shrink_is_stored_raw) {
  // A table built by hand from random keys and random values: no byte of
  // any block repeats another, the compressor cannot shrink it, and the
  // builder writes the block as it was with kNone in the trailer -- so a
  // read costs no decode and the file costs no extra bytes.  The same
  // table with structured entries gets kLz.
  for (const bool structured : {false, true}) {
    TempDir dir;
    const std::string path = dir.file("t.sst");
    Options options = options_with(Options::Compression::kLz);
    {
      std::unique_ptr<WritableFile> file;
      CHECK_OK(WritableFile::open(path, false, &file));
      TableBuilder builder(options, file.get(), bytewise_comparator());
      std::mt19937 rng(99);
      std::string key(16, 'k');
      for (int i = 0; i < 400; ++i) {
        if (structured) {
          builder.add(key_of(i), structured_value(i));
        } else {
          // Keys must ascend: a counter in the first bytes, noise after.
          key[0] = static_cast<char>(i >> 8);
          key[1] = static_cast<char>(i & 0xff);
          for (size_t k = 2; k < key.size(); ++k) {
            key[k] = static_cast<char>(rng() & 0xff);
          }
          builder.add(key, random_value(i, 60));
        }
      }
      CHECK_OK(builder.finish());
      CHECK_OK(file->close());
    }
    const std::string bytes = read_file(path);
    const size_t block = first_block_length(bytes);
    CHECK(block > 0);
    if (block == 0) continue;
    const int type = static_cast<unsigned char>(bytes[block]);
    // Against the literal values, not the enum: the byte on disk is the
    // format.  0 is what every 0.1.0 block carries and 1 is what
    // docs/DESIGN.md says a compressed block carries, which a 0.1.0 reader
    // refuses by name; renumbering the enum would change the files.
    CHECK_EQ(type, structured ? 1 : 0);
  }
}

TEST(compressed_tables, the_index_metaindex_and_filter_are_never_compressed) {
  // The blocks a table is opened through are read once and held, so there
  // is nothing to save by compressing them, and the format says they are
  // always raw.  Their trailers are found through the footer, and the
  // filter's through the metaindex, so this is also the path a reader
  // takes.  The index would shrink -- 4000 separators with a shared
  // prefix -- so a builder that compressed everything would be seen here.
  TempDir dir;
  const auto filter = new_bloom_filter_policy(10);
  {
    Options options = options_with(Options::Compression::kLz);
    options.filter_policy = filter.get();
    std::unique_ptr<DB> db;
    CHECK_OK(DB::open(options, dir.file("db"), &db));
    for (int i = 0; i < 4000; ++i) {
      CHECK_OK(db->put(WriteOptions(), key_of(i), structured_value(i)));
    }
    db->compact_range(nullptr, nullptr);
  }
  const auto tables = tables_in(dir.file("db"));
  CHECK(!tables.empty());
  if (tables.empty()) return;
  const std::string bytes = read_file(tables[0]);
  CHECK(bytes.size() > Footer::kEncodedLength);
  if (bytes.size() <= Footer::kEncodedLength) return;

  auto type_of = [&](const BlockHandle& handle) -> int {
    const size_t at = static_cast<size_t>(handle.offset() + handle.size());
    CHECK(at < bytes.size());
    return at < bytes.size() ? static_cast<unsigned char>(bytes[at]) : -1;
  };

  Footer footer;
  std::string_view tail(bytes.data() + bytes.size() - Footer::kEncodedLength,
                        Footer::kEncodedLength);
  CHECK_OK(footer.decode_from(&tail));
  CHECK_EQ(type_of(footer.index_handle()), 0);
  CHECK_EQ(type_of(footer.metaindex_handle()), 0);

  // The filter, through the metaindex entry that names it.
  const BlockHandle& meta = footer.metaindex_handle();
  BlockContents contents;
  contents.data = std::string_view(bytes.data() + meta.offset(),
                                   static_cast<size_t>(meta.size()));
  contents.cachable = false;
  contents.heap_allocated = false;
  Block metaindex(contents);
  std::unique_ptr<Iterator> iter(metaindex.new_iterator(bytewise_comparator()));
  const std::string filter_key = std::string("filter.") + filter->name();
  iter->seek(filter_key);
  CHECK(iter->valid());
  if (!iter->valid()) return;
  CHECK_EQ(std::string(iter->key()), filter_key);
  BlockHandle filter_handle;
  std::string_view encoded = iter->value();
  CHECK_OK(filter_handle.decode_from(&encoded));
  CHECK_EQ(type_of(filter_handle), 0);

  // And the data block behind the first index entry is compressed, so the
  // table this was checked on is one where compression was in effect.
  const BlockHandle& index = footer.index_handle();
  contents.data = std::string_view(bytes.data() + index.offset(),
                                   static_cast<size_t>(index.size()));
  Block index_block(contents);
  std::unique_ptr<Iterator> first(index_block.new_iterator(
      bytewise_comparator()));
  first->seek_to_first();
  CHECK(first->valid());
  if (!first->valid()) return;
  BlockHandle data_handle;
  encoded = first->value();
  CHECK_OK(data_handle.decode_from(&encoded));
  CHECK_EQ(type_of(data_handle), 1);
}

TEST(compressed_tables, the_block_cache_is_charged_the_decoded_size) {
  // A cache sized to hold the decoded data with room to spare.  If blocks
  // were charged their compressed size the cache would report a fraction
  // of the bytes it actually holds, and the bound in Options::block_cache
  // would be a lie by that fraction.
  TempDir dir;
  const auto cache = new_lru_cache(64 << 20);
  Options options = options_with(Options::Compression::kLz);
  options.block_cache = cache.get();
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(options, dir.file("db"), &db));
  for (int i = 0; i < 4000; ++i) {
    CHECK_OK(db->put(WriteOptions(), key_of(i), structured_value(i)));
  }
  db->compact_range(nullptr, nullptr);
  const uint64_t on_disk = directory_bytes(dir.file("db"));

  // A scan brings every data block into the cache.
  {
    std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
    for (iter->seek_to_first(); iter->valid(); iter->next()) {
    }
  }
  const size_t charged = cache->total_charge();
  std::printf("    %llu bytes on disk, %zu charged to the cache\n",
              static_cast<unsigned long long>(on_disk), charged);
  // The decoded blocks are several times the compressed file.
  CHECK(charged > 2 * on_disk);
}

TEST(compressed_tables, a_damaged_compressed_block_is_an_error) {
  // Every data block in a compressed table is a compressed stream under a
  // checksum.  Flipping a bit in one trips the checksum, as it does for a
  // raw block, and the decoder is never reached; forging the checksum over
  // the damaged stream is what reaches it, and then the decoder has to
  // refuse whatever the flip made of that byte -- a corrupt length, an
  // offset before the start, a match past the declared size.  Both are
  // done here, for every byte of the first block.
  TempDir dir;
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::open(options_with(Options::Compression::kLz), dir.file("db"),
                      &db));
    for (int i = 0; i < 2000; ++i) {
      CHECK_OK(db->put(WriteOptions(), key_of(i), structured_value(i)));
    }
    db->compact_range(nullptr, nullptr);
  }
  const auto tables = tables_in(dir.file("db"));
  CHECK(!tables.empty());
  if (tables.empty()) return;
  const std::string original = read_file(tables[0]);
  const size_t block = first_block_length(original);
  CHECK(block > 0);
  if (block == 0) return;
  // The first block is compressed: its trailer byte says so.
  CHECK_EQ(static_cast<int>(static_cast<unsigned char>(original[block])),
           static_cast<int>(CompressionType::kLz));

  // Opens, looks up the first key and scans everything.  A scan that ends
  // cleanly short of 2000 entries is reported as a corruption of its own,
  // since the engine did not refuse it.
  auto read_all = [&]() -> Status {
    Options options = options_with(Options::Compression::kLz);
    options.create_if_missing = false;
    std::unique_ptr<DB> db;
    Status status = DB::open(options, dir.file("db"), &db);
    if (!status.is_ok()) return status;
    std::string value;
    status = db->get(ReadOptions(), key_of(0), &value);
    if (!status.is_ok()) return status;
    std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
    int seen = 0;
    for (iter->seek_to_first(); iter->valid(); iter->next()) ++seen;
    if (!iter->status().is_ok()) return iter->status();
    if (seen != 2000) return Status::corruption("scan came up short");
    return Status::ok();
  };

  int checksum_caught = 0;
  int decoder_caught = 0;
  int parser_caught = 0;
  int came_up_short = 0;
  int decoded_anyway = 0;
  int read_wrong = 0;
  for (size_t offset = 0; offset < block; ++offset) {
    // The flip alone: caught by the checksum.
    std::string damaged = original;
    damaged[offset] = static_cast<char>(damaged[offset] ^ 0x01);
    write_file(tables[0], damaged);
    if (read_all().is_ok()) {
      ++read_wrong;
    } else {
      ++checksum_caught;
    }

    // The flip with the checksum forged over it: reaches the decoder.
    uint32_t crc = crc32c(std::string_view(damaged.data(), block + 1));
    char forged[4];
    encode_fixed32(forged, crc);
    damaged.replace(block + 1, 4, forged, 4);
    write_file(tables[0], damaged);
    const Status status = read_all();
    const std::string text = status.to_string();
    if (status.is_ok()) {
      // Some flips leave a decodable stream that decodes to different
      // bytes -- a literal changed, say.  Then the block parser or the
      // comparator sees the change, or an entry goes missing and the scan
      // comes up short, or the change is inside a value and nothing sees
      // it.  The checksum was forged, so a wrong answer is not the
      // decoder's failure; what is counted here is that every flip lands
      // in one of these and none crashes.
      ++decoded_anyway;
    } else if (text.find("does not decode") != std::string::npos) {
      ++decoder_caught;
    } else if (text.find("scan came up short") != std::string::npos) {
      ++came_up_short;
    } else {
      ++parser_caught;
    }
  }
  write_file(tables[0], original);

  std::printf("    %zu flips in the first compressed block: %d caught by the "
              "checksum; forged, %d refused by the decoder, %d by the block "
              "parser, %d decoded to a scan that came up short, %d to "
              "something that read\n",
              block, checksum_caught, decoder_caught, parser_caught,
              came_up_short, decoded_anyway);
  CHECK_EQ(read_wrong, 0);
  // The decoder refused some, and never crashed on any: that is the claim.
  // Most single-bit flips in a stream of literals change a literal and
  // decode; flips in tokens, lengths and offsets are what the decoder must
  // refuse or survive.
  CHECK(decoder_caught > 0);

  // A type byte this build does not know, under a checksum that passes:
  // what a later format would write, and what this version says on meeting
  // it -- not corruption, since the checksum covers the byte, but a
  // compression it cannot read, by name, handed to neither the decoder nor
  // the block parser.  The same words a 0.1.0 build has for a block of
  // this version.
  for (const int unknown : {2, 0xff}) {
    std::string damaged = original;
    damaged[block] = static_cast<char>(unknown);
    uint32_t crc = crc32c(std::string_view(damaged.data(), block + 1));
    char forged[4];
    encode_fixed32(forged, crc);
    damaged.replace(block + 1, 4, forged, 4);
    write_file(tables[0], damaged);
    const Status status = read_all();
    CHECK(status.code() == Status::Code::kNotSupported);
    CHECK(status.to_string().find("a compression this build cannot read") !=
          std::string::npos);
  }
  write_file(tables[0], original);
}
