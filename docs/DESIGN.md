# Ambar — design

A log-structured merge-tree storage engine. This document states what the engine
guarantees and how it gets there; the tests exist to falsify those claims.

## Why an LSM tree

A B-tree updates in place, so a random-write workload becomes random I/O. An LSM
tree turns every write into an append — first to a log, then to a sorted
in-memory table, and eventually to an immutable sorted file. Random writes become
sequential. The bill arrives on the read path (a key may live in any of several
files) and in background compaction, and most of the engine's complexity is
about paying it carefully.

## Keys carry a version

The single decision the rest of the design hangs from: the key stored on disk is
not the key the caller passed.

```
internal key := user_key | (sequence << 8) | value_type
                └ bytes ┘ └──────── 8 bytes, little-endian ────────┘

value_type := kDeletion (0) | kValue (1)
```

Ordered by user key ascending, then **sequence descending**. Three consequences,
none of them optional:

* **Recency is a property of the key, not of the file it happens to sit in.**
  When several tables contain the same user key, a merging iterator that simply
  takes the smallest internal key gets the newest version. No file-order
  bookkeeping, no way for compaction to accidentally resurrect an old value.
* **Deletion is a write.** A tombstone is an internal key with `kDeletion`,
  sorting ahead of every older version of that user key. It can only be dropped
  once compaction reaches the oldest table that could still hold something it
  shadows — anywhere earlier and the deleted value comes back.
* **Snapshots are free.** A snapshot is just a sequence number: a read at
  sequence *s* skips every internal key with a higher sequence. No copying, no
  locks held across the read.

Packing type into the low byte of the sequence word means the tombstone for a
key sorts immediately before that key's live versions, which is exactly the
order the merging iterator wants.

## The write path

```
put / delete / write(batch)
   │
   ├─1─ join the writer queue; the writer at the front acts for the group
   ├─2─ assign sequence numbers to the merged batch   (under the mutex)
   │
   │    ── the mutex is released here ──
   │
   ├─3─ append the batch to the write-ahead log       (userspace buffer)
   ├─4─ flush to the kernel;  fsync as well, if opts.sync
   ├─5─ insert into the mutable memtable
   │
   │    ── the mutex is taken again ──
   │
   └─6─ publish: advance last_sequence
```

Step 1 is where a full memtable is dealt with, not the end: `make_room_for_write`
runs before the sequence numbers are handed out, so the memtable that filled up
is made immutable at the head of the *next* write rather than the tail of the
one that filled it. It is also where a writer is delayed or blocked — see
**Backpressure** below.

**Step 6 is the publication point, and it is worth being exact about that**,
because the intuitive answer is step 5. A reader takes its snapshot from
`last_sequence`, and every entry step 5 put in the memtable carries a sequence
above the old value — so between steps 5 and 6 those entries exist and are
invisible to everyone.

The consequence is that the order of steps 3 and 5 cannot be observed. An
earlier version of this document claimed the opposite, in the usual words: that
write-ahead logging means the log record must precede the memtable insert or a
value becomes readable before it is durable. That is the right principle and
the wrong statement of it here, because visibility is not conferred by the
memtable. Swapping steps 3 and 5 was tried as a deliberate mutation and the
crash test passed, correctly — it is an equivalent change.

What is *not* equivalent is step 4, and that was a real bug rather than a
thought experiment. Without the flush, an unsynced record sits in this
process's own stdio buffer while step 6 makes it readable; a reader can then be
handed a value that a `SIGKILL` destroys. The crash test found it by running a
reader alongside the writer and journalling what the reader had been given.

A batch is written to the log as **one record**, so a crash cannot apply half of
it. Atomicity of a multi-key write is inherited from atomicity of a log record,
which in turn comes from its checksum: a torn record fails the check and is
discarded whole.

### Group commit

Writers queue rather than lock. The writer at the front of the queue merges the
batches behind it into one record and performs one log write and one fsync for
all of them, then reports each writer's result individually. Ten threads
committing at once therefore cost one device flush between them rather than
ten. The group is capped, and a writer that asked for `sync` is never merged
into a group that is not syncing — being told a write is durable when it is not
is worse than waiting.

