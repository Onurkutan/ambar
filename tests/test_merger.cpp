// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The merging iterator is where direction changes go wrong.
//
// Walking forward through several sorted sources is easy and a test that only
// does that will pass on a broken implementation.  The failure lives in the
// turn: after a prev(), every source except the current one sits *before* the
// current key, and a next() that does not first move them forward will hand
// back keys that were already returned.  The bug shows up as duplicates or
// gaps in a scan, never as a crash, so the tests below alternate direction
// deliberately and compare against a plain sorted vector rather than against
// what the merger itself thinks it holds.

#include "harness.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../src/comparator.hpp"
#include "../src/dbformat.hpp"
#include "../src/merger.hpp"

using namespace ambar;

namespace {

// A sorted vector presented as an Iterator, so the merge can be tested without
// any file involved.
class VectorIterator final : public Iterator {
 public:
  explicit VectorIterator(std::vector<std::pair<std::string, std::string>> data)
      : data_(std::move(data)) {}

  bool valid() const override { return pos_ < data_.size(); }
  void seek_to_first() override { pos_ = 0; }
  void seek_to_last() override {
    pos_ = data_.empty() ? kInvalid : data_.size() - 1;
  }
  void seek(std::string_view target) override {
    pos_ = static_cast<size_t>(
        std::lower_bound(data_.begin(), data_.end(), target,
                         [](const auto& entry, std::string_view t) {
                           return entry.first < t;
                         }) -
        data_.begin());
  }
  void next() override { ++pos_; }
  void prev() override { pos_ = (pos_ == 0) ? kInvalid : pos_ - 1; }

  std::string_view key() const override { return data_[pos_].first; }
  std::string_view value() const override { return data_[pos_].second; }
  Status status() const override { return Status::ok(); }

 private:
  static constexpr size_t kInvalid = static_cast<size_t>(-1);
  std::vector<std::pair<std::string, std::string>> data_;
  size_t pos_ = kInvalid;
};

using Entries = std::vector<std::pair<std::string, std::string>>;

// Deals `total` distinct keys round-robin into `n` sources, so every source is
// sorted and the merge has to interleave them.
std::vector<Entries> deal(int total, int n, std::mt19937* rng) {
  std::vector<Entries> sources(static_cast<size_t>(n));
  for (int i = 0; i < total; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%08d", i);
    sources[(*rng)() % static_cast<unsigned>(n)].emplace_back(
        buf, "v" + std::to_string(i));
  }
  return sources;
}

Entries flatten(const std::vector<Entries>& sources) {
  Entries all;
  for (const auto& source : sources) {
    all.insert(all.end(), source.begin(), source.end());
  }
  std::sort(all.begin(), all.end());
  return all;
}

Iterator* merge_of(const std::vector<Entries>& sources) {
  std::vector<Iterator*> children;
  children.reserve(sources.size());
  for (const auto& source : sources) {
    children.push_back(new VectorIterator(source));
  }
  return new_merging_iterator(bytewise_comparator(), children.data(),
                              static_cast<int>(children.size()));
}

}  // namespace

TEST(merger, no_sources_is_an_empty_iterator) {
  std::unique_ptr<Iterator> iter(
      new_merging_iterator(bytewise_comparator(), nullptr, 0));
  iter->seek_to_first();
  CHECK(!iter->valid());
  CHECK_OK(iter->status());
}

TEST(merger, forward_scan_returns_every_entry_in_order) {
  std::mt19937 rng(3);
  const auto sources = deal(5000, 6, &rng);
  const Entries expected = flatten(sources);

  std::unique_ptr<Iterator> iter(merge_of(sources));
  size_t i = 0;
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    CHECK(i < expected.size());
    CHECK_EQ(iter->key(), std::string_view(expected[i].first));
    CHECK_EQ(iter->value(), std::string_view(expected[i].second));
    ++i;
  }
  CHECK_EQ(i, expected.size());
}

