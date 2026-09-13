// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The durability contract, checked against a disk that loses what was not
// synced.  See tests/sim_file_system.hpp for the disk and
// tests/powercut_driver.hpp for the workload and the checks.
//
// These are the tests that go red when an fsync goes missing --
// tools/crash_test cannot, because a killed process leaves the page cache
// intact.  mutations/powercut.json removes the engine's syncs one at a
// time, and tools/mutate.py confirms that each removal is caught here.
//
// The first tests are of the disk itself, with no engine involved: a
// simulator that ignored sync would make every test below pass for the
// wrong reason, so what it keeps and what it loses is pinned first.

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

// The disk alone: a directory, one file, written through the seam.
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

// Cycles of writing and cutting, each cut at a random operation, so the
// cuts land in writes, flushes, compactions, log rotations -- and, every
// few cycles, in the open that recovers what an earlier cut left, or in
// the first operations of a life, before anything has synced the directory
// since the compaction that ran at the end of the last one.
void random_cuts(Model m, uint64_t seed, int cycles, bool syncing = true) {
  Driver driver(m, seed, syncing);
  std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
  for (int i = 0; i < cycles; ++i) {
    const uint64_t crash_after = (i % 4 == 3) ? rng() % 6 : 1 + rng() % 700;
    const uint64_t recovery_cut =
        (i % 3 == 2) ? rng() % 20 : SimFileSystem::kNever;
    const Failure failure = driver.cycle(crash_after, 300, recovery_cut);
    if (!failure.ok()) {
      std::printf("  cycle %d, cut %llu operations in: %s\n", i + 1,
                  static_cast<unsigned long long>(crash_after),
                  failure.what.c_str());
      CHECK(failure.ok());
      return;
    }
  }
  const Driver::Stats& stats = driver.stats();
  const SimFileSystem& fs = driver.fs();
  std::printf("    %d cycles, %d batches, %d cut inside recovery, %d repairs, "
              "%d unsynced batches lost; %llu logs, %llu tables, %llu "
              "tables deleted, %llu CURRENT renames\n",
              stats.cycles, stats.batches_written,
              stats.cuts_inside_recovery, stats.repairs, stats.batches_lost,
              static_cast<unsigned long long>(fs.count("create .log")),
              static_cast<unsigned long long>(fs.count("create .sst")),
              static_cast<unsigned long long>(fs.count("remove .sst")),
              static_cast<unsigned long long>(fs.count("rename CURRENT")));
  CHECK(stats.batches_written > 0);
  // The workload has to reach the places a cut is interesting: a log
  // rotation, a flush, a compaction that deletes its inputs.  Sizes that
  // stop it reaching them would hollow the test out without turning it
  // red.
  CHECK(fs.count("create .log") >= 3);
  CHECK(fs.count("create .sst") >= 3);
  CHECK(fs.count("remove .sst") >= 1);
  CHECK(fs.count("rename CURRENT") >= 1);
  CHECK(stats.cuts_inside_recovery >= 1);
}

}  // namespace

// ------------------------------------------------------------- the disk ---
//
// Fixed seeds, and the outcomes they give are pinned to the order in which
// cut() draws from its generator: a draw added there re-rolls every one of
// these, and the "sometimes lost" assertions may need new seeds.

TEST(powercut_disk, unsynced_bytes_may_be_lost_and_synced_ones_never) {
  int lost = 0;
  for (uint64_t seed = 1; seed <= 12; ++seed) {
    Disk disk(model(SimFileSystem::Dirents::kFile,
                    SimFileSystem::Tails::kOrdered),
              seed);
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open("d/f", false, &file));
    CHECK_OK(file->append("synced-part;"));
    CHECK_OK(file->sync());
    CHECK_OK(file->append("unsynced-part"));
    CHECK_OK(file->flush());
    disk.fs.crash();
    file.reset();
    disk.fs.power_on();
    const std::string after = disk.contents("d/f");
    CHECK(after.compare(0, 12, "synced-part;") == 0);
    CHECK(after.size() <= std::string("synced-part;unsynced-part").size());
    if (after.size() < std::string("synced-part;unsynced-part").size()) ++lost;
  }
  CHECK(lost > 0);
}