## The read path

```
get(key, snapshot)
   │
   ├─ mutable memtable           newest
   ├─ immutable memtable (if a flush is in flight)
   ├─ level 0 tables, newest first     ← these overlap each other
   └─ levels 1..N, binary search       ← each level is disjoint, so at most
                                          one table per level can match
```

The search stops at the first internal key matching the user key at or below the
snapshot sequence — including when that key is a tombstone, which reports "not
found" rather than continuing to an older live value.

A table can carry a Bloom filter, consulted before any block is read. It is
opt-in: `Options::filter_policy` defaults to null, and with it null no filter
block is written and every absent-key lookup reads a block per level. A filter
that says "absent" is always right; one that says "present" is right most of the
time. On a workload of misses this turns most levels into a memory probe:
measured in `tests/test_table.cpp`, a thousand lookups for keys that are not in
a table read thirteen blocks with a filter and a thousand without one.

The filter summarises **user keys, not the internal keys the table stores**, and
this is not a detail. A filter built over internal keys knows about `key_42 at
sequence 1017`; a lookup asks about `key_42 as of the newest snapshot`, which is
a different internal key that was never inserted. The filter answers correctly
for the question it was asked, the table skips the block, and the key is
reported missing. Every symptom points away from the cause: the file is intact,
the checksums pass, the index is right, iteration returns every key — only point
lookups fail, and only when a filter is configured. `src/internal_filter_policy.hpp`
carries the wrapper and the story; `tests/test_table.cpp` pins the failure so
that removing the wrapper turns the suite red instead of losing lookups.

### Two comparators

Internal keys are ordered by user key ascending, then by the trailing eight
bytes descending. User keys are ordered bytewise. The engine holds both, and
using the wrong one is a bug with no symptom at the point of the mistake:
`compare_internal_keys` strips the last eight bytes as a trailer, so handing it
a bare user key silently compares a truncated key. That produced overlapping
files in a level that is required to be disjoint, and it was caught by an
assertion in the version builder rather than by a failing read.

### Block cache

Decoded blocks are held in a bounded, sharded LRU cache. Without one, every key
read costs a block read and a parse, and a scan pays that per key rather than
per block.

Entries are reference counted rather than simply evicted, because a reader may
be walking a block when the cache decides to drop it: eviction removes the entry
from the table and the last user frees it. The cache is split into shards chosen
by the key's hash, so a thread only contends with others that hashed the same
way.

Random-read throughput is bound by how much of the working set is resident, and
`tools/bench` reports it as a curve rather than a number for that reason — the
same engine reads at half SQLite's rate with a 1 MB cache and twice its rate
with the whole database resident.

## Durability contract

> If `write()` returned ok **and** `WriteOptions::sync` was set, then after any
> crash at any instant, the next `open()` sees that batch — all of it.

Without `sync`, the promise weakens to a prefix: recovery yields some prefix of
the acknowledged batches, each applied entirely or not at all. Never a partial
batch, never an interleaving, never half a value.

### What the crash test can and cannot check

`tools/crash_test` kills the writer with `SIGKILL` at random instants and
verifies the contract above against a journal of what was acknowledged and what
was read. It is the right tool for ordering and recovery faults, and it found a
real one (step 4 above).

It cannot check `fsync`. `SIGKILL` destroys the process and leaves everything
the process had handed to the kernel intact, so a database that never calls
`fsync` survives a process kill exactly as well as one that does. Verifying the
`sync` half of the contract needs the storage itself to lose writes — a
`dm-flakey` target, or a virtual machine snapshot taken mid-write — and neither
is done here. **The `sync=true` guarantee therefore rests on the `fsync` calls
being correct by inspection, not by test.** `mutations/README.md` records that
explicitly, so that a green crash-test run is not mistaken for evidence it does
not provide.

### After an I/O error

When a manifest write fails, the engine stops: every subsequent write returns
that error, and cleanup does not run. Two consequences a reader should know
about.

