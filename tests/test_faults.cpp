// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// One I/O call fails, at every point of a workload, and the database has
// to survive it: report the error, keep everything it acknowledged, open
// again.  tools/fault_sweep.sh does this on Linux by intercepting fsync and
// rename with LD_PRELOAD, for the manifest's syncs only; this does it on
// the simulated disk, on every platform, for every call the engine makes
// -- append, flush, sync, close, create, rename, remove, directory sync --
// and then cuts the power as well, since a disk that refused a call is not
// a disk to trust with the next one.
//
// The disk's model of a failed sync is ext4's: the error is reported once
// per open file, the pages it could not write are marked clean, and no
// later sync writes them.  A database that retries the sync and believes
// the second answer has a hole it does not know about.

#include <map>
#include <random>

#include "harness.hpp"
#include "powercut_driver.hpp"

using namespace ambar;
using namespace ambar::powercut;

namespace {

using Model = SimFileSystem::Model;

Model model(SimFileSystem::Dirents dirents, SimFileSystem::Tails tails) {
  Model m;
  m.dirents = dirents;
  m.tails = tails;
  return m;
}

struct Disk {
  explicit Disk(Model m, uint64_t seed = 1) : fs(m, seed) {
    previous = set_file_system(&fs);
    fs.create_directory("d");
    fs.sync_directory("d");
  }
  ~Disk() { set_file_system(previous); }

  std::string contents(const std::string& path) {
    std::unique_ptr<SequentialFile> file;
    if (!SequentialFile::open(path, &file).is_ok()) return "<missing>";
    std::string_view chunk;
    std::string scratch;
    std::string all;
    while (file->read(1 << 16, &chunk, &scratch).is_ok() && !chunk.empty()) {
      all.append(chunk);
    }
    return all;
  }

  SimFileSystem fs;
  FileSystem* previous;
};

}  // namespace

// ------------------------------------------------------------- the disk ---

TEST(faults_disk, a_failed_call_does_nothing_and_the_next_one_works) {
  Disk disk(model(SimFileSystem::Dirents::kFile,
                  SimFileSystem::Tails::kOrdered));
  std::unique_ptr<WritableFile> file;
  disk.fs.fail_at(disk.fs.operations());  // the create
  Status status = WritableFile::open("d/f", false, &file);
  CHECK(!status.is_ok());
  CHECK_EQ(disk.contents("d/f"), "<missing>");
  CHECK_EQ(disk.fs.faults(), 1u);
  CHECK_OK(WritableFile::open("d/f", false, &file));
  CHECK_OK(file->append("data"));
  disk.fs.fail_at(disk.fs.operations());  // the flush
  CHECK(!file->flush().is_ok());
  // A failed flush takes the process's buffer with it, as stdio's does;
  // a flush after it has nothing to write.
  CHECK_OK(file->flush());
  CHECK_EQ(disk.contents("d/f"), std::string());
  CHECK_OK(file->append("more"));
  CHECK_OK(file->flush());
  CHECK_EQ(disk.contents("d/f"), "more");
}

TEST(faults_disk, a_failed_sync_leaves_bytes_that_no_later_sync_writes) {
  const std::string a(3000, 'a');
  const std::string b(3000, 'b');
  const std::string c(3000, 'c');
  for (uint64_t seed = 1; seed <= 6; ++seed) {
    Disk disk(model(SimFileSystem::Dirents::kFile,
                    SimFileSystem::Tails::kOrdered),
              seed);
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open("d/f", false, &file));
    CHECK_OK(file->append(a));
    CHECK_OK(file->sync());
    CHECK_OK(file->append(b));
    disk.fs.fail_at(disk.fs.operations());
    CHECK(!file->sync().is_ok());
    CHECK_OK(file->append(c));
    CHECK_OK(file->sync());  // reports success, as Linux would
    // Readable in full while the power is on.
    CHECK_EQ(disk.contents("d/f"), a + b + c);
    disk.fs.crash();
    file.reset();
    disk.fs.power_on();
    // After a cut: a, then a hole where b was, then c -- the later sync
    // covered c and could not cover b.
    const std::string after = disk.contents("d/f");
    CHECK_EQ(after.size(), (a + b + c).size());
    CHECK(after.compare(0, a.size(), a) == 0);
    CHECK_EQ(after.substr(a.size(), b.size()), std::string(b.size(), '\0'));
    CHECK_EQ(after.substr(a.size() + b.size()), c);
  }
}