TEST(merger, reverse_scan_returns_every_entry_in_reverse) {
  std::mt19937 rng(4);
  const auto sources = deal(5000, 6, &rng);
  const Entries expected = flatten(sources);

  std::unique_ptr<Iterator> iter(merge_of(sources));
  size_t i = expected.size();
  for (iter->seek_to_last(); iter->valid(); iter->prev()) {
    CHECK(i > 0);
    --i;
    CHECK_EQ(iter->key(), std::string_view(expected[i].first));
  }
  CHECK_EQ(i, 0u);
}

TEST(merger, empty_sources_are_skipped_not_treated_as_the_end) {
  std::mt19937 rng(5);
  auto sources = deal(500, 3, &rng);
  // Empties at the front, in the middle and at the back: each position is a
  // different branch through find_smallest.
  sources.insert(sources.begin(), Entries{});
  sources.insert(sources.begin() + 2, Entries{});
  sources.push_back(Entries{});

  const Entries expected = flatten(sources);
  std::unique_ptr<Iterator> iter(merge_of(sources));
  size_t i = 0;
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    CHECK_EQ(iter->key(), std::string_view(expected[i].first));
    ++i;
  }
  CHECK_EQ(i, expected.size());
}

TEST(merger, seek_lands_on_the_first_key_at_or_after_the_target) {
  std::mt19937 rng(6);
  // Even keys only, so every odd one is a gap.
  std::vector<Entries> sources(4);
  Entries all;
  for (int i = 0; i < 2000; i += 2) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%08d", i);
    sources[rng() % 4].emplace_back(buf, "v");
    all.emplace_back(buf, "v");
  }
  std::sort(all.begin(), all.end());

  std::unique_ptr<Iterator> iter(merge_of(sources));
  for (int i = 1; i < 1999; i += 2) {
    char probe[32];
    char expected[32];
    std::snprintf(probe, sizeof(probe), "key_%08d", i);
    std::snprintf(expected, sizeof(expected), "key_%08d", i + 1);
    iter->seek(probe);
    CHECK(iter->valid());
    CHECK_EQ(iter->key(), std::string_view(expected));
  }

  iter->seek("key_99999999");
  CHECK(!iter->valid());
}

// The test that matters.  Every alternation of next and prev, from random
// positions, checked against a plain vector.
TEST(merger, alternating_directions_agrees_with_a_sorted_vector) {
  std::mt19937 rng(2026);
  const auto sources = deal(3000, 5, &rng);
  const Entries expected = flatten(sources);

  std::unique_ptr<Iterator> iter(merge_of(sources));

  for (int trial = 0; trial < 200; ++trial) {
    // Start somewhere.
    size_t position = rng() % expected.size();
    iter->seek(expected[position].first);
    CHECK(iter->valid());
    CHECK_EQ(iter->key(), std::string_view(expected[position].first));

    // Then walk, changing direction at random.
    for (int step = 0; step < 50; ++step) {
      if (rng() % 2 == 0) {
        iter->next();
        ++position;
      } else {
        iter->prev();
        position = (position == 0) ? expected.size() : position - 1;
      }

      if (position >= expected.size()) {
        // Ran off either end.  Both ends report the same way, which is why the
        // loop restarts rather than trying to continue from an invalid state.
        CHECK(!iter->valid());
        break;
      }
      if (!iter->valid()) {
        std::printf("    became invalid at position %zu (expected %s)\n",
                    position, expected[position].first.c_str());
        CHECK(false);
        break;
      }
      if (iter->key() != std::string_view(expected[position].first)) {
        std::printf("    at position %zu: got %s, expected %s\n", position,
                    std::string(iter->key()).c_str(),
                    expected[position].first.c_str());
        CHECK(false);
        break;
      }
    }
  }
}

// A next() immediately after a prev() is the narrowest form of the same bug,
// isolated so a failure names it directly.
TEST(merger, next_after_prev_returns_to_the_same_key) {
  std::mt19937 rng(8);
  const auto sources = deal(1000, 4, &rng);
  const Entries expected = flatten(sources);

  std::unique_ptr<Iterator> iter(merge_of(sources));
  for (size_t i = 1; i + 1 < expected.size(); i += 17) {
    iter->seek(expected[i].first);
    CHECK(iter->valid());
    iter->prev();
    CHECK(iter->valid());
    CHECK_EQ(iter->key(), std::string_view(expected[i - 1].first));
    iter->next();
    CHECK(iter->valid());
    CHECK_EQ(iter->key(), std::string_view(expected[i].first));
    iter->next();
    CHECK(iter->valid());
    CHECK_EQ(iter->key(), std::string_view(expected[i + 1].first));
  }
}

