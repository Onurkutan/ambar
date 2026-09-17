// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Writes one small, valid example of each format the fuzzers read, produced
// by the engine's own writers.  A fuzzer that starts from a real block or a
// real table reaches the interesting branches in seconds; one that starts
// from an empty input spends its first minutes rediscovering the magic number.
//
//   fuzz_seeds <dir>
//
// creates <dir>/<target>/... for every fuzz_<target>.  Nothing here is
// checked in: the seeds are regenerated from source, so they cannot drift
// from what the writers actually produce.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include "ambar/filter_policy.hpp"
#include "ambar/options.hpp"
#include "ambar/write_batch.hpp"
#include "block_builder.hpp"
#include "comparator.hpp"
#include "compress.hpp"
#include "dbformat.hpp"
#include "file.hpp"
#include "filter_block.hpp"
#include "table_builder.hpp"
#include "version_edit.hpp"
#include "wal.hpp"
#include "write_batch_internal.hpp"

using namespace ambar;

namespace {

std::string key_of(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%06d", i);
  return buf;
}

// Sequence numbers rise with i; the user keys differ, so the internal order
// follows the user order regardless.
std::string internal(int i) {
  return make_internal_key(key_of(i), static_cast<SequenceNumber>(1000 + i),
                           ValueType::kValue);
}

bool write_bytes(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

std::filesystem::path seed_dir(const std::filesystem::path& root,
                               const char* target) {
  const std::filesystem::path dir = root / target;
  std::filesystem::create_directories(dir);
  return dir;
}

bool seed_version_edit(const std::filesystem::path& root) {
  VersionEdit edit;
  edit.set_comparator_name(internal_key_comparator()->name);
  edit.set_log_number(7);
  edit.set_prev_log_number(6);
  edit.set_next_file(12);
  edit.set_last_sequence(5000);
  edit.set_compact_pointer(1, internal(500));
  edit.remove_file(0, 9);
  edit.add_file(1, 10, 4096, internal(0), internal(199));
  edit.add_file(2, 11, 8192, internal(200), internal(399));

  std::string bytes;
  edit.encode_to(&bytes);
  return write_bytes(seed_dir(root, "version_edit") / "edit", bytes);
}

bool seed_block(const std::filesystem::path& root) {
  const std::filesystem::path dir = seed_dir(root, "block");

  BlockBuilder internal_block(internal_key_comparator(), 16);
  for (int i = 0; i < 64; ++i) {
    internal_block.add(internal(i), "value_" + std::to_string(i));
  }
  bool ok = write_bytes(dir / "internal", internal_block.finish());

  BlockBuilder user_block(bytewise_comparator(), 4);
  for (int i = 0; i < 32; ++i) {
    user_block.add(key_of(i), std::string(static_cast<size_t>(i), 'v'));
  }
  ok = write_bytes(dir / "bytewise", user_block.finish()) && ok;
  return ok;
}

bool seed_filter_block(const std::filesystem::path& root,
                       const FilterPolicy* policy) {
  FilterBlockBuilder builder(policy);
  for (int block = 0; block < 3; ++block) {
    builder.start_block(static_cast<uint64_t>(block) * 4096);
    for (int i = 0; i < 20; ++i) builder.add_key(key_of(block * 100 + i));
  }
  return write_bytes(seed_dir(root, "filter_block") / "filters",
                     builder.finish());
}

bool seed_write_batch(const std::filesystem::path& root) {
  WriteBatch batch;
  batch.put("apple", "red");
  batch.put("banana", "yellow");
  batch.del("cherry");
  batch.put(std::string("nul\0key", 7), std::string(300, 'x'));
  WriteBatchInternal::set_sequence(&batch, 42);
  return write_bytes(seed_dir(root, "write_batch") / "batch",
                     WriteBatchInternal::contents(batch));
}

bool seed_table(const std::filesystem::path& root, const FilterPolicy* policy) {
  const std::filesystem::path path = seed_dir(root, "table") / "table";
  std::unique_ptr<WritableFile> file;
  if (!WritableFile::open(path.string(), /*append=*/false, &file).is_ok()) {
    return false;
  }

  Options options;
  options.filter_policy = policy;
  options.block_size = 512;  // several data blocks, so the index is real
  TableBuilder builder(options, file.get(), internal_key_comparator());
  for (int i = 0; i < 200; ++i) {
    builder.add(internal(i), "value_" + std::to_string(i));
  }
  if (!builder.finish().is_ok()) return false;
  return file->sync().is_ok() && file->close().is_ok();
}

bool seed_wal(const std::filesystem::path& root) {
  const std::filesystem::path path = seed_dir(root, "wal") / "log";
  std::unique_ptr<WritableFile> file;
  if (!WritableFile::open(path.string(), /*append=*/false, &file).is_ok()) {
    return false;
  }

  LogWriter writer(std::move(file));
  for (int i = 0; i < 5; ++i) {
    WriteBatch batch;
    batch.put(key_of(i), "value_" + std::to_string(i));
    batch.del(key_of(i + 100));
    WriteBatchInternal::set_sequence(&batch,
                                     static_cast<SequenceNumber>(1 + 2 * i));
    if (!writer.add_record(WriteBatchInternal::contents(batch)).is_ok()) {
      return false;
    }
  }
  // One record long enough to be split across a 32 KiB block boundary, so the
  // fragment-reassembly path starts out reachable.
  WriteBatch big;
  big.put("big", std::string(40000, 'b'));
  WriteBatchInternal::set_sequence(&big, 11);
  if (!writer.add_record(WriteBatchInternal::contents(big)).is_ok()) {
    return false;
  }
  return writer.sync().is_ok() && writer.close().is_ok();
}

// Compressed blocks of the three shapes the encoder produces: one with
// matches at every length nibble, one that is a single run (an overlapping
// match), and one that is all literals.
bool seed_compress(const std::filesystem::path& root) {
  const std::filesystem::path dir = seed_dir(root, "compress");
  std::string block;
  for (int i = 0; i < 60; ++i) {
    block += key_of(i) + "=value_" + std::to_string(i) + "_";
    block.append(static_cast<size_t>(i % 40), static_cast<char>('a' + i % 26));
  }
  std::string compressed;
  compress_block(block, &compressed);
  bool ok = write_bytes(dir / "block", compressed);

  compressed.clear();
  compress_block(std::string(5000, 'r'), &compressed);
  ok = write_bytes(dir / "run", compressed) && ok;

  std::string noise;
  uint32_t x = 0x9e3779b9u;
  for (int i = 0; i < 700; ++i) {
    x = x * 1664525u + 1013904223u;
    noise.push_back(static_cast<char>(x >> 24));
  }
  compressed.clear();
  compress_block(noise, &compressed);
  return write_bytes(dir / "literals", compressed) && ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
    return 2;
  }
  const std::filesystem::path root(argv[1]);
  const auto policy = new_bloom_filter_policy(10);

  const bool ok = seed_version_edit(root) && seed_block(root) &&
                  seed_filter_block(root, policy.get()) &&
                  seed_write_batch(root) && seed_table(root, policy.get()) &&
                  seed_wal(root) && seed_compress(root);
  if (!ok) {
    std::fprintf(stderr, "could not write every seed under %s\n", argv[1]);
    return 1;
  }
  std::printf("seeds written under %s\n", argv[1]);
  return 0;
}