// ----------------------------------------------------------- the engine ---

// A call fails at every operation of a short life, from an empty
// directory; the database is closed properly, opened again, and must hold
// everything it acknowledged, whatever it said about the batch the fault
// landed in.  Then a second life writes more, to show it still can.
TEST(faults, a_fault_at_every_point_of_a_short_life) {
  const Model m = model(SimFileSystem::Dirents::kFile,
                        SimFileSystem::Tails::kOrdered);
  int points = 0, faults = 0, refused = 0;
  for (uint64_t at = 0; at < 520; at += (at < 100 ? 1 : 9)) {
    ++points;
    Driver driver(m, 500 + at);
    Plan life;
    life.batches = 120;
    life.fault = at;
    life.close_cleanly = true;
    Failure failure = driver.cycle(life);
    if (failure.ok()) {
      Plan again;
      again.batches = 60;
      again.close_cleanly = true;
      failure = driver.cycle(again);
    }
    if (!failure.ok()) {
      std::printf("  fault at operation %llu: %s\n",
                  static_cast<unsigned long long>(at), failure.what.c_str());
      CHECK(failure.ok());
      return;
    }
    faults += driver.stats().faults;
    refused += driver.stats().writes_failed;
    CHECK(driver.stats().faults <= 1);
  }
  std::printf("    %d points, %d faults landed, %d writes refused\n", points,
              faults, refused);
  CHECK(faults >= 80);
  CHECK(refused >= 20);
}

