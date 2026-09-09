// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// How keys are ordered, and how they may be shortened.
//
// Ordering is the obvious part.  The other two are less obvious and belong
// here rather than in the table builder, because whether a shortened key is
// still a valid separator depends entirely on what the key means.
//
// Plain bytes can be truncated anywhere: "the quick brown fox" and "the who"
// are separated by "the r", which is three bytes instead of nineteen and still
// sorts after everything in the first block and before everything in the
// second.  An internal key cannot, because its last eight bytes are a sequence
// number and a type, and truncating into them produces a key that orders
// nowhere sensible.  The internal comparator therefore shortens only the user
// key and reattaches a trailer that sorts before every real version of that
// user key -- which is what a separator needs to do.
//
// Getting this wrong does not crash and does not corrupt a file.  It produces
// an index whose binary search sends lookups to the wrong block, and the keys
// in the skipped block simply stop being found.  That is why the shortening
// functions travel with the comparator instead of being written once against
// whichever key format the author had in mind.

#ifndef AMBAR_COMPARATOR_HPP_
#define AMBAR_COMPARATOR_HPP_

#include <string>
#include <string_view>

namespace ambar {

struct Comparator {
  // Written into the manifest and checked when the database is opened, so a
  // database cannot be read back under a different ordering than it was
  // written with.
  //
  // Note where it is *not*: nothing identifies the ordering inside a table
  // file.  An .sst carries no comparator name, so a table lifted out of one
  // database and dropped into another is read under whatever ordering that
  // database uses.  The check is at the database level, not the file level.
  const char* name;

  // The shortest key this comparator can read without going out of bounds.
  //
  // Internal keys end in an eight-byte trailer, and compare_internal_keys
  // reads it with `decode_fixed64(key.end() - 8)`.  Hand it a three-byte key --
  // which a corrupt or hostile file can contain -- and that reads five bytes
  // *before* the key.  The size arithmetic hides it: `size() - 8` underflows to
  // an enormous number and `substr` clamps, so extract_user_key quietly returns
  // the whole key and nothing looks wrong until the read itself.
  //
  // Rather than making every comparison defensive, the requirement is declared
  // here and checked once, where keys enter the engine from a file.  Bytewise
  // keys have no such requirement and declare zero.
  size_t min_key_length;

  // Negative, zero, positive.
  int (*compare)(std::string_view a, std::string_view b);

  // Replaces *start with a short key in [*start, limit), or leaves it alone if
  // no shorter one exists.  Callers must not rely on the result being shorter.
  void (*find_shortest_separator)(std::string* start, std::string_view limit);

  // Replaces *key with a short key >= *key.
  void (*find_short_successor)(std::string* key);
};

// Plain lexicographic byte order.  Used for the metaindex block, whose keys
// are names such as "filter.ambar.BuiltinBloomFilter2".
const Comparator* bytewise_comparator();

// The ordering the engine's data actually uses: user key ascending, then
// sequence number descending so that the newest version of a key is the first
// one an iterator meets.  Defined in dbformat.hpp.
const Comparator* internal_key_comparator();

}  // namespace ambar

#endif  // AMBAR_COMPARATOR_HPP_
