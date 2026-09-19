// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "block.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "encoding.hpp"

namespace ambar {

Block::Block(const BlockContents& contents)
    : data_(contents.data.data()),
      size_(contents.data.size()),
      owned_(contents.heap_allocated) {
  // Too small to hold even the restart count.  size_ is left as the flag: the
  // iterator checks it and returns an error rather than reading the header.
  if (size_ < sizeof(uint32_t)) {
    size_ = 0;
    return;
  }

  const uint32_t max_restarts =
      static_cast<uint32_t>((size_ - sizeof(uint32_t)) / sizeof(uint32_t));
  num_restarts_ = decode_fixed32(data_ + size_ - sizeof(uint32_t));

  // A corrupt count would send the binary search reading past the block.  The
  // check is arithmetic on values already in hand, so it costs nothing and
  // removes a whole class of out-of-bounds read.
  if (num_restarts_ > max_restarts) {
    size_ = 0;
    return;
  }
  restart_offset_ = static_cast<uint32_t>(
      size_ - (1 + num_restarts_) * sizeof(uint32_t));
}

Block::~Block() {
  if (owned_) {
    delete[] data_;
  }
}

namespace {

// Decodes one entry's header, returning the position of the key delta, or
// nullptr if the entry runs past `limit`.
//
// Every bound is checked against the block's own end rather than trusted from
// the encoded lengths, because those lengths came from the file.  A block that
// passed its checksum can still have been written by an older or buggier
// version of this code.
const char* decode_entry(const char* p, const char* limit, uint32_t* shared,
                         uint32_t* non_shared, uint32_t* value_length) {
  if (limit - p < 3) return nullptr;

  // The common case is three lengths all below 128, which is three bytes and
  // no loop at all.
  *shared = static_cast<uint32_t>(static_cast<unsigned char>(p[0]));
  *non_shared = static_cast<uint32_t>(static_cast<unsigned char>(p[1]));
  *value_length = static_cast<uint32_t>(static_cast<unsigned char>(p[2]));
  if ((*shared | *non_shared | *value_length) < 128) {
    p += 3;
  } else {
    std::string_view input(p, static_cast<size_t>(limit - p));
    if (!get_varint32(&input, shared)) return nullptr;
    if (!get_varint32(&input, non_shared)) return nullptr;
    if (!get_varint32(&input, value_length)) return nullptr;
    p = input.data();
  }

  // Widened before adding.  Two uint32 lengths summed as uint32 wrap around:
  // an entry claiming non_shared = 0xffffffff and value_length = 1 sums to
  // zero, passes this check, and then copies four gigabytes out of a block
  // that may be fifteen bytes long.  The block checksum is no defence — it is
  // computed over whatever the file contains, including that.
  const uint64_t claimed =
      static_cast<uint64_t>(*non_shared) + static_cast<uint64_t>(*value_length);
  if (static_cast<uint64_t>(limit - p) < claimed) {
    return nullptr;
  }
  return p;
}

}  // namespace

class Block::Iter final : public Iterator {
 public:
  Iter(const Comparator* comparator, const char* data, uint32_t restart_offset,
       uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restart_offset_(restart_offset),
        num_restarts_(num_restarts),
        current_(restart_offset),
        restart_index_(num_restarts) {}

  bool valid() const override { return current_ < restart_offset_; }
  Status status() const override { return status_; }

  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }

  void next() override { parse_next_entry(); }

  // Backwards is the expensive direction, and unavoidably so: an entry's key
  // is expressed relative to the one before it, so the only way to know the
  // previous entry is to re-walk from the last restart point before it.  A
  // reverse scan therefore costs O(restart_interval) per step, which is why
  // the engine's own merges never run backwards.
  void prev() override {
    const uint32_t original = current_;

    while (get_restart_point(restart_index_) >= original) {
      if (restart_index_ == 0) {
        current_ = restart_offset_;  // ran off the front
        restart_index_ = num_restarts_;
        return;
      }
      --restart_index_;
    }

    seek_to_restart_point(restart_index_);
    do {
      // Walk forward until the next step would pass where we started.
    } while (parse_next_entry() && next_entry_offset() < original);
  }

