// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "format.hpp"

#include <cstring>

#include "encoding.hpp"
#include "file.hpp"

namespace ambar {

void BlockHandle::encode_to(std::string* dst) const {
  put_varint64(dst, offset_);
  put_varint64(dst, size_);
}

Status BlockHandle::decode_from(std::string_view* input) {
  if (get_varint64(input, &offset_) && get_varint64(input, &size_)) {
    return Status::ok();
  }
  return Status::corruption("bad block handle");
}

void Footer::encode_to(std::string* dst) const {
  const size_t original = dst->size();

  metaindex_handle_.encode_to(dst);
  index_handle_.encode_to(dst);

  // Padded to a constant length: the reader seeks to a fixed distance from the
  // end of the file, so the footer cannot be allowed to vary with how large
  // the offsets happen to be.
  dst->resize(original + 2 * BlockHandle::kMaxEncodedLength, '\0');

  put_fixed32(dst, static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu));
  put_fixed32(dst, static_cast<uint32_t>(kTableMagicNumber >> 32));
}

Status Footer::decode_from(std::string_view* input) {
  if (input->size() < kEncodedLength) {
    return Status::corruption("file is too short to hold a table footer");
  }

  const char* magic_ptr = input->data() + kEncodedLength - 8;
  const uint64_t magic =
      (static_cast<uint64_t>(decode_fixed32(magic_ptr + 4)) << 32) |
      decode_fixed32(magic_ptr);
  if (magic != kTableMagicNumber) {
    // Checked before anything else is decoded.  Without it, an arbitrary file
    // of the right length would be read as a table, and its bytes would be
    // interpreted as offsets to seek to.
    return Status::corruption("not an Ambar table file (bad magic number)");
  }

  std::string_view remaining(input->data(), kEncodedLength - 8);
  Status status = metaindex_handle_.decode_from(&remaining);
  if (status.is_ok()) status = index_handle_.decode_from(&remaining);
  if (status.is_ok()) input->remove_prefix(kEncodedLength);
  return status;
}

Status read_block(RandomAccessFile* file, uint64_t file_size,
                  const BlockHandle& handle, BlockContents* result) {
  result->data = std::string_view();
  result->cachable = false;
  result->heap_allocated = false;

  const uint64_t n = handle.size();

  // Bounded by the file, not by a constant.  Every part of this arithmetic is
  // done in 64 bits and checked for overflow before it is used, because both
  // the offset and the size came out of the file and either may be anything.
  if (n > file_size ||
      handle.offset() > file_size - n ||
      n + kBlockTrailerSize > file_size - handle.offset()) {
    return Status::corruption(
        "block handle points outside the file: offset " +
        std::to_string(handle.offset()) + ", size " + std::to_string(n) +
        ", in a file of " + std::to_string(file_size) + " bytes");
  }

  std::unique_ptr<char[]> buffer(new char[n + kBlockTrailerSize]);
  std::string_view contents;
  Status status = file->read(handle.offset(), static_cast<size_t>(n) +
                                                  kBlockTrailerSize,
                             &contents, buffer.get());
  if (!status.is_ok()) return status;

  // The checksum is computed over what the read actually returned, and the
  // block that is published has to be those same bytes.  RandomAccessFile is
  // allowed to return a pointer into memory it did not copy into `scratch` --
  // that is what the separate result parameter is for, and a memory-mapped
  // implementation would do exactly that -- so publishing `buffer` regardless
  // would checksum one thing and hand back another.  The local implementation
  // always copies, which is why this costs nothing today and would have been a
  // silent disaster the day it did not.
  const char* data = contents.data();
  const bool read_into_buffer = (data == buffer.get());

  // The checksum covers the block *and* the compression byte that follows it,
  // so a flipped bit in the type cannot survive as a valid-looking block of
  // some other kind.
  const uint32_t stored = decode_fixed32(data + n + 1);
  const uint32_t actual = crc32c(std::string_view(data, static_cast<size_t>(n) + 1));
  if (stored != actual) {
    return Status::corruption("block checksum mismatch at offset " +
                              std::to_string(handle.offset()));
  }

  switch (static_cast<CompressionType>(data[n])) {
    case CompressionType::kNone:
      if (read_into_buffer) {
        result->data = std::string_view(buffer.get(), static_cast<size_t>(n));
        result->heap_allocated = true;
        result->cachable = true;
        buffer.release();  // ownership passes on via heap_allocated
      } else {
        // The data lives in memory the file owns — a mapping, say.  It is
        // neither ours to free nor ours to keep in a cache that outlives the
        // read, because the file may unmap it.
        result->data = std::string_view(data, static_cast<size_t>(n));
        result->heap_allocated = false;
        result->cachable = false;
      }
      return Status::ok();

    case CompressionType::kSnappy:
      // Reserved but not implemented.  Refusing is the whole point of having
      // written the byte from the first version: a reader that ignored it
      // would hand compressed bytes to the block parser and report corruption
      // somewhere far from the cause.
      return Status::not_supported(
          "table uses a compression this build cannot read");
  }
  return Status::corruption("unknown compression type in block trailer");
}

}  // namespace ambar
