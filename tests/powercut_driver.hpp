// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Writes to a database on a disk that loses unsynced writes, cuts the power,
// reboots, and checks the durability contract.  The workload and the checks
// are shared by tests/test_powercut.cpp, which runs a bounded sweep on every
// CI platform, and tools/powercut.cpp, which runs for as long as asked.
//
// The contract, from docs/DESIGN.md: if write() returned ok with sync set,
// the batch is there after the cut -- all of it.  Without sync the promise
// is a prefix: what comes back is the acknowledged batches up to some point,
// each whole, in order, and nothing else.  Both halves are checked against a
// model kept in this process, which needs no journal on disk because the
// process does not die -- only the simulated disk does.
//
// The workload is one writer.  Group commit therefore never merges two
// batches into one record, and a follower never rides a leader's fsync;
// tools/crash_test covers concurrent writers against a process kill, and
// those paths are the same code.
#pragma once

#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "ambar/db.hpp"
#include "sim_file_system.hpp"

namespace ambar::powercut {

constexpr int kKeysPerBatch = 4;

inline std::string key_of(int batch, int index) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%08d.%d", batch, index);
  return buf;
}

// Derived from the batch that wrote it, so a value assembled from two
// records, or one an older batch wrote, is detected.
inline std::string value_of(int batch, int index) {
  std::string out = "v" + std::to_string(batch) + "." + std::to_string(index) +
                    ":";
  while (out.size() < 100) out += "0123456789abcdef";
  return out.substr(0, 100);
}

// One write: kKeysPerBatch fresh keys, then a few operations on keys that
// earlier batches put -- an overwrite, a delete, a put of a key that was
// deleted -- so that a write numbered below an existing entry, which loses
// to it, has something to lose to.  Key 0 of a batch is never touched
// again, so its presence says the batch is there.
struct Batch {
  struct Op {
    std::string key;
    std::optional<std::string> value;  // none: delete
  };
  int id = 0;
  bool sync = false;
  std::vector<Op> ops;
};

struct Failure {
  std::string what;
  bool ok() const { return what.empty(); }
};

class Driver {
 public:
  struct Stats {
    int cycles = 0;
    int cuts_inside_open = 0;      // the open that starts a life
    int cuts_inside_recovery = 0;  // the open that follows a cut
    int repairs = 0;
    int batches_written = 0;
    int batches_lost = 0;  // acknowledged without sync and gone: allowed
  };

  Driver(SimFileSystem::Model model, uint64_t seed, bool sync_writes = true)
      : model_(model),
        fs_(model, seed),
        rng_(static_cast<std::mt19937::result_type>(seed)),
        sync_enabled_(sync_writes) {
    previous_ = set_file_system(&fs_);
    fs_.create_directory("sim");  // the parent, so the database can be made
    fs_.sync_directory("sim");
  }

  ~Driver() { set_file_system(previous_); }

  Driver(const Driver&) = delete;
  Driver& operator=(const Driver&) = delete;

  SimFileSystem& fs() { return fs_; }
  const Stats& stats() const { return stats_; }

  // Whether sync is chosen per phase (runs of a few to sixty batches, for
  // the reason crash_test gives: a sync covers everything before it, so
  // alternating would leave almost nothing unsynced to lose) or asked for
  // by every write.
  void set_sync_phases(bool enabled) { phases_ = enabled; }

  // Removes CURRENT, durably, so that the next open refuses and repair has
  // to run.  For sweeping cuts through a repair.
  Status lose_current() {
    Status status = fs_.remove_file(std::string(kName) + "/CURRENT");
    if (status.is_ok()) status = fs_.sync_directory(kName);
    return status;
  }

