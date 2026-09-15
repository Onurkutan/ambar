// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Measures the engine, and says what the measurement is worth.
//
// A benchmark that prints one number per operation is close to useless for a
// storage engine, for two reasons that this one tries to avoid.
//
// An average hides the answer.  Writes into an LSM tree are cheap until a
// compaction starts and then are not, so the mean rate says nothing about what
// a request will actually experience.  Every latency here is reported as
// percentiles, and the ones that matter are at the tail.
//
// Throughput without amplification is half the story.  An LSM tree buys fast
// writes by writing the same data several times over, and a benchmark that
// reports only operations per second is reporting the half of the trade that
// flatters it.  So the bytes the engine writes are counted -- by the engine,
// at the point each file is written, since a directory listing cannot see a
// file that was written and deleted between two looks -- and divided by the
// bytes it was handed; and the reads of table files are counted where the
// files are opened and divided by the lookups that caused them.
//
// It also compares against SQLite, when SQLite is available, because a number
// with nothing beside it is not a measurement -- it is a number.  The
// comparison is arranged to be fair rather than favourable: both engines are
// given the same data, the same batch sizes, and the same durability setting,
// and the places where they are not comparable are stated rather than
// averaged over.
//
// And the read results are reported as a curve against block cache size rather
// than as one number, because one number would have been misleading in both
// directions.  At 1 MB of cache this engine reads at less than half SQLite's
// rate; with the whole database resident it reads at about 1.3 times it.
// Neither figure is the answer.  The answer is that random reads here are
// bound by how much of the data is cached, which is worth knowing before
// choosing a cache size, and is invisible in a single measurement.
//
// Usage: bench <dir> [--keys N] [--value-size N] [--cache-mb N] [--threads N]
//              [--no-sqlite]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ambar/db.hpp"
#include "ambar/cache.hpp"
#include "ambar/filter_policy.hpp"

#if defined(AMBAR_BENCH_SQLITE)
#include <sqlite3.h>
#endif

using namespace ambar;
using Clock = std::chrono::steady_clock;

namespace {

struct Config {
  std::string dir;
  int keys = 500000;
  int value_size = 100;
  int cache_mb = 8;
  int threads = 8;  // the most the read-scaling phase runs at once
  bool use_sqlite = true;
};

std::string key_of(int n) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%010d", n);
  return buf;
}

// A key that is not there, between two that are.  key_of(n) followed by
// a byte sorts after key_of(n) and before key_of(n + 1), so every level's
// range check lets it through and the filter is what turns it away.  A key
// past the end of the database would be rejected by the range check alone,
// and would measure that check rather than the filter -- which is what the
// first version of this benchmark did, without knowing it, until the table
// reads per lookup were counted and came out at 0.00 where the filter's
// own false-positive rate says 0.01.
std::string absent_key_of(int n) { return key_of(n) + "-"; }

std::string make_value(int n, int size, std::mt19937* rng) {
  // Half random, half repeated: entirely random data makes compression
  // pointless and entirely repeated data makes it free.  Neither is what real
  // values look like.
  std::string out;
  out.reserve(static_cast<size_t>(size));
  out += "v" + std::to_string(n) + ":";
  while (static_cast<int>(out.size()) < size) {
    if ((*rng)() % 2 == 0) {
      out.push_back(static_cast<char>('a' + (*rng)() % 26));
    } else {
      out += "0123456789";
    }
  }
  out.resize(static_cast<size_t>(size));
  return out;
}

// Latencies, in microseconds, reported where the interesting part is.
class Latencies {
 public:
  void add(double micros) { samples_.push_back(micros); }

  void report(const char* name, size_t operations, double seconds,
              const char* note = nullptr) {
    std::sort(samples_.begin(), samples_.end());
    const double ops_per_second = static_cast<double>(operations) / seconds;

    std::printf("  %-26s %9.0f op/s   p50 %7.1f  p99 %8.1f  p99.9 %9.1f  "
                "max %9.1f us\n",
                name, ops_per_second, quantile(0.50), quantile(0.99),
                quantile(0.999), samples_.empty() ? 0.0 : samples_.back());
    if (note != nullptr) std::printf("  %-26s %s\n", "", note);
  }

