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

| | ambar | sqlite |
|---|---|---|
| absent keys | **2,600,000 op/s** (p50 0.3 µs) | 404,000 op/s (p50 2.2 µs) |

This is the Bloom filter, and it is the one place the design wins by a wide
margin rather than a narrow one — 6.4× at the default cache size, where every
other read result is a loss. `tests/test_table.cpp` measures the mechanism
directly rather than inferring it from the rate: a thousand lookups for absent
keys read **13 blocks** with a filter configured and **1,000** without one.

Note that the filter is opt-in. `Options::filter_policy` defaults to null, and
with it null this row collapses to roughly the "present keys" rate.

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
5.59, 5.65 and 5.70. The log and flush figures were identical in all three
and only compaction varied, because the background thread's progress against
the writer decides how many level-0 tables each compaction picks up. There is
no SQLite figure beside it: SQLite was not built on that machine, and
counting what it writes would need a hook of its own.

## What is not measured

**Read amplification** — the blocks read for each lookup, the other side of
the levelled trade. `tests/test_table.cpp` counts block reads for one
specific case (the filter), which is the closest this project comes.

**Anything under memory pressure or with a cold page cache.** Every number here
was taken with the whole database in the operating system's page cache. Real
storage latency would change the read figures far more than the write ones.

**Concurrency.** The benchmark is single-threaded. The engine is tested under
several threads in `tests/test_db.cpp` and under ThreadSanitizer, but its
scaling is not measured.
