// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Arbitrary bytes as a compressed block, and as an uncompressed one.
//
// The decoder is handed the input as it is: any length field, any offset,
// any cut-off, and the only acceptable outcomes are a decoded block or a
// corruption status -- never a read past the buffer, which is what the
// sanitizers this target is built with would report.  Then the input is
// taken as plain data, compressed, and decoded again, and the result has
// to be the input: the encoder is under test too, since a block it encodes
// wrongly is a block the engine would serve wrongly with every checksum
// passing.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "compress.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  std::string decoded;
  const ambar::Status status = ambar::decompress_block(input, &decoded);
  if (!status.is_ok() && !status.is_corruption()) {
    // Anything but ok or corruption is a category the decoder must not
    // produce.
    std::abort();
  }

  std::string compressed;
  ambar::compress_block(input, &compressed);
  if (compressed.size() > ambar::max_compressed_size(size)) std::abort();
  std::string round_trip;
  if (!ambar::decompress_block(compressed, &round_trip).is_ok()) std::abort();
  if (round_trip != input) std::abort();
  return 0;
}
