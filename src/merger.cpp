// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "merger.hpp"

#include <cassert>
#include <memory>
#include <vector>

namespace ambar {
namespace {

class MergingIterator final : public Iterator {
 public:
  MergingIterator(const Comparator* comparator, Iterator** children, int n)
      : comparator_(comparator), children_(static_cast<size_t>(n)) {
    for (int i = 0; i < n; ++i) {
      children_[static_cast<size_t>(i)].reset(children[i]);
    }
  }

  bool valid() const override { return current_ != nullptr; }

  std::string_view key() const override { return current_->key(); }
  std::string_view value() const override { return current_->value(); }

  void seek_to_first() override {
    for (auto& child : children_) child->seek_to_first();
    find_smallest();
    direction_ = Direction::kForward;
  }

  void seek_to_last() override {
    for (auto& child : children_) child->seek_to_last();
    find_largest();
    direction_ = Direction::kReverse;
  }

  void seek(std::string_view target) override {
    for (auto& child : children_) child->seek(target);
    find_smallest();
    direction_ = Direction::kForward;
  }

  void next() override {
    // Reversing direction is the awkward case, and getting it wrong produces
    // duplicated or skipped keys rather than a crash.
    //
    // After a prev(), every child except the current one sits *before* the
    // current key, because that is what a backwards scan leaves behind.  To
    // step forward they must first be moved to the first entry at or after it,
    // which is what seeking to the current key does.
    //
    // No child can land exactly on the current key -- that is the precondition
    // in merger.hpp, checked by the assertion above -- so nothing else is
    // needed here.  An extra step to skip such a child would be unreachable
    // code that looks like it handles a case the iterator cannot actually
    // handle, which is worse than not writing it.
    if (direction_ != Direction::kForward) {
      for (auto& child : children_) {
        if (child.get() == current_) continue;
        child->seek(key());
      }
      direction_ = Direction::kForward;
    }

    current_->next();
    find_smallest();
  }

  void prev() override {
    if (direction_ != Direction::kReverse) {
      for (auto& child : children_) {
        if (child.get() == current_) continue;
        child->seek(key());
        if (child->valid()) {
          // Landed at or after the key; one step back puts it strictly before.
          child->prev();
        } else {
          // The seek ran past this child's end, which means every key it holds
          // is below the target -- so its *last* entry is the one a reverse
          // scan should meet next.  Leaving it invalid instead would silently
          // drop that whole child from the rest of the scan, and only a source
          // whose range ends before the current key can show it.
          child->seek_to_last();
        }
      }
      direction_ = Direction::kReverse;
    }

    current_->prev();
    find_largest();
  }

  Status status() const override {
    // The first failing child wins.  Checking only the current one would miss
    // a child that failed while positioned past its end, which is
    // indistinguishable from having finished.
    for (const auto& child : children_) {
      if (!child->status().is_ok()) return child->status();
    }
    return Status::ok();
  }

 private:
  enum class Direction { kForward, kReverse };

  // Debug-only check of the precondition in merger.hpp.  O(n^2) over at most a
  // handful of children, and only where an assertion is compiled in.
  void assert_keys_are_distinct() const {
#ifndef NDEBUG
    for (size_t i = 0; i < children_.size(); ++i) {
      if (!children_[i]->valid()) continue;
      for (size_t j = i + 1; j < children_.size(); ++j) {
        if (!children_[j]->valid()) continue;
        assert(comparator_->compare(children_[i]->key(),
                                    children_[j]->key()) != 0 &&
               "merging iterator: two children hold the same key");
      }
    }
#endif
  }

  // Note on `<` rather than `<=`: under the precondition the two are the same
  // function, because no two children hold an equal key, and mutation testing
  // confirms no test distinguishes them.  That is the correct outcome for a
  // mutation the precondition makes unreachable, and it is recorded here so
  // the next person to run the mutation script does not go looking for the
  // test that is supposedly missing.
  void find_smallest() {
    assert_keys_are_distinct();
    Iterator* smallest = nullptr;
    for (auto& child : children_) {
      if (!child->valid()) continue;
      if (smallest == nullptr ||
          comparator_->compare(child->key(), smallest->key()) < 0) {
        smallest = child.get();
      }
    }
    current_ = smallest;
  }

  void find_largest() {
    assert_keys_are_distinct();
    Iterator* largest = nullptr;
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
      if (!(*it)->valid()) continue;
      if (largest == nullptr ||
          comparator_->compare((*it)->key(), largest->key()) > 0) {
        largest = it->get();
      }
    }
    current_ = largest;
  }

  const Comparator* const comparator_;
  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_ = nullptr;
  Direction direction_ = Direction::kForward;
};

}  // namespace

Iterator* new_merging_iterator(const Comparator* comparator,
                               Iterator** children, int n) {
  if (n == 0) return new_empty_iterator();
  if (n == 1) return children[0];
  return new MergingIterator(comparator, children, n);
}

}  // namespace ambar
