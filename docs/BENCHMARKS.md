# Benchmarks

Produced by `tools/bench`, which is written to be read rather than quoted: it
reports percentiles rather than averages, states what it is not measuring, and
compares against SQLite because a number with nothing beside it is not a
measurement.

## Conditions

Except where noted, one machine, one filesystem — a Linux container on an
overlay filesystem, a
`CMAKE_BUILD_TYPE=Release` build (which is `-O3 -DNDEBUG`), one million keys of
the form `key_0000000042` with 100-byte values, about 109 MB of user data. An
LSM tree's write path is dominated by how the storage handles `fsync`, which
varies by an order of magnitude between a laptop SSD, a cloud volume and a
container's overlay filesystem. **Treat the shape of the results as the finding
and the absolute numbers as local.**

Every figure below is the median of three runs, and the three agreed within a
few percent except where noted.

Every section before *Compression* was measured with block compression off,
which was the default before 0.3.0, and `--compression none` reproduces it.
Compression is on by default now, and *Compression* has both.

SQLite is configured as a user running a key-value workload would configure it:
WAL journalling, `synchronous=NORMAL`, one transaction per thousand rows. Its
defaults would have been a comparison against a configuration nobody uses.

Reproduce with:

    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release
    ./build-release/ambar_bench /tmp/bench --keys 1000000 --value-size 100 --compression none

Benchmarking a Debug build measures the assertions, not the engine — it runs
about three times slower. If a number here looks unreachable, check the build
type first.

## Writes

| | ambar | sqlite |
|---|---|---|
| sequential, no sync | **385,000 op/s** (p50 1.7 µs, p99 5.4 µs) | 325,000 op/s (batched 1000) |
| random, no sync | 140,000 op/s (p50 2.4 µs, p99.9 1.1 ms) | — |
| sequential, `sync=true` | 4,200 op/s (p50 206 µs, p99 429 µs) | — |

Sequential writes are about 1.2× SQLite's rate, which is a modest win and not
the headline the shape of an LSM tree might suggest. The comparison is not
quite like for like in either direction: this engine performs a `write` syscall
per batch, while SQLite amortises one transaction across a thousand rows.

The p99.9 on random writes is the figure worth having, and it is why these are
percentiles. At 1.1 milliseconds against a median of 2.4 microseconds, it is
not noise — it is compaction backpressure, the engine deliberately delaying
writers while level 0 is full. An average would have reported about 7 µs and
hidden it entirely.

The `sync` row measures this filesystem's `fsync` more than it measures this
engine. It is here because leaving it out would let the unsynced numbers stand
as "the write performance", which they are not if durability against power loss
is wanted.

## Reads

Random reads turned out to be bound by how much of the database is resident, so
they are reported as a curve. A single number would have been misleading in
both directions.

| block cache | reads/s | p50 | full scan |
|---|---|---|---|
| 1 MB (1 % of the data) | 62,000 | 16.6 µs | 2.2 M/s |
| 8 MB (7 %) — the default | 61,000 | 16.1 µs | 2.0 M/s |
| 64 MB (58 %) | 95,000 | 4.0 µs | 1.9 M/s |
| 256 MB (231 %) | **177,000** | 4.1 µs | **6.0 M/s** |
| *sqlite* | *137,000* | *6.4 µs* | *5.1 M/s* |

At its 8 MB default this engine reads at less than half SQLite's rate and scans
at 40 % of it. With the whole database resident it reads about 1.3× SQLite and
scans about 1.2×. Both statements are true, and quoting either alone would be a
choice about which impression to leave.

Two things worth noticing rather than smoothing over:

* **The 64 MB row scans slower than the 8 MB row.** A full scan at that size
  evicts its own earlier blocks as it goes, so the cache does work without
  being useful. It only pays once the working set fits.
* **SQLite needs no cache of its own to scan quickly.** It walks B-tree leaf
  pages out of the operating system's page cache with no decoding step, while
  this engine decodes a prefix-compressed block. The comparison is not
  symmetric and the asymmetry favours SQLite.

### Lookups for keys that are not there

The row that stood here — 2,600,000 lookups a second against SQLite's
404,000, credited to the Bloom filter — measured something else. The
benchmark's absent keys were numbered past the last key in the database, and
a key past the end of every table is rejected by the version's range check
before any table, or its filter, is consulted. The row was a memtable miss
and a binary search over file ranges, and it would have read the same with
no filter configured. Nothing in the rate said so; what said so was counting
table reads per lookup, once that count existed: the phase made 0.00, where
a filter with ten bits per key lets about one lookup in a hundred through to
a block. The absent keys now fall between two present keys, so every level's
range check lets them through and the filter is what turns them away.

