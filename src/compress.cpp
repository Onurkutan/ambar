// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "compress.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "encoding.hpp"

namespace ambar {

namespace {

constexpr size_t kMinMatch = 4;
constexpr size_t kMaxOffset = 65535;  // two bytes
constexpr size_t kHashBits = 14;      // 16 K entries, 64 KiB of positions
constexpr size_t kHashSize = size_t{1} << kHashBits;

// Four bytes to a table slot.  The multiply spreads the high bits of the
// word over the whole table; the shift keeps the ones that spread best.
inline uint32_t hash4(const char* p) {
  uint32_t word;
  std::memcpy(&word, p, 4);
  return (word * 2654435761u) >> (32 - kHashBits);
}

// Writes a length in the LZ4 style: a nibble already placed in the token
// holds up to 14, fifteen means "and more follows", and the rest is bytes
// of 255 ending in a byte below 255.
void put_length(std::string* out, size_t length) {
  while (length >= 255) {
    out->push_back(static_cast<char>(255));
    length -= 255;
  }
  out->push_back(static_cast<char>(length));
}

// The nibble for a token, and whether a continuation follows.
inline uint8_t nibble_of(size_t length) {
  return length >= 15 ? 15 : static_cast<uint8_t>(length);
}

void emit_sequence(std::string* out, const char* literals, size_t literal_count,
                   size_t match_length, size_t offset) {
  // match_length is the full length, at least kMinMatch when a match is
  // present, and zero for the trailing literal-only sequence.
  const size_t match_code = match_length == 0 ? 0 : match_length - kMinMatch;
  const uint8_t token =
      static_cast<uint8_t>((nibble_of(literal_count) << 4) |
                           (match_length == 0 ? 0 : nibble_of(match_code)));
  out->push_back(static_cast<char>(token));
  if (literal_count >= 15) put_length(out, literal_count - 15);
  out->append(literals, literal_count);
  if (match_length == 0) return;
  out->push_back(static_cast<char>(offset & 0xff));
  out->push_back(static_cast<char>((offset >> 8) & 0xff));
  if (match_code >= 15) put_length(out, match_code - 15);
}

}  // namespace

size_t max_compressed_size(size_t n) {
  // Five bytes of varint length; then, at worst, one sequence of n
  // literals: a token, n / 255 + 1 length bytes, and the literals.
  return 5 + 1 + n / 255 + 1 + n;
}

void compress_block(std::string_view input, std::string* output) {
  const size_t n = input.size();
  // The length is written as a varint32 and positions are kept in 32
  // bits; a block is kilobytes to megabytes, and a caller with more than
  // four gigabytes has the wrong function.
  assert(n <= UINT32_MAX);
  put_varint32(output, static_cast<uint32_t>(n));
  if (n == 0) {
    // One empty literal run, so the decoder sees a well-formed stream and
    // not a bare length.
    output->push_back(0);
    return;
  }

  const char* const base = input.data();
  std::vector<uint32_t> table(kHashSize, 0);  // position + 1; zero is empty

  size_t literal_start = 0;  // first byte not yet emitted
  size_t pos = 0;
  // A match needs kMinMatch bytes, so the last window that can start one
  // begins kMinMatch bytes before the end; hashing stops there and never
  // reads past the input.
  const size_t last_hashable = n >= kMinMatch ? n - kMinMatch + 1 : 0;

  while (pos < last_hashable) {
    const uint32_t h = hash4(base + pos);
    const uint32_t candidate = table[h];
    table[h] = static_cast<uint32_t>(pos + 1);

    if (candidate == 0) {
      ++pos;
      continue;
    }
    const size_t match_pos = candidate - 1;
    const size_t offset = pos - match_pos;
    if (offset > kMaxOffset ||
        std::memcmp(base + match_pos, base + pos, kMinMatch) != 0) {
      ++pos;
      continue;
    }

    // Extend the match as far as it goes.
    size_t length = kMinMatch;
    while (pos + length < n && base[match_pos + length] == base[pos + length]) {
      ++length;
    }

    emit_sequence(output, base + literal_start, pos - literal_start, length,
                  offset);
    pos += length;
    literal_start = pos;

    // Positions inside the match were not hashed; hashing the one before
    // the new position keeps the table warm across a match without the
    // cost of hashing every skipped byte.
    if (pos >= 1 && pos - 1 < last_hashable) {
      table[hash4(base + pos - 1)] = static_cast<uint32_t>(pos);
    }
  }

  // Whatever is left is literals, always: a stream ends in a literal-only
  // sequence, even an empty one, so the decoder finds a token where it
  // expects one.
  emit_sequence(output, base + literal_start, n - literal_start, 0, 0);
}

Status decompress_block(std::string_view input, std::string* output) {
  output->clear();

  uint32_t declared = 0;
  if (!get_varint32(&input, &declared)) {
    return Status::corruption("compressed block has no length");
  }
  const size_t total = declared;
  // The declared length is the first thing an attacker controls, and the
  // first thing that was trusted: five bytes of input asked for four
  // gigabytes of reservation, which is a denial of service in the engine
  // and an out-of-memory report in the fuzzer.  The format bounds it: no
  // sequence produces more than 255 bytes of output per byte of input --
  // a length byte of 255 is the most any input byte can be worth -- so a
  // stream that declares more than that could not be delivering it.
  if (total > 255 * input.size()) {
    return Status::corruption(
        "compressed block declares more than its bytes could deliver");
  }
  output->reserve(total);

  const char* p = input.data();
  const char* const end = p + input.size();

  // Reads a continued length: bytes of 255 and a final byte below 255,
  // each read checked before it is made.  The sum is bounded by what the
  // caller can still produce, so a stream of 255s cannot run the count
  // past the size it declared -- or past the arithmetic.
  auto read_extra = [&](size_t* length, size_t room) -> bool {
    while (true) {
      if (p >= end) return false;
      const uint8_t byte = static_cast<uint8_t>(*p++);
      *length += byte;
      if (*length > room) return false;
      if (byte != 255) return true;
    }
  };

  while (true) {
    if (p >= end) {
      return Status::corruption("compressed block ends before its last token");
    }
    const uint8_t token = static_cast<uint8_t>(*p++);

    // Literals.
    size_t literal_count = token >> 4;
    const size_t room = total - output->size();
    if (literal_count == 15 && !read_extra(&literal_count, room)) {
      return Status::corruption("literal length overruns the block");
    }
    if (literal_count > room) {
      return Status::corruption("literals overrun the declared size");
    }
    if (static_cast<size_t>(end - p) < literal_count) {
      return Status::corruption("literals overrun the compressed input");
    }
    output->append(p, literal_count);
    p += literal_count;

    if (p == end) {
      // The final, literal-only sequence.  Everything declared must be
      // here, and nothing more.
      if (output->size() != total) {
        return Status::corruption(
            "compressed block delivers less than it declares");
      }
      return Status::ok();
    }

    // A match: offset, then length.
    if (end - p < 2) {
      return Status::corruption("match offset overruns the compressed input");
    }
    const size_t offset =
        static_cast<uint8_t>(p[0]) |
        (static_cast<size_t>(static_cast<uint8_t>(p[1])) << 8);
    p += 2;
    if (offset == 0 || offset > output->size()) {
      return Status::corruption("match reaches before the start of the block");
    }

    size_t match_length = (token & 0xf) + kMinMatch;
    const size_t match_room = total - output->size();
    if ((token & 0xf) == 15 && !read_extra(&match_length, match_room)) {
      return Status::corruption("match length overruns the block");
    }
    if (match_length > match_room) {
      return Status::corruption("match overruns the declared size");
    }

    // Byte by byte, because the source may overlap the destination: an
    // offset of one repeats the last byte.  Appending invalidates nothing
    // here since the capacity was reserved for the whole declared size,
    // but the index is what is kept, not a pointer, all the same.
    const size_t from = output->size() - offset;
    for (size_t i = 0; i < match_length; ++i) {
      output->push_back((*output)[from + i]);
    }
  }
}

}  // namespace ambar