// Every call of a kind, failed in turn: the third sync of the manifest,
// the fourth rename of CURRENT, the ninth sync of a table.  This is what
// tools/fault_sweep.sh does for the manifest's syncs alone, and the sweep
// above cannot do by operation index, since which index the compaction
// thread's calls fall on changes from run to run.  Each life ends by
// compacting everything, so that a compaction's calls are in every life
// and not only in the ones the background thread reached; and each is
// tried twice: closed cleanly, which is where a manifest that was read
// back from the page cache and never reached the disk shows; and with the
// power cut some operations after the fault, which is where the pages a
// failed sync could not write show.
TEST(faults, every_call_of_each_kind_failed_in_turn) {
  const Model m = model(SimFileSystem::Dirents::kFile,
                        SimFileSystem::Tails::kOrdered);
  // A kind, and whether the writes ask for sync.  With no write syncing,
  // the only syncs of a log are the ones a rotation does on the log it
  // retires, so those can be failed in turn; with syncing writes they are
  // a few among hundreds.
  struct Kind {
    const char* name;
    bool syncing;
  };
  const Kind kinds[] = {
      {"sync MANIFEST", true},  {"append MANIFEST", true},
      {"create MANIFEST", true}, {"remove MANIFEST", true},
      {"rename CURRENT", true}, {"sync .dbtmp", true},
      {"sync directory", true}, {"create .sst", true},
      {"append .sst", true},    {"sync .sst", true},
      {"close .sst", true},     {"remove .sst", true},
      {"create .log", true},    {"sync .log", true},
      {"sync .log", false},     {"close .log", true},
      {"remove .log", true},
  };
  constexpr int kBatches = 600;  // enough for several flushes

  // How many of each there are in a life nothing goes wrong in -- the life
  // itself, from its open to its close, not the open that checks it.
  std::map<std::string, uint64_t> in_a_life[2];
  for (int syncing = 0; syncing < 2; ++syncing) {
    Driver probe(m, 61 + static_cast<uint64_t>(syncing), syncing == 1);
    Plan life;
    life.batches = kBatches;
    life.close_cleanly = true;
    life.compact_at_end = true;
    CHECK(probe.cycle(life).ok());
    for (const Kind& kind : kinds) {
      in_a_life[syncing][kind.name] = probe.life_count(kind.name);
    }
  }

  int runs = 0, faults = 0, skipped = 0;
  for (const Kind& kind : kinds) {
    const uint64_t total = in_a_life[kind.syncing ? 1 : 0][kind.name];
    if (total == 0) {
      std::printf("  %s: did not happen in the probe's life\n", kind.name);
      ++skipped;
      continue;
    }
    // Every one of a kind there are a dozen or fewer of -- the manifest's
    // syncs, the rotations, the removals -- and five spread over the life
    // of the kinds there are hundreds of; each twice.
    const uint64_t step = total <= 12 ? 1 : (total + 4) / 5;
    for (uint64_t k = 0; k < total; k += step) {
      for (int cut = 0; cut < 2; ++cut) {
        ++runs;
        Driver driver(m, 600 + static_cast<uint64_t>(runs), kind.syncing);
        Plan life;
        life.batches = kBatches;
        life.fault_kind = kind.name;
        life.fault = k;
        life.compact_at_end = true;
        life.close_cleanly = cut == 0;
        if (cut == 1) life.cut_after_fault = 10 + (k * 37) % 250;
        Failure failure = driver.cycle(life);
        if (failure.ok()) {
          Plan again;
          again.batches = 60;
          again.close_cleanly = true;
          failure = driver.cycle(again);
        }
        if (!failure.ok()) {
          std::printf("  %s%s, the %lluth of %llu, %s: %s\n", kind.name,
                      kind.syncing ? "" : " (no write syncing)",
                      static_cast<unsigned long long>(k),
                      static_cast<unsigned long long>(total),
                      cut == 0 ? "closed cleanly" : "cut after the fault",
                      failure.what.c_str());
          CHECK(failure.ok());
          return;
        }
        faults += driver.stats().faults;
      }
    }
  }
  // Not quite every ordinal lands: a compaction the probe's life finished
  // may be cut short by the close in another, and where the compaction
  // thread's calls fall varies from run to run.
  std::printf("    %d runs over %d kinds, %d faults landed\n", runs,
              static_cast<int>(sizeof(kinds) / sizeof(kinds[0])) - skipped,
              faults);
  CHECK_EQ(skipped, 0);
  CHECK(faults >= runs * 3 / 4);
}

// A call fails, and some time later the power goes: the two things a disk
// does, together.  The cut is what makes a failed sync matter.
TEST(faults, a_fault_and_then_a_cut) {
  for (int variant = 0; variant < 2; ++variant) {
    const Model m = variant == 0 ? model(SimFileSystem::Dirents::kPosix,
                                          SimFileSystem::Tails::kOrdered)
                                 : model(SimFileSystem::Dirents::kFile,
                                          SimFileSystem::Tails::kHoles);
    Driver driver(m, 41 + static_cast<uint64_t>(variant));
    std::mt19937 rng(43 + static_cast<std::mt19937::result_type>(variant));
    for (int i = 0; i < 30; ++i) {
      Plan life;
      life.batches = 300;
      life.fault = rng() % 600;
      life.cut = life.fault + 1 + rng() % 300;
      if (i % 5 == 4) life.recovery_fault = rng() % 20;
      const Failure failure = driver.cycle(life);
      if (!failure.ok()) {
        std::printf("  variant %d, life %d, fault at %llu, cut at %llu: %s\n",
                    variant, i + 1,
                    static_cast<unsigned long long>(life.fault),
                    static_cast<unsigned long long>(life.cut),
                    failure.what.c_str());
        CHECK(failure.ok());
        return;
      }
    }
    const Driver::Stats& stats = driver.stats();
    std::printf("    variant %d: %d lives, %d faults, %d writes refused, %d "
                "opens retried, %d repairs, %d unsynced batches lost\n",
                variant, stats.cycles, stats.faults, stats.writes_failed,
                stats.opens_retried, stats.repairs, stats.batches_lost);
    CHECK(stats.faults >= 20);
  }
}

