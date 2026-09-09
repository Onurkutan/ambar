// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "ambar/iterator.hpp"

#include <cassert>

namespace ambar {

Iterator::~Iterator() = default;

namespace {

// One class serves both empty and failed, because they behave identically:
// never valid, never moving.  The only difference is what status() says, and a
// caller that does not ask cannot tell them apart -- which is the point, since
// a level that does not exist and a level that would not open both contribute
// nothing to a merge.
class EmptyIterator final : public Iterator {
 public:
  explicit EmptyIterator(const Status& status) : status_(status) {}

  bool valid() const override { return false; }
  void seek_to_first() override {}
  void seek_to_last() override {}
  void seek(std::string_view) override {}
  void next() override { assert(false); }
  void prev() override { assert(false); }

  std::string_view key() const override {
    assert(false);
    return std::string_view();
  }
  std::string_view value() const override {
    assert(false);
    return std::string_view();
  }

  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

Iterator* new_empty_iterator() { return new EmptyIterator(Status::ok()); }

Iterator* new_error_iterator(const Status& status) {
  return new EmptyIterator(status);
}

}  // namespace ambar