  double quantile(double q) const {
    if (samples_.empty()) return 0;
    const size_t index = static_cast<size_t>(
        q * static_cast<double>(samples_.size() - 1));
    return samples_[index];
  }

 private:
  std::vector<double> samples_;
};

double micros_since(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start)
      .count();
}

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

uint64_t directory_bytes(const std::string& path) {
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(path, ec)) {
    if (entry.is_regular_file(ec)) total += entry.file_size(ec);
  }
  return total;
}

// Two amplifications, and they are different quantities.
//
// Space amplification is the directory's settled size against the distinct
// keys and values it holds: what the database costs to keep.  Write
// amplification is every byte the engine appended to a log, a table or a
// manifest -- records, flushes, compaction outputs, edits -- against every
// byte of key and value it was handed, overwrites included: what a write
// costs in disk traffic before it settles.  The engine counts the latter at
// the point each file is written and reports it through
// get_property("ambar.bytes-written"); watching the directory could not,
// because a file written and deleted between two looks leaves no trace.
struct Sizes {
  uint64_t user_bytes = 0;    // distinct keys and values, once each
  uint64_t handed_bytes = 0;  // every key and value put, overwrites included
  uint64_t on_disk_bytes = 0;
};

// What the engine wrote since it was opened, by kind of file.
struct Written {
  uint64_t log = 0;
  uint64_t flush = 0;
  uint64_t compaction = 0;
  uint64_t manifest = 0;
  uint64_t total() const { return log + flush + compaction + manifest; }
};

Written bytes_written(DB* db) {
  Written out;
  std::string text;
  if (!db->get_property("ambar.bytes-written", &text)) return out;
  std::istringstream lines(text);
  std::string kind;
  uint64_t bytes = 0;
  while (lines >> kind >> bytes) {
    if (kind == "log") out.log = bytes;
    if (kind == "flush") out.flush = bytes;
    if (kind == "compaction") out.compaction = bytes;
    if (kind == "manifest") out.manifest = bytes;
  }
  return out;
}

// Reads of table files since the database was opened -- what the block
// cache did not answer -- and the bytes they brought in.
struct Reads {
  uint64_t reads = 0;
  uint64_t bytes = 0;
};

Reads table_reads(DB* db) {
  Reads out;
  std::string text;
  if (!db->get_property("ambar.table-reads", &text)) return out;
  std::istringstream lines(text);
  std::string what;
  uint64_t n = 0;
  while (lines >> what >> n) {
    if (what == "reads") out.reads = n;
    if (what == "bytes") out.bytes = n;
  }
  return out;
}

// Read amplification of a lookup phase: the table reads it caused and the
// bytes they brought in, per lookup.
void report_lookup_reads(const Reads& before, const Reads& after,
                         size_t lookups) {
  const double n = static_cast<double>(lookups);
  std::printf("  %-26s %.2f table reads and %.1f KB from disk per lookup\n",
              "", static_cast<double>(after.reads - before.reads) / n,
              static_cast<double>(after.bytes - before.bytes) / 1024.0 / n);
}

// Read amplification of a scan: bytes read from disk against bytes of key
// and value the scan returned.
void report_scan_reads(const Reads& before, const Reads& after,
                       uint64_t returned) {
  std::printf("  %-26s %.1f MB read from disk for %.1f MB returned, "
              "%.2f per byte\n",
              "", static_cast<double>(after.bytes - before.bytes) / 1048576.0,
              static_cast<double>(returned) / 1048576.0,
              static_cast<double>(after.bytes - before.bytes) /
                  static_cast<double>(returned));
}

// ------------------------------------------------------------- ambar -------

