# Benchmarks

Produced by `tools/bench`, which is written to be read rather than quoted: it
reports percentiles rather than averages, states what it is not measuring, and
compares against SQLite because a number with nothing beside it is not a
measurement.

## Conditions

One machine, one filesystem — a Linux container on an overlay filesystem, a
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
| random, `sync=true` | 4,200 op/s (p50 206 µs, p99 429 µs) | — |

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

## What is not measured

**Write amplification** — the bytes an LSM tree eventually writes for each byte
of user data, which is the cost side of the trade it makes, and the number a
reader most wants next to the write throughput above. Measuring it honestly
means counting at the point each file is written, and the engine does not
expose that; sampling the directory size misses files written and deleted
between samples. The table above reports *space* amplification, which is a
different and much easier quantity, and says so rather than letting one word do
duty for both.

**Read amplification.** Same reason. `tests/test_table.cpp` counts block reads
for one specific case (the filter), which is the closest this project comes.

**Anything under memory pressure or with a cold page cache.** Every number here
was taken with the whole database in the operating system's page cache. Real
storage latency would change the read figures far more than the write ones.

**Concurrency.** The benchmark is single-threaded. The engine is tested under
several threads in `tests/test_db.cpp` and under ThreadSanitizer, but its
scaling is not measured.