| | present keys | absent keys |
|---|---|---|
| ambar, this machine | 82,500 op/s (p50 12.4 µs) | **1,244,000 op/s** (p50 0.7 µs) |

An absent key is answered fifteen times faster than a present one, at the
cost of a memory probe, which is the win the filter buys. Both figures are
medians of three runs on the Windows machine of the amplification sections
below, with the lookup path as *Read scaling* below leaves it; they were
64,000 and 774,000 before that path lost its allocations. The Linux figures
above were made with the old keys, and so was the SQLite comparison. SQLite's absent-key rate has not been re-measured;
the SQLite side of the benchmark now draws the same in-range keys, so the
next comparison will be like for like. With the filter an absent key costs
0.01 table reads per lookup against 0.93 for a present one — the false
positives, each reading one block — and `tests/test_table.cpp` measures the
mechanism directly rather than inferring it from a rate: a thousand lookups
for absent keys read **13 blocks** with a filter configured and **1,000**
without one.

The filter is opt-in. `Options::filter_policy` defaults to null, and with it
null an absent key costs a block read at every level it could be in.

## Read amplification

What a lookup costs the disk: the reads of table files that the block cache
did not answer, per lookup, counted by the engine where it opens the files
and checked in `tests/test_stats.cpp` against the simulated disk's own count
of the reads it served.

| block cache | table reads per lookup | bytes read per byte scanned |
|---|---|---|
| 1 MB (1 % of the data) | 0.99 | 1.00 |
| 8 MB (7 %) — the default | 0.93 | 1.00 |
| 64 MB (58 %) | 0.44 | 1.00 |
| 256 MB (231 %) | 0.14 | 0.00 |

The lookups are 200,000 random keys out of a million on a freshly opened
database, as in the reads table above; the scan is the second of two full
passes. A present key costs one read, of its data block, once the table it
lives in has been opened: a table's index and filter are held in memory
from then on, and the levels below zero are disjoint, so the search reads
no block it can rule out. At the default that is 0.93 reads and 3.8 KB per
lookup. The curve is the block cache's hit rate — a cache holding 1 % of
the data answers almost nothing, one holding half answers half, and one
that holds everything still reads 0.14 blocks per lookup because these
lookups start cold: each of the database's roughly 27,000 data blocks is
read the first time a lookup lands in it, 200,000 lookups land in nearly
all of them, and 27,000 first reads over 200,000 lookups is 0.14. A key
that is not there costs 0.01 reads per lookup with the filter configured —
the false positives, each reading a block — which is the Bloom filter row
above measured from the other side. A cold scan reads 0.99 bytes for each
byte it returns — the database once, 108.1 MB for 108.7 MB of keys and
values — and a scan the cache already holds reads nothing.

Measured on the same Windows machine as the write amplification below, and
like it a ratio of counts rather than a speed, so it transfers where the
rates above would not. The figures are from one run; they are counts on a freshly opened
database with a fixed seed, and a second run reproduced every column
to two decimals. There is no
SQLite figure beside it for the same reason as there.

## Space

| | on disk | amplification |
|---|---|---|
| ambar | 110.8 MB | **1.02** |
| sqlite | 147.5 MB | 1.36 |

Measured after a full compaction, so it is the settled size rather than a
moment during one. Both figures reproduced byte-identically across runs.

## Write amplification

| | written | handed in | amplification |
|---|---|---|---|
| ambar | 1,651.9 MB | 305.2 MB | **5.41** |

Every byte the engine appended to a log, a table or a manifest during the
write phases above and the full compaction that follows them, against every
byte of key and value the benchmark handed it; the few bytes of the temporary
file behind each `CURRENT` rename are the one write left out. This is the
cost side of the trade an LSM tree
makes, and the number a reader most wants next to the write throughput. The
count is the engine's own, taken at the point each file is written and read
back through `get_property("ambar.bytes-written")`; watching the directory
could not have produced it, because compaction writes and deletes files
between any two looks. It is checked before it is believed:
`tests/test_stats.cpp` runs the engine on a simulated disk and requires the
engine's figure for each kind of file it counts to equal the disk's own
tally of what
was appended, and `mutations/stats.json` removes each writer from the count
in turn to show that the check notices.

Where the bytes go:

