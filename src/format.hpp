// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The on-disk shape of a table file.
//
// A table is a sequence of blocks, then a footer of fixed size at the very
// end.  Reading one starts from the end: the footer is at a known offset from
// the file's last byte, it points at the index block, the index block points
// at every data block, and nothing else has to be scanned.  That is why the
// footer is fixed-length and padded -- a variable-length footer could not be
// found without already knowing where it started.
//
//   +---------------------+
//   | data block 0        |
//   | ...                 |
//   | data block n        |
//   | filter block        |   optional; summarises the keys in each data block
//   | metaindex block     |   names -> handles, so new block kinds can be
//   |                     |   added without changing the footer
//   | index block         |   last key of each data block -> its handle
//   | footer (48 bytes)   |
//   +---------------------+
//
// Every block, footer excepted, is stored as:
//
//   [ block contents ][ 1 byte compression type ][ 4 byte CRC32C ]
//
// The checksum covers the contents *and* the compression byte.  Leaving the
// type byte out would let a flipped bit in it turn an uncompressed block into
// a corrupt decompression attempt while the checksum still passed.

#ifndef AMBAR_FORMAT_HPP_
#define AMBAR_FORMAT_HPP_

#include <cstdint>
#include <string>
#include <string_view>

#include "ambar/status.hpp"

namespace ambar {

// The byte was written from the first version, when nothing compressed, so
// that a reader could already refuse what it could not handle; from 0.2.0
// a data block may carry kLz, the coder in compress.hpp.  Index, filter and
// metaindex blocks are always kNone.  A value this build does not know is a
// corruption, not a guess.
enum class CompressionType : uint8_t {
  kNone = 0x0,
  kLz = 0x1,
};

// The trailer appended to every block.
constexpr size_t kBlockTrailerSize = 5;  // one type byte, four checksum bytes

// Where a block lives.  Stored as two varints wherever it is referenced, so a
// small file does not pay for 64-bit offsets it will never reach.
class BlockHandle {
 public:
  BlockHandle() = default;

  uint64_t offset() const { return offset_; }
  uint64_t size() const { return size_; }
  void set_offset(uint64_t offset) { offset_ = offset; }
  void set_size(uint64_t size) { size_ = size; }

  void encode_to(std::string* dst) const;
  Status decode_from(std::string_view* input);

  // Two varint64s.
  static constexpr size_t kMaxEncodedLength = 10 + 10;

 private:
  uint64_t offset_ = 0;
  uint64_t size_ = 0;
};

// The last bytes of every table file.
//
// Padded to a constant length so that a reader can seek to (file size - 48)
// without knowing anything about the file, and terminated by a magic number so
// that a file which is not a table, or one truncated to exactly the right
// length by chance, is rejected before its contents are trusted.
class Footer {
 public:
  Footer() = default;

  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  const BlockHandle& index_handle() const { return index_handle_; }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void encode_to(std::string* dst) const;
  Status decode_from(std::string_view* input);

  // Two handles at their maximum encoded length, then the magic number.
  static constexpr size_t kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

// The low 64 bits of the SHA-1 of "ambar.table.format.1", so that it is
// arbitrary in the sense that matters -- no chance of colliding with a
// meaningful byte sequence -- and reproducible.
constexpr uint64_t kTableMagicNumber = 0x9d4f2b6c1a73e850ull;

// The contents of a block, plus who owns them.
//
// A block read from a file owns heap memory that must be freed; a block served
// from a cache does not.  Both reach the same reader, so ownership travels
// with the data rather than being implied by where the reader got it.
struct BlockContents {
  std::string_view data;
  bool cachable = false;      // safe to keep beyond this read
  bool heap_allocated = false;  // data.data() must be delete[]'d
};

class RandomAccessFile;

// Reads the block at `handle`, verifying its checksum.
//
// A checksum failure is reported as corruption rather than retried or
// repaired.  There is nothing to repair: the block's own bytes are the only
// copy, and a filter or index read past a bad checksum can hide keys silently
// (see tests/test_bloom.cpp), which is worse than failing loudly.
//
// `file_size` is not decoration.  A handle is two varints out of the file, so
// its size field is whatever the file says — and the allocation below is made
// before a single byte of the block has been read.  A forty-eight byte file
// with a valid magic number and a handle claiming a gigabyte gets a gigabyte
// allocated, and only then does the read fail.  Bounding the handle by the
// size of the file it came from makes that impossible rather than merely
// capped -- for the bytes read.  A compressed block then declares its
// decoded length, and that one is capped rather than impossible: the
// decoder refuses a length above 255 times the bytes it was given, which
// is the most its format can deliver, so a hostile block of n bytes can
// ask for 255n and no more (see compress.hpp).
Status read_block(RandomAccessFile* file, uint64_t file_size,
                  const BlockHandle& handle, BlockContents* result);

}  // namespace ambar

#endif  // AMBAR_FORMAT_HPP_