  // One life of the process: opens the database, writes up to `batches`
  // batches, and cuts the power `crash_after` operations from now -- or at
  // the end, if the writes finish first, so that every cycle ends in a cut.
  // Then reboots and checks.  The open that does the checking is the one
  // that recovers the log the cut left, and `recovery_cut_after` cuts the
  // power that many operations into it -- inside the table it writes from
  // the log, the manifest edit, the new log, the removal of the old one --
  // after which the reboot and the open are repeated.  Returns the first
  // thing found wrong.
  Failure cycle(uint64_t crash_after, int batches,
                uint64_t recovery_cut_after = SimFileSystem::kNever) {
    ++stats_.cycles;
    arm(crash_after);

    std::unique_ptr<DB> db;
    Status status = open(&db);
    // An open can return ok with the power already off: a cut that lands
    // on the removal of an old log, whose failure open ignores.  So the
    // disk is asked, not the status.
    if (!status.is_ok() || !fs_.powered()) {
      if (fs_.powered()) return fail("open failed: " + status.to_string());
      ++stats_.cuts_inside_open;
    } else {
      for (int n = 0; n < batches; ++n) {
        const Batch batch = next_batch();
        WriteBatch updates;
        for (int i = 0; i < kKeysPerBatch; ++i) {
          updates.put(key_of(batch.id, i), value_of(batch.id, i));
        }
        for (const Batch::Op& op : batch.ops) {
          if (op.value) {
            updates.put(op.key, *op.value);
          } else {
            updates.del(op.key);
          }
        }
        WriteOptions options;
        options.sync = batch.sync;
        in_flight_ = batch;
        status = db->write(options, &updates);
        if (!status.is_ok()) {
          if (fs_.powered()) return fail("write failed: " + status.to_string());
          break;
        }
        acknowledged_.push_back(batch);
        in_flight_.reset();
        ++stats_.batches_written;
      }
    }

    db.reset();
    if (fs_.powered()) fs_.crash();
    fs_.power_on();
    survived_ = fs_.describe();
    return verify(recovery_cut_after);
  }

 private:
  static constexpr const char* kName = "sim/db";

  void arm(uint64_t after) {
    fs_.crash_at(after == SimFileSystem::kNever ? SimFileSystem::kNever
                                                : fs_.operations() + after);
  }

  Options options() const {
    Options options;
    options.create_if_missing = true;
    // Small, so that a few hundred batches cross flushes, log rotations and
    // compactions, and the cut lands in the middle of those as well as in
    // the middle of a write.
    options.write_buffer_size = 64 << 10;
    options.max_file_size = 1 << 20;
    return options;
  }

  // Opens the database.  Whether the power is on is asked of the disk, not
  // read off the Status: recovery reports an unreadable file as corruption
  // and a missing one as a missing database, whatever made it so.
  //
  // With the power on, a refusal is a failure -- except under the holes
  // and garbage models, where a zeroed or overwritten span with intact
  // records after it reads as damage by design, and the remedy is repair.
  // Repair is then run and the open retried; a refusal for a missing
  // CURRENT is included, because a cut inside a repair leaves exactly that.
  Status open(std::unique_ptr<DB>* db) {
    Status status = DB::open(options(), kName, db);
    if (status.is_ok() || !fs_.powered()) return status;
    if (model_.tails == SimFileSystem::Tails::kOrdered ||
        !status.is_corruption()) {
      return status;
    }
    ++stats_.repairs;
    // What a repair may bring back is decided by what was applied before
    // it: every batch settled, and every one acknowledged in this life.
    repaired_ = true;
    repair_mark_ = settled_.size() + acknowledged_.size() +
                   (in_flight_ ? 1u : 0u);
    RepairReport report;
    status = repair_db(kName, options(), &report);
    if (!status.is_ok()) return status;
    return DB::open(options(), kName, db);
  }

  // A key an earlier batch put, other than its key 0, or nothing.
  std::optional<std::string> old_key() {
    std::vector<const Batch*> candidates;
    for (const Batch& b : settled_) candidates.push_back(&b);
    for (const Batch& b : acknowledged_) candidates.push_back(&b);
    if (candidates.size() < 4) return std::nullopt;
    const size_t pick = rng_() % candidates.size();
    const int index = 1 + static_cast<int>(rng_() % (kKeysPerBatch - 1u));
    return key_of(candidates[pick]->id, index);
  }

