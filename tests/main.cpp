// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
//   ambar_tests [name-substring] [-v]
//
// The substring selects tests; -v names each one before it runs, which is the
// only way to identify a test that crashes the process rather than failing a
// check -- a sanitizer report, or a null dereference after a failed check.
// The CI workflow passes it for that reason.

#include <cstring>

#include "harness.hpp"

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
