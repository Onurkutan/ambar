// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Writes to a database on a disk that loses unsynced writes and refuses
// calls, cuts the power, reboots, and checks the durability contract.  The
// workload and the checks are shared by tests/test_powercut.cpp and
// tests/test_faults.cpp, which run bounded sweeps on every CI platform, and
// tools/powercut.cpp, which runs for as long as asked.
//
// The contract, from docs/DESIGN.md: if write() returned ok with sync set,
// the batch is there after the cut -- all of it.  Without sync the promise
// is a prefix: what comes back is the acknowledged batches up to some point,
// each whole, in order, and nothing else.  And without a cut -- the process
// closed the database and opened it again -- everything acknowledged is
// there, sync or not.  A write that returned an error may be there or not,
// but whole if it is.  All of it is checked against a model kept in this
// process, which needs no journal on disk because the process does not die
// -- only the simulated disk does.
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

// A batch the life tried to write, and whether write() returned ok.
struct Attempt {
  Batch batch;
  bool acknowledged;
};

// One life of the process.  Operation counts are relative to the start of
// the life; kNever means not at all.
struct Plan {
  int batches = 300;
  uint64_t cut = SimFileSystem::kNever;    // the power fails this far in
  uint64_t fault = SimFileSystem::kNever;  // one call fails this far in --
  std::string fault_kind;  // or the fault-th call of this kind this life
  uint64_t cut_after_fault = SimFileSystem::kNever;  // and the power this
                                                     // far after it
  // Applied to the open that follows the cut and checks the result: the
  // power fails again this far into it, or a call fails this far into it.
  uint64_t recovery_cut = SimFileSystem::kNever;
  uint64_t recovery_fault = SimFileSystem::kNever;
  // Whether a life whose writes finish before the cut lands is closed
  // properly rather than cut at its end.  Then nothing acknowledged may be
  // missing afterwards, synced or not.
  bool close_cleanly = false;
  // Whether the life compacts the whole database after its writes, so that
  // a compaction's calls -- its outputs, the manifest edit that installs
  // them, the removal of its inputs -- happen in every life, at its end,
  // rather than when the background thread gets to them.
  bool compact_at_end = false;
};

class Driver {
 public:
  struct Stats {
    int cycles = 0;
    int cuts_inside_open = 0;      // the open that starts a life
    int cuts_inside_recovery = 0;  // the open that follows a cut
    int repairs = 0;
    int faults = 0;         // calls made to fail
    int writes_failed = 0;  // writes that returned an error, power on
    int opens_retried = 0;  // opens that failed on a fault and were retried
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

  // How many operations of a kind the last life performed, from its open
  // to its close -- not counting the open that checked it afterwards.
  uint64_t life_count(const std::string& kind) const {
    const auto end = counts_at_end_.find(kind);
    const auto start = counts_at_start_.find(kind);
    return (end == counts_at_end_.end() ? 0 : end->second) -
           (start == counts_at_start_.end() ? 0 : start->second);
  }

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

  // The old shape, for the callers that only cut.
  Failure cycle(uint64_t cut, int batches,
                uint64_t recovery_cut = SimFileSystem::kNever) {
    Plan plan;
    plan.cut = cut;
    plan.batches = batches;
    plan.recovery_cut = recovery_cut;
    return cycle(plan);
  }