TEST(powercut_disk, bytes_still_in_the_process_never_land) {
  for (uint64_t seed = 1; seed <= 6; ++seed) {
    Disk disk(model(SimFileSystem::Dirents::kFile,
                    SimFileSystem::Tails::kOrdered),
              seed);
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open("d/f", false, &file));
    CHECK_OK(file->append("never flushed"));  // smaller than a buffer
    disk.fs.crash();
    file.reset();
    disk.fs.power_on();
    // The name may or may not have landed; the bytes never.
    const std::string after = disk.contents("d/f");
    CHECK(after.empty() || after == "<missing>");
  }
}

TEST(powercut_disk, holes_zero_pages_of_the_tail_but_never_the_synced_prefix) {
  // The synced prefix ends at an odd offset inside a page, so that a hole
  // that ignored the boundary would eat the end of it.
  const std::string prefix(5000, 'p');
  const std::string tail(3 * 4096, 't');
  int holes = 0;
  for (uint64_t seed = 1; seed <= 16; ++seed) {
    Disk disk(model(SimFileSystem::Dirents::kFile,
                    SimFileSystem::Tails::kHoles),
              seed);
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open("d/f", false, &file));
    CHECK_OK(file->append(prefix));
    CHECK_OK(file->sync());
    CHECK_OK(file->append(tail));
    CHECK_OK(file->flush());
    disk.fs.crash();
    file.reset();
    disk.fs.power_on();
    const std::string after = disk.contents("d/f");
    CHECK(after.size() >= prefix.size());
    CHECK(after.compare(0, prefix.size(), prefix) == 0);
    for (size_t i = prefix.size(); i < after.size(); ++i) {
      CHECK(after[i] == 't' || after[i] == '\0');
      if (after[i] == '\0') ++holes;
    }
  }
  CHECK(holes > 0);
}

TEST(powercut_disk, a_new_file_needs_a_directory_sync_only_under_posix) {
  for (int posix = 0; posix < 2; ++posix) {
    int vanished = 0;
    for (uint64_t seed = 1; seed <= 12; ++seed) {
      Disk disk(model(posix ? SimFileSystem::Dirents::kPosix
                            : SimFileSystem::Dirents::kFile,
                      SimFileSystem::Tails::kOrdered),
                seed);
      std::unique_ptr<WritableFile> file;
      CHECK_OK(WritableFile::open("d/f", false, &file));
      CHECK_OK(file->append("data"));
      CHECK_OK(file->sync());
      disk.fs.crash();
      file.reset();
      disk.fs.power_on();
      if (disk.contents("d/f") == "<missing>") ++vanished;
    }
    if (posix) {
      CHECK(vanished > 0);
    } else {
      CHECK_EQ(vanished, 0);
    }
  }
  // And with the directory synced, never, whatever the model.
  for (uint64_t seed = 1; seed <= 6; ++seed) {
    Disk disk(model(SimFileSystem::Dirents::kPosix,
                    SimFileSystem::Tails::kOrdered),
              seed);
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open("d/f", false, &file));
    CHECK_OK(file->append("data"));
    CHECK_OK(file->sync());
    CHECK_OK(sync_directory("d"));
    disk.fs.crash();
    file.reset();
    disk.fs.power_on();
    CHECK_EQ(disk.contents("d/f"), "data");
  }
}

// A name removed and created again: the second file's sync commits its
// name, and the removal that made room for it has to have landed first, so
// the name can never come back holding the first file.
TEST(powercut_disk, a_name_reused_never_shows_the_file_that_was_removed) {
  for (uint64_t seed = 1; seed <= 12; ++seed) {
    Disk disk(model(SimFileSystem::Dirents::kFile,
                    SimFileSystem::Tails::kOrdered),
              seed);
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open("d/f", false, &file));
    CHECK_OK(file->append("first"));
    CHECK_OK(file->sync());
    CHECK_OK(file->close());
    CHECK_OK(remove_file("d/f"));
    CHECK_OK(WritableFile::open("d/f", false, &file));
    CHECK_OK(file->append("second"));
    CHECK_OK(file->sync());
    disk.fs.crash();
    file.reset();
    disk.fs.power_on();
    CHECK_EQ(disk.contents("d/f"), "second");
  }
}

