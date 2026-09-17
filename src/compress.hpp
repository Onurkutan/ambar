// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// Block compression: an LZ77 coder in the shape of LZ4, written here rather
// than taken from a library, because a decoder is a parser of untrusted
// bytes and this project's claim is that every such parser is its own, is
// fuzzed, and refuses what it cannot handle.
//
// The shape.  A compressed block is its uncompressed length as a varint,
// then a run of sequences.  A sequence is a token byte -- the high four bits
// a literal count, the low four a match length less four -- with each nibble
// extended past fifteen by bytes of 255 and a remainder, then the literals,
// then a two-byte little-endian offset back into what has been produced so
// far, and the match copies that many bytes forward from there.  The last
// sequence carries literals and no match.  A match may overlap what it
// copies -- an offset of one with a length of forty is forty copies of one
// byte -- which is how a run is encoded, and why the copy below goes byte
// by byte rather than through memcpy.
//
// The encoder hashes each four-byte window into a table of positions and
// takes the first match of at least four bytes it finds there, greedily.
// That is the cheapest LZ there is and it shows in the ratio: LZ4 with its
// tuned matcher does better, and when the engine writes compressed blocks
// docs/BENCHMARKS.md will say by how much rather than pretending
// otherwise.  The decoder is the part that has to
// be right, and it is bounded at every step: a length that would run past
// the declared size, an offset that reaches before the start, a stream
// that ends inside a sequence -- each is a corruption, reported, never
// read.
#ifndef AMBAR_COMPRESS_HPP_
#define AMBAR_COMPRESS_HPP_

#include <cstddef>
#include <string>
#include <string_view>

#include "ambar/status.hpp"

namespace ambar {

// Appends the compressed form of `input` to *output.  Never fails: input
// that does not compress is carried as one run of literals, at a cost of
// a few bytes over its own length.  Inputs up to 2^32 - 1 bytes, which
// is asserted rather than checked: a block is never near it.
void compress_block(std::string_view input, std::string* output);

// The most compress_block can produce for an input of `n` bytes, so a
// caller can reserve once.
size_t max_compressed_size(size_t n);

// Decodes what compress_block produced into *output, replacing its
// contents.  Any input at all may be handed in; a malformed one is refused
// with kCorruption and *output is left in an unspecified state.  The
// decoded length is what the stream declared, checked as it is produced,
// so a stream that claims more than it delivers, or delivers more than it
// claims, is refused too.
Status decompress_block(std::string_view input, std::string* output);

}  // namespace ambar

#endif  // AMBAR_COMPRESS_HPP_