| | MB | per byte handed in |
|---|---|---|
| log | 355.8 | 1.17 |
| flush — tables written from memtables | 312.1 | 1.02 |
| compaction — tables rewritten on the way down | 984.0 | 3.22 |
| manifest | < 0.1 | — |

The denominator is not the 108.7 MB the space row is measured against. The
benchmark puts every key once in order, once more in random order, a
fiftieth of them again with `sync`, and then the two write-scaling phases
below make 37,500 and 750,000 writes from several threads over the first
20,000 and 400,000 keys; the engine writes each of those, so it is
305.2 MB. The log costs 1.17 because each
record carries a seven-byte header, twelve bytes of batch framing, and a
type byte and a length per field — a little less than the 1.19 of a workload
of single puts, since a group of batches shares one header; the flush costs
1.02 because a table holds the same bytes once, with an index and a filter
on top; and compaction — the part of the trade levelled compaction chose to
pay — costs 3.2 more as each byte is rewritten on its way down through the
levels. The table by level below is from the workload before the scaling
phases were added, when the figure was 5.65 on 219.6 MB handed in; it shows
the shape, which did not change, at the numbers it had then. From one such
run, whose amplification was 5.67:

| level | files now | size now (MB) | time (s) | read (MB) | written (MB) |
|---|---|---|---|---|---|
| 0 | 0 | 0.0 | 1.86 | 0.0 | 115.4 |
| 1 | 0 | 0.0 | 6.69 | 196.1 | 194.4 |
| 2 | 0 | 0.0 | 14.22 | 543.0 | 552.8 |
| 3 | 57 | 110.8 | 4.24 | 132.4 | 119.6 |

*Written* at a level is the tables put into it, by flushes and by the
compactions out of the level above; *read* is what those compactions read,
from both levels; *time* is what the flushes and compactions that wrote
there took, on the background thread. The written column sums to the
982.2 MB of tables that run wrote. Level 0 reads nothing because a flush
reads nothing, and it holds
less than the 224.8 MB of flushes because a memtable whose range overlaps
nothing below is written straight to a deeper level, which the sequential
pass does often. Level 2 is where the cost is: each level-1 table compacted
into it drags the level-2 tables it overlaps through the merge, at every
level-1 compaction, so level 2 was rewritten about five times over. The 57
files at level 3 are the settled database; its row is everything compacted
into level 3 since the open, most of it by the final full compaction.

Measured on a different machine from the tables above — Windows 11 on NTFS,
an MSVC Release build, the same workload — because the Linux container they
were taken in is no longer available. The ratio transfers where a throughput
figure would not: it is decided by the sizes of the memtable and the levels
and by when compaction ran, not by how fast the disk is. On the present
workload three runs gave 5.33, 5.41 and 5.43; on the earlier one, 5.59,
5.65 and 5.70, with a later 5.49. The log and flush figures were identical
within each workload and only compaction varied, because the background
thread's progress against the writer decides how many level-0 tables each
compaction picks up; a run made while the machine was also compiling gave
6.06, the writer having got that much further ahead. There is no SQLite
figure beside it: SQLite was not built on that machine, and counting what
it writes would need a hook of its own.

## Read scaling

The engine promises any number of readers alongside one writer and a
compaction. This is what the promise is worth in throughput: random lookups
for present keys on one, two, four and eight threads, 100,000 per thread,
each thread with its own sequence of keys, on the same Windows machine as the
two sections above, which has twelve cores.

| threads | cache as given (8 MB) | | whole database resident | |
|---|---|---|---|---|
| 1 | 80,200 read/s | ×1.00 | 508,000 read/s | ×1.00 |
| 2 | 149,400 | ×1.86 | 854,000 | ×1.68 |
| 4 | 259,800 | ×3.24 | 1,478,000 | ×2.91 |
| 8 | 394,600 | ×4.92 | 2,082,000 | ×4.10 |

Two columns because they measure different things. With the cache as given,
nine lookups in ten read a block from the file, and the operating system's
page cache and the reads themselves are most of the cost; the engine's own
locks are hidden behind them, and eight threads get five times one. With the
whole database resident — a cache four times the data, filled by one scan,
and 0.000 table reads per lookup over the run — nothing is left to contend
for but the engine, and that column is the one that says what the engine
costs.