TEST(powercut_disk, a_rename_lands_whole_or_not_at_all) {
  int landed = 0;
  for (uint64_t seed = 1; seed <= 12; ++seed) {
    Disk disk(model(SimFileSystem::Dirents::kPosix,
                    SimFileSystem::Tails::kOrdered),
              seed);
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open("d/old", false, &file));
    CHECK_OK(file->append("old"));
    CHECK_OK(file->sync());
    CHECK_OK(file->close());
    CHECK_OK(sync_directory("d"));
    CHECK_OK(WritableFile::open("d/tmp", false, &file));
    CHECK_OK(file->append("new"));
    CHECK_OK(file->sync());
    CHECK_OK(file->close());
    CHECK_OK(rename_file("d/tmp", "d/old"));
    disk.fs.crash();
    disk.fs.power_on();
    const std::string after = disk.contents("d/old");
    CHECK(after == "old" || after == "new");
    if (after == "new") {
      ++landed;
      CHECK_EQ(disk.contents("d/tmp"), "<missing>");
    }
  }
  CHECK(landed > 0);
  CHECK(landed < 12);
}

TEST(powercut_disk, truncating_a_file_lands_with_its_next_sync) {
  int old_kept = 0;
  for (uint64_t seed = 1; seed <= 12; ++seed) {
    Disk disk(model(SimFileSystem::Dirents::kPosix,
                    SimFileSystem::Tails::kOrdered),
              seed);
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open("d/f", false, &file));
    CHECK_OK(file->append("first contents"));
    CHECK_OK(file->sync());
    CHECK_OK(file->close());
    CHECK_OK(sync_directory("d"));
    // Truncated and rewritten, but not yet synced: either may show.
    CHECK_OK(WritableFile::open("d/f", false, &file));
    CHECK_OK(file->append("second"));
    CHECK_OK(file->flush());
    disk.fs.crash();
    file.reset();
    disk.fs.power_on();
    const std::string after = disk.contents("d/f");
    CHECK(after == "first contents" ||
          std::string("second").compare(0, after.size(), after) == 0);
    if (after == "first contents") ++old_kept;
  }
  CHECK(old_kept > 0);
  // Synced after the truncation: never the old contents, whatever the
  // directory did.
  for (uint64_t seed = 1; seed <= 6; ++seed) {
    Disk disk(model(SimFileSystem::Dirents::kPosix,
                    SimFileSystem::Tails::kOrdered),
              seed);
    std::unique_ptr<WritableFile> file;
    CHECK_OK(WritableFile::open("d/f", false, &file));
    CHECK_OK(file->append("first contents"));
    CHECK_OK(file->sync());
    CHECK_OK(file->close());
    CHECK_OK(sync_directory("d"));
    CHECK_OK(WritableFile::open("d/f", false, &file));
    CHECK_OK(file->append("second"));
    CHECK_OK(file->sync());
    disk.fs.crash();
    file.reset();
    disk.fs.power_on();
    CHECK_EQ(disk.contents("d/f"), "second");
  }
}

// ----------------------------------------------------------- the engine ---

// The disk has to be shown to lose something the engine wrote, or a green
// run below says nothing: with no write ever synced, some acknowledged
// batch must go.
TEST(powercut, the_disk_loses_what_the_engine_did_not_sync) {
  Driver driver(model(SimFileSystem::Dirents::kFile,
                      SimFileSystem::Tails::kOrdered),
                7, /*sync_writes=*/false);
  std::mt19937 rng(7);
  for (int i = 0; i < 20; ++i) {
    const Failure failure = driver.cycle(1 + rng() % 400, 300);
    if (!failure.ok()) {
      std::printf("  %s\n", failure.what.c_str());
      CHECK(failure.ok());
      return;
    }
  }
  CHECK(driver.stats().batches_lost > 0);
}

TEST(powercut, every_synced_batch_survives_when_a_sync_commits_the_files_name) {
  random_cuts(model(SimFileSystem::Dirents::kFile,
                    SimFileSystem::Tails::kOrdered),
              11, 24);
}

TEST(powercut, every_synced_batch_survives_when_only_a_directory_sync_names) {
  random_cuts(model(SimFileSystem::Dirents::kPosix,
                    SimFileSystem::Tails::kOrdered),
              13, 24);
}

