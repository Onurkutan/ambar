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

What that is worth is measured rather than asserted. The engine counts the
records it appends to the log and the batches they carried, and reports both
through `get_property("ambar.log-writes")`; `tools/bench` runs synced writes
from one to eight threads and prints the rate beside the batches each
`fsync` served. On this machine eight threads write at 4.7 times one thread's
rate and each `fsync` carries 4.8 batches, and the two figures moving
together is what says the throughput is the grouping and nothing else.
`tests/test_stats.cpp` holds the count from both ends: written one at a
time the two numbers are equal, and from eight threads at once every batch
is counted exactly once.

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

What a lookup costs the disk is counted rather than inferred. Every table
file is opened in `TableCache`, which hands it to the table through a wrapper
that counts each data block read the block cache did not answer, and the
footer and index read when the table is opened — and the metaindex and
filter, when a filter is configured — with the bytes they brought in, on any
thread and under no lock,
and reports both through `get_property("ambar.table-reads")`. `tools/bench`
divides them by the lookups that caused them and calls the quotient read
amplification; `docs/BENCHMARKS.md` has it at each cache size, and
`tests/test_stats.cpp` requires the engine's count to equal the simulated
disk's own count of the reads it served.

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
same engine reads at less than half SQLite's rate with a 1 MB cache and
about 1.3 times it with the whole database resident, and the table reads per
lookup at each cache size, which `docs/BENCHMARKS.md` reports beside it, say
why.

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
`sync` half of the contract needs the storage itself to lose writes, and that
is what the next section is for.

### The power cut

Everything the engine does to the disk goes through `src/file.hpp`, and the
operating system behind it can be replaced — by a test, between databases —
with `tests/sim_file_system.hpp`: a disk that keeps what `fsync` covered and,
at a chosen instant, loses a random amount of the rest.
`tests/test_powercut.cpp` runs the engine on it, cuts the power a few hundred
times per platform, reboots, and compares the database with a model of what
was acknowledged. Every batch
acknowledged with `sync` must be there; what comes back must be a prefix of the
acknowledged batches, each whole, in order; and nothing may be there that
nobody wrote. `tools/powercut` runs the same for as long as asked.

The disk's model is written out in its header so a green run can be judged.
Briefly: a file is a synced prefix and an unsynced tail, and a cut keeps a
random length of the tail, independently per file. The size may run ahead of
the data, leaving pages that read as zeros (what ext4 and xfs leave today) or
as garbage (what their older writeback modes left). Names — creating,
renaming, deleting a file — land in journal order, committed by a directory
sync; or,
on the more forgiving setting, also by a sync of the file they name, which is
what every filesystem in use does and POSIX does not promise. Nothing is
modelled that would make the disk kinder than any of those, and the tests
first check the disk itself: that unsynced bytes are lost sometimes and synced
ones never, that a file whose name was never synced can vanish, that the synced
prefix survives whatever the tail does.

The cuts landed in the first minute, before any mutation was tried:

* **The prefix promise was broken at every log rotation.** When the memtable
  filled, the old log was closed and a new one opened, and nothing synced the
  old one until its memtable had been written out as a table. A `sync` write
  into the new log made *that* file durable and nothing else, so a power cut in
  the window left the new log's batches on disk and the old log's unsynced
  tail gone: later writes present, earlier ones missing, which is not a prefix.
  A process kill cannot show this, because the kernel keeps both tails. The
  old log is now synced before the new one takes over, once per
  `write_buffer_size` of writes and never on the path of a single one.
* **A new log's name was never synced.** The first synced write into a log
  created a moment earlier was durable in a file whose directory entry was not.
  On Linux filesystems `fsync` of a new file happens to carry its name; POSIX
  says nothing of the kind, and the stricter setting of the simulator lost the
  batch. The directory is now synced after every table is created and after
  every log a rotation creates, before anything depends on the name; the
  log an open creates is covered by the directory sync that installs the
  fresh manifest, which comes before any write.

The mutations in `mutations/powercut.json` remove the engine's syncs one at a
time — the log's, a table's, a compaction output's, the manifest's,
`CURRENT`'s, the two above — and `tools/mutate.py` reports every one caught,
which is the sentence the previous version of this section could not write. Two syncs are
listed as expected to survive, with the reason: repair's sync of its own
manifest, and the directory sync after the `CURRENT` rename, which the
journal's own ordering makes harmless — a cut there leaves the previous
manifest in charge of files that are all still present.

