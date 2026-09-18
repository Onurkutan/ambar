// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// What the block coder in src/compress.hpp is worth on real blocks: the
// ratio it reaches and the rate it compresses and decompresses at, over
// the data blocks of an existing database.  It reads the tables directly --
// footer, index, each data block by its handle -- so the input is exactly
// what the builder handed the coder, restart array and internal-key
// trailers included, and not a value stream that would flatter or damn it.
// A block stored compressed is decoded first and the raw form is what is
// measured.
//
// The numbers a reader wants beside these are LZ4's and zlib's on the same
// bytes, and those are not linked here, since the engine has no
// dependencies: --dump writes the blocks to a file and
// tools/compare_codecs.py runs the other coders over it.  docs/BENCHMARKS.md
// has both tables.
//
// Usage: codec_bench <db-dir> [--max-mb N] [--dump <file>]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ambar/iterator.hpp"
#include "ambar/status.hpp"
#include "block.hpp"
#include "comparator.hpp"
#include "compress.hpp"
#include "encoding.hpp"
#include "format.hpp"

using namespace ambar;
using Clock = std::chrono::steady_clock;

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

// Every data block of one table, raw, appended to *blocks.  Returns what
// went wrong with the file, if anything did; a table this cannot read is
// reported and skipped rather than trusted.
Status data_blocks(const std::string& bytes, std::vector<std::string>* blocks,
                   size_t* stored_compressed) {
  if (bytes.size() < Footer::kEncodedLength) {
    return Status::corruption("shorter than a footer");
  }
  Footer footer;
  std::string_view tail(bytes.data() + bytes.size() - Footer::kEncodedLength,
                        Footer::kEncodedLength);
  Status status = footer.decode_from(&tail);
  if (!status.is_ok()) return status;

  auto slice = [&](const BlockHandle& handle, std::string_view* out,
                   int* type) -> bool {
    const uint64_t end = handle.offset() + handle.size();
    if (handle.offset() > bytes.size() || end > bytes.size() ||
        end + kBlockTrailerSize > bytes.size()) {
      return false;
    }
    *out = std::string_view(bytes.data() + handle.offset(),
                            static_cast<size_t>(handle.size()));
    *type = static_cast<unsigned char>(bytes[static_cast<size_t>(end)]);
    return true;
  };

  std::string_view index_bytes;
  int index_type = 0;
  if (!slice(footer.index_handle(), &index_bytes, &index_type) ||
      index_type != static_cast<int>(CompressionType::kNone)) {
    return Status::corruption("index handle points outside the file");
  }
  BlockContents contents;
  contents.data = index_bytes;
  contents.cachable = false;
  contents.heap_allocated = false;
  Block index(contents);
  std::unique_ptr<Iterator> iter(index.new_iterator(internal_key_comparator()));
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    std::string_view encoded = iter->value();
    BlockHandle handle;
    status = handle.decode_from(&encoded);
    if (!status.is_ok()) return status;
    std::string_view stored;
    int type = 0;
    if (!slice(handle, &stored, &type)) {
      return Status::corruption("data handle points outside the file");
    }
    if (type == static_cast<int>(CompressionType::kLz)) {
      std::string raw;
      status = decompress_block(stored, &raw);
      if (!status.is_ok()) return status;
      *stored_compressed += stored.size();
      blocks->push_back(std::move(raw));
    } else if (type == static_cast<int>(CompressionType::kNone)) {
      blocks->emplace_back(stored);
    } else {
      return Status::corruption("unknown compression type in block trailer");
    }
  }
  return iter->status();
}

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <db-dir> [--max-mb N] [--dump <file>]\n", argv[0]);
    return 2;
  }
  const std::filesystem::path dir(argv[1]);
  size_t max_bytes = static_cast<size_t>(64) << 20;
  std::string dump;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--max-mb" && i + 1 < argc) {
      max_bytes = static_cast<size_t>(std::atoi(argv[++i])) << 20;
    } else if (arg == "--dump" && i + 1 < argc) {
      dump = argv[++i];
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return 2;
    }
  }

  // Tables in name order, so that two runs over one database see the same
  // blocks in the same order.
  std::vector<std::filesystem::path> tables;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".sst") tables.push_back(entry.path());
  }
  std::sort(tables.begin(), tables.end());

  std::vector<std::string> blocks;
  size_t raw_bytes = 0;
  size_t stored_compressed = 0;
  int files = 0;
  for (const auto& path : tables) {
    if (raw_bytes >= max_bytes) break;
    const std::string bytes = read_file(path);
    std::vector<std::string> from_this;
    const Status status = data_blocks(bytes, &from_this, &stored_compressed);
    if (!status.is_ok()) {
      std::fprintf(stderr, "skipping %s: %s\n", path.string().c_str(),
                   status.to_string().c_str());
      continue;
    }
    ++files;
    for (std::string& block : from_this) {
      raw_bytes += block.size();
      blocks.push_back(std::move(block));
    }
  }
  if (blocks.empty()) {
    std::fprintf(stderr, "no data blocks under %s\n", dir.string().c_str());
    return 1;
  }

  // Ratio, once.
  std::vector<std::string> compressed(blocks.size());
  size_t compressed_bytes = 0;
  size_t would_shrink = 0;
  for (size_t i = 0; i < blocks.size(); ++i) {
    compress_block(blocks[i], &compressed[i]);
    compressed_bytes += compressed[i].size();
    if (compressed[i].size() < blocks[i].size()) ++would_shrink;
  }

  // Rates: the best of five passes over every block, which is what the
  // coder does when nothing else is in the way.  A pass is well over a
  // hundred milliseconds at these sizes, so the clock is not the noise.
  constexpr int kPasses = 5;
  double compress_seconds = 1e300;
  std::string scratch;
  for (int pass = 0; pass < kPasses; ++pass) {
    const auto start = Clock::now();
    for (const std::string& block : blocks) {
      scratch.clear();
      compress_block(block, &scratch);
    }
    compress_seconds = std::min(compress_seconds, seconds_since(start));
  }
  double decompress_seconds = 1e300;
  bool round_trips = true;
  for (int pass = 0; pass < kPasses; ++pass) {
    const auto start = Clock::now();
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (!decompress_block(compressed[i], &scratch).is_ok() ||
          (pass == 0 && scratch != blocks[i])) {
        round_trips = false;
      }
    }
    decompress_seconds = std::min(decompress_seconds, seconds_since(start));
  }

  constexpr double kMB = 1048576.0;
  const double raw_mb = static_cast<double>(raw_bytes) / kMB;
  std::printf("%zu data blocks from %d tables, %.1f MB raw, %.1f MB compressed"
              " (%.2fx; %zu of %zu blocks would shrink)\n",
              blocks.size(), files, raw_mb,
              static_cast<double>(compressed_bytes) / kMB,
              static_cast<double>(raw_bytes) /
                  static_cast<double>(compressed_bytes),
              would_shrink, blocks.size());
  if (stored_compressed > 0) {
    std::printf("  stored compressed on disk: %.1f MB\n",
                static_cast<double>(stored_compressed) / kMB);
  }
  std::printf("  compress   %7.0f MB/s of input\n", raw_mb / compress_seconds);
  std::printf("  decompress %7.0f MB/s of output\n",
              raw_mb / decompress_seconds);
  if (!round_trips) {
    std::printf("  A BLOCK DID NOT ROUND-TRIP\n");
    return 1;
  }

  if (!dump.empty()) {
    // Each block as a four-byte little-endian length and its bytes, which
    // is what tools/compare_codecs.py reads.
    std::ofstream out(dump, std::ios::binary);
    for (const std::string& block : blocks) {
      char length[4];
      encode_fixed32(length, static_cast<uint32_t>(block.size()));
      out.write(length, sizeof(length));
      out.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    if (!out) {
      std::fprintf(stderr, "could not write %s\n", dump.c_str());
      return 1;
    }
    std::printf("  %zu blocks written to %s\n", blocks.size(), dump.c_str());
  }
  return 0;
}
