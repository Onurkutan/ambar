// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "arena.hpp"

namespace ambar {
namespace {

constexpr size_t kBlockSize = 4096;

}  // namespace

Arena::~Arena() {
  for (char* block : blocks_) {
    delete[] block;
  }
}

char* Arena::allocate_fallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // A large request gets its own block rather than wasting the remainder of
    // the current one.  The quarter-block threshold bounds that waste: we throw
    // away at most 25% of a block, and only when the next request is big.
    return allocate_new_block(bytes);
  }
  alloc_ptr_ = allocate_new_block(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::allocate_aligned(size_t bytes) {
  constexpr size_t kAlign = alignof(std::max_align_t) > 8
                                ? alignof(std::max_align_t)
                                : size_t{8};
  static_assert((kAlign & (kAlign - 1)) == 0, "alignment must be a power of two");

  const size_t current = reinterpret_cast<uintptr_t>(alloc_ptr_) & (kAlign - 1);
  const size_t slop = current == 0 ? 0 : kAlign - current;
  const size_t needed = bytes + slop;

  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // A fresh block from new[] is already suitably aligned.
    result = allocate_fallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (kAlign - 1)) == 0);
  return result;
}

char* Arena::allocate_new_block(size_t block_bytes) {
  char* result = new char[block_bytes];
  blocks_.push_back(result);
  memory_usage_.fetch_add(block_bytes + sizeof(char*),
                          std::memory_order_relaxed);
  return result;
}

}  // namespace ambar