Three things the simulation does not do, stated rather than glossed. It does
not exercise the platform: whether `fsync` on macOS reaches the platter
(`F_FULLFSYNC` is used, for files and directories), whether Windows flushes a
directory entry (the call is undocumented and attempted anyway, its failure
not reported), whether a disk lies. It runs one writer, so group commit never
merges two batches into one record; `tools/crash_test` covers that against a
process kill and the code is the same. And its garbage is random bytes, not a
deleted file's: a writeback-mode filesystem can extend a new log over blocks
that still hold intact records of the log it replaced, and the log format
carries nothing that would tell those from new ones. That is the one state
that could make the engine replay old writes as new rather than refuse, it is
what RocksDB's recycled-log record header exists for, and it is not modelled.

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

That sweep failed one kind of call at a time, on Linux, through `LD_PRELOAD`.
`tests/test_faults.cpp` fails every kind the engine makes — append, flush,
sync, close, create, rename, remove, directory sync — at every point of a
workload, on the simulated disk, on every platform, and then cuts the power
as well. The disk's model of a failed `fsync` is ext4's: the error is
reported once per open file, the pages that could not be written are marked
clean and stay readable, and no later `fsync` writes them, so the bytes that
were unsynced when the call failed can never become durable. Two more things
came out of it, on the first run:

* **The manifest was reused across opens.** A small manifest used to be
  appended to at the next open rather than replaced. After an `fsync` of it
  had failed, the record whose sync failed was in the file the next process
  read — the page cache had it — and would never be on the disk; appending
  after it made a hole that nothing could see until the power went, and then
  the manifest read as damaged in the middle. Every open now writes a fresh
  manifest from a snapshot, into fresh pages the process syncs itself, and
  leaves the old one for cleanup. One snapshot per open is the cost.
* **One failed directory sync made the database unopenable.** `CURRENT` is
  replaced by rename and then the directory is synced. When the sync failed,
  the rename had already happened, and the caller's failure path removed the
  new manifest — the one `CURRENT` now named. The failure path now looks at
  `CURRENT` before removing anything.

`mutations/faults.json` breaks each of the error paths in turn — deleting
compaction outputs on a failed install, removing the manifest `CURRENT`
names, carrying on after a failed log write, carrying on after a failed sync
at rotation, cleaning up with an error outstanding — and every one is
caught. The manifest reuse cannot be put back by a mutation, since the code
that did it is gone.

Two further things the engine does not promise, stated rather than glossed:

* It cannot survive a lying disk. If `fsync` reports success without the data
  reaching stable storage, no user-space program can help. (On macOS this is why
  `F_FULLFSYNC` is used: plain `fsync` there returns once the drive *cache* has
  the data.)
* On Windows the platform documents no way to flush a directory's entries.
  `FlushFileBuffers` on a directory handle succeeds on a local NTFS volume and
  is attempted, but its failure is not reported, so a power cut on a share can
  still lose a newly created file's directory entry. For a table the manifest
  records the file, so recovery detects the absence and reports corruption
  rather than silently serving a database with a hole in it. For a log there
  is no such record: a synced batch written into a log created moments
  before the cut would be gone without a report. That is the case the
  simulator's stricter setting is for, and on a share it is not covered.

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

"Any number of readers" is a promise about correctness, and what it is worth
in throughput is measured rather than assumed: `tools/bench` runs random
lookups on one, two, four and eight threads, once with the cache it was given
and once with the whole database resident, so that no lookup touches the disk
and the engine's own locks are all that is left to contend for. The first
measurement found the promise worth 2.8× on eight threads with everything in
memory — against 4.9× when the disk was in the path and hid the locks. Two
locks were on the read path that did not need to be, and each was found by
taking it out and measuring again. The table cache had one lock over every
open table, taken twice per lookup, to find the table and to let go of it;
it is sharded now, sixteen ways by file number, which took eight threads
from 1.08 to 1.19 million lookups a second. The database mutex was taken a
second time on the way out of every lookup, to drop its references and to
charge a seek against the file searched in vain; that acquisition alone was
worth the rest of the way to 1.33 million. A lookup now leaves without it:
the memtables' counts were already atomic, the seek charge is only there
when a lookup had to consult a second file — never, once the data sits in
one level; on most lookups while level 0 is deep — and the version's count
is atomic with one rule kept from the old design. Dropping the last reference
unlinks the version from the set's list and releases its files, and no atomic
counter makes that safe, so a lookup drops its reference without the mutex
only while it is not the last one, and takes the mutex to drop the last: the
count reaches zero, and the destructor runs, under the mutex on every path.
A version that is current holds a reference of its own, so a lookup's is
never the last while the version is current; it is the last only for a
version compaction has already replaced, and then the lookup pays for the
lock once, as it always did. The first cut of this dropped the reference
first and took the mutex afterwards to check whether the count was still
zero, and tracing that protocol before it was committed found a window it
admitted: between the two steps, a thread holding the mutex could take a
reference and drop it, deleting the version under the reader about to read
its count. No path in this engine can take a reference to a version that is
no longer current, so the window was not reachable — but a protocol that is
sound only by that invariant would break the day someone walks the version
list and takes one, and the form above does not depend on it. The rest of
the gap is the block cache and the memtable, and `docs/BENCHMARKS.md` says
what each of the three numbers was.