TEST(merger, prev_after_next_returns_to_the_same_key) {
  std::mt19937 rng(9);
  const auto sources = deal(1000, 4, &rng);
  const Entries expected = flatten(sources);

  std::unique_ptr<Iterator> iter(merge_of(sources));
  for (size_t i = 1; i + 1 < expected.size(); i += 13) {
    iter->seek(expected[i].first);
    iter->next();
    CHECK(iter->valid());
    CHECK_EQ(iter->key(), std::string_view(expected[i + 1].first));
    iter->prev();
    CHECK(iter->valid());
    CHECK_EQ(iter->key(), std::string_view(expected[i].first));
  }
}

// A single source is returned unwrapped, which is worth confirming: a caller
// that assumed otherwise would leak the child.
TEST(merger, one_source_is_passed_through) {
  Entries data = {{"a", "1"}, {"b", "2"}};
  Iterator* child = new VectorIterator(data);
  std::unique_ptr<Iterator> iter(
      new_merging_iterator(bytewise_comparator(), &child, 1));
  CHECK(iter.get() == child);
  iter->seek_to_first();
  CHECK_EQ(iter->key(), std::string_view("a"));
}

// Internal keys, which is what the engine actually merges: the same user key
// at several sequence numbers must come back newest first, without the merge
// knowing anything about recency.
TEST(merger, internal_keys_arrive_newest_first) {
  std::vector<Entries> sources(3);
  // Three sources each holding one version of the same user key.
  sources[0].emplace_back(make_internal_key("k", 10, ValueType::kValue), "old");
  sources[1].emplace_back(make_internal_key("k", 30, ValueType::kValue), "new");
  sources[2].emplace_back(make_internal_key("k", 20, ValueType::kValue), "mid");

  std::vector<Iterator*> children;
  for (const auto& source : sources) {
    children.push_back(new VectorIterator(source));
  }
  std::unique_ptr<Iterator> iter(new_merging_iterator(
      internal_key_comparator(), children.data(), 3));

  iter->seek_to_first();
  CHECK(iter->valid());
  CHECK_EQ(iter->value(), std::string_view("new"));
  iter->next();
  CHECK_EQ(iter->value(), std::string_view("mid"));
  iter->next();
  CHECK_EQ(iter->value(), std::string_view("old"));
  iter->next();
  CHECK(!iter->valid());
}

// The two cases the earlier tests were blind to, found by mutating the
// direction-change code and noticing that nothing failed.

// A child whose keys all lie below the current position.  Round-robin dealing
// gives every source the full key range, so no test above can reach the branch
// where a backwards seek runs off the front of a child.
TEST(merger, reverse_scan_crosses_into_a_source_that_ends_early) {
  Entries low;
  Entries high;
  for (int i = 0; i < 200; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%08d", i);
    low.emplace_back(buf, "low");
  }
  for (int i = 200; i < 400; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%08d", i);
    high.emplace_back(buf, "high");
  }
  const std::vector<Entries> sources = {low, high};
  const Entries expected = flatten(sources);

  std::unique_ptr<Iterator> iter(merge_of(sources));

  // Start inside the upper source, then walk back across the boundary.  The
  // lower source's seek runs past its end, and without seek_to_last it stays
  // invalid and its two hundred keys vanish from the scan.
  iter->seek(expected[250].first);
  CHECK(iter->valid());
  for (size_t i = 250; i > 0; --i) {
    iter->prev();
    if (!iter->valid()) {
      // Returning rather than continuing: key() on an invalid iterator is
      // undefined, and a test that crashes reports nothing useful.
      std::printf("    scan ended at position %zu, expected to reach 0\n", i);
      CHECK(false);
      return;
    }
    if (iter->key() != std::string_view(expected[i - 1].first)) {
      std::printf("    stepping back to %zu: got %s, expected %s\n", i - 1,
                  std::string(iter->key()).c_str(),
                  expected[i - 1].first.c_str());
      CHECK(false);
      return;
    }
  }
  iter->prev();
  CHECK(!iter->valid());
}

