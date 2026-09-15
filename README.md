# Ambar

[![CI](https://github.com/Onurkutan/ambar/actions/workflows/ci.yml/badge.svg)](https://github.com/Onurkutan/ambar/actions/workflows/ci.yml)

A log-structured merge-tree storage engine in C++17, written from scratch: a
write-ahead log, a concurrent skip list, immutable sorted table files with
Bloom filters, a manifest, levelled compaction, snapshots, and a bounded block
cache. About 10,000 lines of engine and 6,000 of tests and tools, with no
dependencies beyond the standard library.

It is a learning project, and the interesting part is not that it works — it is
what it took to find out that it did not.

```cpp
#include "ambar/db.hpp"

ambar::Options options;
options.create_if_missing = true;

std::unique_ptr<ambar::DB> db;
ambar::DB::open(options, "/tmp/mydb", &db);

db->put(ambar::WriteOptions(), "key", "value");

std::string value;
ambar::Status status = db->get(ambar::ReadOptions(), "key", &value);
```

## What it does

* **Ordered key/value storage** on disk, keys and values arbitrary byte
  strings, with `put`, `get`, `delete`, batched writes, forward and reverse
  iteration, and snapshots.
* **Atomic multi-key writes.** A batch is applied entirely or not at all,
  whatever happens — inherited from the atomicity of a single log record rather
  than implemented separately.
* **Crash recovery.** Reopening replays the write-ahead log and the manifest.
  A write acknowledged with `sync` survives a power cut, and a sequence of
  writes without it survives as a prefix -- tested against a simulated disk
  that loses what was not synced, not only asserted.
* **Concurrent reads and writes.** One writer at a time, any number of
  readers, and compaction running alongside both, with no lock held across I/O.
* **One process at a time.** Opening a directory that another process already
  has open is refused rather than allowed to destroy both copies.
* **Damaged files produce errors, not crashes.** Every table file is treated as
  untrusted input.
* **Repair.** A database whose manifest is lost or damaged is refused rather
  than guessed at, and `ambar_repair` rebuilds it from the table and log files
  that survive -- keeping what reads back, setting aside what does not, merging
  what it kept, and opening the result to prove it.

## What was found while building it

The tests are the point of the project, so these are the results rather than a
feature list. Each is a bug that a passing test suite did not catch, found by a
specific technique, and each now has a test that fails if it comes back — each
of those tests checked by reverting the fix and watching it go red.

**A Bloom filter that hid keys that were present.** A table stores internal
keys — a user key plus a sequence number — so a filter built over what the
table writes knows about `key_42 at sequence 1017`, while a lookup asks about
`key_42 as of the newest snapshot`. The filter answered correctly for the
question it was asked, and the table skipped the block. Every symptom pointed
away from the cause: the file was intact, the checksums passed, iteration
returned every key, and only point lookups failed. It was caught by a test that
*counts disk reads*: a thousand lookups for keys that were present read a dozen
blocks between them, which is not a plausible number for a thousand successful
lookups. The test that pins it today measures the same thing from the other
side and prints `unwrapped filter: 17 of 2000 present keys found`.

**A hash that was weak only on realistic keys.** The Bloom filter's false
positive rate on keys of the form `key_00000042` was 15.7 % against a predicted
0.8 %. It passed every distribution check that is easy to write — 10,000 keys
gave 10,000 distinct hashes, 10,000 distinct probe deltas, six distinct probe
positions each, and a bit array at the theoretically optimal density. On random
keys it measured 0.8 %. The property that separates a good hash from that one
is avalanche, and it can only be measured on the hash itself, so
`tests/test_hash.cpp` measures it, with thresholds calibrated against a hash
already known to be sound rather than chosen to pass.

**A durability contract that was false.** `docs/DESIGN.md` promised that a
write returning ok survives the process dying even without `sync`. It did not:
the log record sat in this process's own buffer while the write became
readable, so a `SIGKILL` destroyed values that a reader had already been given.
Found by `tools/crash_test` running a reader alongside the writer and
journalling what the reader had been handed.

**A cleanup that deleted the manifest the database needed.** The database
worked for as long as the process lived and then would not open again, with
every table file intact. A sibling of the same bug leaked one log file per
open, which was harmless and would have gone unnoticed indefinitely.

**Two data races** on version reference counts, found by ThreadSanitizer once
the tests actually ran several threads at once.

**A missing database lock.** Two processes could open the same directory, and
both would succeed. They replay each other's logs, hand out the same file
numbers, and each one's cleanup deletes files the other is using. Two processes
writing 6,000 keys each destroyed 11,032 of the 12,000 acknowledged writes
between them, and neither reported an error.

**Four ways a hostile table file could break the reader**, including an entry
whose declared lengths summed to zero in 32-bit arithmetic and then copied four
gigabytes out of a fifteen-byte block. Found by an adversarial review that
forged files rather than damaging real ones.

**The prefix promise was broken at every log rotation.** Without `sync`, the
contract is that recovery yields some prefix of the acknowledged writes. When
the memtable filled, the engine closed the old log and opened a new one, and
synced nothing until the memtable had been written out as a table; a `sync`
write into the new log made that file durable and nothing else. A power cut in
the window kept the new log's writes and lost the old log's unsynced tail:
later writes present, earlier ones gone. No process-kill test can see this,
because the kernel keeps both tails, and every `fsync` in the code was where
inspection said it should be. Found in the first minute of running the engine
on a simulated disk that loses unsynced writes -- along with a second, smaller
one: a new log's name was never synced, which Linux filesystems forgive and
POSIX does not. Both fixed, and `mutations/powercut.json` now removes the
engine's syncs one at a time to show that each removal is caught.

**A manifest that had once failed to sync was appended to, and read as
damaged after the next power cut.** After an `fsync` fails, ext4 reports it
once per open file, marks the pages it could not write as clean, and never
writes them; the data stays readable from the page cache. The next open read the manifest from
that cache, saw a record that would never be on the disk, and appended after
it — a hole no one could see until the power went. Found by failing one call
of each kind at every point of a workload on the simulated disk. The manifest
is now rewritten fresh at every open, and a sibling finding from the same
sweep — a failed directory sync after the `CURRENT` rename made the caller
delete the manifest `CURRENT` had just been pointed at — is fixed beside it.

**A single failed `fsync` could make a database permanently unopenable.** When
a manifest write failed, compaction deleted the output files it had just
written — but the record naming them had already reached the file, so the
manifest referenced tables that no longer existed. Sweeping every manifest
`fsync` in a workload, 17 of 85 injection points produced a database that would
never open again with all of its data still on disk.

**Losing a sixteen-byte file lost the database.** `CURRENT` names the manifest,
and a crash on a filesystem that does not make the rename durable can leave it
missing. With `create_if_missing` set — as the quick-start above sets it — the
open path saw an empty directory, created a fresh database, and then ran the
cleanup that follows every successful open, which deletes each table the
manifest does not name. The fresh manifest named none of them. Found by reading
the open path rather than by any test, because every corruption test opened
with `create_if_missing` off; the test that pins it now opens the other way and
counts the tables before and after.

**One flipped bit in the manifest rolled the database back, then deleted what
it rolled back past.** The log reader stopped at any checksum failure and
called it a torn tail, and recovery treated a torn tail as harmless — correctly
for a tail, where nothing after the damage was ever written. A bit flipped in
the *middle* of the manifest left every later edit intact on disk, unread. Open
succeeded on the older state, cleanup deleted every table the unread edits
named, and because new edits are appended after the damage, the next open did
it again. The corruption suite had opened 132 damaged copies and accepted every
status, including the successful ones; it never asked what the successful ones
had done. The reader now looks past a failure for a record it can verify, and a
manifest or log that has one is refused with its files intact.

**A benchmark row that measured the wrong thing.** The absent-key lookups —
the row credited to the Bloom filter, 6.4× SQLite — used keys numbered past
the last key in the database, and a key past the end of every table is
rejected by a range check before any table, or its filter, is consulted. The
row was a binary search over file ranges, and would have read the same with
no filter at all. Nothing in the rate said so. It was found the day table
reads per lookup were counted: the phase made 0.00, where a filter with ten
bits per key lets about one lookup in a hundred through to a block. The
keys now fall between present ones, and `docs/BENCHMARKS.md` reports what
the filter actually costs and saves.

**Two locks on the read path that did not need to be there.** The engine
promises any number of readers, and the first time the benchmark ran lookups
on eight threads with the whole database in memory, it got 2.8 times the
rate of one thread on a twelve-core machine. With the disk in the path the
same threads got 4.9 times, because the reads hid the locks. Each was found
by taking a lock out and measuring again: the table cache's one lock over
every open table, taken twice per lookup, which is sharded now; and a second
acquisition of the database mutex on the way out of every lookup, to drop
its reference counts, which a lookup now does without it — the version's
count is atomic, and only the last reference is still dropped under the
mutex, which a lookup's never is while the version is current. The first
cut of that dropped the reference first and took the mutex afterwards to
see whether the count was still zero, and tracing it before it was
committed found the window that protocol admits, where a thread under the
mutex could take and drop a reference and delete the version under the
reader; nothing in the engine can take that reference today, and the form
that replaced it does not need that to stay true. Eight threads read 1.3
million keys a second from memory now, up from 1.1 million, and
`docs/BENCHMARKS.md` says where the rest of the gap is.

**And several claims that were simply wrong.** The design document said, as
such documents usually do, that the log record must precede the memtable insert
or a value becomes readable before it is durable. Swapping the two and running
the crash test showed no failure — correctly, because visibility in this engine
is conferred by advancing the sequence number, which happens after both.

That was not the only one. A documentation audit against the code found twenty
false or unsupported statements: a benchmark table that quoted a SQLite figure
40 % below what the machine produces, two options documented as controlling
checksum verification that no code path consulted, a test file cited by name
that did not exist, and a measurement attributed to a tool that had never been
committed. Each is fixed in the obvious way — the numbers re-measured, the dead
options removed, the missing test written, the fault-injection harness added to
`tools/` so the measurement can be re-run rather than believed. The audit
missed one: the comment at the top of the benchmark said that the bytes
written were counted and divided by the bytes of user data, and the same for
reads, when nothing in the file counted either. Writes are counted now, by the
engine, and the comment says what still is not.

## Verification

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ./build/ambar_tests

217 tests, no external framework. Also:

    cmake -S . -B build-asan -DAMBAR_SANITIZE=address   # ASan + UBSan
    cmake -S . -B build-tsan -DAMBAR_SANITIZE=thread    # ThreadSanitizer

**Corruption suite.** Opens 132 randomly damaged copies of a real database
under AddressSanitizer — bit flips, truncations, runs of garbage — plus
hand-forged table files built to defeat specific bounds checks, and requires
that each produces a status rather than a crash.

**Crash injection.** Kills a writing child process with `SIGKILL` at random
instants, reopens, and checks that nothing acknowledged was lost, nothing a
read returned was lost, no batch arrived in pieces, and nothing appeared that
was never written. Rounds accumulate, so each one recovers from a database that
a previous crash left behind.

    ./build/ambar_crash_test /tmp/crash run 20

**Power cuts.** The engine's whole view of the disk is one small interface,
and the tests replace it with a disk that keeps what `fsync` covered and loses
a random amount of the rest -- pages of zeros, pages of garbage, a name that
never landed. A few hundred cuts run on every platform in the unit suite; this
runs as many as asked, under whichever filesystem model, and keeps the disk a
failure left for `ambar_repair` and a hex editor.

    ./build/ambar_powercut --dirents posix --tails holes --seeds 8 --cycles 300

**Mutation testing.** A green suite says the tests pass. It does not say they
would fail if the code were wrong, and those are different claims.

    python3 tools/mutate.py mutations/merger.json

This breaks the code on purpose, one defect at a time, rebuilds, and reports
which tests notice. It is how the merging iterator's direction-change tests
were shown to have teeth, how the crash test's reader was designed, and how one
surviving mutation turned out to be genuinely equivalent rather than a missing
test — recorded in `mutations/README.md` so the next person does not go looking
for a test that should not exist.

**Failure injection.** One I/O call of each kind — append, sync, close,
create, rename, remove, directory sync — fails at every point of a workload
on the simulated disk, then the power goes as well; the database has to
report the error, keep what it acknowledged, and open again. In the unit
suite on every platform, and in `ambar_powercut --faults 50`. The original
form, one `fsync` or `rename` returning `EIO` through the real system calls
under `LD_PRELOAD`, is kept for the real syscall path:

    ./tools/fault_sweep.sh build 3

**Fuzzing.** The corruption suite damages files in ways somebody thought of.
This hands each parser of untrusted bytes — table, block, filter block, log,
write batch, manifest — inputs nobody thought of, under ASan and UBSan,
starting from a valid example of each format that the engine's own writers
produce. Needs clang, which is where libFuzzer lives.

    CC=clang CXX=clang++ cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Debug -DAMBAR_FUZZ=ON
    cmake --build build-fuzz --target fuzzers
    ./build-fuzz/fuzz_seeds corpus
    ./build-fuzz/fuzz_table -max_len=16384 corpus/table

CI runs every target for a minute on each push as a smoke test; a real
campaign is the same command left running. An input that crashes a parser is
written to the working directory and is, on its own, the whole bug report.

**Benchmarks**, against SQLite where it is available:

    ./build/ambar_bench /tmp/bench --keys 1000000

Throughput and latency as percentiles, and three amplifications kept apart:
write, from the engine's own count of every byte it appended to a log, a
table or a manifest, against every byte it was handed; read, from its count
of every table block the cache did not answer, against the lookups that
caused them; and space, the settled size on disk against the distinct data.
The counts are checked rather than trusted -- `tests/test_stats.cpp` runs
the engine on the simulated disk and requires the engine's figures to equal
the disk's own tallies of what was appended and what was served, and
`mutations/stats.json` removes each counter in turn. And random reads on one
to eight threads, with the database on disk and with it held entirely in
memory, where the engine's own locks are all that is left to measure. See
`docs/BENCHMARKS.md`
for the numbers and what they do and do not show.

## Documentation

* `docs/DESIGN.md` — what the engine guarantees, how, and what it explicitly
  does not guarantee.
* `docs/BENCHMARKS.md` — measurements, with the conditions that produced them.
* `mutations/README.md` — which deliberate faults the tests catch, and which
  they cannot.

## Limitations

Stated so that the absence is a decision rather than something a reader has to
discover: no compression (the format reserves the field), one compaction
thread, no column families or transactions. `docs/DESIGN.md` says why for
each. Repair rebuilds a database but not the history behind it: a key deleted
before the damage can come back if a stale pre-compaction file survived, and
`docs/DESIGN.md` says exactly when.

The `sync=true` guarantee is tested against a simulated disk, not a real one:
whether `fsync` on a given platform reaches the platter, and whether a
filesystem in writeback mode hands a new log the intact records of a deleted
one, are outside what the simulation can see, and `docs/DESIGN.md` says so.

Both amplifications are measured from the engine's own counts — write, of
the logs, tables and manifests it writes; read, of the table blocks the
cache did not answer — and `docs/BENCHMARKS.md` reports them beside the
space amplification that used to stand in for the first, which is a
different quantity. Neither is measured under memory pressure or with a cold
page cache.

## Licence

MIT. See `LICENSE`.