  // One life of the process: opens the database, writes up to
  // `plan.batches` batches, with one call failing `plan.fault` operations
  // in and the power failing `plan.cut` operations in -- or at the end, if
  // the writes finish first and the plan does not say to close cleanly.
  // Then reboots and checks.  The open that does the checking is the one
  // that recovers what the life left, and `plan.recovery_cut` and
  // `plan.recovery_fault` land inside it -- inside the table it writes from
  // the log, the manifest edit, the new log, the removal of the old one --
  // after which the open is repeated.  Returns the first thing found wrong.
  Failure cycle(const Plan& plan) {
    ++stats_.cycles;
    fs_.forget();
    counts_at_start_ = fs_.counts();
    const uint64_t start = fs_.operations();
    fs_.crash_at(at(start, plan.cut));
    if (plan.fault_kind.empty()) {
      fs_.fail_at(at(start, plan.fault));
    } else if (plan.fault != SimFileSystem::kNever) {
      fs_.fail_at(plan.fault_kind, fs_.count(plan.fault_kind) + plan.fault);
    }
    fs_.crash_after_fault(plan.cut_after_fault);
    cut_ = false;

    std::unique_ptr<DB> db;
    Status status = open(&db);
    // An open can return ok with the power already off: a cut that lands
    // on the removal of an old log, whose failure open ignores.  So the
    // disk is asked, not the status.
    if (!status.is_ok() || !fs_.powered()) {
      if (fs_.powered()) return fail("open failed: " + status.to_string());
      ++stats_.cuts_inside_open;
    } else {
      int failed_in_a_row = 0;
      for (int n = 0; n < plan.batches && failed_in_a_row < 3; ++n) {
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
        attempted_.push_back(Attempt{batch, false});
        status = db->write(options, &updates);
        // An ok from write() is an ok, whatever the compaction thread did to
        // the power in the meantime: every operation of the write itself
        // succeeded, or the write would not have returned ok.
        attempted_.back().acknowledged = status.is_ok();
        if (!fs_.powered()) break;  // the cut: the last attempt is in doubt
        if (!status.is_ok()) {
          // A write refused while the power is on: only a fault explains
          // it, and then the batch may or may not be there.  The engine
          // may refuse everything from here on, which is its right; three
          // in a row and the life is over.  (A fault that landed inside a
          // successful open counts as an explanation too: the open may
          // have started a compaction that met it.)
          if (fs_.faults() == faults_seen_) {
            return fail("write failed with no fault injected: " +
                        status.to_string());
          }
          ++stats_.writes_failed;
          ++failed_in_a_row;
          continue;
        }
        failed_in_a_row = 0;
        ++stats_.batches_written;
      }
    }

    if (db && plan.compact_at_end && fs_.powered()) {
      db->compact_range(nullptr, nullptr);
    }

    db.reset();
    counts_at_end_ = fs_.counts();
    if (fs_.powered() && !plan.close_cleanly) fs_.crash();
    cut_ = !fs_.powered();
    if (cut_) fs_.power_on();
    fs_.fail_at(SimFileSystem::kNever);
    fs_.fail_at(std::string(), 0);
    fs_.crash_after_fault(SimFileSystem::kNever);
    stats_.faults += static_cast<int>(fs_.faults() - faults_seen_);
    faults_seen_ = fs_.faults();
    survived_ = fs_.describe();
    return verify(plan);
  }

 private:
  static constexpr const char* kName = "sim/db";

