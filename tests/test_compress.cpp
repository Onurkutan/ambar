// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The block compressor and, more to the point, its decoder.  Round trips
// say the two agree; the rest of this file hands the decoder streams that
// no encoder produced -- lengths that overrun, offsets that reach before
// the start, streams cut off at every byte -- and requires an error every
// time, never a read past a buffer, which is what the fuzz target in
// fuzz/fuzz_compress.cpp then does with inputs nobody thought of.

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "compress.hpp"
#include "encoding.hpp"
#include "harness.hpp"

using namespace ambar;

namespace {

std::string round_trip(const std::string& input, Status* status) {
  std::string compressed;
  compress_block(input, &compressed);
  CHECK(compressed.size() <= max_compressed_size(input.size()));
  std::string out;
  *status = decompress_block(compressed, &out);
  return out;
}

void check_round_trip(const std::string& input, const char* what) {
  Status status;
  const std::string out = round_trip(input, &status);
  if (!status.is_ok()) {
    std::printf("    [%s] decompress failed: %s\n", what,
                status.to_string().c_str());
  }
  CHECK(status.is_ok());
  CHECK(out == input);
}

std::string random_bytes(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::string out(n, '\0');
  for (char& c : out) c = static_cast<char>(rng() & 0xff);
  return out;
}

// What a data block looks like: prefix-compressed keys and values with
// structure, so there is something to match.
std::string block_like(int entries) {
  std::string out;
  for (int i = 0; i < entries; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "key_%08d", i);
    out += key;
    out += "value_" + std::to_string(i) + "_";
    out.append(40, static_cast<char>('a' + i % 26));
  }
  return out;
}

// A stream assembled by hand: the varint length, then the bytes given.
// Every well-formed stream ends in a literal-only token, so a body that is
// meant to be complete ends with kEnd.
std::string stream(uint32_t declared, const std::string& body) {
  std::string out;
  put_varint32(&out, declared);
  return out + body;
}

const std::string kEnd("\x00", 1);

bool refused(const std::string& input) {
  std::string out;
  return decompress_block(input, &out).is_corruption();
}

// Refused, and for the reason given: the decoder has one check per way a
// stream can be wrong, and a stream built to overrun one of them must be
// caught by that one.  A refusal from a later check would mean the earlier
// one had let the overrun through -- and read out of bounds on the way,
// which nothing here but a sanitizer would see.
bool refused_because(const std::string& input, const char* reason) {
  std::string out;
  const Status status = decompress_block(input, &out);
  if (!status.is_corruption()) {
    std::printf("    accepted, wanted \"%s\"\n", reason);
    return false;
  }
  if (status.to_string().find(reason) == std::string::npos) {
    std::printf("    refused with \"%s\", wanted \"%s\"\n",
                status.to_string().c_str(), reason);
    return false;
  }
  return true;
}

}  // namespace

TEST(compress, round_trips_the_shapes_a_block_takes) {
  check_round_trip("", "empty");
  check_round_trip("a", "one byte");
  check_round_trip("abc", "shorter than a match");
  check_round_trip("abcd", "exactly a match");
  check_round_trip(std::string(1000, 'x'), "one run");
  check_round_trip("abcabcabcabcabcabcabcabc", "short period");
  check_round_trip(block_like(200), "a data block");
  check_round_trip(random_bytes(4096, 1), "incompressible");
  check_round_trip(random_bytes(300, 2) + std::string(300, 'z') +
                       random_bytes(300, 3),
                   "a run in the middle");
  // Every literal-count and match-length nibble, including the extensions
  // past fifteen and past 255 + 15.
  for (int literals : {0, 1, 14, 15, 16, 100, 269, 270, 271, 600}) {
    for (int match : {4, 5, 18, 19, 20, 100, 273, 274, 275, 700}) {
      const std::string input = random_bytes(static_cast<size_t>(literals),
                                             static_cast<uint32_t>(literals)) +
                                std::string(static_cast<size_t>(match), 'q');
      check_round_trip(input, "nibble sweep");
    }
  }
}