Eight threads with everything resident read 1,080,000 a second when this
phase was first run, 2.8 times one thread, and the two changes that took
that to 1,330,000 were each found by taking a lock out and measuring again.
The table cache had one lock over every open table, taken twice per lookup;
sharded sixteen ways by file number, eight threads went to 1,190,000. The
database mutex was then taken twice per lookup as well, the second time on
the way out, to drop its references and charge a seek; removing that
acquisition in an experiment gave 1,480,000, and removing it properly — the
memtables' counts were already atomic, the version's is now, with the last
reference still dropped under the mutex, and the seek charge only taken when
a lookup consulted a second file — gave the row above. `docs/DESIGN.md`
says why the split is sound. The next suspect was the block cache, whose
table was keyed by a `std::string` built from the sixteen-byte block key on
every lookup — an allocation per block, two per point lookup, on MSVC and
libstdc++ both, whose small-string buffers hold fifteen. The table is keyed
by the sixty-four-bit hash now and a lookup allocates nothing, and the
eight-thread rate did not move: 1,420,000 on an idle machine, at the top
of the 1,270,000 to 1,410,000 the previous form had produced and inside
its noise. The allocation was real and was not the bottleneck.

What was is the reason the table above reads 508,000 and 2,082,000 where it
read 350,000 and 1,333,000. The remaining suspects were measured by
removal: `ambar_lookup_probe` runs resident lookups on the benchmark
database, and a scratch build had one suspect at a time taken out — the
database mutex at the top of `get`, the memtables' and the version's
reference counts, the block cache's lock and its list moves — and each was
worth a few percent at eight threads and nothing at one. Then the probe's
allocator was swapped for a thread-local free list, and one thread ran a
fifth faster on a wide working set and a third faster on a hot one: the
lookup made nine heap allocations. Two copies of the lookup key, a list of
candidate files at level 0, an iterator over the index block and one over
the data block with the string each rebuilt its keys into, and the wrapper
that held the data block's cache handle. It makes one now — the string the
found key is rebuilt into — and on a quiet machine, before and after
interleaved, a resident lookup on one thread went from 2.3 µs to 1.8 on the
million keys and from 1.7 to 1.2 on a hot set of twenty thousand; eight
threads from 1,690,000 to 2,110,000 on the wide set and from 1,630,000 to
1,830,000 on the hot one. The lookups that miss the cache gained as much
in proportion, 14.8 µs to 12.4 at the median and 64,000 to 82,500 a
second, since a miss made the same nine allocations and then the read.

The hot set is the one to watch. Its eight threads reach 2.1 times one
where the wide set's reach 4.1, and neither the allocator nor any lock the
probe took out on its own moves that: with every lookup in the same twenty
thousand keys the threads share the same handful of blocks and the same two
tables, and what they contend for is the table cache's shard lock, taken
twice per lookup, and the reference counts on the version and the table,
which every lookup writes. That is the next measurement.

The eight-thread rate is the steadier of the two figures. Across five runs
of the previous form it stayed between 1,270,000 and 1,410,000, while the
single-thread rate moved between 300,000 and 390,000 depending on what
else the machine was doing, so the ratio on the last row read anywhere
from 3.8× to 4.5× for what was the same engine. A ratio with a noisy
denominator is a poor headline; the rates are what the changes were judged
by, and as everywhere in this document they belong to this machine. The
table above is the median of three runs on a quiet evening; the resident
rows agreed within 4 % and the cache-as-given rows within 6 %.

## Write scaling

Writes are serialised by design: one writer at a time, through a queue, and
the writer at the front merges every batch queued behind it into one log
record with one `fsync`. Whether that turns several threads' worth of synced
writes into more than one thread's rate is a measurement, and so is the size
of the groups it makes — which the engine counts, as records appended to the
log and the batches they carried, and reports through
`get_property("ambar.log-writes")`. Synced writes from one, two, four and
eight threads, 2,500 per thread, each thread on its own keys, on the Windows
machine of the sections above:

| threads | writes/s | | batches per `fsync` |
|---|---|---|---|
| 1 | 1,588 | ×1.00 | 1.00 |
| 2 | 2,346 | ×1.48 | 1.48 |
| 4 | 3,948 | ×2.49 | 2.50 |
| 8 | 7,384 | ×4.65 | 4.77 |

The two columns move together, and that agreement is the finding: the rate
climbs exactly as far as the groups grow, so it is group commit that buys
the throughput and nothing else. One thread can never share an `fsync`,
because there is never anyone queued behind it; eight threads share each
one between nearly five. The device flush is the cost that does not scale,
at about 600 µs on this disk, and every batch that rides one already paid
for is a batch that did not wait for its own.

