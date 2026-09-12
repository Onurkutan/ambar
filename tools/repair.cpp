// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Rebuilds a database from the files in its directory.
//
//   ambar_repair <dir> [--force]
//
// For a database that open() refuses -- CURRENT lost, a manifest damaged in
// the middle, a table the manifest names that is not there.  Reads every
// table and log, keeps what reads back, moves what does not to <dir>/lost/,
// merges what it kept into fresh tables, writes a manifest naming them, and
// opens the result to prove it.  Prints what it did; exits 0 when the
// database opened afterwards.
//
// A database that opens is left alone unless --force is given: repair
// rewrites every table, and an intact database gains nothing from that.  The
// check is an ordinary open, with everything an open does.  See repair_db in
// include/ambar/db.hpp for what a repair cannot restore.
//
// Exit codes: 0 repaired and opened; 1 repair failed; 2 usage; 3 the database
// opens as it is, so nothing was done.

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "ambar/db.hpp"

int main(int argc, char** argv) {
  bool force = false;
  std::string name;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--force") == 0) {
      force = true;
    } else if (name.empty()) {
      name = argv[i];
    } else {
      name.clear();
      break;
    }
  }
  if (name.empty()) {
    std::fprintf(stderr, "usage: %s <database-directory> [--force]\n", argv[0]);
    return 2;
  }

  if (!force) {
    ambar::Options options;
    options.create_if_missing = false;
    std::unique_ptr<ambar::DB> db;
    const ambar::Status opens = ambar::DB::open(options, name, &db);
    if (opens.is_ok()) {
      db.reset();
      std::printf("%s opens; nothing to repair (pass --force to rewrite it "
                  "anyway)\n", name.c_str());
      return 3;
    }
    std::printf("open refused: %s\n", opens.to_string().c_str());
  }

  ambar::RepairReport report;
  const ambar::Status status = ambar::repair_db(name, ambar::Options(), &report);

  for (const std::string& note : report.notes) {
    std::printf("  %s\n", note.c_str());
  }
  std::printf("tables kept: %d, set aside: %d\n", report.tables_kept,
              report.tables_set_aside);
  std::printf("logs replayed: %d, set aside: %d\n", report.logs_converted,
              report.logs_set_aside);
  std::printf("tables written: %d, last sequence: %llu\n",
              report.tables_written,
              static_cast<unsigned long long>(report.last_sequence));

  if (!status.is_ok()) {
    std::fprintf(stderr, "repair failed: %s\n", status.to_string().c_str());
    return 1;
  }
  std::printf("the repaired database opened; it is ready to use\n");
  return 0;
}
