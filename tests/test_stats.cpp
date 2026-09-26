// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Write amplification: the bytes an LSM tree writes for each byte it is
// handed.  The engine counts what it writes -- log records, tables from
// flushes and from compactions, manifest edits -- and reports the totals
// through get_property("ambar.bytes-written").  A count that missed one of
// the writers would report a number that looked plausible and was wrong,
// so it is checked here against a second count the engine cannot see: the
// simulated disk's tally of what was appended to files of each kind.
// tools/bench divides the engine's number by the bytes it handed in; this
// is what makes the quotient worth printing.
//
// Read amplification the same way: every read of a table file, counted by
// the engine where it opens them and by the disk where it serves them.
// And group commit: the records the log received and the batches they
// carried, counted from both ends of the writer queue.

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ambar/db.hpp"
#include "ambar/filter_policy.hpp"
#include "harness.hpp"
#include "sim_file_system.hpp"
#include "wal.hpp"

using namespace ambar;

namespace {

struct Disk {
  Disk() : fs(SimFileSystem::Model(), /*seed=*/1) {
    previous = set_file_system(&fs);
    fs.create_directory("sim");
    fs.sync_directory("sim");
  }
  ~Disk() { set_file_system(previous); }

  SimFileSystem fs;
  FileSystem* previous;
};

// Small, so that a few thousand keys produce real flushes and real
// compactions rather than sitting in one memtable and testing nothing.
Options small_options() {
  Options options;
  options.create_if_missing = true;
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

// About a hundred bytes, varying a little, so that the log's records fall
// on its block boundaries in every way they can -- a record that fits, one
// that is split, a block with too few bytes left for a header and padded.
std::string value_of(int i) {
  std::string out = "v" + std::to_string(i) + ":";
  while (out.size() < 110) out += "0123456789abcdef";
  return out.substr(0, static_cast<size_t>(96 + i % 11));
}

// Puts `count` keys from `first` and returns the bytes of key and value
// handed in, which is what amplification is measured against.
uint64_t put_keys(DB* db, int first, int count) {
  uint64_t handed = 0;
  for (int i = first; i < first + count; ++i) {
    const std::string key = key_of(i);
    const std::string value = value_of(i);
    CHECK_OK(db->put(WriteOptions(), key, value));
    handed += key.size() + value.size();
  }
  return handed;
}

// A property of "name 123\n" lines, as a map.
std::map<std::string, uint64_t> lines_of(DB* db, const char* property) {
  std::string text;
  CHECK(db->get_property(property, &text));
  std::map<std::string, uint64_t> out;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t space = text.find(' ', pos);
    const size_t newline = text.find('\n', pos);
    if (space == std::string::npos || newline == std::string::npos) break;
    out[text.substr(pos, space - pos)] =
        std::stoull(text.substr(space + 1, newline - space - 1));
    pos = newline + 1;
  }
  return out;
}

std::map<std::string, uint64_t> bytes_written(DB* db) {
  return lines_of(db, "ambar.bytes-written");
}

std::map<std::string, uint64_t> log_writes(DB* db) {
  return lines_of(db, "ambar.log-writes");
}

std::map<std::string, uint64_t> table_reads(DB* db) {
  return lines_of(db, "ambar.table-reads");
}

uint64_t at(const std::map<std::string, uint64_t>& written, const char* kind) {
  const auto it = written.find(kind);
  return it == written.end() ? 0 : it->second;
}

unsigned long long ull(uint64_t n) {
  return static_cast<unsigned long long>(n);
}

}  // namespace

