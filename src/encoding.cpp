// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "encoding.hpp"

#include <array>

namespace ambar {

void put_varint32(std::string* dst, uint32_t value) {
  while (value >= 0x80) {
    dst->push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  dst->push_back(static_cast<char>(value));
}

void put_varint64(std::string* dst, uint64_t value) {
  while (value >= 0x80) {
    dst->push_back(static_cast<char>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  dst->push_back(static_cast<char>(value));
}

namespace {

// Shared by both widths.  `max_bytes` stops a corrupt file from making us read
// forever: a 32-bit varint is at most 5 bytes, a 64-bit one at most 10.
template <typename T>
bool get_varint(std::string_view* input, T* value, size_t max_bytes) {
  T result = 0;
  unsigned shift = 0;
  for (size_t i = 0; i < max_bytes; ++i) {
    if (i >= input->size()) {
      return false;  // ran off the end mid-number
    }
    const auto byte = static_cast<unsigned char>((*input)[i]);
    result |= static_cast<T>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      input->remove_prefix(i + 1);
      return true;
    }
    shift += 7;
  }
  return false;  // continuation bits never stopped
}

}  // namespace

bool get_varint32(std::string_view* input, uint32_t* value) {
  return get_varint<uint32_t>(input, value, 5);
}

bool get_varint64(std::string_view* input, uint64_t* value) {
  return get_varint<uint64_t>(input, value, 10);
}

void put_length_prefixed(std::string* dst, std::string_view value) {
  put_varint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

bool get_length_prefixed(std::string_view* input, std::string_view* value) {
  uint32_t length = 0;
  if (!get_varint32(input, &length)) {
    return false;
  }
  if (input->size() < length) {
    return false;  // the declared length outruns the buffer
  }
  *value = input->substr(0, length);
  input->remove_prefix(length);
  return true;
}

namespace {

// Castagnoli polynomial, bit-reversed (0x82F63B78).  The table is built once at
// first use rather than checked in as 1,024 magic numbers, so the polynomial is
// visible and the table cannot silently disagree with it.
const std::array<uint32_t, 256>& crc_table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    constexpr uint32_t kPolynomial = 0x82F63B78u;
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1) ? (crc >> 1) ^ kPolynomial : (crc >> 1);
      }
      t[i] = crc;
    }
    return t;
  }();
  return table;
}

}  // namespace

uint32_t crc32c_extend(uint32_t crc, std::string_view data) {
  const auto& table = crc_table();
  crc = ~crc;
  for (char c : data) {
    crc = table[(crc ^ static_cast<unsigned char>(c)) & 0xff] ^ (crc >> 8);
  }
  return ~crc;
}

uint32_t crc32c(std::string_view data) { return crc32c_extend(0, data); }

}  // namespace ambar
