// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "compress.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>

#include "encoding.hpp"

namespace ambar {

namespace {

constexpr size_t kMinMatch = 4;
constexpr size_t kMaxOffset = 65535;  // two bytes

// The hash table is sized to the input, a slot per position between these
// bounds, because it is zeroed for every block: 64 KiB of positions for a
// 4 KiB block cost more to clear than the block cost to match, and were
// measured to -- tools/codec_bench, before and after.  The largest table
// is 16 K entries, 64 KiB, on the stack.
constexpr int kMinHashBits = 8;
constexpr int kMaxHashBits = 14;

int hash_bits_for(size_t n) {
  int bits = kMinHashBits;
  while (bits < kMaxHashBits && (size_t{1} << bits) < n) ++bits;
  return bits;
}

// Four bytes to a table slot.  The multiply spreads the high bits of the
// word over the whole table; the shift keeps the ones that spread best.
inline uint32_t hash4(const char* p, int bits) {
  uint32_t word;
  std::memcpy(&word, p, 4);
  return (word * 2654435761u) >> (32 - bits);
}

// The encoder writes through a pointer into a string sized for the worst
// case up front, and cuts the string to what it wrote at the end: a
// push_back per byte was a capacity check per byte, and measurable.

// Writes a length in the LZ4 style: a nibble already placed in the token
// holds up to 14, fifteen means "and more follows", and the rest is bytes
// of 255 ending in a byte below 255.
inline char* put_length(char* out, size_t length) {
  while (length >= 255) {
    *out++ = static_cast<char>(255);
    length -= 255;
  }
  *out++ = static_cast<char>(length);
  return out;
}

// The nibble for a token, and whether a continuation follows.
inline uint8_t nibble_of(size_t length) {
  return length >= 15 ? 15 : static_cast<uint8_t>(length);
}

// Copies n bytes eight at a time, and so writes up to seven bytes past
// to + n and reads up to seven past from + n -- a whole step when n is
// zero, since it always takes one: the caller has shown there is a step
// of room for both, and what spills is overwritten by the sequence that
// follows.  `from` must be at least kWildCopyStep behind `to`, or a step
// would read bytes the step before it had not written yet.  This is what
// LZ4's speed is made of: a copy of a few bytes costs a call and a
// length dispatch as memcpy, and one instruction as this.  A do-while,
// because most copies are one step and the test on the way in cost a
// quarter of the decoder's rate when it was a while.
constexpr size_t kWildCopyStep = 8;

inline void wild_copy(char* to, const char* from, size_t n) {
  char* const stop = to + n;
  do {
    std::memcpy(to, from, kWildCopyStep);
    to += kWildCopyStep;
    from += kWildCopyStep;
  } while (to < stop);
}

char* emit_sequence(char* out, const char* literals, size_t literal_count,
                    size_t match_length, size_t offset) {
  // match_length is the full length, at least kMinMatch when a match is
  // present, and zero for the trailing literal-only sequence.
  const size_t match_code = match_length == 0 ? 0 : match_length - kMinMatch;
  const uint8_t token =
      static_cast<uint8_t>((nibble_of(literal_count) << 4) |
                           (match_length == 0 ? 0 : nibble_of(match_code)));
  *out++ = static_cast<char>(token);
  if (literal_count >= 15) out = put_length(out, literal_count - 15);
  std::memcpy(out, literals, literal_count);
  out += literal_count;
  if (match_length == 0) return out;
  *out++ = static_cast<char>(offset & 0xff);
  *out++ = static_cast<char>((offset >> 8) & 0xff);
  if (match_code >= 15) out = put_length(out, match_code - 15);
  return out;
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
  // Room for the worst case, then a pointer; cut to size at the end.
  const size_t start = output->size();
  output->resize(start + max_compressed_size(n));
  char* out = output->data() + start;
  if (n == 0) {
    // One empty literal run, so the decoder sees a well-formed stream and
    // not a bare length.
    *out++ = 0;
    output->resize(static_cast<size_t>(out - output->data()));
    return;
  }

  const char* const base = input.data();
  const int bits = hash_bits_for(n);
  uint32_t table[size_t{1} << kMaxHashBits];  // position + 1; zero is empty
  std::memset(table, 0, sizeof(uint32_t) << bits);

  size_t literal_start = 0;  // first byte not yet emitted
  size_t pos = 0;
  // A match needs kMinMatch bytes, so the last window that can start one
  // begins kMinMatch bytes before the end; hashing stops there and never
  // reads past the input.
  const size_t last_hashable = n >= kMinMatch ? n - kMinMatch + 1 : 0;

  while (pos < last_hashable) {
    const uint32_t h = hash4(base + pos, bits);
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

    // Extend the match as far as it goes: eight bytes at a time while
    // eight remain and agree, then one at a time to the first that does
    // not.  A byte-order-free way to compare a word, and the compilers
    // this builds under turn a memcmp of eight into one.
    size_t length = kMinMatch;
    while (pos + length + 8 <= n &&
           std::memcmp(base + match_pos + length, base + pos + length, 8) == 0) {
      length += 8;
    }
    while (pos + length < n && base[match_pos + length] == base[pos + length]) {
      ++length;
    }

    out = emit_sequence(out, base + literal_start, pos - literal_start,
                        length, offset);
    pos += length;
    literal_start = pos;

    // Positions inside the match were not hashed; hashing the one before
    // the new position keeps the table warm across a match without the
    // cost of hashing every skipped byte.
    if (pos >= 1 && pos - 1 < last_hashable) {
      table[hash4(base + pos - 1, bits)] = static_cast<uint32_t>(pos);
    }
  }

  // Whatever is left is literals, always: a stream ends in a literal-only
  // sequence, even an empty one, so the decoder finds a token where it
  // expects one.
  out = emit_sequence(out, base + literal_start, n - literal_start, 0, 0);
  output->resize(static_cast<size_t>(out - output->data()));
}

Status compressed_length(std::string_view input, size_t* length,
                         std::string_view* body) {
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
  *length = total;
  *body = input;
  return Status::ok();
}

Status decompress_block(std::string_view input, std::string* output) {
  output->clear();
  size_t total = 0;
  std::string_view body;
  Status status = compressed_length(input, &total, &body);
  if (!status.is_ok()) return status;
  // Sized once, written in place.  On a refusal the string holds whatever
  // was written before it, which no caller reads.
  output->resize(total);
  return decompress_into(body, output->data(), total);
}

Status decompress_into(std::string_view input, char* out, size_t total) {
  // Every write below is bounded by what `total` has room for, checked
  // before the copy, so there is no per-byte check to pay; `produced` is
  // how far the writing has got.  A stream that declared more than
  // `total` -- a caller that passed a smaller buffer than the length it
  // was given -- overruns the room and is refused like any other.
  size_t produced = 0;

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
    const size_t room = total - produced;
    if (literal_count == 15 && !read_extra(&literal_count, room)) {
      return Status::corruption("literal length overruns the block");
    }
    if (literal_count > room) {
      return Status::corruption("literals overrun the declared size");
    }
    if (static_cast<size_t>(end - p) < literal_count) {
      return Status::corruption("literals overrun the compressed input");
    }
    // Eight at a time when the input has a step to spare past the
    // literals and the output a step to spare past where they land;
    // exactly, otherwise, which is how every stream's last run goes.
    if (static_cast<size_t>(end - p) >= literal_count + kWildCopyStep &&
        room >= literal_count + kWildCopyStep) {
      wild_copy(out + produced, p, literal_count);
    } else {
      std::memcpy(out + produced, p, literal_count);
    }
    produced += literal_count;
    p += literal_count;

    if (p == end) {
      // The final, literal-only sequence.  Everything declared must be
      // here, and nothing more.
      if (produced != total) {
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
    if (offset == 0 || offset > produced) {
      return Status::corruption("match reaches before the start of the block");
    }

    size_t match_length = (token & 0xf) + kMinMatch;
    const size_t match_room = total - produced;
    if ((token & 0xf) == 15 && !read_extra(&match_length, match_room)) {
      return Status::corruption("match length overruns the block");
    }
    if (match_length > match_room) {
      return Status::corruption("match overruns the declared size");
    }

    // The source is behind the destination by `offset`.  With a step or
    // more between them and a step of room past the match, eight at a
    // time; with less between them the match repeats what it is
    // producing -- an offset of one repeats the last byte -- and goes
    // byte by byte, since a wider copy would read bytes it had not yet
    // written.  Nearly every match in data with structure takes the
    // first path, and the decoder's whole cost was the third when every
    // match took it.
    const char* const from = out + produced - offset;
    char* const to = out + produced;
    if (offset >= kWildCopyStep && match_room >= match_length + kWildCopyStep) {
      wild_copy(to, from, match_length);
    } else if (offset >= match_length) {
      std::memcpy(to, from, match_length);
    } else {
      for (size_t i = 0; i < match_length; ++i) to[i] = from[i];
    }
    produced += match_length;
  }
}

}  // namespace ambar