// The precondition, stated as a test rather than only as a comment.
//
// The engine merges internal keys, and the claim that two sources can never
// hold the same one rests on sequence numbers being globally unique.  This
// checks the claim where it is cheap to check: a key written twice produces
// two different internal keys, so a merge of the two sources sees two distinct
// entries and returns both, newest first.
TEST(merger, the_same_user_key_in_two_sources_is_two_distinct_internal_keys) {
  Entries older;
  Entries newer;
  for (int i = 0; i < 200; ++i) {
    const std::string user = "key_" + std::to_string(i);
    older.emplace_back(make_internal_key(user, static_cast<SequenceNumber>(i),
                                         ValueType::kValue),
                       "old");
    newer.emplace_back(
        make_internal_key(user, static_cast<SequenceNumber>(1000 + i),
                          ValueType::kValue),
        "new");
  }
  std::sort(older.begin(), older.end(),
            [](const auto& a, const auto& b) {
              return compare_internal_keys(a.first, b.first) < 0;
            });
  std::sort(newer.begin(), newer.end(),
            [](const auto& a, const auto& b) {
              return compare_internal_keys(a.first, b.first) < 0;
            });

  std::vector<Iterator*> children = {new VectorIterator(newer),
                                     new VectorIterator(older)};
  std::unique_ptr<Iterator> iter(new_merging_iterator(
      internal_key_comparator(), children.data(), 2));

  // Every user key appears twice, newest first, and the debug assertion in
  // find_smallest would fire if any pair collided.
  int entries = 0;
  std::string previous_user;
  bool expecting_old = false;
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    const std::string user(extract_user_key(iter->key()));
    if (user != previous_user) {
      CHECK(!expecting_old);  // the previous key must have had both versions
      CHECK_EQ(iter->value(), std::string_view("new"));
      previous_user = user;
      expecting_old = true;
    } else {
      CHECK_EQ(iter->value(), std::string_view("old"));
      expecting_old = false;
    }
    ++entries;
  }
  CHECK_EQ(entries, 400);
  CHECK(!expecting_old);
}

// A child that fails must be reported even when it is not the current one.
//
// Found by mutating status() to report only the current child, which no test
// noticed.  The mutation matters: a level whose block would not decode looks,
// to a forward scan, exactly like a level that has run out, so a merge that
// only asks the current child returns a short result and calls it success.
namespace {

class FailingIterator final : public Iterator {
 public:
  // Behaves like an empty source, which is the shape a failed child takes:
  // never valid, and carrying the reason.
  bool valid() const override { return false; }
  void seek_to_first() override {}
  void seek_to_last() override {}
  void seek(std::string_view) override {}
  void next() override {}
  void prev() override {}
  std::string_view key() const override { return {}; }
  std::string_view value() const override { return {}; }
  Status status() const override {
    return Status::corruption("block would not decode");
  }
};

}  // namespace

TEST(merger, a_failing_child_is_reported_even_when_another_is_current) {
  Entries good = {{"a", "1"}, {"b", "2"}};
  std::vector<Iterator*> children = {new VectorIterator(good),
                                     new FailingIterator()};
  std::unique_ptr<Iterator> iter(new_merging_iterator(
      bytewise_comparator(), children.data(), 2));

  // The scan completes and looks entirely normal.
  int seen = 0;
  for (iter->seek_to_first(); iter->valid(); iter->next()) ++seen;
  CHECK_EQ(seen, 2);

  // But it is not success: one source never contributed, and the caller has to
  // be able to find that out.
  CHECK(iter->status().is_corruption());
}

TEST(merger, a_failing_child_is_reported_when_it_is_the_only_one) {
  std::vector<Iterator*> children = {new FailingIterator(),
                                     new FailingIterator()};
  std::unique_ptr<Iterator> iter(new_merging_iterator(
      bytewise_comparator(), children.data(), 2));
  iter->seek_to_first();
  CHECK(!iter->valid());
  CHECK(iter->status().is_corruption());
}
