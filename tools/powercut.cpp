// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Cuts the power on a simulated disk, over and over, and checks what
// survived.
//
// tools/crash_test kills the process; this kills the disk.  The engine runs
// on tests/sim_file_system.hpp, which keeps what fsync covered and loses a
// random amount of the rest, and after every cut the database is reopened
// and compared with a model of what was acknowledged: every synced batch
// present, the rest a prefix, nothing torn, nothing invented.  The unit
// suite runs a few hundred cuts of this on every platform; this runs as
// many as asked, under whichever model, and keeps the disk a failure left.
//
//   ambar_powercut [--dirents file|posix] [--tails ordered|holes|garbage]
//                  [--seed N] [--seeds N] [--cycles N] [--batches N]
//                  [--dump DIR]
//
// Each seed is one database, taken through `cycles` lives of up to
// `batches` writes, each ended by a cut at a random operation.  A failure
// stops the run, prints what was wrong and the disk as the reboot found it,
// and with --dump writes that disk into DIR for ambar_repair and a hex
// editor.  Exit 0 when every cut was survived, 1 otherwise, 2 on usage.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "powercut_driver.hpp"

using namespace ambar;
using namespace ambar::powercut;

namespace {

int usage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [--dirents file|posix] [--tails ordered|holes|"
               "garbage]\n           [--seed N] [--seeds N] [--cycles N] "
               "[--batches N] [--dump DIR]\n",
               program);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  SimFileSystem::Model model;
  model.dirents = SimFileSystem::Dirents::kPosix;
  model.tails = SimFileSystem::Tails::kHoles;
  uint64_t first_seed = 1;
  int seeds = 4;
  int cycles = 100;
  int batches = 400;
  std::string dump;

  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (value == nullptr) return usage(argv[0]);
    ++i;
    if (flag == "--dirents") {
      if (std::strcmp(value, "file") == 0) {
        model.dirents = SimFileSystem::Dirents::kFile;
      } else if (std::strcmp(value, "posix") == 0) {
        model.dirents = SimFileSystem::Dirents::kPosix;
      } else {
        return usage(argv[0]);
      }
    } else if (flag == "--tails") {
      if (std::strcmp(value, "ordered") == 0) {
        model.tails = SimFileSystem::Tails::kOrdered;
      } else if (std::strcmp(value, "holes") == 0) {
        model.tails = SimFileSystem::Tails::kHoles;
      } else if (std::strcmp(value, "garbage") == 0) {
        model.tails = SimFileSystem::Tails::kGarbage;
      } else {
        return usage(argv[0]);
      }
    } else if (flag == "--seed") {
      first_seed = std::strtoull(value, nullptr, 10);
    } else if (flag == "--seeds") {
      seeds = std::atoi(value);
    } else if (flag == "--cycles") {
      cycles = std::atoi(value);
    } else if (flag == "--batches") {
      batches = std::atoi(value);
    } else if (flag == "--dump") {
      dump = value;
    } else {
      return usage(argv[0]);
    }
  }
  if (seeds <= 0 || cycles <= 0 || batches <= 0) return usage(argv[0]);

  std::printf("dirents %s, tails %s; %d seed(s) from %llu, %d cycles of up to "
              "%d batches each\n",
              model.dirents == SimFileSystem::Dirents::kFile ? "file" : "posix",
              model.tails == SimFileSystem::Tails::kOrdered   ? "ordered"
              : model.tails == SimFileSystem::Tails::kHoles ? "holes"
                                                              : "garbage",
              seeds, static_cast<unsigned long long>(first_seed), cycles,
              batches);

  int total_cycles = 0, total_repairs = 0, total_lost = 0, total_in_open = 0;
  for (int s = 0; s < seeds; ++s) {
    const uint64_t seed = first_seed + static_cast<uint64_t>(s);
    Driver driver(model, seed);
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed * 7919));
    for (int c = 0; c < cycles; ++c) {
      // Mostly inside the writes; sometimes early, inside the open.
      const uint64_t crash_after =
          (rng() % 8 == 0) ? rng() % 60 : 1 + rng() % 1200;
      const Failure failure = driver.cycle(crash_after, batches);
      if (!failure.ok()) {
        std::printf("seed %llu, cycle %d, cut %llu operations in:\n  %s\n",
                    static_cast<unsigned long long>(seed), c + 1,
                    static_cast<unsigned long long>(crash_after),
                    failure.what.c_str());
        if (!dump.empty()) {
          const Status dumped = driver.fs().dump_to(dump);
          std::printf("%s\n", dumped.is_ok()
                                  ? ("the disk has been written to " + dump)
                                        .c_str()
                                  : dumped.to_string().c_str());
        }
        return 1;
      }
    }
    const Driver::Stats& stats = driver.stats();
    const SimFileSystem& fs = driver.fs();
    std::printf("seed %llu: %d cycles, %d batches, %d cut inside open, %d "
                "repairs, %d unsynced batches lost; %llu logs, %llu tables, "
                "%llu compactions' inputs deleted\n",
                static_cast<unsigned long long>(seed), stats.cycles,
                stats.batches_written, stats.cuts_inside_open, stats.repairs,
                stats.batches_lost,
                static_cast<unsigned long long>(fs.count("create .log")),
                static_cast<unsigned long long>(fs.count("create .sst")),
                static_cast<unsigned long long>(fs.count("remove .sst")));
    total_cycles += stats.cycles;
    total_repairs += stats.repairs;
    total_lost += stats.batches_lost;
    total_in_open += stats.cuts_inside_open;
  }
  std::printf("\n%d cuts survived (%d inside an open, %d needed repair, %d "
              "unsynced batches lost)\n",
              total_cycles, total_in_open, total_repairs, total_lost);
  return 0;
}