// A call fails inside the open that recovers a log: the open reports it,
// the next open succeeds, and every synced batch is there.
TEST(faults, a_fault_at_every_point_of_recovery) {
  const Model m = model(SimFileSystem::Dirents::kPosix,
                        SimFileSystem::Tails::kOrdered);
  Driver driver(m, 47);
  driver.set_sync_phases(false);
  for (uint64_t at = 0; at < 40; ++at) {
    Plan life;
    life.batches = 20;
    life.recovery_fault = at;
    const Failure failure = driver.cycle(life);
    if (!failure.ok()) {
      std::printf("  fault %llu operations into the recovery: %s\n",
                  static_cast<unsigned long long>(at), failure.what.c_str());
      CHECK(failure.ok());
      return;
    }
  }
  std::printf("    %d faults landed, %d opens retried\n",
              driver.stats().faults, driver.stats().opens_retried);
  CHECK(driver.stats().faults >= 20);
  CHECK_EQ(driver.stats().batches_lost, 0);
}

// A call fails inside a repair: the repair reports it, leaves the
// directory as it found it or repaired, and the next attempt succeeds.
// The database is small so that the whole repair -- reading, merging, the
// manifest, CURRENT, the moves into lost/, the open that checks -- fits in
// the swept range, and the calls after CURRENT are swept by kind as well,
// since a fault there is the one that leaves the directory repaired.
TEST(faults, a_fault_at_every_point_of_a_repair) {
  const Model m = model(SimFileSystem::Dirents::kPosix,
                        SimFileSystem::Tails::kHoles);
  Driver driver(m, 53);
  driver.set_sync_phases(false);
  CHECK(driver.cycle(SimFileSystem::kNever, 40).ok());
  int repaired_once = 0;  // the retry opened without repairing again
  auto sweep = [&](const Plan& life) {
    const int repairs = driver.stats().repairs;
    const Failure failure = driver.cycle(life);
    if (failure.ok() && driver.stats().repairs == repairs + 1) ++repaired_once;
    return failure;
  };
  for (uint64_t at = 0; at < 70; ++at) {
    CHECK_OK(driver.lose_current());
    Plan life;
    life.batches = 5;
    life.fault = at;
    const Failure failure = sweep(life);
    if (!failure.ok()) {
      std::printf("  fault %llu operations into the repair: %s\n",
                  static_cast<unsigned long long>(at), failure.what.c_str());
      CHECK(failure.ok());
      return;
    }
  }
  const char* kinds[] = {"mkdir",          "rename .log",    "rename MANIFEST",
                         "rename .sst",    "sync .sst",      "close .sst",
                         "create MANIFEST", "sync MANIFEST", "sync .dbtmp",
                         "rename CURRENT", "sync directory", "remove .log"};
  for (const char* kind : kinds) {
    CHECK_OK(driver.lose_current());
    Plan life;
    life.batches = 5;
    life.fault_kind = kind;
    life.fault = 0;
    const Failure failure = sweep(life);
    if (!failure.ok()) {
      std::printf("  the first %s of the repair failed: %s\n", kind,
                  failure.what.c_str());
      CHECK(failure.ok());
      return;
    }
  }
  std::printf("    %d faults landed, %d opens retried, %d repairs, %d of "
              "them not repeated\n",
              driver.stats().faults, driver.stats().opens_retried,
              driver.stats().repairs, repaired_once);
  CHECK(driver.stats().faults >= 60);
  CHECK(repaired_once >= 3);
  CHECK_EQ(driver.stats().batches_lost, 0);
}
