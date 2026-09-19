// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "dbformat.hpp"

#include <cstring>

namespace ambar {

LookupKey::LookupKey(std::string_view user_key, SequenceNumber snapshot) {
  const size_t internal_size = user_key.size() + 8;
  // The varint32 of the length is at most five bytes.
  const size_t needed = 5 + internal_size;
  char* dst = needed <= sizeof(space_) ? space_ : new char[needed];
  start_ = dst;
  dst = encode_varint32(dst, static_cast<uint32_t>(internal_size));
  kstart_ = dst;
  if (!user_key.empty()) {  // an empty view may carry a null pointer
    std::memcpy(dst, user_key.data(), user_key.size());
    dst += user_key.size();
  }
  encode_fixed64(dst, pack_sequence_and_type(snapshot, kValueTypeForSeek));
  end_ = dst + 8;
}

LookupKey::~LookupKey() {
  if (start_ != space_) delete[] start_;
}

}  // namespace ambar