TEST(stats, counts_every_byte_the_disk_saw) {
  // Twice: with the tables raw, and compressed as they are by default.
  // Compressed bytes are counted like any others -- the counts are of what
  // reached a file -- but there are fewer of them, which the sanity check
  // on the totals has to know about.
  uint64_t raw_table_bytes = 0;
  for (const auto compression :
       {Options::Compression::kNone, Options::Compression::kLz}) {
    const bool compressed = compression == Options::Compression::kLz;
    Disk disk;
    Options options = small_options();
    options.compression = compression;
    std::unique_ptr<DB> db;
    CHECK_OK(DB::open(options, "sim/db", &db));

    // Enough keys to flush many times and compact several; then half of
    // them again, so a compaction has versions to drop; then the whole
    // tree swept, so nothing is left in motion when the counts are
    // compared.
    uint64_t handed = put_keys(db.get(), 0, 4000);
    for (int i = 0; i < 4000; i += 2) {
      const std::string key = key_of(i);
      const std::string value = value_of(i + 1);
      CHECK_OK(db->put(WriteOptions(), key, value));
      handed += key.size() + value.size();
    }
    db->compact_range(nullptr, nullptr);

    const auto engine = bytes_written(db.get());
    const SimFileSystem& fs = disk.fs;

    // The workload has to have reached every writer being counted.  Sizes
    // that kept everything in one memtable would pass with nothing checked.
    CHECK(fs.count("create .sst") >= 8);
    CHECK(at(engine, "flush") > 0);
    CHECK(at(engine, "compaction") > 0);
    CHECK(at(engine, "manifest") > 0);

    // Every byte, of every kind, by both counts.
    CHECK_EQ(at(engine, "log"), fs.bytes_appended(".log"));
    CHECK_EQ(at(engine, "flush") + at(engine, "compaction"),
             fs.bytes_appended(".sst"));
    CHECK_EQ(at(engine, "manifest"), fs.bytes_appended("MANIFEST"));

    const uint64_t tables = at(engine, "flush") + at(engine, "compaction");
    const uint64_t total = at(engine, "log") + tables + at(engine, "manifest");
    // The log alone holds everything handed in, plus headers, compressed
    // or not: records are never compressed.  Raw, the tables hold it all
    // again at least once; compressed, they hold it in fewer bytes than
    // that -- these values compress -- and in more than none.
    CHECK(at(engine, "log") > handed);
    if (!compressed) {
      CHECK(total > 2 * handed);
      raw_table_bytes = tables;
    } else {
      CHECK(tables < raw_table_bytes);
    }
    std::printf("    %s: write amplification %.2f, %llu bytes written for "
                "%llu handed in (log %llu, flush %llu, compaction %llu, "
                "manifest %llu)\n",
                compressed ? "compressed" : "raw",
                static_cast<double>(total) / static_cast<double>(handed),
                ull(total), ull(handed), ull(at(engine, "log")),
                ull(at(engine, "flush")), ull(at(engine, "compaction")),
                ull(at(engine, "manifest")));

    // The human-readable form carries the same totals.
    std::string stats;
    CHECK(db->get_property("ambar.stats", &stats));
    CHECK(stats.find("written since open: log") != std::string::npos);
  }
}

// The log writer's count is of what reached the file, not of what it was
// given: seven bytes of header per fragment, and the zeros that pad out a
// block with too little room left for a header.  A workload rarely lands
// on that last case -- the workload tests here did not, and a mutation
// that stopped counting the padding survived them -- so it is hit on
// purpose: a record sized to leave three bytes in the block, then one more.
TEST(stats, the_log_counts_its_headers_and_padding) {
  Disk disk;
  std::unique_ptr<WritableFile> file;
  CHECK_OK(WritableFile::open("sim/000001.log", /*append=*/false, &file));
  LogWriter log(std::move(file));

  const std::string filling(kBlockSize - kHeaderSize - 3, 'a');
  CHECK_OK(log.add_record(filling));
  CHECK_EQ(log.bytes_written(), uint64_t{kBlockSize - 3});
  CHECK_OK(log.add_record("b"));
  CHECK_OK(log.flush());

  // Three bytes of padding, then a header and one byte in the next block.
  CHECK_EQ(log.bytes_written(), uint64_t{kBlockSize + kHeaderSize + 1});
  CHECK_EQ(log.bytes_written(), disk.fs.bytes_appended(".log"));
}

// The reads, by the same method: every read of a table file -- by a
// compaction, by the check each new table gets, by a lookup, by a scan --
// counted by the engine where it opens the file and by the disk where it
// serves the read, and the two must agree.
TEST(stats, counts_every_table_read_the_disk_served) {
  Disk disk;
  const auto policy = new_bloom_filter_policy(10);
  Options options = small_options();
  options.filter_policy = policy.get();
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(options, "sim/db", &db));
  put_keys(db.get(), 0, 4000);
  db->compact_range(nullptr, nullptr);

  // Compaction read its inputs, and every output was read back once.
  const auto compacted = table_reads(db.get());
  CHECK(at(compacted, "reads") > 0);
  CHECK_EQ(at(compacted, "reads"), disk.fs.reads(".sst"));
  CHECK_EQ(at(compacted, "bytes"), disk.fs.bytes_read(".sst"));

  // Then lookups for keys that are there; for keys that are not, placed
  // between keys that are, so that the range check lets them through and
  // the filter is what answers them; and a scan over everything.
  std::string value;
  for (int i = 0; i < 4000; i += 7) {
    CHECK_OK(db->get(ReadOptions(), key_of(i), &value));
  }
  const uint64_t before_absent = at(table_reads(db.get()), "reads");
  for (int i = 0; i < 500; ++i) {
    CHECK(db->get(ReadOptions(), key_of(i * 7) + "-", &value).is_not_found());
  }
  // The filter answered nearly all of them: a few false positives read a
  // block each, against one block for nearly every present key above.
  const uint64_t absent_reads =
      at(table_reads(db.get()), "reads") - before_absent;
  CHECK(absent_reads < 50);
  {
    std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
    int scanned = 0;
    for (iter->seek_to_first(); iter->valid(); iter->next()) ++scanned;
    CHECK_EQ(scanned, 4000);
  }
  const auto looked_up = table_reads(db.get());
  CHECK(at(looked_up, "reads") > at(compacted, "reads"));
  CHECK_EQ(at(looked_up, "reads"), disk.fs.reads(".sst"));
  CHECK_EQ(at(looked_up, "bytes"), disk.fs.bytes_read(".sst"));
}

