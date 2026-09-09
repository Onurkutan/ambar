// A bump allocator for everything a memtable owns.
//
// A memtable is written once, read many times, and then thrown away whole.  That
// lifetime makes individual deallocation pure waste: there is never a moment
// where one node should be freed while its neighbours live.  So allocation is a
// pointer increment, and the destructor releases every block at once.
//
// The second reason is measurement.  A skip list of millions of small nodes has
// per-allocation malloc overhead that is invisible in the code and very visible
// in memory use; an arena makes "how big is this memtable" a number the engine
// knows exactly, which is what the flush threshold is compared against.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ambar {

class Arena {
 public:
  Arena() = default;
  ~Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  char* allocate(size_t bytes);

  // Allocation aligned for any scalar the engine stores.  Skip-list nodes end
  // with an array of atomic pointers, so they must not be handed a misaligned
  // address -- on some platforms that is a fault, on others just slow.
  char* allocate_aligned(size_t bytes);

  // Total bytes handed out plus block overhead: what the flush threshold reads.
  size_t memory_usage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  char* allocate_fallback(size_t bytes);
  char* allocate_new_block(size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<char*> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

inline char* Arena::allocate(size_t bytes) {
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return allocate_fallback(bytes);
}

}  // namespace ambar
