// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Kills the database mid-write, over and over, and checks what survived.
//
// Every other test in this project runs the engine to completion.  That is the
// case durability is *not* about.  The claims in docs/DESIGN.md are all of the
// form "if the process dies here, then afterwards ...", and the only way to
// test them is to actually kill the process at a moment it did not choose.
//
// How it works.  A child process opens the database and writes in a loop,
// recording what it did to a journal that is a shared memory mapping (see
// below for why that, and not a written-and-fsynced file).  The parent
// SIGKILLs the child at a random
// instant -- SIGKILL specifically, because it cannot be caught, so nothing
// runs on the way out: no destructor, no flush, no tidy close.  What reaches
// the disk is exactly what the operating system already had.
//
// Then the parent reopens the database and checks two things:
//
//   Nothing acknowledged was lost.   Every key the journal says was written
//                                    with sync=true must be present, with the
//                                    right value.  A single missing one is a
//                                    durability failure, and the whole point.
//
//   Nothing was invented.            Every key present must be one the child
//                                    actually wrote, with a value it actually
//                                    wrote.  A key that appears from nowhere
//                                    means recovery mixed up a record.
//
//   Nothing observed was lost.       A reader thread inside the child reads
//                                    back what the writer is producing and
//                                    journals what it saw.  Every key a read
//                                    returned must still be there afterwards.
//
// That third check exists because of a real bug the first two could not see.
// The engine wrote its log record into a userspace buffer and then made the
// value readable, without pushing the record to the kernel -- so an unsynced
// write was visible before it could survive a process kill.  Nothing that had
// been *acknowledged* was lost, because the write had not returned yet, and
// both of the first two checks passed.  What was wrong was that a concurrent
// reader had been handed a value the crash then destroyed.  Adding the reader
// turned that run red; `mutations/durability.json` keeps it that way.
//
// A related mutation -- writing the memtable *before* the log record --
// survives this test, and correctly so: visibility is conferred by advancing
// the sequence number, which happens after both, so swapping them changes
// nothing observable.  mutations/README.md records why.
//
// A write that was in flight when the kill landed may be present or absent --
// both are correct, and the checker accepts either.  What it does not accept
// is a write that was acknowledged and then vanished, or a batch that arrived
// in pieces.
//
// The journal is a memory-mapped file, not a written-and-fsynced one, and that
// is not an optimisation.  A store into a shared mapping is in the kernel's
// page cache the instant it retires, so it survives the kill with no syscall
// at all -- while an fsync per entry takes about a millisecond.  The
// difference decides what the test can see: the window between a write
// becoming visible and becoming durable is a few microseconds, so any journal
// that costs a millisecond to append has already outlived the window it was
// trying to catch, and every observation it records is one that was safe by
// the time it was written down.
//
// What this test cannot check, and should not be read as checking: a power
// cut.  SIGKILL destroys the process, and everything it had handed to the
// kernel survives -- so a missing fsync is invisible here, because the page
// cache keeps the data either way.  Verifying fsync needs the storage itself
// to lose writes: a device-mapper flakey target, or a virtual machine
// snapshot taken mid-write.  docs/DESIGN.md says which claims rest on that.
//
// Usage:
//   crash_test <dir> child <seed>     -- the child, not run by hand
//   crash_test <dir> run <rounds>     -- kill, reopen and verify, `rounds` times

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "ambar/db.hpp"
#include "ambar/filter_policy.hpp"

using namespace ambar;

namespace {

constexpr int kBatchKeys = 4;

std::string key_of(int n) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%08d", n);
  return buf;
}

// The value is derived from the key, so the checker can tell a value that was
// written from one that was assembled out of parts of two records.
std::string value_of(int n) {
  std::string out = "v" + std::to_string(n) + ":";
  while (out.size() < 64) out += "0123456789abcdef";
  return out.substr(0, 64);
}

Options db_options(const FilterPolicy* policy) {
  Options options;
  options.create_if_missing = true;
  options.filter_policy = policy;
  // Small enough that a few hundred writes cross a flush and a compaction, so
  // the kill lands in interesting places rather than always in the memtable.
  //
  // write_buffer_size is honoured; max_file_size is silently raised to the 1
  // MiB floor that sanitize_options imposes, so asking for less than that
  // achieves nothing.  Set to the floor rather than below it, so the code says
  // what will actually happen.
  options.write_buffer_size = 64 << 10;
  options.max_file_size = 1 << 20;
  return options;
}

// ------------------------------------------------------------- journal -----

