#include "harness.hpp"

#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>

#include "../src/encoding.hpp"

using namespace ambar;

TEST(Encoding, fixed_width_round_trips) {
  std::string buf;
  put_fixed32(&buf, 0x01020304u);
  put_fixed64(&buf, 0x0102030405060708ull);
  CHECK_EQ(buf.size(), size_t{12});
  CHECK_EQ(decode_fixed32(buf.data()), 0x01020304u);
  CHECK_EQ(decode_fixed64(buf.data() + 4), 0x0102030405060708ull);
}

TEST(Encoding, fixed_width_is_little_endian_on_every_host) {
  // The byte order is part of the file format, so it is asserted rather than
  // inherited from whatever the build machine happens to be.
  std::string buf;
  put_fixed32(&buf, 1u);
  CHECK_EQ(static_cast<unsigned char>(buf[0]), 1u);
  CHECK_EQ(static_cast<unsigned char>(buf[3]), 0u);
}

TEST(Encoding, varint_round_trips_across_the_range) {
  const uint64_t values[] = {0, 1, 127, 128, 300, 16383, 16384,
                             uint64_t{1} << 30, uint64_t{1} << 40,
                             std::numeric_limits<uint64_t>::max()};
  for (uint64_t value : values) {
    std::string buf;
    put_varint64(&buf, value);
    std::string_view input(buf);
    uint64_t decoded = 0;
    CHECK(get_varint64(&input, &decoded));
    CHECK_EQ(decoded, value);
    CHECK(input.empty());
  }
}

TEST(Encoding, varint_spends_one_byte_on_small_numbers) {
  // The reason varints exist here; if this regresses, every file grows.
  std::string buf;
  put_varint32(&buf, 127);
  CHECK_EQ(buf.size(), size_t{1});
  buf.clear();
  put_varint32(&buf, 128);
  CHECK_EQ(buf.size(), size_t{2});
}

TEST(Encoding, truncated_varint_is_rejected_not_guessed) {
  std::string buf;
  put_varint32(&buf, 300);           // two bytes
  std::string truncated = buf.substr(0, 1);  // keep only the continuation byte
  std::string_view input(truncated);
  uint32_t value = 0;
  CHECK(!get_varint32(&input, &value));
}

TEST(Encoding, varint_longer_than_the_type_is_rejected) {
  // A corrupt file can contain an unbounded run of continuation bits; the
  // decoder must stop rather than read past its own type.
  const std::string evil(10, static_cast<char>(0x80));
  std::string_view input(evil);
  uint32_t value = 0;
  CHECK(!get_varint32(&input, &value));
}

TEST(Encoding, length_prefixed_round_trips_including_empty_and_nul) {
  const std::string with_nul("a\0b", 3);
  std::string buf;
  put_length_prefixed(&buf, "hello");
  put_length_prefixed(&buf, "");
  put_length_prefixed(&buf, with_nul);

  std::string_view input(buf);
  std::string_view out;
  CHECK(get_length_prefixed(&input, &out));
  CHECK_EQ(std::string(out), std::string("hello"));
  CHECK(get_length_prefixed(&input, &out));
  CHECK_EQ(out.size(), size_t{0});
  CHECK(get_length_prefixed(&input, &out));
  CHECK_EQ(std::string(out), with_nul);  // values are bytes, not C strings
  CHECK(input.empty());
}

TEST(Encoding, length_prefix_that_outruns_the_buffer_is_rejected) {
  std::string buf;
  put_varint32(&buf, 100);  // claims 100 bytes...
  buf.append("short");      // ...but supplies 5
  std::string_view input(buf);
  std::string_view out;
  CHECK(!get_length_prefixed(&input, &out));
}

TEST(Crc32c, matches_the_published_test_vectors) {
  // From RFC 3720 appendix B.4.  Checked against an external reference so a
  // wrong polynomial cannot pass by agreeing with itself.
  CHECK_EQ(crc32c(std::string(32, '\0')), 0x8A9136AAu);
  CHECK_EQ(crc32c(std::string(32, '\xff')), 0x62A8AB43u);
  CHECK_EQ(crc32c(""), 0u);
  CHECK_EQ(crc32c("a"), 0xC1D04330u);
  CHECK_EQ(crc32c("123456789"), 0xE3069283u);
}

TEST(Crc32c, extending_in_pieces_equals_hashing_at_once) {
  const std::string data = "the quick brown fox jumps over the lazy dog";
  const uint32_t whole = crc32c(data);
  uint32_t piecewise = 0;
  for (size_t i = 0; i < data.size(); i += 7) {
    piecewise = crc32c_extend(piecewise, std::string_view(data).substr(i, 7));
  }
  CHECK_EQ(piecewise, whole);
}

TEST(Crc32c, a_single_flipped_bit_changes_the_checksum) {
  // The property the format actually relies on when a sector goes bad.
  std::mt19937 rng(1234);
  std::string data(256, '\0');
  for (char& c : data) c = static_cast<char>(rng() & 0xff);
  const uint32_t original = crc32c(data);
  for (int trial = 0; trial < 64; ++trial) {
    std::string mutated = data;
    const size_t byte = rng() % mutated.size();
    mutated[byte] = static_cast<char>(mutated[byte] ^ (1 << (rng() % 8)));
    CHECK(crc32c(mutated) != original);
  }
}