  Batch next_batch() {
    Batch batch;
    batch.id = next_id_++;
    if (sync_enabled_) {
      if (phases_) {
        if (phase_remaining_ == 0) {
          phase_sync_ = (rng_() % 2) == 0;
          phase_remaining_ = 5 + static_cast<int>(rng_() % 60);
        }
        --phase_remaining_;
        batch.sync = phase_sync_;
      } else {
        batch.sync = true;
      }
    }
    // Something on older keys, most of the time: an overwrite, a delete, or
    // a put of a key that a batch deleted.  A key is used once per batch.
    std::set<std::string> used;
    const int extras = static_cast<int>(rng_() % 3);
    for (int i = 0; i < extras; ++i) {
      const std::optional<std::string> key = old_key();
      if (!key || used.count(*key) != 0) continue;
      used.insert(*key);
      const bool erase = (rng_() % 3) == 0;
      Batch::Op op;
      op.key = *key;
      if (!erase) op.value = value_of(batch.id, 10 + i);
      batch.ops.push_back(op);
    }
    return batch;
  }

  // What a repair could bring back for a key: the values put for it before
  // the batch that last deleted it, and which batch that was.
  struct Deleted {
    size_t by = 0;  // index of the deleting batch in the applied order
    std::set<std::string> values;
  };

  // The expected contents after a batch, at index `at` in the applied
  // order, plus what would explain the key coming back after a repair.
  static void apply(const Batch& batch, size_t at,
                    std::map<std::string, std::string>* model,
                    std::map<std::string, std::set<std::string>>* ever,
                    std::map<std::string, Deleted>* deleted) {
    for (int i = 0; i < kKeysPerBatch; ++i) {
      (*model)[key_of(batch.id, i)] = value_of(batch.id, i);
      (*ever)[key_of(batch.id, i)].insert(value_of(batch.id, i));
    }
    for (const Batch::Op& op : batch.ops) {
      if (op.value) {
        (*model)[op.key] = *op.value;
        (*ever)[op.key].insert(*op.value);
      } else {
        model->erase(op.key);
        (*deleted)[op.key] = Deleted{at, (*ever)[op.key]};
      }
    }
  }

  // The description is of the disk as the reboot found it, before the open
  // that checks it changed anything.
  Failure fail(const std::string& what) {
    Failure failure;
    failure.what = what;
    for (const std::string& line : survived_) {
      failure.what += "\n    " + line;
    }
    return failure;
  }

  // After the reboot: opens -- with the power cut again inside that open,
  // if asked, and the reboot and open repeated -- finds how far the
  // acknowledged batches got, and checks that the database is exactly that
  // prefix, with every synced batch inside it.
  Failure verify(uint64_t recovery_cut_after) {
    std::unique_ptr<DB> db;
    while (true) {
      arm(recovery_cut_after);
      recovery_cut_after = SimFileSystem::kNever;
      const Status status = open(&db);
      if (status.is_ok() && fs_.powered()) break;
      if (fs_.powered()) {
        return fail("after the cut, open failed: " + status.to_string());
      }
      ++stats_.cuts_inside_recovery;  // whatever open said: see cycle()
      db.reset();
      fs_.power_on();
      survived_ = fs_.describe();
    }
    // Disarmed for the checks: an open that took fewer operations than the
    // cut was set for leaves it pending, and the compaction the open may
    // have started would trip it under the reads below.
    fs_.crash_at(SimFileSystem::kNever);

    std::vector<Batch> candidates = acknowledged_;
    if (in_flight_) candidates.push_back(*in_flight_);

    // k: the last candidate whose key 0 is there.
    int k = -1;
    std::string value;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (db->get(ReadOptions(), key_of(candidates[i].id, 0), &value).is_ok()) {
        k = static_cast<int>(i);
      }
    }

