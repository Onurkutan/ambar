// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// The version is written in two places by hand -- project() in
// CMakeLists.txt and ambar::kVersion in the public header -- and a release
// whose binary reports one number while its build system says another is
// the kind of small wrongness that outlives everyone's memory of how it
// happened.  So the build hands the test the CMake number, and the test
// holds the header to it.

#include <string>

#include "ambar/db.hpp"
#include "harness.hpp"

TEST(version, the_header_and_the_build_agree) {
  CHECK_EQ(std::string(ambar::kVersion), std::string(AMBAR_CMAKE_VERSION));
  CHECK_EQ(std::string(ambar::kVersion),
           std::to_string(ambar::kMajorVersion) + "." +
               std::to_string(ambar::kMinorVersion) + "." +
               std::to_string(ambar::kPatchVersion));
}
