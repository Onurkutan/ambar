// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Arbitrary bytes into VersionEdit::decode_from, which is what recovery does
// with every record it reads out of a MANIFEST.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "version_edit.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  ambar::VersionEdit edit;
  if (!edit.decode_from(input).is_ok()) return 0;

  // An edit the reader accepted must be one the writer can produce, and that
  // must read back: decode(encode(decode(x))) cannot fail once decode(x)
  // succeeded.  If it does, one side of the format disagrees with the other.
  std::string encoded;
  edit.encode_to(&encoded);
  ambar::VersionEdit again;
  if (!again.decode_from(encoded).is_ok()) std::abort();
  (void)again.debug_string();
  return 0;
}