// An append-only log of what the child did, in a shared mapping.
//
// Layout: a count, then that many (kind, id) pairs.  The id is stored before
// the count is raised, so a reader after the crash never sees a slot that was
// not written -- the release fence is what orders the two stores as the CPU
// retires them, and page-cache visibility follows retirement.
class Journal {
 public:
  enum Kind : uint32_t {
    kIntent = 1,     // a batch is about to be written
    kConfirmed = 2,  // a batch was acknowledged with sync
    kObserved = 3,   // a read returned this key
  };

  struct Entry {
    uint32_t kind;
    uint32_t id;
  };

  static constexpr size_t kCapacity = 1u << 21;  // 2M entries, 16 MB

  static Journal* open(const std::string& path) {
#if defined(_WIN32)
    (void)path;
    return nullptr;
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return nullptr;

    const size_t bytes = sizeof(Header) + kCapacity * sizeof(Entry);
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
      ::close(fd);
      return nullptr;
    }
    void* mapped = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                          fd, 0);
    ::close(fd);  // the mapping keeps the file alive
    if (mapped == MAP_FAILED) return nullptr;

    return new Journal(static_cast<char*>(mapped));
  }

  void append(Kind kind, int id) {
    const uint32_t index = header_->count;
    if (index >= kCapacity) return;  // full; the round has run long enough
    entries_[index].kind = kind;
    entries_[index].id = static_cast<uint32_t>(id);
    // The entry must be in memory before the count that publishes it.
    std::atomic_thread_fence(std::memory_order_release);
    header_->count = index + 1;
  }
#endif

 private:
  struct Header {
    uint32_t count;
    uint32_t reserved;
  };

  explicit Journal(char* base)
      : header_(reinterpret_cast<Header*>(base)),
        entries_(reinterpret_cast<Entry*>(base + sizeof(Header))) {}

  Header* header_;
  Entry* entries_;
};

// --------------------------------------------------------------- child -----

// Writes until killed.  Before each batch is submitted, the intent is appended
// to the journal and fsynced, so the parent knows what *might* be there; after
// the write returns, a confirmation is appended and fsynced, so the parent
// knows what *must* be there.
//
// The journal is written with plain stdio and fsync rather than through the
// engine, because a journal that shared the engine's code could share its bugs
// and then agree with it about a loss.
int run_child(const std::string& dir, unsigned seed) {
  const auto policy = new_bloom_filter_policy(10);
  std::unique_ptr<DB> db;
  const Status status = DB::open(db_options(policy.get()), dir + "/db", &db);
  if (!status.is_ok()) {
    std::fprintf(stderr, "child: open failed: %s\n",
                 status.to_string().c_str());
    return 2;
  }

  Journal* journal = Journal::open(dir + "/journal");
  if (journal == nullptr) {
    std::fprintf(stderr, "child: cannot map the journal\n");
    return 2;
  }

  std::mt19937 rng(seed);
  int n = static_cast<int>(seed % 1000) * 100000;
  std::atomic<int> newest{n};

  // Sync is chosen per *phase*, not per batch, and the difference decides what
  // the test can find.
  //
  // Alternating sync and non-sync writes one at a time looks like better
  // coverage and is worse: every sync flushes the log's userspace buffer, so a
  // record never stays in this process for more than a batch or two, and the
  // window in which an unsynced write is visible but not yet handed to the
  // kernel is almost never open when the kill lands.  A run of a few hundred
  // unsynced writes keeps that window wide, which is also what an application
  // that leaves sync off actually does.
  int phase_remaining = 0;
  bool phase_sync = false;

  // The reader.  It chases the writer and journals every key it is actually
  // handed, because that is a promise the engine has to keep: a value a read
  // returned must survive a process crash, whether or not the write that
  // produced it asked for sync.
  //
  // Where it reads is the whole design, and there is more than one window to
  // aim at.  Both were found by mutating the engine and watching which reader
  // noticed:
  //
  //   The in-flight batch (`newest`).  Between a write becoming visible in the
  //   memtable and its log record existing at all.  A few microseconds wide,
  //   and the only place the ordering of those two steps can be observed.
  //
  //   The last few batches.  Between a log record being written and being
  //   pushed to the kernel.  Wider -- it lasts until the buffer fills or
  //   something flushes it -- and reading only the in-flight batch misses it
  //   entirely, because those keys are already through by the time they are
  //   more than a batch old.
  //
  //   Older keys still.  Ordinary lookups down through the tables, so the test
  //   is not exclusively about the two windows above.
  //
  // Sampling one of the three is what an earlier version did, in each case
  // letting a real fault pass every round.
  std::thread reader([&] {
    std::mt19937 reader_rng(seed ^ 0x5bf03635u);
    std::string value;
    while (true) {
      const int in_flight = newest.load(std::memory_order_acquire);
      const auto pick = static_cast<unsigned>(reader_rng() % 10);
      int id;
      if (pick < 4) {
        id = in_flight + static_cast<int>(reader_rng() % kBatchKeys);
      } else if (pick < 8) {
        id = in_flight - static_cast<int>(reader_rng() % 64);
      } else {
        id = in_flight - static_cast<int>(reader_rng() % 4096);
      }
      if (id < 0) continue;
      if (db->get(ReadOptions(), key_of(id), &value).is_ok()) {
        if (value != value_of(id)) {
          std::fprintf(stderr, "child: read a value that was never written\n");
          std::_Exit(3);
        }
        journal->append(Journal::kObserved, id);
      }
    }
  });
  reader.detach();

  while (true) {
    WriteBatch batch;
    for (int i = 0; i < kBatchKeys; ++i) {
      const int id = n + i;
      batch.put(key_of(id), value_of(id));
    }

    journal->append(Journal::kIntent, n);

    if (phase_remaining == 0) {
      phase_sync = (rng() % 2) == 0;
      phase_remaining = 20 + static_cast<int>(rng() % 400);
    }
    --phase_remaining;

    WriteOptions write_options;
    write_options.sync = phase_sync;
    const Status write_status = db->write(write_options, &batch);
    if (!write_status.is_ok()) {
      std::fprintf(stderr, "child: write failed: %s\n",
                   write_status.to_string().c_str());
      return 2;
    }

    if (write_options.sync) {
      journal->append(Journal::kConfirmed, n);
    }

    n += kBatchKeys;
    newest.store(n, std::memory_order_release);
  }
}

