// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// A skip list that one thread writes and any number read, without locking the
// readers.
//
// Why this and not std::map: the memtable is read concurrently with the writes
// that are still filling it, and a balanced tree cannot be traversed safely
// while it rotates.  A skip list never moves an existing node -- insertion only
// ever splices a new one in -- so a reader that is mid-traversal keeps seeing a
// consistent structure.  That is the entire reason LevelDB, RocksDB and this
// engine use one.
//
// The memory ordering is the load-bearing part:
//
//   * A new node is fully initialised **before** any pointer to it is published.
//     Publication uses a release store; traversal uses acquire loads.  Together
//     they guarantee a reader that observes the pointer also observes the node's
//     contents -- without them a reader can legally see a pointer to
//     uninitialised memory, on real hardware, rarely, which is the worst kind of
//     bug to own.
//
//   * Levels are published bottom-up.  The list is a valid list at every instant
//     in between, just with the new node reachable at fewer levels.  A reader
//     during that window finds the node by a slower path, never a broken one.
//
// Requires: keys are never removed, and never modified after insertion.  Both
// hold for a memtable, which is append-only until it is discarded whole.
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <random>

#include "arena.hpp"

namespace ambar {

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  // `arena` outlives the list; the list allocates every node from it.
  explicit SkipList(Comparator comparator, Arena* arena);

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Single writer only.  The key must not already be present.
  void insert(const Key& key);

  bool contains(const Key& key) const;

  // Forward and backward traversal over a live list.  Safe to use while another
  // thread inserts; a newly inserted key may or may not be seen, but nothing
  // else changes under the iterator.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    bool valid() const { return node_ != nullptr; }
    const Key& key() const {
      assert(valid());
      return node_->key;
    }
    void next() {
      assert(valid());
      node_ = node_->next(0);
    }
    void prev();
    void seek(const Key& target);
    void seek_to_first() { node_ = list_->head_->next(0); }
    void seek_to_last();

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

 private:
  // Twelve levels covers a few million entries at p=1/4 before the top level
  // stops helping; a memtable is flushed long before that.
  static constexpr int kMaxHeight = 12;
  static constexpr unsigned kBranching = 4;

  Node* new_node(const Key& key, int height);
  int random_height();

  bool equal(const Key& a, const Key& b) const {
    return compare_(a, b) == 0;
  }
  bool key_is_after_node(const Key& key, Node* node) const {
    return node != nullptr && compare_(node->key, key) < 0;
  }

  // The last node < key at each level, so insert knows where to splice.  When
  // `prev` is null the caller only wants the successor.
  Node* find_greater_or_equal(const Key& key, Node** prev) const;
  Node* find_less_than(const Key& key) const;
  Node* find_last() const;

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;

  // Only the writer touches this, but readers load it, so it is atomic.
  std::atomic<int> max_height_{1};

  std::mt19937 rng_{0x9E3779B9u};
};

// ------------------------------------------------------------------ Node ---
template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}

  Key const key;

  Node* next(int level) {
    assert(level >= 0);
    // Acquire: pairs with the release in set_next, so whatever the writer wrote
    // into the node before publishing it is visible to us afterwards.
    return next_[level].load(std::memory_order_acquire);
  }
  void set_next(int level, Node* node) {
    assert(level >= 0);
    next_[level].store(node, std::memory_order_release);
  }

  // Used only before a node is reachable, where no other thread can see it.
  Node* nobarrier_next(int level) {
    return next_[level].load(std::memory_order_relaxed);
  }
  void nobarrier_set_next(int level, Node* node) {
    next_[level].store(node, std::memory_order_relaxed);
  }

 private:
  // A flexible array: the node is allocated with exactly `height` pointers, so
  // a level-1 node costs one pointer rather than kMaxHeight of them.  Most
  // nodes are level 1, so this is most of the list's memory.
  std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::new_node(
    const Key& key, int height) {
  char* const memory = arena_->allocate_aligned(
      sizeof(Node) + sizeof(std::atomic<Node*>) * static_cast<size_t>(height - 1));
  return new (memory) Node(key);
}

// -------------------------------------------------------------- Iterator ---
template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Iterator::seek(const Key& target) {
  node_ = list_->find_greater_or_equal(target, nullptr);
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Iterator::prev() {
  // No back pointers: walking forward from the head is the price of keeping
  // nodes half the size.  Reverse scans are rare; forward ones are not.
  assert(valid());
  node_ = list_->find_less_than(node_->key);
  if (node_ == list_->head_) node_ = nullptr;
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Iterator::seek_to_last() {
  node_ = list_->find_last();
  if (node_ == list_->head_) node_ = nullptr;
}

// ----------------------------------------------------------------- list ----
template <typename Key, class Comparator>
int SkipList<Key, Comparator>::random_height() {
  int height = 1;
  while (height < kMaxHeight && (rng_() % kBranching) == 0) {
    ++height;
  }
  return height;
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::find_greater_or_equal(const Key& key,
                                                 Node** prev) const {
  Node* node = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    Node* next = node->next(level);
    if (key_is_after_node(key, next)) {
      node = next;  // still to the left of the target at this level
    } else {
      if (prev != nullptr) prev[level] = node;
      if (level == 0) return next;
      --level;  // drop a level and search more finely
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::find_less_than(const Key& key) const {
  Node* node = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    Node* next = node->next(level);
    if (next != nullptr && compare_(next->key, key) < 0) {
      node = next;
    } else {
      if (level == 0) return node;
      --level;
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::find_last()
    const {
  Node* node = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    Node* next = node->next(level);
    if (next != nullptr) {
      node = next;
    } else {
      if (level == 0) return node;
      --level;
    }
  }
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator comparator, Arena* arena)
    : compare_(comparator), arena_(arena), head_(new_node(Key{}, kMaxHeight)) {
  for (int i = 0; i < kMaxHeight; ++i) {
    head_->set_next(i, nullptr);
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* next = find_greater_or_equal(key, prev);

  // The memtable never inserts a duplicate internal key: the sequence number
  // makes every write unique.  If this fires, something upstream reused one.
  assert(next == nullptr || !equal(key, next->key));
  (void)next;  // only the assertion reads it, and a release build has none

  const int height = random_height();
  if (height > max_height_.load(std::memory_order_relaxed)) {
    for (int i = max_height_.load(std::memory_order_relaxed); i < height; ++i) {
      prev[i] = head_;
    }
    // Safe without a barrier: a concurrent reader that sees the old, smaller
    // height simply searches from a lower level, which is correct, only slower.
    // One that sees the new height finds head_->next[i] either null (fine, it
    // drops down) or the new node (fine, it is fully built by then).
    max_height_.store(height, std::memory_order_relaxed);
  }

  Node* node = new_node(key, height);
  for (int i = 0; i < height; ++i) {
    // No barrier needed writing into a node nobody can reach yet...
    node->nobarrier_set_next(i, prev[i]->nobarrier_next(i));
    // ...but publishing it needs one, and this is that moment.
    prev[i]->set_next(i, node);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::contains(const Key& key) const {
  Node* node = find_greater_or_equal(key, nullptr);
  return node != nullptr && equal(key, node->key);
}

}  // namespace ambar
