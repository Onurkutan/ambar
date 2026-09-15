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

SQLite is configured as a user running a key-value workload would configure it:
WAL journalling, `synchronous=NORMAL`, one transaction per thousand rows. Its
defaults would have been a comparison against a configuration nobody uses.

Reproduce with:

    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release
    ./build-release/ambar_bench /tmp/bench --keys 1000000 --value-size 100

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
| ambar, this machine | 64,000 op/s (p50 14.7 µs) | **774,000 op/s** (p50 1.0 µs) |

An absent key is answered twelve times faster than a present one, at the
cost of a memory probe, which is the win the filter buys. Both figures are
from one run on the Windows machine of the amplification sections below,
because the
Linux figures above were made with the old keys, and so was the SQLite
comparison. SQLite's absent-key rate has not been re-measured;
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
| ambar | 1,241.6 MB | 219.6 MB | **5.65** |

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
| log | 262.0 | 1.19 |
| flush — tables written from memtables | 224.8 | 1.02 |
| compaction — tables rewritten on the way down | 754.7 | 3.44 |
| manifest | < 0.1 | — |

The denominator is not the 108.7 MB the space row is measured against. The
benchmark puts every key once in order, once more in random order, and a
fiftieth of them again with `sync`, and the engine writes each of those, so
it is 219.6 MB. The log costs 1.19 because each record carries a seven-byte
header, twelve bytes of batch framing, and a type byte and a length per
field; the flush
costs 1.02 because a table holds the same bytes once, with an index and a
filter on top; and compaction — the part of the trade levelled compaction
chose to pay — costs 3.4 more as each byte is rewritten on its way down
through the levels. By level, from a fourth run, whose amplification was
5.67:

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
and by when compaction ran, not by how fast the disk is. Three runs gave
5.59, 5.65 and 5.70, and a later one 5.49. The log and flush figures were
identical in all of them and only compaction varied, because the background
thread's progress against the writer decides how many level-0 tables each
compaction picks up; a run made while the machine was also compiling gave
6.06, the writer having got that much further ahead. There is
no SQLite figure beside it: SQLite was not built on that machine, and
counting what it writes would need a hook of its own.

## Read scaling

The engine promises any number of readers alongside one writer and a
compaction. This is what the promise is worth in throughput: random lookups
for present keys on one, two, four and eight threads, 100,000 per thread,
each thread with its own sequence of keys, on the same Windows machine as the
two sections above, which has twelve cores.

| threads | cache as given (8 MB) | | whole database resident | |
|---|---|---|---|---|
| 1 | 59,000 read/s | ×1.00 | 350,000 read/s | ×1.00 |
| 2 | 118,000 | ×1.99 | 549,000 | ×1.57 |
| 4 | 207,000 | ×3.49 | 954,000 | ×2.73 |
| 8 | 315,000 | ×5.32 | 1,333,000 | ×3.81 |

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
says why the split is sound. What remains is the block cache, sixteen shards
with a string allocation per lookup, and the memtable probe; neither has
been measured on its own.

The eight-thread rate is the steadier of the two figures. Across five runs it
stayed between 1,270,000 and 1,410,000, while the single-thread rate moved
between 300,000 and 390,000 depending on what else the machine was doing, so
the ratio on the last row read anywhere from 3.8× to 4.5× for what was the
same engine. A ratio with a noisy denominator is a poor headline; the rates
are what the changes were judged by, and as everywhere in this document they
belong to this machine.

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

## What is not measured

**Anything under memory pressure or with a cold page cache.** Every number here
was taken with the whole database in the operating system's page cache. Real
storage latency would change the read figures far more than the write ones.

**Unsynced write scaling.** Without `sync` there is no device flush to
share, and what several writers cost each other is the queue and the log
append; not measured, since the read side's two locks were the ones with a
number to find, and this side's lock is the design.
