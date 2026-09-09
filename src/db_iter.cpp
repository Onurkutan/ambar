// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "db_iter.hpp"

#include <cassert>
#include <memory>
#include <string>

namespace ambar {
namespace {

class DBIter final : public Iterator {
 public:
  DBIter(const Comparator* user_comparator, Iterator* internal_iter,
         SequenceNumber sequence)
      : user_comparator_(user_comparator),
        iter_(internal_iter),
        sequence_(sequence) {}

  bool valid() const override { return valid_; }

  std::string_view key() const override {
    assert(valid_);
    return direction_ == Direction::kForward ? extract_user_key(iter_->key())
                                             : saved_key_;
  }

  std::string_view value() const override {
    assert(valid_);
    return direction_ == Direction::kForward ? iter_->value() : saved_value_;
  }

  Status status() const override {
    return status_.is_ok() ? iter_->status() : status_;
  }

  void next() override;
  void prev() override;
  void seek(std::string_view target) override;
  void seek_to_first() override;
  void seek_to_last() override;

 private:
  enum class Direction { kForward, kReverse };

  void find_next_user_entry(bool skipping, std::string* skip);
  void find_prev_user_entry();

  bool parse_key(ParsedInternalKey* key);

  void save_key(std::string_view key, std::string* dst) {
    dst->assign(key.data(), key.size());
  }
  void clear_saved_value() {
    // A large value that has been scanned past should not keep its memory: a
    // reverse scan over a table of big values would otherwise hold the largest
    // one seen for the life of the iterator.
    if (saved_value_.capacity() > 1048576) {
      std::string empty;
      saved_value_.swap(empty);
    } else {
      saved_value_.clear();
    }
  }

  const Comparator* const user_comparator_;
  std::unique_ptr<Iterator> iter_;
  const SequenceNumber sequence_;

  Status status_;
  std::string saved_key_;    // the user key, when scanning backwards
  std::string saved_value_;  // its value
  Direction direction_ = Direction::kForward;
  bool valid_ = false;
};

bool DBIter::parse_key(ParsedInternalKey* key) {
  if (!parse_internal_key(iter_->key(), key)) {
    status_ = Status::corruption("malformed internal key in a table");
    return false;
  }
  return true;
}

void DBIter::next() {
  assert(valid_);

  if (direction_ == Direction::kReverse) {
    // A reverse scan leaves the underlying iterator positioned *before* the
    // entry it is reporting, because that is how it found the boundary.  Going
    // forward again means stepping over the versions of the current key.
    direction_ = Direction::kForward;
    if (!iter_->valid()) {
      iter_->seek_to_first();
    } else {
      iter_->next();
    }
    if (!iter_->valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
    // saved_key_ already holds the key being left behind.
  } else {
    save_key(extract_user_key(iter_->key()), &saved_key_);
    iter_->next();
    if (!iter_->valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
  }

  find_next_user_entry(/*skipping=*/true, &saved_key_);
}

// Walks forward to the next entry that should be visible.
//
// `skipping` means the versions of *skip are already accounted for and must be
// passed over.  The loop is the whole of the collapse rule: for each user key
// in turn, the first entry at or below the snapshot decides it -- a value
// makes it visible, a tombstone makes it invisible and puts the key into
// `skip` so its older versions are stepped over rather than surfacing.
void DBIter::find_next_user_entry(bool skipping, std::string* skip) {
  assert(iter_->valid());
  assert(direction_ == Direction::kForward);

  do {
    ParsedInternalKey key;
    if (!parse_key(&key)) {
      valid_ = false;
      return;
    }
    if (key.sequence <= sequence_) {
      switch (key.type) {
        case ValueType::kDeletion:
          // Arranging for every older version of this key to be skipped.
          // Without this the entry underneath the tombstone would be returned
          // and the delete would appear not to have happened.
          save_key(key.user_key, skip);
          skipping = true;
          break;
        case ValueType::kValue:
          if (skipping &&
              user_comparator_->compare(key.user_key, *skip) <= 0) {
            break;  // hidden by a newer version or a tombstone
          }
          valid_ = true;
          saved_key_.clear();
          return;
      }
    }
    iter_->next();
  } while (iter_->valid());

  saved_key_.clear();
  valid_ = false;
}

void DBIter::prev() {
  assert(valid_);

  if (direction_ == Direction::kForward) {
    // Going backwards means finding the *first* entry of the current user key
    // and stepping before it, because forward scanning left the iterator on
    // whichever version happened to be visible.
    assert(iter_->valid());
    save_key(extract_user_key(iter_->key()), &saved_key_);
    while (true) {
      iter_->prev();
      if (!iter_->valid()) {
        valid_ = false;
        saved_key_.clear();
        clear_saved_value();
        return;
      }
      if (user_comparator_->compare(extract_user_key(iter_->key()),
                                    saved_key_) < 0) {
        break;
      }
    }
    direction_ = Direction::kReverse;
  }

  find_prev_user_entry();
}

// Walks backwards to the newest visible version of the previous user key.
//
// Backwards is harder than forwards, and the reason is that the versions of a
// key are stored newest first: scanning back, the *last* version seen for a
// key is the newest one.  So the loop cannot report a key as soon as it meets
// it -- it has to keep the best candidate and only commit once it has stepped
// past the key entirely.
void DBIter::find_prev_user_entry() {
  assert(direction_ == Direction::kReverse);

  ValueType type = ValueType::kDeletion;

  if (iter_->valid()) {
    do {
      ParsedInternalKey key;
      if (!parse_key(&key)) {
        valid_ = false;
        saved_key_.clear();
        clear_saved_value();
        return;
      }
      if (key.sequence <= sequence_) {
        if (type != ValueType::kDeletion &&
            user_comparator_->compare(key.user_key, saved_key_) < 0) {
          break;  // stepped past the key being decided; the candidate stands
        }
        type = key.type;
        if (type == ValueType::kDeletion) {
          saved_key_.clear();
          clear_saved_value();
        } else {
          save_key(key.user_key, &saved_key_);
          saved_value_.assign(iter_->value().data(), iter_->value().size());
        }
      }
      iter_->prev();
    } while (iter_->valid());
  }

  if (type == ValueType::kDeletion) {
    valid_ = false;
    saved_key_.clear();
    clear_saved_value();
    direction_ = Direction::kForward;
  } else {
    valid_ = true;
  }
}

void DBIter::seek(std::string_view target) {
  direction_ = Direction::kForward;
  clear_saved_value();
  saved_key_ = make_lookup_key(target, sequence_);
  iter_->seek(saved_key_);
  if (iter_->valid()) {
    find_next_user_entry(/*skipping=*/false, &saved_key_);
  } else {
    valid_ = false;
  }
}

void DBIter::seek_to_first() {
  direction_ = Direction::kForward;
  clear_saved_value();
  iter_->seek_to_first();
  if (iter_->valid()) {
    find_next_user_entry(/*skipping=*/false, &saved_key_);
  } else {
    valid_ = false;
  }
}

void DBIter::seek_to_last() {
  direction_ = Direction::kReverse;
  clear_saved_value();
  iter_->seek_to_last();
  find_prev_user_entry();
}

}  // namespace

Iterator* new_db_iterator(DBImpl*, const Comparator* user_comparator,
                          Iterator* internal_iter, SequenceNumber sequence,
                          uint32_t) {
  return new DBIter(user_comparator, internal_iter, sequence);
}

}  // namespace ambar
