// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// What a lookup costs when the disk is not involved.  Opens an existing
// database -- the benchmark's, say -- with a block cache large enough to
// hold all of it, scans once so that every block is resident, and then
// measures random lookups of present keys: the heap allocations one makes,
// the latency of one, and the rate on one, two, four and eight threads.
//
// Two things tools/bench does not separate are separated here.  The
// working set: --keys N restricts the lookups to the first N keys, so a
// small N is a hot set that sits in the processor's caches and contends on
// the same few blocks and tables, and the full count is a wide set that
// misses to memory on every lookup and spreads the threads out.  And the
// allocator: every allocation in the process is counted, since a lookup's
// cost is made of small things and nine of them were allocations before
// this counted them.
//
// Usage: lookup_probe <db-dir> [--keys N] [--per-thread N] [--max-threads N]
//
// The keys are the benchmark's, key_%010d, and N must not exceed what the
// database holds.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ambar/cache.hpp"
#include "ambar/db.hpp"
#include "ambar/filter_policy.hpp"

using namespace ambar;
using Clock = std::chrono::steady_clock;

namespace {

// Every allocation the process makes, counted on the thread that made it.
unsigned long long& thread_allocations() {
  thread_local unsigned long long count = 0;
  return count;
}

std::string key_of(int n) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%010d", n);
  return buf;
}

}  // namespace

void* operator new(std::size_t n) {
  ++thread_allocations();
  if (void* p = std::malloc(n > 0 ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <db-dir> [--keys N] [--per-thread N] "
                 "[--max-threads N]\n",
                 argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  int keys = 1000000;
  int per_thread = 200000;
  int max_threads = 8;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--keys" && i + 1 < argc) {
      keys = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--per-thread" && i + 1 < argc) {
      per_thread = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--max-threads" && i + 1 < argc) {
      max_threads = std::max(1, std::atoi(argv[++i]));
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return 2;
    }
  }

  const auto policy = new_bloom_filter_policy(10);
  const auto cache = new_lru_cache(size_t{1} << 30);
  Options options;
  options.filter_policy = policy.get();
  options.block_cache = cache.get();
  options.create_if_missing = false;
  std::unique_ptr<DB> db;
  const Status status = DB::open(options, dir, &db);
  if (!status.is_ok()) {
    std::printf("open: %s\n", status.to_string().c_str());
    return 1;
  }

  // One scan brings every block in; the lookups below never read a file,
  // which the table-read count at the end confirms.
  {
    std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
    size_t entries = 0;
    for (iter->seek_to_first(); iter->valid(); iter->next()) ++entries;
    std::printf("%zu entries scanned, %zu bytes in the block cache; lookups "
                "over the first %d keys\n",
                entries, cache->total_charge(), keys);
  }
  std::string reads_before;
  db->get_property("ambar.table-reads", &reads_before);

  const auto random_lookups = [&](uint32_t seed, int count,
                                  std::vector<double>* latencies) {
    std::mt19937 rng(seed);
    std::string value;
    for (int i = 0; i < count; ++i) {
      const int id = static_cast<int>(rng() % static_cast<unsigned>(keys));
      const auto start = Clock::now();
      db->get(ReadOptions(), key_of(id), &value);
      if (latencies != nullptr) {
        latencies->push_back(
            std::chrono::duration<double, std::micro>(Clock::now() - start)
                .count());
      }
    }
  };

  {
    const unsigned long long before = thread_allocations();
    random_lookups(3, 10000, nullptr);
    std::printf("allocations per lookup: %.2f\n",
                static_cast<double>(thread_allocations() - before) / 10000.0);
  }
  {
    std::vector<double> latencies;
    latencies.reserve(static_cast<size_t>(per_thread));
    random_lookups(7, per_thread, &latencies);
    std::sort(latencies.begin(), latencies.end());
    std::printf("one thread: p50 %.2f  p90 %.2f  p99 %.2f us\n",
                latencies[latencies.size() / 2],
                latencies[latencies.size() * 9 / 10],
                latencies[latencies.size() * 99 / 100]);
  }

  double one = 0;
  for (int threads = 1; threads <= max_threads; threads *= 2) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
      pool.emplace_back([&, t] {
        ready.fetch_add(1);
        while (!go.load()) {
        }
        random_lookups(static_cast<uint32_t>(1000 + t), per_thread, nullptr);
      });
    }
    while (ready.load() < threads) {
    }
    const auto start = Clock::now();
    go.store(true);
    for (auto& thread : pool) thread.join();
    const double seconds =
        std::chrono::duration<double>(Clock::now() - start).count();
    const double rate = static_cast<double>(threads) *
                        static_cast<double>(per_thread) / seconds;
    if (threads == 1) one = rate;
    std::printf("%d thread%s %9.0f lookups/s  x%.2f\n", threads,
                threads == 1 ? ": " : "s:", rate, rate / one);
  }

  std::string reads_after;
  db->get_property("ambar.table-reads", &reads_after);
  if (reads_after != reads_before) {
    std::printf("table reads during the lookups -- the cache did not hold "
                "everything:\n%s", reads_after.c_str());
  }
  return 0;
}