Compaction outputs are **not** deleted when their install fails. The edit
reaches the manifest file before the `fsync` that fails, so a record naming
those files may already be durable; deleting them would leave a manifest that
permanently references a table that no longer exists, and the database would
refuse to open with all of its data still on disk. `tools/fault_sweep.sh` found
exactly that — 17 of 85 injection points produced a permanently unopenable
database — and the fix is to keep the files.

The cost is that after an I/O error the directory may hold unreferenced files
that are not reclaimed until a clean restart. That is the right side to err on:
`remove_obsolete_files` declines to run at all while a background error is
outstanding, because with the manifest in an unknown state a file that looks
unreferenced may be the only copy of data a reopen will name.

Two further things the engine does not promise, stated rather than glossed:

* It cannot survive a lying disk. If `fsync` reports success without the data
  reaching stable storage, no user-space program can help. (On macOS this is why
  `F_FULLFSYNC` is used: plain `fsync` there returns once the drive *cache* has
  the data.)
* On Windows the containing directory is not fsynced after a file is created,
  because the platform offers no handle for it. A crash in that window can lose
  a newly created table's directory entry. The manifest records the table, so
  recovery detects the absence and reports corruption rather than silently
  serving a database with a hole in it.

## One process at a time

A database directory may be open in exactly one process. `DB::open` takes an
exclusive lock on `LOCK` and returns `kIoError` if another process holds it;
`destroy_db` takes the same lock.

This is a refusal rather than a warning because the alternative is not
degradation, it is destruction. Two processes both replay each other's
in-flight logs, both hand out the same file numbers, both append to the same
manifest, and each one's cleanup deletes files the other is using — with no
error reported to either. Measured on this engine before the lock existed: two
processes writing 6,000 keys each destroyed 11,032 of the 12,000 acknowledged
writes between them, and both reported success throughout. The test that keeps
it that way is `db.a_second_process_cannot_open_the_same_database`, which forks
a real child, because a POSIX record lock does not conflict with itself inside
one process.

What it does **not** do, stated because the mechanism cannot:

* It does not stop a second `DB` object inside the same process. POSIX record
  locks are held per process, so the second acquire succeeds.
* On POSIX the lock is released by closing *any* descriptor for the file, so
  nothing else may open `LOCK`.
* It is advisory. A program that ignores it and writes into the directory
  directly is not stopped by anything here.

## Concurrency

One mutex guards mutable state: the memtable pointers, the sequence counter, and
the version list. It is **not** held during I/O.

Readers do not block writers and writers do not block readers, because the set of
live files is an immutable, reference-counted `Version`. A reader takes a
reference under the mutex, releases the mutex, and then reads for as long as it
likes; compaction meanwhile installs a *new* Version. The old one is destroyed
when its last reader leaves, and only then are the files it alone referenced
deleted. This is the same reason a compaction can never pull a file out from
under an in-flight scan.

The memtable is a skip list with atomic forward pointers: insertion publishes a
node with a release store, traversal reads with acquire loads, so a concurrent
reader either sees a complete node or does not see it at all. It admits one
writer at a time (the write mutex ensures that) and any number of readers with no
locking on the read side.

## On-disk layout

```
<dir>/
  LOCK                held open by whichever process has the database open
  CURRENT             one line: the name of the live manifest
  MANIFEST-000007     the version-edit log: which tables exist at which level
  000012.log          write-ahead log for the mutable memtable
  000009.sst          immutable sorted table
```

`LOCK` is never deleted by `destroy_db` while it is held, and the file itself
survives after the lock is released: removing it would let a second process
create and lock a *new* file of the same name while a third still held the old
one.

An SSTable is a sequence of blocks:

```
[data block]*        sorted key/value pairs, prefix-compressed with restart
                     points every 16 keys so a block is binary-searchable
[filter block]       the Bloom filters, if a filter policy is configured
[metaindex block]    names to handles: "filter.<policy>" -> the filter block
[index block]        one entry per data block: its last key and its location
[footer]             fixed 48 bytes: metaindex handle, index handle, magic
```

The footer points at the metaindex and the index; the filter is reached through
the metaindex, which is what lets a later version add a block kind without
moving anything the footer names. Opening a table therefore reads the footer,
the metaindex, the filter and the index — four reads, of which the index and
the filter are kept for the life of the open table.