`tests/test_stats.cpp` checks the count from both ends — written one at a
time, records and batches are equal; from eight threads at once, every batch
is counted and no record carries none — and `mutations/stats.json` removes
each half of it in turn. Three runs agreed more closely than any other
figure in this document — the eight-thread row moved between 7,190 and
7,380 writes a second and between 4.72 and 4.77 batches per `fsync` — which
is what a phase bound by the device rather than by the CPU looks like on a
machine doing other things; the table is the first of the three.

### Without sync

The same phase with `sync` off, 50,000 writes per thread, is the other half
of the answer. As first measured, with every waiting writer parking on its
condition variable at once, it was not a flattering one:

| threads | writes/s | | batches per log write |
|---|---|---|---|
| 1 | 158,000 | ×1.00 | 1.00 |
| 2 | 71,000 | ×0.45 | 1.30 |
| 4 | 86,000 | ×0.54 | 2.14 |
| 8 | 88,000 | ×0.56 | 4.29 |

Two threads wrote at less than half the rate of one, and eight never got
back to it. The grouping still happened — the last column climbs as it did
with `sync` — but there was nothing left for it to save: a log append costs
a few microseconds where an `fsync` cost six hundred, and what a group cost
was now the larger number. A writer that was not at the front parked on its
own condition variable; the leader wrote the group, then woke each member
in turn and the next leader after them, and every one of those wake-ups was
a context switch that the single thread, which never queues, never pays.

So a waiting writer now watches its state for fifty microseconds before it
parks, and is nearly always answered in that time; `docs/DESIGN.md` says
how. The same rows, with the engine as the benchmark configures it — a 4 MB
memtable, so that flushes and compaction run through the phase — and with
the share of writers that still had to park:

| threads | writes/s | | batches per log write | parked |
|---|---|---|---|---|
| 1 | 164,000 | ×1.00 | 1.00 | 0.0 % |
| 2 | 115,000 | ×0.70 | 1.01 | 0.3 % |
| 4 | 127,000 | ×0.77 | 2.04 | 1.1 % |
| 8 | 92,000 | ×0.56 | 4.26 | 3.3 % |

The parking is gone — three writers in a hundred sleep, where every one of
them did — and two threads recovered from 0.45× to 0.70×, but eight did
not move. That is not the queue any more. With a 64 MB memtable, so that
the phase runs with no flush and no compaction in the way, the same
benchmark gives:

| threads | writes/s | | batches per log write | parked |
|---|---|---|---|---|
| 1 | 197,000 | ×1.00 | 1.00 | 0.0 % |
| 2 | 154,000 | ×0.78 | 1.01 | 0.3 % |
| 4 | 213,000 | ×1.08 | 2.04 | 1.2 % |
| 8 | 261,000 | ×1.32 | 4.31 | 2.4 % |

Eight threads write a third faster than one, four threads a little faster,
and the queue does what it was built to do: the append and its `flush`
are shared across a group of four. What the 4 MB rows show on top of that
is compaction — eight writers fill a memtable every fifth of a second, and
the background thread that flushes and compacts it takes a core, a share
of the mutex, and the writers' time through backpressure — and that is a
cost of the engine's configuration, not of its queue.

Two threads write below one in both tables, and a probe that timed the
leader's own work said why: with one thread spinning beside it, the
leader's four-microsecond append and insert took six, from sharing cache
lines and, on a hyperthreaded core, a pipeline, and a group of one batch
amortises nothing against that. A yield in place of the pause was tried
and was slower at every count. Below four threads the queue costs more
than it saves without `sync`, and the engine is not tuned around that:
a single writer is the case the design is for.

Three runs at 4 MB put the eight-thread row between 84,000 and 112,000
and the two-thread row between 109,000 and 134,000; two runs at 64 MB put
the eight-thread row at 260,000 and 261,000, and the single-thread row,
which moves most with the machine's load, at 179,000 and 216,000. The
tables are medians.

## Compression

Every figure above is with compression off, the default before 0.3.0, and
it is on by default now. This is the same benchmark with it on — the same
million keys, the same 100-byte values, half random and half repeated so
that compression is
neither pointless nor free — on the Windows machine of the sections above,
each table the median of three runs, and a run of the coder table the
best of five passes. Two things were measured: the coder on its own,
against the coders it stands in for, and then the engine with the coder
in it.

### The coder

