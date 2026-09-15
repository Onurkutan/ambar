# Mutations

Each file here is a set of deliberate defects, applied one at a time, to check
that the tests would fail if the code were wrong. A green suite says the tests
pass; it does not say they would notice a bug, and those are different claims.

    python3 tools/mutate.py mutations/merger.json
    python3 tools/mutate.py mutations/bloom.json
    python3 tools/mutate.py mutations/write_batch.json
    python3 tools/mutate.py mutations/recovery.json
    python3 tools/mutate.py mutations/powercut.json
    python3 tools/mutate.py mutations/faults.json
    python3 tools/mutate.py mutations/stats.json

Each mutation is applied to the source, the project is rebuilt, the unit suite
is run, and the script reports which tests noticed. The original file is
restored afterwards, including when the script fails partway.

Two sets do not work that way, and the script cannot run them:

* **`durability.json`** — faults that only appear when a process is killed.
  Apply one by hand and run `./build/ambar_crash_test <dir> run 10`. Running
  them through `mutate.py` would run the unit suite, which cannot see them.
* **`skiplist.json`** — data races. Apply one by hand to a ThreadSanitizer
  build and run that binary; a race is a report, not a failed assertion.

## Mutations expected to survive

A surviving mutation usually means a missing test. Sometimes it means the
change was not a change at all, and those are recorded here so the next person
does not go looking for a test that should not exist. Each is present in the
JSON files so it can be re-run, labelled where the file allows it.

**`merger.json`: breaking ties the other way in `find_smallest`.**
The merging iterator requires that no two children hold an equal key, and
internal keys guarantee it, so the tie-breaking branch is unreachable. See
`src/merger.hpp`.

**`durability.json`: writing the memtable before the log record.**
Visibility is conferred by `versions_->set_last_sequence`, which runs after
both. Until it does, the memtable entries carry sequence numbers above every
reader's snapshot and cannot be read. Swapping the two changes when a key is in
memory, not when it can be seen. See the comment at that line in
`src/db_impl.cpp`.

**`durability.json`, formerly: removing `log_->sync()` while keeping the
flush.** This one used to be listed here as expected to survive, because
`crash_test` cannot see it: `SIGKILL` leaves everything the process handed to
the kernel intact, so a database that never calls `fsync` survives a process
kill exactly as well as one that does. It is now the first entry of
`powercut.json`, which `tools/mutate.py` runs like any other set, and it is
caught -- by `tests/test_powercut.cpp`, which runs the engine on a disk that
loses what was not synced. The engine's other syncs are in that file too.

**`powercut.json`: skipping the directory sync after the `CURRENT` rename,
and skipping repair's sync of its own manifest.** Both are real syncs, and
both survive, for the same reason: names land in journal order, so a cut that
loses the rename of `CURRENT` -- or the bytes of repair's manifest -- leaves
the previous manifest in charge, and every file that manifest names is still
there, because the deletions that would have removed them come later in the
journal than the rename. The database opens on its previous state, the log
is replayed, nothing acknowledged is missing. They stay in the file so the
argument is re-run rather than remembered.

**`stats.json`: counting only the leader's batch — may survive on a loaded
machine.** The test that catches it writes from eight threads at once and
checks that every batch was counted, which the mutation breaks only when
two writers were queued together; whether they ever are depends on the
scheduler. On this machine they always are, by hundreds. A survival on a
machine where no group formed says nothing about the count, and the test
prints how many groups it saw so the two cases can be told apart.

## The other direction: failure injection

`tools/mutate.py` breaks the code. `tests/test_faults.cpp` breaks the *disk*
— it makes one I/O call of each kind fail, at each point in a workload where
one occurs, and checks the database reports it, still holds everything it
acknowledged, and opens again; `faults.json` then breaks each of the error
paths that makes that true, to show the sweep would notice.
`tools/fault_sweep.sh` is the older form of the same thing through the real
system calls, on Linux:

    ./tools/fault_sweep.sh build 3

That harness found a defect no test had: compaction used to delete its output
files when the manifest install failed, but the edit reaches the file before
the `fsync` that fails, so the manifest kept a record naming files that had
been deleted — and the database then refused to open, permanently, with all of
its data still on disk. 17 of 85 injection points produced that. The fix is to
retain the outputs; every injection point is clean now.