  static uint64_t at(uint64_t start, uint64_t after) {
    return after == SimFileSystem::kNever ? SimFileSystem::kNever
                                          : start + after;
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
  //
  // An open that fails because a call was made to fail inside it is tried
  // once more: the fault was one call, and an open that cannot recover from
  // one refused call would be the finding.
  Status open(std::unique_ptr<DB>* db) {
    Status status = open_once(db);
    if (!status.is_ok() && fs_.powered() && fs_.faults() != faults_seen_) {
      ++stats_.opens_retried;
      stats_.faults += static_cast<int>(fs_.faults() - faults_seen_);
      faults_seen_ = fs_.faults();
      status = open_once(db);
    }
    return status;
  }

  Status open_once(std::unique_ptr<DB>* db) {
    Status status = DB::open(options(), kName, db);
    if (status.is_ok() || !fs_.powered()) return status;
    if (model_.tails == SimFileSystem::Tails::kOrdered ||
        !status.is_corruption()) {
      return status;
    }
    ++stats_.repairs;
    // What a repair may bring back is decided by what was applied before
    // it: every batch settled, and every one attempted in this life.
    repaired_ = true;
    repair_mark_ = settled_.size() + attempted_.size();
    RepairReport report;
    status = repair_db(kName, options(), &report);
    if (!status.is_ok()) return status;
    return DB::open(options(), kName, db);
  }

  // A key an earlier batch put, other than its key 0, or nothing.
  std::optional<std::string> old_key() {
    std::vector<const Batch*> candidates;
    for (const Batch& b : settled_) candidates.push_back(&b);
    for (const Attempt& a : attempted_) {
      if (a.acknowledged) candidates.push_back(&a.batch);
    }
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
  static void apply(const Batch& batch, size_t index,
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
        (*deleted)[op.key] = Deleted{index, (*ever)[op.key]};
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

  // After the life: opens -- with the power cut again inside that open,
  // or a call failed inside it, if asked, and the open repeated -- finds
  // how far the attempted batches got, and checks that the database is
  // exactly that prefix, with every synced batch inside it, and every
  // acknowledged one if there was no cut.
  Failure verify(const Plan& plan) {
    std::unique_ptr<DB> db;
    const int repairs_before = stats_.repairs;
    uint64_t recovery_cut = plan.recovery_cut;
    uint64_t recovery_fault = plan.recovery_fault;
    while (true) {
      const uint64_t start = fs_.operations();
      fs_.crash_at(at(start, recovery_cut));
      fs_.fail_at(at(start, recovery_fault));
      recovery_cut = recovery_fault = SimFileSystem::kNever;
      const Status status = open(&db);
      fs_.fail_at(SimFileSystem::kNever);
      if (status.is_ok() && fs_.powered()) break;
      if (fs_.powered()) {
        return fail("after the life, open failed: " + status.to_string());
      }
      ++stats_.cuts_inside_recovery;  // whatever open said: see cycle()
      cut_ = true;
      db.reset();
      fs_.power_on();
      survived_ = fs_.describe();
    }
    // Disarmed for the checks: an open that took fewer operations than the
    // cut was set for leaves it pending, and the compaction the open may
    // have started would trip it under the reads below.
    fs_.crash_at(SimFileSystem::kNever);
    stats_.faults += static_cast<int>(fs_.faults() - faults_seen_);
    faults_seen_ = fs_.faults();

    // Which attempts are there, and k: the last one that is.
    std::vector<bool> present(attempted_.size(), false);
    int k = -1;
    std::string value;
    for (size_t i = 0; i < attempted_.size(); ++i) {
      const Batch& batch = attempted_[i].batch;
      if (db->get(ReadOptions(), key_of(batch.id, 0), &value).is_ok()) {
        present[i] = true;
        k = static_cast<int>(i);
      }
    }

    // Every acknowledged batch with sync must be at or before k; without a
    // cut, every acknowledged batch must be.
    for (size_t i = 0; i < attempted_.size(); ++i) {
      const Attempt& attempt = attempted_[i];
      if (!attempt.acknowledged || static_cast<int>(i) <= k) continue;
      if (attempt.batch.sync || !cut_) {
        const std::string last =
            k < 0 ? std::string("none of this life's")
                  : std::to_string(attempted_[static_cast<size_t>(k)].batch.id);
        return fail("batch " + std::to_string(attempt.batch.id) +
                    (attempt.batch.sync ? " was acknowledged with sync"
                                        : " was acknowledged, and the power "
                                          "never went out,") +
                    " and it is gone (the last batch present is " + last +
                    ")");
      }
      ++stats_.batches_lost;
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
    //
    // A batch whose write returned an error counts if it is there and not
    // if it is not; an acknowledged one at or before k must be there, and
    // the scan below says so if it is not.
    std::map<std::string, std::string> expected;
    std::map<std::string, std::set<std::string>> ever;
    std::map<std::string, Deleted> deleted;
    size_t index = 0;
    for (const Batch& batch : settled_) {
      apply(batch, index++, &expected, &ever, &deleted);
    }
    std::vector<Batch> kept;
    for (int i = 0; i <= k; ++i) {
      const Attempt& attempt = attempted_[static_cast<size_t>(i)];
      if (attempt.acknowledged || present[static_cast<size_t>(i)]) {
        apply(attempt.batch, index++, &expected, &ever, &deleted);
        kept.push_back(attempt.batch);
      }
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

    // Settle: what is there is the history from here on.  A repair that
    // ran in this open saw exactly the settled batches: the mark taken when
    // it ran counted every attempt, and the ones not kept were not there.
    settled_.insert(settled_.end(), kept.begin(), kept.end());
    attempted_.clear();
    if (stats_.repairs != repairs_before) repair_mark_ = settled_.size();
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
  bool cut_ = false;                 // the power went out this life
  uint64_t faults_seen_ = 0;         // fs_.faults() as of the last look
  std::map<std::string, uint64_t> counts_at_start_;  // of the last life
  std::map<std::string, uint64_t> counts_at_end_;
  std::vector<Batch> settled_;       // present in the database, in order
  std::vector<Attempt> attempted_;   // this life, in order
  std::vector<std::string> survived_;  // the disk as the last reboot found it
  Stats stats_;
};

}  // namespace ambar::powercut