    // Every synced batch must be at or before k.
    for (size_t i = 0; i < acknowledged_.size(); ++i) {
      if (acknowledged_[i].sync && static_cast<int>(i) > k) {
        const std::string last =
            k < 0 ? std::string("none of this cycle's")
                  : std::to_string(candidates[static_cast<size_t>(k)].id);
        return fail("batch " + std::to_string(acknowledged_[i].id) +
                    " was acknowledged with sync and is gone (the last batch "
                    "present is " + last + ")");
      }
    }
    if (k + 1 < static_cast<int>(acknowledged_.size())) {
      stats_.batches_lost +=
          static_cast<int>(acknowledged_.size()) - (k + 1);
    }

    // The database must be exactly the model after the settled batches and
    // candidates[0..k]: nothing missing, nothing extra, nothing torn.
    //
    // One allowance, once a repair has run.  Repair reads every table in
    // the directory, and a compaction that dropped a tombstone at the
    // bottom level may have left its input behind -- the delete of the
    // input is metadata that need not have landed -- so a key deleted
    // before the repair may come back, holding a value put before the
    // delete, and stay until something writes it again.  docs/DESIGN.md
    // states this under Repair; the check here is that it is only ever
    // that: never a key deleted after the last repair, never a value from
    // after the delete, never a value nobody wrote.
    std::map<std::string, std::string> expected;
    std::map<std::string, std::set<std::string>> ever;
    std::map<std::string, Deleted> deleted;
    size_t at = 0;
    for (const Batch& batch : settled_) {
      apply(batch, at++, &expected, &ever, &deleted);
    }
    for (int i = 0; i <= k; ++i) {
      apply(candidates[static_cast<size_t>(i)], at++, &expected, &ever,
            &deleted);
    }

    std::unique_ptr<Iterator> iter(db->new_iterator(ReadOptions()));
    auto want = expected.begin();
    for (iter->seek_to_first(); iter->valid(); iter->next()) {
      const std::string key(iter->key());
      if (want != expected.end() && want->first < key) {
        return fail(want->first + " is missing (an earlier batch than the "
                    "last one present, or a key a batch put with the ones "
                    "that did survive)");
      }
      if (want == expected.end() || key < want->first) {
        const auto was = deleted.find(key);
        if (repaired_ && was != deleted.end() &&
            was->second.by < repair_mark_ &&
            was->second.values.count(std::string(iter->value())) != 0) {
          continue;  // a deleted key repair brought back, with a real value
        }
        return fail("the database holds " + key +
                    ", which should not be there");
      }
      if (iter->value() != std::string_view(want->second)) {
        return fail(key + " holds " + std::string(iter->value()) +
                    ", not the value the last batch to write it put");
      }
      ++want;
    }
    if (!iter->status().is_ok()) {
      return fail("the scan failed: " + iter->status().to_string());
    }
    if (want != expected.end()) {
      return fail(want->first + " is missing");
    }

    // Settle: what is there is the history from here on.
    for (int i = 0; i <= k; ++i) {
      settled_.push_back(candidates[static_cast<size_t>(i)]);
    }
    acknowledged_.clear();
    in_flight_.reset();
    return Failure();
  }

  const SimFileSystem::Model model_;
  SimFileSystem fs_;
  FileSystem* previous_;
  std::mt19937 rng_;
  const bool sync_enabled_;
  bool phases_ = true;
  int phase_remaining_ = 0;
  bool phase_sync_ = false;
  int next_id_ = 0;
  bool repaired_ = false;            // some open so far needed repair
  size_t repair_mark_ = 0;           // batches applied before the last one
  std::vector<Batch> settled_;       // present in the database, in order
  std::vector<Batch> acknowledged_;  // this cycle, in order
  std::optional<Batch> in_flight_;   // the write the cut interrupted
  std::vector<std::string> survived_;  // the disk as the last reboot found it
  Stats stats_;
};

}  // namespace ambar::powercut