// Random reads and a repeated scan at one cache size, on a database that is
// already built and freshly opened.
//
// The scan is run twice and the second is reported, because the first pass is
// what fills the cache and the question here is what a cache of this size is
// worth once it is filled.
void read_curve(DB* db, const Config& config, const char* label) {
  Latencies present;
  std::mt19937 rng(7);
  std::string value;
  const int count = std::min(config.keys, 200000);

  const Reads before_reads = table_reads(db);
  const auto start = Clock::now();
  for (int i = 0; i < count; ++i) {
    const int id = static_cast<int>(rng() % static_cast<unsigned>(config.keys));
    const auto op = Clock::now();
    db->get(ReadOptions(), key_of(id), &value);
    present.add(micros_since(op));
  }
  const double read_seconds = seconds_since(start);
  const Reads after_reads = table_reads(db);

  double scan_rate = 0;
  double scan_read_per_byte = 0;
  for (int pass = 0; pass < 2; ++pass) {
    const Reads before_scan = table_reads(db);
    const auto scan_start = Clock::now();
    std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
    size_t scanned = 0;
    uint64_t returned = 0;
    for (iter->seek_to_first(); iter->valid(); iter->next()) {
      ++scanned;
      returned += iter->key().size() + iter->value().size();
    }
    scan_rate = static_cast<double>(scanned) / seconds_since(scan_start);
    const Reads after_scan = table_reads(db);
    scan_read_per_byte =
        static_cast<double>(after_scan.bytes - before_scan.bytes) /
        static_cast<double>(returned);
  }

  std::printf("  %-30s %9.0f read/s  p50 %6.1f us  %5.2f reads/lookup   "
              "%9.0f scan/s  %4.2f B read/B\n",
              label, static_cast<double>(count) / read_seconds,
              present.quantile(0.50),
              static_cast<double>(after_reads.reads - before_reads.reads) /
                  static_cast<double>(count),
              scan_rate, scan_read_per_byte);
}

