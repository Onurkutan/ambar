# Mutations

Each file here is a set of deliberate defects, applied one at a time, to check
that the tests would fail if the code were wrong. A green suite says the tests
pass; it does not say they would notice a bug, and those are different claims.

    python3 tools/mutate.py mutations/merger.json
    python3 tools/mutate.py mutations/bloom.json
    python3 tools/mutate.py mutations/write_batch.json
    python3 tools/mutate.py mutations/recovery.json

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
does not go looking for a test that should not exist. All three are present in
the JSON files so they can be re-run, labelled where the file allows it.

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

**`durability.json`: removing `log_->sync()` while keeping the flush.**
Not equivalent — it is a real durability fault — but `crash_test` cannot see
it, and no number of extra rounds will change that. `SIGKILL` destroys the
process and leaves everything it handed to the kernel intact, so a database
that never calls `fsync` survives a process kill exactly as well as one that
does. Catching it needs the storage itself to lose writes: a `dm-flakey`
target, or a virtual machine snapshot taken mid-write. `docs/DESIGN.md` records
which claims rest on that and are therefore untested here.

## The other direction: failure injection

`tools/mutate.py` breaks the code. `tools/fault_sweep.sh` breaks the *machine*
— it makes one `fsync` or `rename` return `EIO`, at each point in a workload
where one occurs, and checks the database still opens and still holds
everything it acknowledged:

    ./tools/fault_sweep.sh build 3

That harness found a defect no test had: compaction used to delete its output
files when the manifest install failed, but the edit reaches the file before
the `fsync` that fails, so the manifest kept a record naming files that had
been deleted — and the database then refused to open, permanently, with all of
its data still on disk. 17 of 85 injection points produced that. The fix is to
retain the outputs; every injection point is clean now.