TEST(compress, compresses_what_has_structure_and_not_what_does_not) {
  std::string compressed;
  compress_block(block_like(200), &compressed);
  const size_t structured = compressed.size();
  compressed.clear();
  compress_block(random_bytes(block_like(200).size(), 7), &compressed);
  const size_t random = compressed.size();
  const size_t n = block_like(200).size();
  // The block-like input has a repeated 40-byte run in every entry and
  // shares prefixes; less than half its size is the least to expect.
  CHECK(structured < n / 2);
  // Random bytes cannot shrink; the overhead is the bound.
  CHECK(random >= n);
  CHECK(random <= max_compressed_size(n));
  std::printf("    %zu bytes: structured -> %zu (%.2fx), random -> %zu\n", n,
              structured,
              static_cast<double>(n) / static_cast<double>(structured), random);
}

TEST(compress, a_run_is_one_sequence_with_an_overlapping_match) {
  std::string compressed;
  compress_block(std::string(10000, 'r'), &compressed);
  // Length varint, one literal 'r' as a token+literal, then a match at
  // offset 1 for 9999 bytes: a token, two offset bytes, and the length
  // extension of (9999 - 4 - 15) / 255 + 1 bytes.  About 45 bytes.
  CHECK(compressed.size() < 60);
  std::string out;
  CHECK_OK(decompress_block(compressed, &out));
  CHECK(out == std::string(10000, 'r'));
}

TEST(compress, refuses_a_stream_cut_off_at_every_byte) {
  std::string compressed;
  compress_block(block_like(50), &compressed);
  // Every proper prefix is refused: a stream that ends inside a length,
  // inside literals, inside an offset, or before its last token.
  for (size_t cut = 0; cut < compressed.size(); ++cut) {
    std::string out;
    const Status status = decompress_block(compressed.substr(0, cut), &out);
    if (!status.is_corruption()) {
      std::printf("    prefix of %zu bytes was accepted\n", cut);
    }
    CHECK(status.is_corruption());
  }
  // And one byte too many is refused too: the last sequence must end the
  // stream exactly.
  std::string out;
  CHECK(decompress_block(compressed + "x", &out).is_corruption());
}

TEST(compress, refuses_lengths_that_overrun) {
  // Declares 4 bytes, delivers 6 literals: caught by the declared size,
  // before the literals are touched.
  CHECK(refused_because(stream(4, std::string("\x60", 1) + "abcdef"),
                        "literals overrun the declared size"));
  // Declares 100, delivers 4 literals then ends: everything read, too
  // little produced.
  CHECK(refused_because(stream(100, std::string("\x40", 1) + "abcd"),
                        "delivers less than it declares"));
  // Declares 100, the token claims 6 literals, the input holds 4: the
  // declared size has room, so only the input bound can catch this -- and
  // it must, before the copy, or the copy reads two bytes past the end.
  CHECK(refused_because(stream(100, std::string("\x60", 1) + "abcd"),
                        "literals overrun the compressed input"));
  // Literal extension bytes of 255 without end, past the declared size:
  // the extension is bounded as it is read, so the sum stops at the room
  // and never reaches the copy.
  CHECK(refused_because(stream(50, std::string("\xf0\xff\xff\xff\xff\xff", 6)),
                        "literal length overruns the block"));
  // A match longer than what remains: 4 literals, then a match of 4 + 15
  // + 200 at offset 1, into a block that declared 10.  The nibble's 19
  // alone is past the 6 that remain, so the extension is refused as it is
  // read.
  CHECK(refused_because(stream(10, std::string("\x4f", 1) + "abcd" +
                                         std::string("\x01\x00", 2) +
                                         std::string("\xc8", 1) + kEnd),
                        "match length overruns the block"));
  // And a match that fits its nibble but not the room: 4 literals, then a
  // match of 4 + 3 at offset 1, into a block that declared 8: one byte
  // over, caught before the copy.
  CHECK(refused_because(stream(8, std::string("\x43", 1) + "abcd" +
                                        std::string("\x01\x00", 2) + kEnd),
                        "match overruns the declared size"));
  // The same match into a block that declared enough is accepted, which
  // is what says the refusal above was for the length and not the shape.
  std::string out;
  CHECK_OK(decompress_block(stream(223, std::string("\x4f", 1) + "abcd" +
                                            std::string("\x01\x00", 2) +
                                            std::string("\xc8", 1) + kEnd),
                            &out));
  CHECK_EQ(out.size(), size_t{223});
  CHECK(out == "abc" + std::string(220, 'd'));
  // Declares zero and delivers a literal.
  CHECK(refused(stream(0, std::string("\x10", 1) + "a")));
  // A declared length the varint cannot hold: five bytes with the high
  // bit set throughout is not a varint32.
  CHECK(refused(std::string("\xff\xff\xff\xff\xff\xff", 6)));
  // No length at all.
  CHECK(refused(""));
}