// -------------------------------------------------------------- parent -----

struct JournalContents {
  // Batches the child confirmed as synced: these must be present, all of them.
  std::vector<int> confirmed;
  // Batches the child began: these may be present or absent, but if any key of
  // one is present then every key of it must be.
  std::vector<int> attempted;
  // Individual keys a read inside the child returned.  Each one must still be
  // there: handing a value to a reader is a stronger promise than
  // acknowledging a write, because the reader has already acted on it.
  std::vector<int> observed;
};

JournalContents read_journal(const std::string& path) {
  JournalContents contents;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return contents;

  uint32_t header[2] = {0, 0};
  if (std::fread(header, sizeof(header), 1, file) != 1) {
    std::fclose(file);
    return contents;
  }
  const uint32_t count =
      header[0] > Journal::kCapacity ? Journal::kCapacity : header[0];

  std::vector<Journal::Entry> entries(count);
  if (count > 0 &&
      std::fread(entries.data(), sizeof(Journal::Entry), count, file) != count) {
    entries.clear();
  }
  std::fclose(file);

  for (const Journal::Entry& entry : entries) {
    switch (entry.kind) {
      case Journal::kIntent:
        contents.attempted.push_back(static_cast<int>(entry.id));
        break;
      case Journal::kConfirmed:
        contents.confirmed.push_back(static_cast<int>(entry.id));
        break;
      case Journal::kObserved:
        contents.observed.push_back(static_cast<int>(entry.id));
        break;
      default:
        // A partially written entry, or a slot the count never reached.  Both
        // mean the record is not one the child completed, so it is skipped
        // rather than treated as damage.
        break;
    }
  }
  return contents;
}

