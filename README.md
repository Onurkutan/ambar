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
* **Concurrent reads and writes.** One writer at a time, any number of
  readers, and compaction running alongside both, with no lock held across I/O.
* **One process at a time.** Opening a directory that another process already
  has open is refused rather than allowed to destroy both copies.
* **Damaged files produce errors, not crashes.** Every table file is treated as
  untrusted input.

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
`tools/` so the measurement can be re-run rather than believed.

## Verification

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ./build/ambar_tests

180 tests, no external framework. Also:

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

**Mutation testing.** A green suite says the tests pass. It does not say they
would fail if the code were wrong, and those are different claims.

    python3 tools/mutate.py mutations/merger.json

This breaks the code on purpose, one defect at a time, rebuilds, and reports
which tests notice. It is how the merging iterator's direction-change tests
were shown to have teeth, how the crash test's reader was designed, and how one
surviving mutation turned out to be genuinely equivalent rather than a missing
test — recorded in `mutations/README.md` so the next person does not go looking
for a test that should not exist.

**Failure injection.** Makes one `fsync` or `rename` return `EIO`, at each
point in a workload where one occurs, and checks the database still opens and
still holds everything it acknowledged. A crash test lands somewhere random;
this visits the rare instant deliberately.

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

See `docs/BENCHMARKS.md` for the numbers and what they do and do not show.

## Documentation

* `docs/DESIGN.md` — what the engine guarantees, how, and what it explicitly
  does not guarantee.
* `docs/BENCHMARKS.md` — measurements, with the conditions that produced them.
* `mutations/README.md` — which deliberate faults the tests catch, and which
  they cannot.

## Limitations

Stated so that the absence is a decision rather than something a reader has to
discover: no compression (the format reserves the field), no repair tool (a
database that has lost its manifest is refused rather than rebuilt — or, since
this release, created over), one compaction thread, no column families or
transactions. `docs/DESIGN.md` says why for each.

The `sync=true` durability guarantee rests on the `fsync` calls being correct
by inspection, not by test: `SIGKILL` leaves everything the kernel has, so a
process-kill test cannot tell a database that calls `fsync` from one that never
does. Checking it needs the storage to lose writes.

Write amplification — the number a reader most wants beside the write
throughput — is not measured either. `docs/BENCHMARKS.md` reports space
amplification and says plainly that the two are different quantities.

## Licence

MIT. See `LICENSE`.