Reading a key touches the index block (held for the life of the open table), the
filter (likewise), and at most one data block (from the block cache, or from
disk and then into it). Prefix compression exploits the fact that adjacent keys in a
sorted file usually share a prefix; restart points bound how much has to be
decoded to answer a query, trading a little space for the ability to binary
search inside a block instead of scanning it.

The manifest is a log of *edits* rather than a snapshot, so installing a
compaction result is one small append plus one fsync, not a rewrite of the whole
file set.

## Compaction

Levelled, RocksDB/LevelDB style.

* **Level 0** holds freshly flushed memtables, so its files overlap each other
  and a read must check all of them. It is kept small for that reason.
* **Levels 1 and below** are disjoint: within a level, key ranges do not overlap,
  so a read binary-searches to at most one file. Each level holds roughly ten
  times the previous one.

A compaction takes the files of level *n* whose ranges overlap a chosen file, and
the overlapping files of level *n+1*, merges them, and writes the result to
*n+1*. Because inputs are sorted, the merge is a linear pass.

**Two things start one, not one.** The obvious trigger is size: a level holding
more than its share scores above one and is compacted. The other is *seeks*: a
file that repeatedly gets searched and repeatedly does not have the key is
costing every one of those reads, so each miss is charged against it and enough
of them schedule its compaction regardless of size. A reader trying to work out
when this engine does I/O needs both; the size trigger alone explains about half
of it.

The alternative — size-tiered compaction, which merges same-sized files within a
level — is cheaper to write and cheaper on writes, but leaves overlapping files
everywhere and pays for it on every read. Levelled is chosen here because reads
are the harder promise to keep.

The cost of that choice is write amplification, and **this project does not
measure it.** `tools/bench` reports space amplification — bytes on disk against
bytes of user data — which is a different and much easier quantity.
`docs/BENCHMARKS.md` says so under *What is not measured* rather than letting
one word stand in for the other.

## Backpressure

A write is not always allowed to proceed at full speed. When level 0 reaches
eight files each write sleeps for a millisecond, once; at twelve, writers block
until compaction catches up.

The delay is deliberate and gradual. Waiting until the hard limit and then
blocking for seconds turns a gentle backlog into a latency spike the
application experiences as a hang, whereas handing back a millisecond per write
early lets compaction catch up while the application keeps making progress. It
is visible in the benchmark as a p99.9 of about a millisecond against a median
of two microseconds — which is why those numbers are reported as percentiles.

## Untrusted files

A file is input, and that is true of the engine's own files. A disk flips bits,
a backup truncates, and a directory can be handed over by somebody who built it
by hand. The rule is that damage produces a `kCorruption` status: never a
crash, never a read outside a buffer, never an allocation of whatever size the
file happened to ask for.

Concretely, and each of these was a defect before it was a rule:

* Every block handle is bounded by the size of the file it came from, not by a
  constant. A handle claiming a gigabyte in a forty-eight byte file used to
  allocate the gigabyte before discovering the file was short.
* Entry lengths are widened before they are added. Two `uint32` lengths summed
  as `uint32` wrap: an entry declaring a key of `0xffffffff` bytes and a value
  of one summed to zero, passed a 32-bit bounds check, and copied four
  gigabytes out of a fifteen-byte block.
* Every key entering from a file is checked against `Comparator::min_key_length`
  before any comparison. `compare_internal_keys` reads an eight-byte trailer
  from the end of a key, so a three-byte key made it read five bytes *before*
  the key — invisibly, because the size arithmetic underflowed and `substr`
  clamped.
* Restart offsets are range-checked before they become pointers, the filter
  block's region-size byte is rejected beyond 63 (a shift by 200 is undefined,
  not merely wrong), the manifest rejects internal keys shorter than their
  trailer, and file numbers that overflow 64 bits are not parsed.

`tests/test_corrupt.cpp` covers these, and also opens 132 randomly damaged
copies of a real database — bit flips, truncations, runs of garbage — under
AddressSanitizer, requiring only that each one produces a status rather than a
crash.