The memtable is a skip list with atomic forward pointers: insertion publishes a
node with a release store, traversal reads with acquire loads, so a concurrent
reader either sees a complete node or does not see it at all. It admits one
writer at a time (the write mutex ensures that) and any number of readers with no
locking on the read side.

Everything that rotates the log goes through the writer queue, including
`compact_range`, which flushes the memtable as a write with nothing in it.
The rotation releases the mutex around two syncs (see *The power cut* above)
on the strength of being the only writer, and only the front of the queue is
that; `compact_range` used to rotate under the mutex alone, which was a race
against a writer using the log with the mutex released, and with the syncs
would have been two rotations at once. Found by review of that change.

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

### Format stability

The layout above is the format of version 0.1.0, and `ambar::kVersion` in
the public header says which version a build is. Before 1.0 the format may
change between minor versions, and a database written by one is not promised
to open under another: a table's footer carries a magic number that says
what kind of file it is, and nothing on disk says which version wrote it, so
an older build reading a newer file would fail a checksum or a bounds check
rather than a version check, and report damage. A change to the format is a
change to the minor version, and this section will say what changed and from
which version. Nothing has changed yet.

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

The cost of that choice is write amplification: the bytes the engine writes
for each byte it is handed. The engine counts them itself, at the point each
file is written — a log record as it is appended, a table as its builder
finishes, a manifest edit as it lands — and reports the totals through
`get_property("ambar.bytes-written")`, by kind. The counters themselves move
under the database mutex once each write has finished, so a reader never
sees one in motion. Counting there rather than by watching the directory is
what makes the number honest: a file written and
deleted between two looks at the directory leaves no trace, and compaction
does exactly that all day. `tools/bench` divides the total by the bytes it
handed in, overwrites included, and prints it beside the space amplification
that used to stand in for it — a different and much easier quantity, which
`docs/BENCHMARKS.md` keeps apart. The count is checked, not trusted:
`tests/test_stats.cpp` runs the engine on the simulated disk and requires the
engine's figure for each kind of file it counts to equal the disk's own
tally of what
was appended, and `mutations/stats.json` removes each writer from the count in
turn to show the check would notice.

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

## Repair

A manifest is a log of edits that, replayed, says which table sits at which
level covering which keys. Everything it says can also be found out the slow
way: a table carries its own keys, so its range and its newest sequence number
are a matter of reading it. `repair_db` -- `ambar_repair <dir>` -- does that
for a database `open()` refuses: `CURRENT` lost, a manifest damaged in the
middle, a table the manifest names that is not there.

It takes the lock, reads every table from beginning to end, and keeps each one
that reads back in order. It replays every log into tables the way recovery
would, as far as each can be read, checking each batch -- that it parses, that
its sequence numbers follow the last batch's and fit -- before applying it.
Then it merges what it kept: every user key once, at its newest version, in
order, into tables that do not overlap, in rounds of at most 256 inputs so
that a large database does not need more open files than a process is
allowed. It reads the merged tables back, writes a fresh manifest naming them,
points `CURRENT` at it, and finally opens the database, which is what says the
repair worked. Nothing is deleted by repair itself: a file that would not read
is moved to `<dir>/lost/`, the old manifests are moved there too as the record
of what went wrong, and the files the merge replaced are left for that last
open's cleanup, which removes only what the new manifest does not name. A log
that stopped early -- a torn tail, or damage -- is replayed to the stop and
then moved to `lost/`, with a note that says whether intact records followed
the stop, so the bytes past it are still there for a person.

Nothing is moved until the new manifest is in place. The first draft moved
each log into `lost/` as soon as it had been converted, and a power cut between
that and the new `CURRENT` would have left the old manifest in charge of a
directory the log had already left -- which opens, since the manifest knows
nothing of the log, and whose cleanup then deletes the tables the log had
become. The batches in that log, synced ones included, would have been gone
without a word. It was found by review of the power-cut simulation's checks,
before the sweep that now cuts the power at every point of a repair could find
it.