`src/compress.hpp` is an LZ77 coder in the shape of LZ4, written here
rather than taken from a library because its decoder is a parser of
untrusted bytes. `ambar_codec_bench` runs it over the data blocks of an
existing database — the first 65.0 MB of the uncompressed benchmark
database's blocks, 16,220 of them, exactly what the builder hands the
coder, restart arrays and internal-key trailers included — and
`tools/compare_codecs.py` runs zlib and LZ4 over the same blocks, dumped
to a file, so that the engine takes no dependency for the comparison's
sake:

| coder | ratio | compress | decompress |
|---|---|---|---|
| `src/compress.hpp`, as first written | 2.92× | 325 MB/s | 420 MB/s |
| `src/compress.hpp`, now | 2.92× | 389 MB/s | **1,620 MB/s** |
| LZ4 1.9.4, default | 2.93× | 516 MB/s | 2,485 MB/s |
| LZ4 1.9.4, high compression | 3.54× | 58 MB/s | 2,559 MB/s |
| zlib, level 1 | 4.09× | 90 MB/s | 255 MB/s |
| zlib, level 6 | 4.63× | 48 MB/s | 299 MB/s |

The ratio is LZ4's to the second decimal, and that is no coincidence: LZ4's
default level is the same idea — a hash of each four-byte window, the
first candidate taken, greedily — and the details that differ, how far a
match is extended backwards over the literals before it and how positions
are skipped when nothing matches, come out even on this data. zlib finds
more matches and then entropy-codes what is left, at a fifth to a tenth of
the speed in, and LZ4's high-compression level tries more candidates for
each; both are a better ratio bought with compression time, and neither
is what the engine needs on a write path that is bound by `fsync` and
compaction.

The gap was speed, and where it was is the finding. As first written the
decoder ran at a sixth of LZ4's rate, because it appended every byte of a
match one at a time: a match may overlap what it copies — an offset of one
repeats the last byte — and the byte loop was the safe way to copy one,
with a capacity check on every `push_back`. It now writes into a string
sized once for the declared length, and copies eight bytes at a time when
there is a step of room to spill into past the literals in the input and
past where they land in the output, and when the match's source is at least
a step behind its destination, so that no step reads what the step before
it has not yet written; where there is not, it copies exactly, one memcpy
for a match that does not overlap and one byte at a time for one that
does. Every bounds check the decoder had it still has, one per sequence
rather than one per byte, and `mutations/compress.json` removes each in
turn, the room checks on the wide copies included. That took decoding from
420 to 1,620 MB/s, two thirds of LZ4's rate; the rest of the gap is
years of work on exactly this loop, wider steps and fewer branches, which
this project is not going to reproduce. Compression went from 325 to 389
MB/s, three quarters of LZ4's, from sizing the hash table to the block
instead of zeroing 64 KiB of table for every 4 KiB block, and from writing
the output through a pointer rather than a `push_back` per byte.

The LZ4 and zlib rates are those of the C libraries behind Python's
bindings, net of the cost of calling into them from Python, which
`tools/compare_codecs.py` measures on a sixteen-byte block — all call and
no work — and subtracts once per block: 0.2 µs for an LZ4 decode, which
at 2.5 GB/s on a four-kilobyte block is an eighth of the decode itself,
and 0.4 µs for a compress. As timed, before the subtraction, LZ4 decoded
at 2,195 MB/s. The subtraction takes with it whatever the library does
per call in C, so it flatters the library a little, which is the right
direction for a comparison this coder is on the other side of. zlib's
compress call costs 4 µs on the sixteen-byte block, and that is zlib
setting up, so its net figure is flattered by a tenth.

### The engine

The same benchmark, compression off and on, before and after the decoder
above was made fast. Space and write amplification are what compression
is for; the read rows are what it costs, or was expected to.

The off column is a fresh run of the same benchmark; it differs from the
sections above where compaction's timing does — 5.24 here against 5.41
under *Write amplification* — and agrees where it does not, the log to
within a megabyte and the directory to the byte.

| | off | on, decoder as first written | on, now |
|---|---|---|---|
| on disk, after compaction | 110.8 MB | 39.9 MB | 39.9 MB |
| space amplification | 1.02 | **0.37** | **0.37** |
| write amplification | 5.24 (1,600.8 MB) | 2.53 (771.4 MB) | **2.59** (791.5 MB) |
| — of which flush / compaction | 312.1 / 932.1 MB | 112.9 / 301.9 MB | 113.0 / 321.9 MB |
| write sequential, no sync | 163,600 op/s | 159,100 op/s | 165,800 op/s |
| write random, no sync | 72,400 op/s (p99.9 104 µs) | 79,300 op/s (p99.9 104 µs) | **89,500 op/s** (p99.9 96 µs) |
| read random, present, 8 MB cache | 64,300 op/s (p50 14.8 µs) | 48,400 op/s (p50 19.8 µs) | **77,100 op/s** (p50 12.0 µs) |
| — bytes read from disk per lookup | 3.8 KB | 1.3 KB | 1.3 KB |
| read random, absent | 762,900 op/s | 714,800 op/s | 747,800 op/s |
| scan, cold blocks | 2,155,000 op/s | 1,633,000 op/s | **2,462,000 op/s** |
| — bytes read from disk per byte scanned | 0.99 | 0.34 | 0.34 |