TEST(powercut, every_synced_batch_survives_when_the_tail_has_holes) {
  random_cuts(model(SimFileSystem::Dirents::kPosix,
                    SimFileSystem::Tails::kHoles),
              17, 24);
}

TEST(powercut, every_synced_batch_survives_when_the_tail_holds_garbage) {
  random_cuts(model(SimFileSystem::Dirents::kPosix,
                    SimFileSystem::Tails::kGarbage),
              19, 20);
}

// A cut at every operation of a short life, from an empty directory: the
// first open, the first writes, the first flush.  Then one more cut at a
// random point, because recovery from a cut that followed a cut is the case
// most likely to be wrong.
//
// The life is 120 batches, some 450 operations; the sweep runs past that,
// and a point past the end is a cut at the end.  Which operation a given
// index names varies a little from run to run, since the background flush
// and the shutdown race for the last few, so a failure is reported with
// the operation the cut landed on rather than trusted to reproduce from its
// index alone.
TEST(powercut, a_cut_at_every_point_of_a_short_life) {
  const Model m = model(SimFileSystem::Dirents::kPosix,
                        SimFileSystem::Tails::kOrdered);
  std::mt19937 rng(23);
  int points = 0;
  for (uint64_t at = 0; at < 520; at += (at < 120 ? 1 : 9)) {
    ++points;
    Driver driver(m, 100 + at);
    Failure failure = driver.cycle(at, 120);
    if (failure.ok()) failure = driver.cycle(1 + rng() % 300, 120);
    if (!failure.ok()) {
      std::printf("  cut at operation %llu: %s\n",
                  static_cast<unsigned long long>(at), failure.what.c_str());
      CHECK(failure.ok());
      return;
    }
  }
  std::printf("    %d cut points\n", points);
}

// A cut inside the open that follows a cut: recovery writes a table from
// the log, a manifest edit, a new log, and deletes the old one, and a cut
// between any two of those must lose nothing that was synced.  Each life
// writes twenty synced batches and is cut at its end, so the log the next
// open replays is never empty, and that open is cut `at` operations in.
TEST(powercut, a_cut_at_every_point_of_recovery) {
  const Model m = model(SimFileSystem::Dirents::kPosix,
                        SimFileSystem::Tails::kOrdered);
  Driver driver(m, 29);
  driver.set_sync_phases(false);  // every write synced: all must survive
  const uint64_t before = driver.fs().count("create .sst");
  for (uint64_t at = 0; at < 40; ++at) {
    const Failure failure = driver.cycle(SimFileSystem::kNever, 20, at);
    if (!failure.ok()) {
      std::printf("  cut %llu operations into the recovery: %s\n",
                  static_cast<unsigned long long>(at), failure.what.c_str());
      CHECK(failure.ok());
      return;
    }
  }
  std::printf("    %d of 40 cuts landed inside the recovery\n",
              driver.stats().cuts_inside_recovery);
  CHECK(driver.stats().cuts_inside_recovery >= 20);
  CHECK(driver.fs().count("create .sst") - before >= 40);  // one per replay
  CHECK_EQ(driver.stats().batches_lost, 0);
}

// A cut inside a repair: CURRENT is lost, so the open refuses and repair
// runs -- reading every table and log, writing merged tables, a manifest,
// CURRENT, and only then moving what it replaced into lost/.  A cut
// anywhere in that must leave a directory that either refuses for the same
// reason, and is repaired again, or opens; never one that opens with less.
TEST(powercut, a_cut_at_every_point_of_a_repair) {
  const Model m = model(SimFileSystem::Dirents::kPosix,
                        SimFileSystem::Tails::kHoles);
  Driver driver(m, 31);
  driver.set_sync_phases(false);
  CHECK(driver.cycle(SimFileSystem::kNever, 400).ok());
  for (uint64_t at = 0; at < 90; at += 2) {
    CHECK_OK(driver.lose_current());
    const Failure failure = driver.cycle(at, 10);
    if (!failure.ok()) {
      std::printf("  cut %llu operations into the repair: %s\n",
                  static_cast<unsigned long long>(at), failure.what.c_str());
      CHECK(failure.ok());
      return;
    }
  }
  CHECK(driver.stats().repairs >= 45);
  CHECK(driver.stats().cuts_inside_open > 0);
  CHECK_EQ(driver.stats().batches_lost, 0);
}
