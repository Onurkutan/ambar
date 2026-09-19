// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
//   ambar_tests [name-substring] [-v]
//
// The substring selects tests; -v names each one before it runs, which is the
// only way to identify a test that crashes the process rather than failing a
// check -- a sanitizer report, or a null dereference after a failed check.
// The CI workflow passes it for that reason.

#include <cstdlib>
#include <cstring>
#include <new>

#include "harness.hpp"

// Every allocation in the test binary passes through here and is counted
// for the thread that made it; see ambar::testing::thread_allocations().
// The cost is one increment of a thread-local, which no test can notice.
void* operator new(std::size_t n) {
  ++ambar::testing::thread_allocations();
  if (void* p = std::malloc(n > 0 ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
  const char* filter = nullptr;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-v") == 0) {
      verbose = true;
    } else {
      filter = argv[i];
    }
  }
  return ambar::testing::run_all(filter, verbose);
}
