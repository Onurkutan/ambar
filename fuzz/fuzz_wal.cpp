// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Arbitrary bytes as a write-ahead log, read the way recovery reads one: every
// record the framing yields is decoded as a WriteBatch.
//
// LogReader reads a SequentialFile and nothing else, so each input touches
// disk once.  One path per process, overwritten every run; the cost is
// throughput, not correctness.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>

#include "ambar/write_batch.hpp"
#include "file.hpp"
#include "wal.hpp"
#include "write_batch_internal.hpp"

namespace {

const std::string& scratch_path() {
  static const std::string path = [] {
    std::random_device seed;
    return (std::filesystem::temp_directory_path() /
            ("ambar_fuzz_wal_" + std::to_string(seed())))
        .string();
  }();
  return path;
}

class Discard final : public ambar::WriteBatch::Handler {
 public:
  void put(std::string_view, std::string_view) override {}
  void del(std::string_view) override {}
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const std::string& path = scratch_path();

  {
    std::unique_ptr<ambar::WritableFile> file;
    if (!ambar::WritableFile::open(path, /*append=*/false, &file).is_ok()) {
      std::abort();  // the harness's problem, not the engine's
    }
    if (!file->append(input).is_ok() || !file->close().is_ok()) std::abort();
  }

  std::unique_ptr<ambar::SequentialFile> source;
  if (!ambar::SequentialFile::open(path, &source).is_ok()) std::abort();

  ambar::LogReader reader(std::move(source));
  Discard discard;
  std::string_view record;
  std::string scratch;
  while (reader.read_record(&record, &scratch)) {
    // Mirrors DBImpl::recover_log_file: a record too short to be a batch ends
    // replay, and one that does not decode ends it too.
    if (record.size() < ambar::WriteBatchInternal::kHeader) break;
    ambar::WriteBatch batch;
    if (!ambar::WriteBatchInternal::set_contents(&batch, record).is_ok()) break;
    (void)batch.iterate(&discard);
  }
  (void)reader.truncated();
  (void)reader.failure_reason();
  return 0;
}
