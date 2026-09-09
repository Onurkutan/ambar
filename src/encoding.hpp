// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// On-disk primitives: integers, checksums, and the framing every record uses.
//
// Everything Ambar writes goes through here, so the format is defined in one
// place and both writer and reader are forced to agree.  All integers are
// little-endian on disk regardless of the host, so a database file written on
// one machine reads correctly on another.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace ambar {

// ------------------------------------------------------------ fixed width --
// Writes into a caller-supplied buffer.  Needed where a field is patched
// after the surrounding bytes already exist -- a write batch learns its
// sequence number only once it reaches the log, long after its records were
// appended.
inline void encode_fixed32(char* dst, uint32_t value) {
  dst[0] = static_cast<char>(value & 0xff);
  dst[1] = static_cast<char>((value >> 8) & 0xff);
  dst[2] = static_cast<char>((value >> 16) & 0xff);
  dst[3] = static_cast<char>((value >> 24) & 0xff);
}

inline void encode_fixed64(char* dst, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<char>((value >> (8 * i)) & 0xff);
  }
}

inline void put_fixed32(std::string* dst, uint32_t value) {
  char buf[4];
  encode_fixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

inline void put_fixed64(std::string* dst, uint64_t value) {
  char buf[8];
  encode_fixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

inline uint32_t decode_fixed32(const char* p) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(p[0]))) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

inline uint64_t decode_fixed64(const char* p) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return value;
}

// ---------------------------------------------------------------- varint ---
// Small numbers should cost small space: key and value lengths are usually far
// below 128, and a 7-bit-per-byte encoding spends one byte on those instead of
// four.  Across millions of records that is most of a file's overhead.
void put_varint32(std::string* dst, uint32_t value);
void put_varint64(std::string* dst, uint64_t value);

// Reads a varint from [*input]; on success advances *input past it.  Returns
// false if the buffer ends mid-varint or the encoding is longer than the type
// allows -- both of which a truncated or corrupt file can produce.
bool get_varint32(std::string_view* input, uint32_t* value);
bool get_varint64(std::string_view* input, uint64_t* value);

// Decodes a varint this process wrote, reading exactly its own bytes and not
// one more.
//
// The bounded `get_varint32` above needs to know how much buffer there is.
// Inside an arena there is no such boundary to hand it: a record is followed
// immediately by the next record, and the last one by the end of the block.
// Passing a generous guess reads past short records -- invisibly, because the
// sanitizer only tracks the block, not the records inside it.  Terminating on
// the high bit instead makes the read exactly as long as the number.
//
// Returns the position just past the varint, or nullptr if the encoding runs
// longer than a uint32 can hold, which for data written by this process means
// memory corruption rather than a malformed file.
inline const char* decode_varint32_exact(const char* p, uint32_t* value) {
  uint32_t result = 0;
  for (unsigned shift = 0; shift <= 28; shift += 7) {
    const uint32_t byte = static_cast<unsigned char>(*p++);
    result |= (byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return p;
    }
  }
  return nullptr;
}

// A length-prefixed byte string, the shape every key and value is stored in.
void put_length_prefixed(std::string* dst, std::string_view value);
bool get_length_prefixed(std::string_view* input, std::string_view* value);

// -------------------------------------------------------------- checksum ---
// CRC32C (Castagnoli).  Chosen over CRC32 because it is the polynomial with
// hardware support on x86 and ARM, and over a cryptographic hash because the
// threat here is a torn write or a bad sector, not an adversary.
uint32_t crc32c(std::string_view data);
uint32_t crc32c_extend(uint32_t crc, std::string_view data);

}  // namespace ambar