By block cache size, random reads of present keys on a freshly opened
database, as in the read tables above (a separate phase from the read row
in the table above, which runs on the database as the writes left it);
the cache holds decoded blocks, so
its sizes are the same fraction of the data in both modes and a given size
misses at the same rate in both — 0.93 reads per lookup at 8 MB, 0.14 at
256 MB — and what differs is what a miss costs:

| block cache | off | on, decoder as first written | on, now |
|---|---|---|---|
| 1 MB (1 % of the data) | 64,700 read/s | 47,300 | 77,300 |
| 8 MB (7 %) — the default | 65,500 | 48,200 | 76,200 |
| 64 MB (58 %) | 104,800 | 85,300 | 116,400 |
| 256 MB (231 %) | 195,800 | 168,900 | 220,000 |
| eight threads, 8 MB cache | 323,900 | 214,300 | 326,700 |
| eight threads, everything resident | 1,446,000 | 1,421,000 | 1,508,000 |

The space and write columns are the ones a reader wants first. On these
values the database on disk is 36 % of what it was, and write
amplification halves: the log is the same 356.6 MB in both modes, since it
is written before any block is built, but flush and compaction write a
third of the bytes, because it is compressed blocks that get flushed and
then compacted, and compaction moves what it reads. The random-write row
follows from that: with a third of the compaction I/O in its way it is
24 % faster and its tail is lower, and the sequential row does not move,
because it never waited on compaction to begin with.

The read rows were expected to be the cost, and with the decoder as first
written they were: a lookup that missed the cache read a 1.3 KB block and
then turned it into 3.8 KB, nine microseconds of decoding at that
decoder's rate, and the p50 rose by five. With the decoder as it is now,
compression on reads *faster* than compression off at every cache size,
and the cold scan is 14 % faster.
Two things this benchmark cannot separate are behind that, and both are
consequences of compression rather than accidents. A miss moves 1.3 KB
through the file API instead of 3.8, and on a database that sits in the
operating system's page cache that copy is a large part of what a miss
costs, so the 2.4 µs of decoding at 1.6 GB/s is paid for out of the bytes
it replaces. And the tree is shallower: the levels are sized in bytes on
disk, ten megabytes at level 1 and ten times that at each level below, so
39.9 MB of tables settles at level 2 in 20 files where 110.8 MB settled
at level 3 in 57 or so, and a lookup that reaches the bottom searches one
index and one filter fewer on its way. The absent-key row, which reads no
block at all, does not separate the three columns — eight of the nine
runs lie between 700,000 and 770,000, and the ninth, an off run, at
584,000 — which says the filter is neither helped nor hurt.

With everything resident nothing is decoded, and the resident columns
agree to within their spread. What compression costs, then, is a decode
per cache miss; what it saves is the bytes of every miss, of every flush
and compaction, and of the directory; and on this machine, with this data
and the page cache warm, the decode is cheaper than the bytes it replaces.
On real storage, where a miss is a device read rather than a page-cache
copy, the bytes saved would be worth more still and the decode the same.

The tables are medians of three runs. The read-present row moved by 2 %
across the three off runs, 4 % on, and 5 % now; the scan by 2 %, 3 % and
4 %; the write-random row by 9 %, 4 % and 7 %; compaction's bytes by 2 %,
7 % and 9 %, which is the scheduler deciding when compactions overlap the
writes. The coder's decoding rate is the figure that moves most with the
machine's state: 1,606 to 1,653 MB/s across the three runs behind the
table, and 1,512 to 1,545 an hour later on the same binary and blocks.

