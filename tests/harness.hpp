// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// A test harness in sixty lines, so the project has no dependency to install.
//
// Registers each TEST() at static-initialisation time and runs them all from
// main().  A failing CHECK prints the file, line and both values, then keeps
// going within the test so one run reports every failure rather than the first.
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace ambar::testing {

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& failure_count() {
  static int failures = 0;
  return failures;
}

// Failure messages are flushed as they are written.
//
// A test that fails a check and then crashes -- dereferencing the null pointer
// that the failed check was about, say -- takes the buffered explanation with
// it, and the crash arrives with no reason attached.  That is exactly the
// situation where the message matters most.
inline void note_failure() { std::fflush(stdout); }

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body) {
    registry().push_back({suite, name, std::move(body)});
  }
};

template <typename T>
std::string show(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}
inline std::string show(bool value) { return value ? "true" : "false"; }
inline std::string show(const std::string& value) { return "\"" + value + "\""; }

inline int run_all(const char* filter, bool verbose) {
  int run = 0;
  for (const auto& test : registry()) {
    const std::string full = test.suite + "." + test.name;
    if (filter != nullptr && full.find(filter) == std::string::npos) {
      continue;
    }
    ++run;

    // The name is written, and the stream flushed, *before* the test runs.
    //
    // A test that crashes rather than failing a check -- a null dereference
    // after an assertion that did not abort, a sanitizer stopping the process
    // -- otherwise leaves no trace of which test it was, because buffered
    // output dies with the process.  That happened while mutation-testing the
    // merging iterator: a mutation was caught, but only as a segfault with no
    // name attached, which reads like a broken harness rather than a working
    // test.  One flushed line per test costs nothing and turns that into a
    // diagnosis.
    if (verbose) {
      std::printf("  ... %s\n", full.c_str());
      std::fflush(stdout);
    }

    const int before = failure_count();
    try {
      test.body();
    } catch (const std::exception& e) {
      ++failure_count();
      std::printf("  [threw] %s: %s\n", full.c_str(), e.what());
    }
    if (failure_count() != before) {
      std::printf("FAIL  %s\n", full.c_str());
    }
  }
  const int failures = failure_count();
  std::printf("\n%d test%s run, %d failure%s\n", run, run == 1 ? "" : "s",
              failures, failures == 1 ? "" : "s");
  return failures == 0 ? 0 : 1;
}

}  // namespace ambar::testing

#define TEST(suite, name)                                                     \
  static void suite##_##name##_body();                                        \
  static ::ambar::testing::Registrar suite##_##name##_registrar(              \
      #suite, #name, suite##_##name##_body);                                  \
  static void suite##_##name##_body()

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      ++::ambar::testing::failure_count();                                     \
      ::ambar::testing::note_failure();                                        \
      std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__,          \
                  #condition);                                               \
    }                                                                         \
  } while (false)

#define CHECK_EQ(a, b)                                                        \
  do {                                                                        \
    const auto& lhs_ = (a);                                                   \
    const auto& rhs_ = (b);                                                   \
    if (!(lhs_ == rhs_)) {                                                    \
      ++::ambar::testing::failure_count();                                     \
      ::ambar::testing::note_failure();                                        \
      std::printf("  %s:%d: CHECK_EQ(%s, %s) failed: %s vs %s\n", __FILE__,   \
                  __LINE__, #a, #b,                                           \
                  ::ambar::testing::show(lhs_).c_str(),                        \
                  ::ambar::testing::show(rhs_).c_str());                       \
    }                                                                         \
  } while (false)

#define CHECK_OK(expr)                                                        \
  do {                                                                        \
    const ::ambar::Status status_ = (expr);                                    \
    if (!status_.is_ok()) {                                                   \
      ++::ambar::testing::failure_count();                                     \
      ::ambar::testing::note_failure();                                        \
      std::printf("  %s:%d: %s returned %s\n", __FILE__, __LINE__, #expr,     \
                  status_.to_string().c_str());                              \
    }                                                                         \
  } while (false)
