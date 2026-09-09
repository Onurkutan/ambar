// Internal keys: the representation everything below the public API speaks.
//
// A user key on its own cannot answer "which of these two copies is newer?", and
// an LSM tree asks that question constantly -- of the memtable and a table, of
// two tables in the same level, of every input to a compaction.  So the key that
// reaches disk carries its own version:
//
//     internal_key := user_key | (sequence << 8 | type)   [8 bytes, little-endian]
//
// Ordered by user key ascending, then by the trailing 8 bytes *descending*, so
// the newest version of a key sorts first and a merging iterator gets recency
// for free.  Packing the type into the low byte puts a tombstone immediately
// ahead of the versions it shadows, which is the order the merge wants.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "encoding.hpp"

namespace ambar {

enum class ValueType : uint8_t {
  kDeletion = 0,
  kValue = 1,
};

// The largest type byte, used when building a lookup key: since the trailing
// word sorts descending, pairing a sequence with the *highest* type gives a key
// that sorts at or before every real entry with that sequence.
constexpr ValueType kValueTypeForSeek = ValueType::kValue;

using SequenceNumber = uint64_t;

// Sequence numbers occupy 56 bits; the low 8 belong to the type.
constexpr SequenceNumber kMaxSequenceNumber = (uint64_t{1} << 56) - 1;

inline uint64_t pack_sequence_and_type(SequenceNumber seq, ValueType type) {
  return (seq << 8) | static_cast<uint8_t>(type);
}

// A key plus its version, kept apart before being serialised.
struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;
};

inline void append_internal_key(std::string* dst, const ParsedInternalKey& key) {
  dst->append(key.user_key.data(), key.user_key.size());
  put_fixed64(dst, pack_sequence_and_type(key.sequence, key.type));
}

inline std::string make_internal_key(std::string_view user_key,
                                     SequenceNumber sequence, ValueType type) {
  std::string out;
  out.reserve(user_key.size() + 8);
  append_internal_key(&out, {user_key, sequence, type});
  return out;
}

// Everything but the trailing 8 bytes.  Undefined for a key shorter than that,
// which is why parse_internal_key exists for untrusted input.
inline std::string_view extract_user_key(std::string_view internal_key) {
  return internal_key.substr(0, internal_key.size() - 8);
}

inline SequenceNumber extract_sequence(std::string_view internal_key) {
  return decode_fixed64(internal_key.data() + internal_key.size() - 8) >> 8;
}

inline ValueType extract_value_type(std::string_view internal_key) {
  return static_cast<ValueType>(
      decode_fixed64(internal_key.data() + internal_key.size() - 8) & 0xff);
}

// Validating parse, for keys that came off disk.  A file can be corrupt in ways
// that produce a key too short to hold a trailer or a type byte that is neither
// value nor deletion, and neither should reach the rest of the engine.
inline bool parse_internal_key(std::string_view internal_key,
                               ParsedInternalKey* parsed) {
  if (internal_key.size() < 8) return false;
  const uint64_t trailer =
      decode_fixed64(internal_key.data() + internal_key.size() - 8);
  const auto type = static_cast<ValueType>(trailer & 0xff);
  if (type != ValueType::kValue && type != ValueType::kDeletion) return false;
  parsed->user_key = internal_key.substr(0, internal_key.size() - 8);
  parsed->sequence = trailer >> 8;
  parsed->type = type;
  return true;
}

// The ordering the whole engine is built on.  Returns <0, 0 or >0.
//
// User keys compare bytewise; ties break on the trailer in *reverse*, so a
// higher sequence -- a newer write -- sorts first.
inline int compare_internal_keys(std::string_view a, std::string_view b) {
  const std::string_view user_a = extract_user_key(a);
  const std::string_view user_b = extract_user_key(b);
  if (const int by_user = user_a.compare(user_b); by_user != 0) {
    return by_user;
  }
  const uint64_t trailer_a = decode_fixed64(a.data() + a.size() - 8);
  const uint64_t trailer_b = decode_fixed64(b.data() + b.size() - 8);
  if (trailer_a > trailer_b) return -1;  // newer first
  if (trailer_a < trailer_b) return 1;
  return 0;
}

struct InternalKeyComparator {
  bool operator()(std::string_view a, std::string_view b) const {
    return compare_internal_keys(a, b) < 0;
  }
};

// A key that sorts at or before every entry for `user_key` visible at
// `snapshot`: the seek target for a point lookup.
inline std::string make_lookup_key(std::string_view user_key,
                                   SequenceNumber snapshot) {
  return make_internal_key(user_key, snapshot, kValueTypeForSeek);
}

}  // namespace ambar