// Returns the number of failures found.
int verify(const std::string& dir, const JournalContents& journal) {
  const auto policy = new_bloom_filter_policy(10);
  Options options = db_options(policy.get());
  options.create_if_missing = false;

  std::unique_ptr<DB> db;
  const Status status = DB::open(options, dir + "/db", &db);
  if (!status.is_ok()) {
    std::printf("  FAIL  the database will not reopen: %s\n",
                status.to_string().c_str());
    return 1;
  }

  int failures = 0;
  std::string value;

  // 1. Everything acknowledged with sync must still be there.
  for (const int first : journal.confirmed) {
    for (int i = 0; i < kBatchKeys; ++i) {
      const int id = first + i;
      const Status get = db->get(ReadOptions(), key_of(id), &value);
      if (!get.is_ok()) {
        std::printf("  FAIL  %s was acknowledged with sync and is gone (%s)\n",
                    key_of(id).c_str(), get.to_string().c_str());
        ++failures;
      } else if (value != value_of(id)) {
        std::printf("  FAIL  %s came back with the wrong value\n",
                    key_of(id).c_str());
        ++failures;
      }
      if (failures > 10) return failures;
    }
  }

  // 2. Everything a read returned must still be there.
  for (const int id : journal.observed) {
    const Status get = db->get(ReadOptions(), key_of(id), &value);
    if (!get.is_ok()) {
      std::printf("  FAIL  %s was returned by a read and is gone (%s)\n",
                  key_of(id).c_str(), get.to_string().c_str());
      ++failures;
    } else if (value != value_of(id)) {
      std::printf("  FAIL  %s was read once and now holds a different value\n",
                  key_of(id).c_str());
      ++failures;
    }
    if (failures > 10) return failures;
  }

  // 3. A batch is all or nothing, whether or not it was acknowledged.
  for (const int first : journal.attempted) {
    int present = 0;
    for (int i = 0; i < kBatchKeys; ++i) {
      if (db->get(ReadOptions(), key_of(first + i), &value).is_ok()) {
        ++present;
      }
    }
    if (present != 0 && present != kBatchKeys) {
      std::printf("  FAIL  batch at %d is torn: %d of %d keys survived\n",
                  first, present, kBatchKeys);
      ++failures;
      if (failures > 10) return failures;
    }
  }

  // 4. Nothing may be present that was never written.
  std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
  int scanned = 0;
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    ++scanned;
    const std::string key(iter->key());
    int id = 0;
    if (std::sscanf(key.c_str(), "key_%d", &id) != 1) {
      std::printf("  FAIL  the database holds a key nobody wrote: %s\n",
                  key.c_str());
      ++failures;
      break;
    }
    if (iter->value() != std::string_view(value_of(id))) {
      std::printf("  FAIL  %s holds a value that was never written\n",
                  key.c_str());
      ++failures;
      break;
    }
  }
  if (!iter->status().is_ok()) {
    std::printf("  FAIL  the scan failed: %s\n",
                iter->status().to_string().c_str());
    ++failures;
  }

  std::printf("  ok    %d keys; %zu synced batches, %zu reads, %zu attempts\n",
              scanned, journal.confirmed.size(), journal.observed.size(),
              journal.attempted.size());
  return failures;
}

int run_rounds(const std::string& program, const std::string& dir, int rounds) {
#if defined(_WIN32)
  std::printf("The crash test needs fork and SIGKILL; not supported here.\n");
  return 0;
#else
  std::mt19937 rng(std::random_device{}());
  int failures = 0;

  for (int round = 1; round <= rounds; ++round) {
    // Each round starts from the state the previous kill left behind, so the
    // database accumulates real history rather than being torn down and rebuilt
    // -- recovery from a crash that followed an earlier crash is the case most
    // likely to be broken.
    const auto seed = static_cast<unsigned>(rng());

    const pid_t child = ::fork();
    if (child < 0) {
      std::perror("fork");
      return 1;
    }
    if (child == 0) {
      ::execl(program.c_str(), program.c_str(), dir.c_str(), "child",
              std::to_string(seed).c_str(), nullptr);
      ::_exit(127);
    }

    // Between 40 and 400 milliseconds: long enough that the child has written
    // something, short enough that the kill lands while it is still writing.
    // The spread matters more than the range -- a fixed delay would keep
    // hitting the same point in the write loop.
    const int millis = 40 + static_cast<int>(rng() % 360);
    ::usleep(static_cast<useconds_t>(millis) * 1000);

    ::kill(child, SIGKILL);
    int wait_status = 0;
    ::waitpid(child, &wait_status, 0);

    if (!WIFSIGNALED(wait_status) || WTERMSIG(wait_status) != SIGKILL) {
      // The child exited on its own, which means it failed to open or to
      // write.  That is a real failure and not a crash test.
      std::printf("round %2d: child exited on its own; see the message above\n",
                  round);
      ++failures;
      continue;
    }

    std::printf("round %2d (killed after %3d ms): ", round, millis);
    std::fflush(stdout);
    const int round_failures = verify(dir, read_journal(dir + "/journal"));
    failures += round_failures;
    if (round_failures > 0) {
      std::printf("  the database directory has been left at %s\n",
                  dir.c_str());
      return failures;
    }
  }

  std::printf("\n%d rounds, %d failures\n", rounds, failures);
  return failures;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: %s <dir> run <rounds>\n       %s <dir> child <seed>\n",
                argv[0], argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const std::string mode = argv[2];

  if (mode == "child") {
    return run_child(dir, argc > 3 ? static_cast<unsigned>(std::stoul(argv[3]))
                                   : 1u);
  }
  if (mode == "run") {
    const int rounds = argc > 3 ? std::atoi(argv[3]) : 20;
    return run_rounds(argv[0], dir, rounds) == 0 ? 0 : 1;
  }
  std::printf("unknown mode '%s'\n", mode.c_str());
  return 2;
}
