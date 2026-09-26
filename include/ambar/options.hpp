// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The knobs, and what turning each one costs.
//
// Every default here is a trade, and the comment on each one names the other
// side of it.  A default with no stated cost is a default nobody has thought
// about.

#ifndef AMBAR_OPTIONS_HPP_
#define AMBAR_OPTIONS_HPP_

#include <cstddef>

namespace ambar {

class Cache;
class FilterPolicy;

// Sizes are clamped, not trusted: write_buffer_size to [64 KiB, 1 GiB],
// max_file_size to [1 MiB, 1 GiB], block_size to [1 KiB, 4 MiB], and
// max_open_files to at least 20.  A caller who asks for less than the floor
// gets the floor with no warning, so a test setting max_file_size to 256 KiB
// is really running at 1 MiB.  Zero for the write buffer would flush on every
// key and look like a hung database; four gigabytes would lose four gigabytes
// of work to a crash.  Both are likelier to be mistakes than intentions.
struct Options {
  // Create the database if the directory holds none.
  bool create_if_missing = false;

  // Refuse to open a directory that already holds one.
  bool error_if_exists = false;

  // There is deliberately no option to turn checksum verification off.
  //
  // Every block is verified on every read, always.  An engine that trusts its
  // own files has no way to tell a bug from a bad disk, and the cost is a CRC
  // over bytes that are already in cache.  A knob here would exist only to let
  // someone turn off the check that tells them their data is wrong.
  //
  // Two such knobs used to be here -- `paranoid_checks` and
  // `ReadOptions::verify_checksums` -- and neither did anything: the
  // verification in read_block was unconditional and no code path consulted
  // either field.  A documented option that does nothing is worse than no
  // option, because someone will set it and believe they have changed
  // something.

  // How much a memtable may hold before it is flushed to a table file.
  //
  // Larger means fewer, bigger tables and less write amplification, at the
  // cost of memory (two of these can be live at once, the one being filled and
  // the one being flushed) and a longer log to replay after a crash.
  size_t write_buffer_size = 4 * 1024 * 1024;

  // Uncompressed bytes per data block.
  //
  // Larger blocks make a scan cheaper and an index smaller; smaller blocks
  // make a point lookup read less.  Four kilobytes is roughly a page.
  size_t block_size = 4 * 1024;

  // Entries between restart points inside a block.  See block_builder.hpp: it
  // trades the space saved by prefix compression against how far a lookup
  // scans after its binary search.
  int block_restart_interval = 16;

  // Whether data blocks are compressed on the way to disk.
  //
  // kLz is an LZ77 coder written for this engine (src/compress.hpp): on
  // keys and values with structure it makes a block a fraction of its size,
  // which is that fraction off every table written, compacted and read from
  // disk; on data with none it stores the block as it was -- any block
  // that would not shrink by an eighth -- so the cost of
  // asking is a pass over the block at write time.  What it costs a read is
  // a decode per block the cache did not answer.  Off by default, so that
  // the figures in docs/BENCHMARKS.md describe the engine as configured; a
  // database written with it on is read by any build that has it, with it
  // on or off, since the choice is recorded per block.  Index and filter
  // blocks are never compressed: they are read once per open and held, so
  // there is nothing to save.
  enum class Compression { kNone, kLz };
  Compression compression = Compression::kNone;

  // The largest a table file may grow during a compaction.
  size_t max_file_size = 2 * 1024 * 1024;

  // Open file handles the table cache may hold.  Each open table costs a file
  // descriptor and its index block in memory.
  int max_open_files = 1000;

  // Decoded blocks are kept here, or in a private cache when null.
  //
  // Sharing one cache between several databases is the reason this is a
  // pointer rather than a size: two databases in one process should compete
  // for one memory budget, not have one each.
  //
  // Null does not mean "no cache" -- it means a private one of 8 MiB, created
  // for this database alone.  There is no way through this API to run with no
  // cache at all, because a table read without one costs a block read and a
  // parse per key rather than per block.
  //
  // How much cache is worth having is a measurement rather than a default, and
  // it is the measurement that moves most: tools/bench reports random reads and
  // scans against cache size, and on a database of 109 MB they range from
  // 62,000 reads/s at 1 MB to 177,000 at 256 MB, crossing SQLite's rate
  // somewhere in between.  docs/BENCHMARKS.md has the curve.
  Cache* block_cache = nullptr;

  // The bloom filter, or null for none.
  //
  // Null is not "off, so it costs nothing": it is "every level is searched on
  // disk before a missing key can be reported missing".  Ten bits per key
  // costs about 1.25 bytes per key of memory and removes about 99 % of those
  // reads (measured in tests/test_bloom.cpp).
  const FilterPolicy* filter_policy = nullptr;
};

struct ReadOptions {
  // Whether blocks read for this operation should stay in the cache.  A bulk
  // scan sets it false so that a one-off pass over the whole database does not
  // evict the working set of everything else.
  bool fill_cache = true;

  // Read as of this snapshot, or null for the latest state.
  const struct Snapshot* snapshot = nullptr;
};

struct WriteOptions {
  // Wait for the write to reach the disk before returning.
  //
  // False is the fast default and it is honest about what it gives up: the
  // write survives this process dying, because the bytes are with the kernel,
  // and does not survive the machine losing power.  True costs a device flush
  // -- on the order of a millisecond on rotating storage, less on an SSD, and
  // nothing at all on hardware that lies about it, which is why
  // docs/DESIGN.md states the assumption rather than hiding it.
  bool sync = false;
};

}  // namespace ambar

#endif  // AMBAR_OPTIONS_HPP_