// A block the cache holds is not read again: the second lookup of a key
// costs the disk nothing.  That is the whole point of the cache, and the
// reason the benchmark's read curve moves with its size.
TEST(stats, a_lookup_the_cache_answers_reads_nothing) {
  Disk disk;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(small_options(), "sim/db", &db));
  put_keys(db.get(), 0, 4000);
  db->compact_range(nullptr, nullptr);

  std::string value;
  CHECK_OK(db->get(ReadOptions(), key_of(1234), &value));
  const uint64_t after_first = at(table_reads(db.get()), "reads");
  CHECK_OK(db->get(ReadOptions(), key_of(1234), &value));
  CHECK_EQ(at(table_reads(db.get()), "reads"), after_first);
}

// Group commit, counted: one record per group, and the batches it carried.
// Written one at a time there is never anyone to group with, so the two
// counts are equal and both are the number of writes; written from several
// threads at once they are not, and the difference is what group commit
// saved.  How many groups form depends on scheduling, so the threaded half
// pins what must hold on any machine -- every batch counted once, never
// more records than batches -- and prints what it saw.  tools/bench reports
// the size of the groups under a load it controls.
TEST(stats, counts_the_batches_each_log_write_carried) {
  Disk disk;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(small_options(), "sim/db", &db));

  put_keys(db.get(), 0, 200);
  {
    WriteBatch batch;
    batch.put("a", "1");
    batch.put("b", "2");
    CHECK_OK(db->write(WriteOptions(), &batch));
  }
  auto serial = log_writes(db.get());
  CHECK_EQ(at(serial, "records"), uint64_t{201});
  CHECK_EQ(at(serial, "batches"), uint64_t{201});

  // A flush through compact_range is a write with no batch: it joins no
  // group and counts as none.
  db->compact_range(nullptr, nullptr);
  serial = log_writes(db.get());
  CHECK_EQ(at(serial, "records"), uint64_t{201});
  CHECK_EQ(at(serial, "batches"), uint64_t{201});

  constexpr int kThreads = 8;
  constexpr int kPerThread = 250;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      WriteOptions sync;
      sync.sync = true;
      for (int i = 0; i < kPerThread; ++i) {
        const int id = 1000 + t * kPerThread + i;
        if (!db->put(sync, key_of(id), value_of(id)).is_ok()) ++failures;
      }
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK_EQ(failures.load(), 0);

  const auto grouped = log_writes(db.get());
  const uint64_t batches = at(grouped, "batches") - 201;
  const uint64_t records = at(grouped, "records") - 201;
  CHECK_EQ(batches, uint64_t{kThreads * kPerThread});
  CHECK(records >= 1);
  CHECK(records <= batches);
  // Behind a synced leader a follower parks at once rather than watching,
  // so every batch that rode another's record was a writer that slept:
  // parked is at least batches less records.  Written one at a time, above,
  // nobody had anyone to wait for.
  CHECK_EQ(at(serial, "parked"), uint64_t{0});
  CHECK(at(grouped, "parked") >= batches - records);
  std::printf("    %llu writes from %d threads went to the log in %llu "
              "records: %.2f batches per record\n",
              ull(batches), kThreads, ull(records),
              static_cast<double>(batches) / static_cast<double>(records));
}

TEST(stats, counts_what_recovery_writes_and_starts_from_open) {
  Disk disk;
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::open(small_options(), "sim/db", &db));
    // Fewer than a memtable holds, so the log carries all of it into the
    // next open and nothing has been flushed when recovery starts.
    put_keys(db.get(), 0, 300);
  }
  const SimFileSystem& fs = disk.fs;
  const uint64_t log_before = fs.bytes_appended(".log");
  const uint64_t manifest_before = fs.bytes_appended("MANIFEST");
  CHECK(log_before > 0);
  CHECK_EQ(fs.bytes_appended(".sst"), uint64_t{0});

  std::unique_ptr<DB> db;
  CHECK_OK(DB::open(small_options(), "sim/db", &db));
  const auto engine = bytes_written(db.get());

  // Recovery writes the replayed log out as a table and every open writes
  // a fresh manifest; the new log has nothing in it yet.  All of it is
  // counted, and nothing from the previous open is.
  CHECK(at(engine, "flush") > 0);
  CHECK(at(engine, "manifest") > 0);
  CHECK_EQ(at(engine, "log"), uint64_t{0});
  CHECK_EQ(fs.bytes_appended(".log"), log_before);
  CHECK_EQ(at(engine, "flush") + at(engine, "compaction"),
           fs.bytes_appended(".sst"));
  CHECK_EQ(at(engine, "manifest"),
           fs.bytes_appended("MANIFEST") - manifest_before);
}