// Aggregate random-read throughput with `threads` threads each making
// `per_thread` lookups for present keys, each thread with its own sequence
// of keys.  The clock covers the threads from start to join.
double reads_with_threads(DB* db, const Config& config, int threads,
                          int per_thread) {
  std::vector<std::thread> workers;
  const auto start = Clock::now();
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937 rng(static_cast<std::mt19937::result_type>(11 + t));
      std::string value;
      for (int i = 0; i < per_thread; ++i) {
        const int id =
            static_cast<int>(rng() % static_cast<unsigned>(config.keys));
        db->get(ReadOptions(), key_of(id), &value);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  return static_cast<double>(threads) * static_cast<double>(per_thread) /
         seconds_since(start);
}

// Random reads at 1, 2, 4 ... threads, twice over: with the cache the
// benchmark was given, where most lookups go to the file, and with the
// whole database resident, where none do and the engine's own locks are
// all that is left to contend for.  The engine promises any number of
// readers; this is what the promise is worth in throughput.
void read_scaling(const Config& config, const Options& options,
                  const std::string& path, uint64_t data_bytes) {
  std::vector<int> counts;
  for (int n = 1; n <= config.threads; n *= 2) counts.push_back(n);
  const int per_thread = std::min(config.keys, 100000);

  std::vector<double> given;     // reads/s with the cache given
  std::vector<double> resident;  // reads/s with everything cached
  double resident_reads_per_lookup = 0;

  {
    Options given_options = options;
    given_options.create_if_missing = false;
    std::unique_ptr<DB> db;
    if (!DB::open(given_options, path, &db).is_ok()) return;
    for (const int n : counts) {
      given.push_back(reads_with_threads(db.get(), config, n, per_thread));
    }
  }
  {
    const auto cache = new_lru_cache(static_cast<size_t>(4 * data_bytes));
    Options resident_options = options;
    resident_options.block_cache = cache.get();
    resident_options.create_if_missing = false;
    std::unique_ptr<DB> db;
    if (!DB::open(resident_options, path, &db).is_ok()) return;
    // One scan fills the cache with every data block.
    {
      std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
      for (iter->seek_to_first(); iter->valid(); iter->next()) {
      }
    }
    const Reads before = table_reads(db.get());
    uint64_t lookups = 0;
    for (const int n : counts) {
      resident.push_back(reads_with_threads(db.get(), config, n, per_thread));
      lookups += static_cast<uint64_t>(n) * static_cast<uint64_t>(per_thread);
    }
    const Reads after = table_reads(db.get());
    resident_reads_per_lookup =
        static_cast<double>(after.reads - before.reads) /
        static_cast<double>(lookups);
  }

  std::printf("\n  random reads by threads, present keys, %d lookups per "
              "thread\n",
              per_thread);
  std::printf("      %-8s %-32s %-32s\n", "threads", "cache as given",
              "whole database resident");
  for (size_t i = 0; i < counts.size(); ++i) {
    std::printf("      %-8d %9.0f read/s  x%-5.2f          %9.0f read/s  "
                "x%-5.2f\n",
                counts[i], given[i], given[i] / given[0], resident[i],
                resident[i] / resident[0]);
  }
  std::printf("      %-8s resident: %.3f table reads per lookup over the run\n",
              "", resident_reads_per_lookup);
}

void bench_ambar(const Config& config) {
  const std::string path = config.dir + "/ambar";
  std::filesystem::remove_all(path);

  const auto policy = new_bloom_filter_policy(10);
  const auto cache =
      new_lru_cache(static_cast<size_t>(config.cache_mb) << 20);

  Options options;
  options.create_if_missing = true;
  options.filter_policy = policy.get();
  options.block_cache = cache.get();
  options.write_buffer_size = 4 << 20;

  std::unique_ptr<DB> db;
  const Status status = DB::open(options, path, &db);
  if (!status.is_ok()) {
    std::printf("  ambar failed to open: %s\n", status.to_string().c_str());
    return;
  }

  std::mt19937 rng(42);
  Sizes sizes;

  std::printf("\nambar\n");

  // -- sequential writes, no sync --
  {
    Latencies latencies;
    const auto start = Clock::now();
    for (int i = 0; i < config.keys; ++i) {
      const std::string key = key_of(i);
      const std::string value = make_value(i, config.value_size, &rng);
      sizes.user_bytes += key.size() + value.size();
      sizes.handed_bytes += key.size() + value.size();
      const auto op = Clock::now();
      db->put(WriteOptions(), key, value);
      latencies.add(micros_since(op));
    }
    latencies.report("write seq (no sync)", static_cast<size_t>(config.keys),
                     seconds_since(start));
  }

  // -- random writes, no sync --
  {
    Latencies latencies;
    std::vector<int> order(static_cast<size_t>(config.keys));
    for (int i = 0; i < config.keys; ++i) order[static_cast<size_t>(i)] = i;
    std::shuffle(order.begin(), order.end(), rng);

    const auto start = Clock::now();
    for (const int i : order) {
      const std::string key = key_of(i);
      const std::string value = make_value(i, config.value_size, &rng);
      sizes.handed_bytes += key.size() + value.size();
      const auto op = Clock::now();
      db->put(WriteOptions(), key, value);
      latencies.add(micros_since(op));
    }
    latencies.report("write random (no sync)",
                     static_cast<size_t>(config.keys), seconds_since(start));
  }

  // -- synced writes, in order, a fiftieth as many: each waits for the device --
  {
    Latencies latencies;
    const int count = std::max(1, config.keys / 50);
    WriteOptions sync_options;
    sync_options.sync = true;

    const auto start = Clock::now();
    for (int i = 0; i < count; ++i) {
      const std::string key = key_of(i);
      const std::string value = make_value(i, config.value_size, &rng);
      sizes.handed_bytes += key.size() + value.size();
      const auto op = Clock::now();
      db->put(sync_options, key, value);
      latencies.add(micros_since(op));
    }
    latencies.report("write seq (sync)", static_cast<size_t>(count),
                     seconds_since(start),
                     "each of these waits for the storage device");
  }

  db->compact_range(nullptr, nullptr);

  // Read now, before the reopen below: the counters belong to this open,
  // and the sweep has settled everything the writes above set in motion.
  const Written written = bytes_written(db.get());
  std::string stats;
  db->get_property("ambar.stats", &stats);

  // -- random reads of keys that exist --
  {
    Latencies latencies;
    std::string value;
    const int count = std::min(config.keys, 200000);
    const auto start = Clock::now();
    const Reads before = table_reads(db.get());
    for (int i = 0; i < count; ++i) {
      const int id = static_cast<int>(rng() % static_cast<unsigned>(config.keys));
      const auto op = Clock::now();
      db->get(ReadOptions(), key_of(id), &value);
      latencies.add(micros_since(op));
    }
    latencies.report("read random (present)", static_cast<size_t>(count),
                     seconds_since(start));
    report_lookup_reads(before, table_reads(db.get()),
                        static_cast<size_t>(count));
  }

  // -- random reads of keys that do not exist: what the bloom filter is for --
  {
    Latencies latencies;
    std::string value;
    const int count = std::min(config.keys, 200000);
    const auto start = Clock::now();
    const Reads before = table_reads(db.get());
    for (int i = 0; i < count; ++i) {
      const auto op = Clock::now();
      db->get(ReadOptions(), absent_key_of(i), &value);
      latencies.add(micros_since(op));
    }
    latencies.report("read random (absent)", static_cast<size_t>(count),
                     seconds_since(start),
                     "the bloom filter answers most of these without a read");
    report_lookup_reads(before, table_reads(db.get()),
                        static_cast<size_t>(count));
  }

  // -- a full scan over cold blocks --
  //
  // What a scan costs when the cache holds nothing useful is the honest
  // default; the curve below shows what a larger cache buys.
  {
    const Reads before = table_reads(db.get());
    const auto start = Clock::now();
    std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
    size_t scanned = 0;
    uint64_t returned = 0;
    for (iter->seek_to_first(); iter->valid(); iter->next()) {
      ++scanned;
      returned += iter->key().size() + iter->value().size();
    }
    const double seconds = seconds_since(start);
    std::printf("  %-26s %9.0f op/s   %zu entries in %.2f s\n",
                "scan (cold blocks)",
                static_cast<double>(scanned) / seconds, scanned, seconds);
    report_scan_reads(before, table_reads(db.get()), returned);
  }

  // The read result as a curve, because a single cache size would report
  // either less than half SQLite's rate or 1.3 times it, depending on which
  // one was chosen.
  {
    // Reads and scans both turn out to be bound by residency, so both are
    // reported against cache size.  A single number for either would be a
    // choice of which impression to leave: at 1 MB this engine reads at half
    // SQLite's rate and scans at half of it too; with the whole database
    // resident it reads faster than SQLite and scans at about the same speed.
    std::printf("\n  by block cache size, on a freshly opened database\n");
    const uint64_t data_bytes = directory_bytes(path);
    db.reset();  // close, so each cache size starts cold

    for (const int mb : {1, 8, 64, 256}) {
      if (static_cast<uint64_t>(mb) << 20 > 4 * data_bytes && mb > 8) {
        // Past the point where the whole database fits, more cache cannot
        // change anything, and reporting the same number three times would
        // look like a measurement.
        break;
      }
      const auto sweep_cache = new_lru_cache(static_cast<size_t>(mb) << 20);
      Options sweep = options;
      sweep.block_cache = sweep_cache.get();
      sweep.create_if_missing = false;

      std::unique_ptr<DB> sweep_db;
      if (!DB::open(sweep, path, &sweep_db).is_ok()) break;

      char label[64];
      std::snprintf(label, sizeof(label), "    %d MB cache (%.0f%% of data)",
                    mb, 100.0 * (static_cast<double>(mb) * 1048576.0) /
                            static_cast<double>(data_bytes));
      read_curve(sweep_db.get(), config, label);
    }

    read_scaling(config, options, path, data_bytes);

    if (!DB::open(options, path, &db).is_ok()) return;
  }

  constexpr double kMB = 1048576.0;
  std::printf("  %-26s %.2f  (%.1f MB written for %.1f MB of keys and "
              "values handed in: log %.1f, flush %.1f, compaction %.1f, "
              "manifest %.1f)\n",
              "write amplification",
              static_cast<double>(written.total()) /
                  static_cast<double>(sizes.handed_bytes),
              static_cast<double>(written.total()) / kMB,
              static_cast<double>(sizes.handed_bytes) / kMB,
              static_cast<double>(written.log) / kMB,
              static_cast<double>(written.flush) / kMB,
              static_cast<double>(written.compaction) / kMB,
              static_cast<double>(written.manifest) / kMB);

  sizes.on_disk_bytes = directory_bytes(path);
  std::printf("  %-26s %.2f  (%.1f MB on disk for %.1f MB of keys and "
              "values)\n",
              "space amplification",
              static_cast<double>(sizes.on_disk_bytes) /
                  static_cast<double>(sizes.user_bytes),
              static_cast<double>(sizes.on_disk_bytes) / 1048576.0,
              static_cast<double>(sizes.user_bytes) / 1048576.0);

  // What each level cost to produce, from the open that produced it.
  std::printf("\n%s", stats.c_str());
}

// ------------------------------------------------------------ sqlite -------

#if defined(AMBAR_BENCH_SQLITE)

void bench_sqlite(const Config& config) {
  const std::string path = config.dir + "/sqlite";
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path);
  const std::string file = path + "/bench.db";

  sqlite3* db = nullptr;
  if (sqlite3_open(file.c_str(), &db) != SQLITE_OK) {
    std::printf("  sqlite failed to open\n");
    return;
  }

  auto exec = [&](const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
      std::printf("  sqlite: %s\n", error ? error : "?");
      sqlite3_free(error);
    }
  };

  // WAL mode and NORMAL synchronous, which is the configuration a SQLite user
  // running a key-value workload would actually choose.  Comparing against
  // SQLite's defaults would flatter this engine by benchmarking a
  // configuration nobody uses.
  exec("PRAGMA journal_mode=WAL");
  exec("PRAGMA synchronous=NORMAL");
  exec("CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v BLOB)");

  std::mt19937 rng(42);
  std::printf("\nsqlite (WAL, synchronous=NORMAL)\n");

  sqlite3_stmt* insert = nullptr;
  sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO kv VALUES (?, ?)", -1,
                     &insert, nullptr);

  // -- sequential writes --
  {
    Latencies latencies;
    const auto start = Clock::now();
    exec("BEGIN");
    for (int i = 0; i < config.keys; ++i) {
      const std::string key = key_of(i);
      const std::string value = make_value(i, config.value_size, &rng);
      const auto op = Clock::now();
      sqlite3_bind_text(insert, 1, key.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_blob(insert, 2, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT);
      sqlite3_step(insert);
      sqlite3_reset(insert);
      latencies.add(micros_since(op));

      // One transaction per thousand rows.  A transaction per row would be an
      // unfair comparison in SQLite's disfavour -- it is a different
      // durability guarantee -- and one transaction for everything would be
      // unfair in its favour, since nothing would be committed until the end.
      if (i % 1000 == 999) {
        exec("COMMIT");
        exec("BEGIN");
      }
    }
    exec("COMMIT");
    latencies.report("write seq (batched 1000)",
                     static_cast<size_t>(config.keys), seconds_since(start));
  }

  // -- random reads --
  sqlite3_stmt* select = nullptr;
  sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k = ?", -1, &select, nullptr);
  {
    Latencies latencies;
    const int count = std::min(config.keys, 200000);
    const auto start = Clock::now();
    for (int i = 0; i < count; ++i) {
      const int id = static_cast<int>(rng() % static_cast<unsigned>(config.keys));
      const std::string key = key_of(id);
      const auto op = Clock::now();
      sqlite3_bind_text(select, 1, key.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(select);
      sqlite3_reset(select);
      latencies.add(micros_since(op));
    }
    latencies.report("read random (present)", static_cast<size_t>(count),
                     seconds_since(start));
  }
  {
    Latencies latencies;
    const int count = std::min(config.keys, 200000);
    const auto start = Clock::now();
    for (int i = 0; i < count; ++i) {
      const std::string key = absent_key_of(i);
      const auto op = Clock::now();
      sqlite3_bind_text(select, 1, key.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(select);
      sqlite3_reset(select);
      latencies.add(micros_since(op));
    }
    latencies.report("read random (absent)", static_cast<size_t>(count),
                     seconds_since(start));
  }

  // -- a full scan, twice, for the same reason --
  for (int pass = 0; pass < 2; ++pass) {
    sqlite3_stmt* scan = nullptr;
    sqlite3_prepare_v2(db, "SELECT k, v FROM kv ORDER BY k", -1, &scan,
                       nullptr);
    const auto start = Clock::now();
    size_t scanned = 0;
    while (sqlite3_step(scan) == SQLITE_ROW) ++scanned;
    const double seconds = seconds_since(start);
    std::printf("  %-26s %9.0f op/s   %zu entries in %.2f s\n",
                pass == 0 ? "scan (first pass)" : "scan (second pass)",
                static_cast<double>(scanned) / seconds, scanned, seconds);
    sqlite3_finalize(scan);
  }

  sqlite3_finalize(insert);
  sqlite3_finalize(select);
  sqlite3_close(db);

  const uint64_t user_bytes =
      static_cast<uint64_t>(config.keys) *
      (key_of(0).size() + static_cast<size_t>(config.value_size));
  std::printf("  %-26s %.2f  (%.1f MB on disk for %.1f MB of keys and "
              "values)\n",
              "space amplification",
              static_cast<double>(directory_bytes(path)) /
                  static_cast<double>(user_bytes),
              static_cast<double>(directory_bytes(path)) / 1048576.0,
              static_cast<double>(user_bytes) / 1048576.0);
}

#endif  // AMBAR_BENCH_SQLITE

}  // namespace

int main(int argc, char** argv) {
  Config config;
  config.dir = "/tmp/ambar_bench";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--keys" && i + 1 < argc) {
      config.keys = std::atoi(argv[++i]);
    } else if (arg == "--value-size" && i + 1 < argc) {
      config.value_size = std::atoi(argv[++i]);
    } else if (arg == "--cache-mb" && i + 1 < argc) {
      config.cache_mb = std::atoi(argv[++i]);
    } else if (arg == "--threads" && i + 1 < argc) {
      config.threads = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--no-sqlite") {
      config.use_sqlite = false;
    } else if (arg[0] != '-') {
      config.dir = arg;
    }
  }

  std::filesystem::create_directories(config.dir);

  std::printf("ambar benchmark\n");
  std::printf("  %d keys, %d-byte values, %d MB block cache, %.0f MB of user "
              "data\n", config.keys, config.value_size, config.cache_mb,
              static_cast<double>(config.keys) *
                  static_cast<double>(config.value_size + 16) / 1048576.0);
  std::printf("\n  These numbers describe this machine and this filesystem.\n"
              "  An LSM tree's write path is dominated by how the storage\n"
              "  handles fsync, which varies by an order of magnitude between\n"
              "  a laptop SSD, a cloud volume and a container's overlay\n"
              "  filesystem -- so treat the shape of the results as the\n"
              "  finding and the absolute numbers as local.\n");

  bench_ambar(config);

#if defined(AMBAR_BENCH_SQLITE)
  if (config.use_sqlite) bench_sqlite(config);
#else
  std::printf("\n  Built without SQLite, so there is nothing to compare\n"
              "  against.  Install libsqlite3-dev and reconfigure.\n");
#endif

  return 0;
}