  void seek(std::string_view target) override {
    // Binary search over the restart points for the last one whose key is
    // below the target, then a linear walk of at most restart_interval
    // entries.  The two halves are what the restart array buys: without it
    // this would be a linear walk of the whole block.
    uint32_t left = 0;
    uint32_t right = num_restarts_ == 0 ? 0 : num_restarts_ - 1;
    while (left < right) {
      const uint32_t mid = (left + right + 1) / 2;
      const uint32_t region_offset = get_restart_point(mid);
      // Checked before it becomes a pointer.  Forming a pointer past the end
      // of an object is undefined even where it is never dereferenced, and
      // this offset came out of the file.
      if (region_offset >= restart_offset_) {
        corrupted("restart point lies outside the entry region");
        return;
      }

      uint32_t shared = 0;
      uint32_t non_shared = 0;
      uint32_t value_length = 0;
      const char* key_ptr =
          decode_entry(data_ + region_offset, data_ + restart_offset_, &shared,
                       &non_shared, &value_length);
      if (key_ptr == nullptr || shared != 0) {
        // A restart point whose entry does not stand alone is a contradiction
        // in terms; the block is not what it claims to be.
        corrupted("restart point does not begin a key");
        return;
      }
      const std::string_view mid_key(key_ptr, non_shared);
      if (non_shared < comparator_->min_key_length) {
        corrupted("key is too short for this comparator");
        return;
      }
      if (comparator_->compare(mid_key, target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }

    seek_to_restart_point(left);
    while (true) {
      if (!parse_next_entry()) return;
      if (comparator_->compare(key_, target) >= 0) return;
    }
  }

  void seek_to_first() override {
    seek_to_restart_point(0);
    parse_next_entry();
  }

  void seek_to_last() override {
    if (num_restarts_ == 0) {
      current_ = restart_offset_;
      return;
    }
    seek_to_restart_point(num_restarts_ - 1);
    while (parse_next_entry() && next_entry_offset() < restart_offset_) {
      // Walk to the end of the final restart region.
    }
  }

 private:
  uint32_t next_entry_offset() const {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }

  uint32_t get_restart_point(uint32_t index) const {
    return decode_fixed32(data_ + restart_offset_ + index * sizeof(uint32_t));
  }

  void seek_to_restart_point(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    const uint32_t offset = get_restart_point(index);
    if (offset > restart_offset_) {
      corrupted("restart point lies outside the entry region");
      return;
    }
    // value_ is left empty at the right place so that next_entry_offset()
    // returns `offset` on the first call, and parse_next_entry needs no
    // special case for the start of a region.
    value_ = std::string_view(data_ + offset, 0);
  }

  void corrupted(const char* what) {
    current_ = restart_offset_;
    restart_index_ = num_restarts_;
    status_ = Status::corruption(std::string("bad block entry: ") + what);
    key_.clear();
    value_ = std::string_view();
  }

  bool parse_next_entry() {
    // current_ is the offset of the entry about to be decoded, not of the key
    // inside it.  prev() and seek_to_last() both compare offsets against it,
    // and pointing it at the key would make those comparisons subtly wrong for
    // entries whose header is more than three bytes.
    current_ = next_entry_offset();

    const char* p = data_ + current_;
    const char* limit = data_ + restart_offset_;
    if (p >= limit) {
      current_ = restart_offset_;  // the end, not an error
      restart_index_ = num_restarts_;
      return false;
    }

    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_length = 0;
    p = decode_entry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr) {
      corrupted("entry runs past the end of the block");
      return false;
    }
    if (shared > key_.size()) {
      // The entry claims to share more of the previous key than exists.
      corrupted("shared prefix longer than the previous key");
      return false;
    }

    key_.resize(shared);
    key_.append(p, non_shared);

    // Every key that leaves this iterator is long enough for the comparator
    // that will be used on it.  This is the one place keys enter the engine
    // from a file, so checking here means nothing downstream — the binary
    // search, the merging iterator, the user-facing iterator — has to.
    if (key_.size() < comparator_->min_key_length) {
      corrupted("key is too short for this comparator");
      return false;
    }

    value_ = std::string_view(p + non_shared, value_length);

    // Keep restart_index_ in step so prev() can find its way back.
    while (restart_index_ + 1 < num_restarts_ &&
           get_restart_point(restart_index_ + 1) < next_entry_offset()) {
      ++restart_index_;
    }
    return true;
  }

  const Comparator* const comparator_;
  const char* const data_;
  const uint32_t restart_offset_;
  const uint32_t num_restarts_;

  uint32_t current_;
  uint32_t restart_index_;
  std::string key_;
  std::string_view value_;
  Status status_;
};

Iterator* Block::new_iterator(const Comparator* comparator) const {
  if (size_ < sizeof(uint32_t)) {
    return new_error_iterator(Status::corruption("block is too small"));
  }
  if (num_restarts_ == 0) {
    return new_empty_iterator();
  }
  return new Iter(comparator, data_, restart_offset_, num_restarts_);
}

bool Block::find(const Comparator* comparator, std::string_view target,
                 std::string* key, std::string_view* value,
                 Status* status) const {
  *status = Status::ok();
  if (size_ < sizeof(uint32_t)) {
    *status = Status::corruption("block is too small");
    return false;
  }
  if (num_restarts_ == 0) return false;

  const char* const limit = data_ + restart_offset_;
  const auto restart_point = [&](uint32_t index) {
    return decode_fixed32(data_ + restart_offset_ + index * sizeof(uint32_t));
  };
  const auto corrupted = [&](const char* what) {
    *status = Status::corruption(std::string("bad block entry: ") + what);
    return false;
  };

  // The same two halves as the iterator's seek: a binary search over the
  // restart points for the last whose key is below the target, then a
  // walk of at most one restart interval.  Every offset and length is
  // checked against the block before it is used, as there.
  uint32_t left = 0;
  uint32_t right = num_restarts_ - 1;
  while (left < right) {
    const uint32_t mid = (left + right + 1) / 2;
    const uint32_t region_offset = restart_point(mid);
    if (region_offset >= restart_offset_) {
      return corrupted("restart point lies outside the entry region");
    }
    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_length = 0;
    const char* key_ptr = decode_entry(data_ + region_offset, limit, &shared,
                                       &non_shared, &value_length);
    if (key_ptr == nullptr || shared != 0) {
      return corrupted("restart point does not begin a key");
    }
    if (non_shared < comparator->min_key_length) {
      return corrupted("restart key is too short for this comparator");
    }
    if (comparator->compare(std::string_view(key_ptr, non_shared), target) <
        0) {
      left = mid;
    } else {
      right = mid - 1;
    }
  }

  const uint32_t start = restart_point(left);
  if (start > restart_offset_) {
    return corrupted("restart point lies outside the entry region");
  }
  key->clear();
  const char* p = data_ + start;
  while (p < limit) {
    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_length = 0;
    p = decode_entry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr) return corrupted("entry runs past the end of the block");
    if (shared > key->size()) {
      return corrupted("shared prefix longer than the previous key");
    }
    key->resize(shared);
    key->append(p, non_shared);
    if (key->size() < comparator->min_key_length) {
      return corrupted("key is too short for this comparator");
    }
    p += non_shared;
    if (comparator->compare(*key, target) >= 0) {
      *value = std::string_view(p, value_length);
      return true;
    }
    p += value_length;
  }
  return false;  // every entry is below the target
}

}  // namespace ambar