The tables above were taken before the lookup path lost its allocations
(*Read scaling* above), which lifted every read row in both columns. With
that path, medians of three runs on a quiet evening, compression off and
on: a cache-missing lookup 82,500 and 93,100 a second (p50 12.4 and
10.7 µs), an absent key 1,244,000 and 1,199,000, the cold scan 2,328,000
and 2,667,000, random writes 72,200 and 90,700; on disk, write
amplification and the bytes per miss as in the table. The shape holds
wherever a block is read: on is faster than off. With everything resident
one thread read 508,000 with compression off and 456,000 with it on, a gap
of a tenth that nothing in the engine accounts for, since nothing resident
is decoded; interleaved runs a week later, below, put the two level —
522,000 and 521,000 — and it was the machine. The same interleaved runs,
off and on: 80,400 and 100,200 cache-missing lookups a second (p50 12.3
and 9.5 µs), 60,200 and 90,000 random writes, and a cold scan 2,418,000
and 2,786,000. Reproduce with:

    ./build/ambar_bench /tmp/bench --keys 1000000 --value-size 100
    ./build/ambar_bench /tmp/bench --keys 1000000 --value-size 100 --compression none
    ./build/ambar_codec_bench /tmp/bench/ambar --dump blocks.bin
    python3 tools/compare_codecs.py blocks.bin      # pip install lz4, optionally

### Where there is nothing to gain

The values above are half random and half repeated, which is what real
values tend to look like. This is the other end: `--values random`, a
hundred bytes of random data each, which no coder can shrink. The keys and
the internal-key trailers still compress, so a block is not quite
incompressible: it comes out two percent smaller. The question is what
asking costs when the answer is that.

It was first measured with the builder keeping any saving, and those runs
found reads a few percent slower with compression on: every block had
shrunk by its two percent, so every lookup that missed the cache paid a
decode for it. The builder now keeps a compressed block only when it saves
at least an eighth, which is LevelDB's rule, and the table has both. The
three columns were run interleaved — off, then on with the old rule, then
on with the new, three times over — because a machine's state drifts over
an evening by more than the differences here, and the first attempt at
these numbers, run one column at a time, had put the write cost at twice
what it is. The machine was idle before every run. The first and third
columns are the same binary with a different option; the middle one is a
separate build of the previous commit, which matters below.

| | off | on, any saving kept | on, an eighth or more |
|---|---|---|---|
| on disk, after compaction | 110.8 MB | 108.2 MB | 110.8 MB |
| write amplification | 5.21 (1,590 MB) | 5.03 (1,536 MB) | 5.09 (1,554 MB) |
| write sequential, no sync | 136,900 op/s | 138,100 op/s | 133,000 op/s |
| write random, no sync | 59,500 op/s | 54,600 op/s | **54,000 op/s** |
| read random, present, 8 MB cache | 79,900 op/s (p50 12.3 µs) | 76,500 op/s (p50 12.9 µs) | **79,400 op/s** (p50 12.3 µs) |
| — the same, freshly opened, 8 MB | 80,400 | 76,100 | 79,900 |
| — the same, freshly opened, 256 MB | 258,100 | 242,600 | 257,400 |
| eight threads, 8 MB cache | 386,900 read/s | 370,000 read/s | 388,200 read/s |
| eight threads, everything resident | 2,143,000 read/s | 2,023,000 read/s | 2,128,000 read/s |

Under the eighth the random blocks are stored raw — the directory is the
size it is with compression off, a lookup reads the same 3.8 KB — so a
lookup reads and decodes exactly what it does with compression off, and
every read row is level with that column: the same binary, within a
percent. Under the old rule the read rows were four to six percent behind,
which is about what decoding a block of nearly all literals should cost,
half a microsecond on the median lookup; but the resident row, which
decodes nothing, is as far behind in that column, so some of that gap may
be the separate build rather than the rule. What the eighth demonstrably
does is take the decode out, and what is left is the write row. The coder runs on every block
whatever it decides — about 1.5 GB of blocks at 390 MB/s, four seconds of
a core on the thread that compacts, which is what random writes wait for —
and random writes are nine percent slower for it, off and on measured by
the one binary. The sequential row did
not move within its spread, which was the widest in the table. A cold scan
is not in the table: its three runs spread by twelve to fifteen percent in
each column, more than any difference between them.

So on data with no structure, compression on costs a tenth of the random
write rate and nothing else, and on data with structure it saves two thirds
of the space and is faster at everything. The engine does not try to
notice that a *file* is not compressing and stop asking, which is what
would take back that tenth; `docs/DESIGN.md` lists that under what is not
implemented.

## What is not measured

**Anything under memory pressure or with a cold page cache.** Every number here
was taken with the whole database in the operating system's page cache. Real
storage latency would change the read figures far more than the write ones.