TEST(compress, refuses_offsets_that_reach_before_the_start) {
  // 4 literals, then a match at offset 5: one byte before the start.
  CHECK(refused(stream(8, std::string("\x40", 1) + "abcd" +
                                 std::string("\x05\x00", 2) + kEnd)));
  // Offset zero is never valid.
  CHECK(refused(stream(8, std::string("\x40", 1) + "abcd" +
                                 std::string("\x00\x00", 2) + kEnd)));
  // Offset exactly at the start is fine: the match copies from byte 0.
  std::string out;
  CHECK_OK(decompress_block(stream(8, std::string("\x40", 1) + "abcd" +
                                            std::string("\x04\x00", 2) + kEnd),
                            &out));
  CHECK(out == "abcdabcd");
  // A match before any literal has nothing to copy from.
  CHECK(refused(stream(4, std::string("\x00", 1) +
                                 std::string("\x01\x00", 2) + kEnd)));
  // And a stream that stops after a match, with no closing literal token,
  // is not well-formed: the encoder always writes one.
  CHECK(refused(stream(8, std::string("\x40", 1) + "abcd" +
                                 std::string("\x04\x00", 2))));
}

TEST(compress, a_truncated_length_is_an_error_not_a_hang) {
  // A stream whose literal extension is one byte short of the value it
  // implies is refused at the end of input, not looped on.  The declared
  // sizes are within what the bytes could deliver, so it is the
  // truncation that refuses them and not the bound below.
  CHECK(refused_because(stream(500, std::string("\xf0\xff", 2)),
                        "literal length overruns the block"));
  CHECK(refused_because(stream(200, std::string("\xf0", 1)),
                        "literal length overruns the block"));
}

TEST(compress, refuses_a_declared_size_the_bytes_could_not_deliver) {
  // Five bytes that declare four gigabytes: the first draft reserved
  // them.  No input byte is worth more than 255 bytes of output, so the
  // bound is the format's and refuses nothing an encoder can produce.
  CHECK(refused_because(std::string("\xff\xff\xff\xff\x0f", 5),
                        "declares more than its bytes could deliver"));
  CHECK(refused_because(stream(1000, std::string("\x00", 1)),
                        "declares more than its bytes could deliver"));
  // Exactly at the bound is allowed through to the checks that follow: a
  // one-byte body may declare 255.
  CHECK(refused_because(stream(255, std::string("\x00", 1)),
                        "delivers less than it declares"));
  // And the encoder's own output never trips it, whatever it compresses.
  std::string compressed;
  compress_block(std::string(100000, 'r'), &compressed);
  std::string out;
  CHECK_OK(decompress_block(compressed, &out));
  CHECK_EQ(out.size(), size_t{100000});
}

TEST(compress, a_wide_copy_stays_inside_the_declared_size) {
  // The decoder copies eight bytes at a time when there is a step to spill
  // into past the literals in the input and past where they land in the
  // output, and one at a time otherwise.  A stream an encoder wrote never
  // has the first without the second, so the output's guard is reached
  // only by a hostile stream: literals that end within a step of the
  // declared size, with bytes to spare after them -- garbage, which the
  // decoder refuses when it reaches it.  Without the guard a wide copy
  // would already have written past the declared size, into whatever lies
  // beyond the string; a sanitizer sees that, and this build sees the
  // terminator the spill overwrites.  4095 leaves the string's storage no
  // slack for a spill to hide in, and the garbage is not zero.
  const size_t declared = 4095;
  std::string body;
  body += std::string("\x10", 1) + "a";              // one literal, and a
  body += std::string("\x01\x00", 2);                 // match of four from
                                                    // one behind: "aaaaa"
  body += std::string("\xf0", 1);                    // 15 literals and more:
  body += std::string(15, static_cast<char>(255));  // 4090 - 15 = 4075
  body += std::string("\xfa", 1);                    // = 15 * 255 + 250
  body += std::string(4090, 'b');                   // which end at 4095
  body += std::string(8, static_cast<char>(255));   // then an offset of 65535
  std::string out;
  const Status status = decompress_block(stream(declared, body), &out);
  CHECK(status.is_corruption());
  CHECK(status.to_string().find("match reaches before the start") !=
        std::string::npos);
  CHECK_EQ(out.size(), declared);
  CHECK_EQ(out.c_str()[out.size()], '\0');
}