Merging is the part that matters, and the first draft did not do it. The
obvious repair names the surviving tables in a manifest at level 0 and lets
compaction sort them out. Level 0 is where files may overlap, and a lookup
there resolves overlap by file number, newest first -- which stands in for age
only among files that were all flushed from memtables. A compaction output
carries a newer number than a flush that came before it and older versions of
the same keys, so after that repair `get()` answered with the overwritten
value while a scan over the same files answered correctly. The survivors can
also hold the same entry twice, a compaction output beside the input it was
made from, which the engine's own merge is entitled to assume never happens.
A merge in repair that tolerates both -- equal keys collapse, the first version
of a user key is the newest one -- is a few dozen lines; the alternative was a
precondition weakened everywhere.

The merged tables go to the last level. Nothing lies beneath it, so a
deletion whose version is the newest can be dropped rather than carried; and
the last level is never scored for compaction, so the first open after a
repair does not set off a rewrite of everything repair just wrote. New
writes land at level 0 as always and are pushed down as always.

What repair cannot restore is the history that produced the files, and two
things follow from that, both rare:

* **A dropped tombstone.** A compaction at the bottom level drops a tombstone
  once nothing older can exist. If a stale input from before that compaction
  survived -- the manifest went before the cleanup did -- repair keeps it, and
  the value it holds for that key is no longer shadowed. The key comes back.
* **A set-aside table's deletions.** A table that would not read takes its
  tombstones with it as well as its values, so a key it deleted may reappear
  from an older table.

The sequence number matters more than either. Recovery numbers new writes from
the manifest's `last_sequence`, and a write numbered below an entry that
already exists loses to it in every merge; so repair reads every entry of
every table rather than trusting anything, refuses a log whose batch headers
claim a sequence that does not follow or does not fit, and
`tests/test_repair.cpp` checks that a write made after a repair reads back --
and, with two tables built by hand so that the lower-numbered one holds the
newer versions, that repair takes the newest version of a key regardless of
which file it sits in.

## What the tests are for

* `tests/` — unit tests per component, plus model-based tests that drive the
  engine and a `std::map` through tens of thousands of random operations and
  compare every key afterwards, and again after closing and reopening.
* `tests/test_corrupt.cpp` — damaged and hostile files. See *Untrusted files*
  below.
* `tools/crash_test` — runs a writer in a child process, kills it with `SIGKILL`
  at a random instant, reopens the database, and checks the durability contract
  above. Ordering bugs are invisible to ordinary tests and obvious to this one.
* `tests/test_powercut.cpp` and `tools/powercut` — the same contract against a
  disk that loses unsynced writes, in this process, on every platform. Missing
  `fsync`s are invisible to the crash test and obvious to this one; it found
  two, described under *Durability* above.
* `tests/test_faults.cpp` — one I/O call of each kind failing at every point
  of a workload on the same disk, then the power cut as well. Found the two
  described under *After an I/O error* above.
* `tests/test_stats.cpp` — the engine's counts of what it wrote and of the
  table reads it made, against the simulated disk's counts of what was
  appended and what was served. See *Compaction* and *The read path* above
  for why the counts are the engine's to keep.
* `tools/bench` — throughput and latency for sequential and random workloads,
  with and without `sync`, reported as percentiles because an average hides
  what compaction does to the tail. It reports three amplifications and
  keeps them apart: **write**, from the engine's own count of the bytes it
  wrote against the bytes it was handed; **read**, from its count of the
  table blocks the cache did not answer against the lookups that caused
  them; and **space**, the settled size on disk against the distinct data it
  holds. And both kinds of scaling: random reads on one to eight threads,
  with the database on disk and with it resident, which is what found the
  two locks described under *Concurrency* above; and synced writes on one
  to eight threads beside the batches each `fsync` carried, which is what
  *Group commit* above promises. Compared against SQLite in WAL mode where
  it is available.
* `tools/fault_sweep.sh` with `tools/fault_inject.c` — makes one `fsync` or
  `rename` return `EIO`, at each point in a workload where one occurs, through
  the real system calls, and checks the database still opens and still holds
  what it acknowledged. It found the compaction-output rule described under
  *Durability* above; `tests/test_faults.cpp` does the same for every kind of
  call, and this is kept for the real syscall path.
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
* **Parallel compaction.** One background thread. Compaction is IO bound and
  its inputs and outputs are ordered with respect to each other, so a second
  thread would mostly contend for the same lock; doing it properly needs
  per-level scheduling.
* **Column families, transactions, merge operators.** Out of scope.