A status rather than a crash is the floor. Two more rules sit above it, both
about damage to the files that *describe* the database rather than the ones
that hold it, because there the wrong answer is not a crash but a quiet loss:

* **A lost `CURRENT` is refused, not created over.** With `create_if_missing`,
  a directory with no `CURRENT` used to read as empty and get a fresh manifest
  — after which the cleanup that follows every open deleted each table the
  fresh manifest did not name. A directory holding table or log files but no
  `CURRENT` is now a database that lost its manifest, and open says so. Only
  a manifest with nothing pointing at it *and* nothing beside it — the shape
  `new_db` leaves when it dies before writing `CURRENT` — is still created
  over.
* **Damage in the middle of a log-format file is told apart from a torn
  tail.** Both stop the reader at the same place. A torn tail is the record
  being written when the process died, with nothing after it, and recovery is
  right to treat everything before it as the whole file. Damage in the middle
  has intact records after it, whose contents did become durable; treating
  the stop as the end serves a state the file had moved past — for the
  manifest, an older version of the database, after which cleanup deleted
  every table the newer records named. `LogReader` now looks past a failure
  for a record it can verify, and `damaged()` says whether it found one; both
  the manifest reader and log replay refuse in that case, with every file left
  in place.

What this does *not* claim: the engine is not hardened against an adversary who
can also choose when to interrupt it, and a file that passes every check can
still contain wrong data. The checksum tells you a block is the block that was
written; nothing tells you the block was right when it was written.

## What the tests are for

* `tests/` — unit tests per component, plus model-based tests that drive the
  engine and a `std::map` through tens of thousands of random operations and
  compare every key afterwards, and again after closing and reopening.
* `tests/test_corrupt.cpp` — damaged and hostile files. See *Untrusted files*
  below.
* `tools/crash_test` — runs a writer in a child process, kills it with `SIGKILL`
  at a random instant, reopens the database, and checks the durability contract
  above. Ordering bugs are invisible to ordinary tests and obvious to this one.
* `tools/bench` — throughput and latency for sequential and random workloads,
  with and without `sync`, reported as percentiles because an average hides
  what compaction does to the tail. It measures **space** amplification —
  bytes on disk against bytes of user data — and says so; write amplification
  would need counting at the point each file is written, which the engine does
  not yet expose. Compared against SQLite in WAL mode where it is available.
* `tools/fault_sweep.sh` with `tools/fault_inject.c` — makes one `fsync` or
  `rename` return `EIO`, at each point in a workload where one occurs, and
  checks the database still opens and still holds what it acknowledged. A crash
  test lands somewhere random; this visits the rare instant deliberately. It
  found the compaction-output rule described under *Durability* above.
* `tools/mutate.py` with `mutations/` — breaks the code on purpose, one defect
  at a time, and reports which tests notice. A green suite says the tests pass;
  this says they would fail if the code were wrong, which is a different claim.
  Mutations that survive because they are genuinely equivalent are listed in
  `mutations/README.md` rather than chased.

## What is not implemented

Stated so that the absence is a decision rather than an omission a reader has
to discover.

* **Compression.** The block trailer reserves a type byte and the reader
  refuses a type it cannot handle, so adding it later does not change the
  format for existing files. Nothing compresses today.
* **A repair tool.** A database whose manifest is lost cannot currently be
  rebuilt from the table files that survive, though the information to do it is
  present in them. This matters more than it would in an engine that never
  refuses to open: recovery reports corruption when the manifest names a table
  that is missing, and there is nothing to run afterwards. What the engine does
  guarantee is that it will not make things worse: a directory that holds
  tables or logs but no `CURRENT` is refused, even with `create_if_missing`,
  rather than treated as empty and created over -- which would have let the
  cleanup after the open delete every table the new manifest did not name.
* **Parallel compaction.** One background thread. Compaction is IO bound and
  its inputs and outputs are ordered with respect to each other, so a second
  thread would mostly contend for the same lock; doing it properly needs
  per-level scheduling.
* **Column families, transactions, merge operators.** Out of scope.
