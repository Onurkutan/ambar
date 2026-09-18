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
// byte -- which is how a run is encoded.
//
// The encoder hashes each four-byte window into a table of positions and
// takes the first match of at least four bytes it finds there, greedily.
// That is the cheapest LZ there is, and it is also LZ4's default level:
// on the engine's own blocks the two reach the same ratio, 2.92x against
// 2.93x, and the coders that search harder -- LZ4's high level, zlib --
// reach a fifth to a half more at a tenth of the speed in.  This one
// compresses at three quarters of LZ4's rate and decodes at two thirds
// of it; docs/BENCHMARKS.md has the tables, and what the engine gains and
// pays with it on.  The decoder is the part that has to be right, and it
// is bounded at every step: a length that would run past the declared
// size, an offset that reaches before the start, a stream that ends
// inside a sequence -- each is a corruption, reported, never read.  Its
// speed is the copies: a sequence at a time, eight bytes at a step where
// it has shown there is room for the step to spill, one at a time where
// there is not, and the checks come before the copy either way.
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

// The most compress_block can produce for an input of `n` bytes, which
// is what it sizes its output to before it writes.
size_t max_compressed_size(size_t n);

// Decodes what compress_block produced into *output, replacing its
// contents.  Any input at all may be handed in; a malformed one is refused
// with kCorruption, and *output is then sized to the length the stream
// declared and holds whatever was decoded before the refusal, which no
// caller should read.  The decoded length is what the stream declared,
// checked as it is produced, so a stream that claims more than it
// delivers, or delivers more than it claims, is refused too.
Status decompress_block(std::string_view input, std::string* output);

}  // namespace ambar

#endif  // AMBAR_COMPRESS_HPP_
