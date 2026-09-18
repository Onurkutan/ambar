cmake_minimum_required(VERSION 3.16)
# The version here and ambar::kVersion in include/ambar/db.hpp are the same
# number by hand; the test below fails the build if they drift apart.
project(ambar VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Sanitizers are mutually exclusive at link time, so this is a choice, not a
# set of flags.  The thread sanitizer is the one that matters for the skip list:
# it is what proves the acquire/release pairing rather than asserting it.
set(AMBAR_SANITIZE "off" CACHE STRING "off | address | thread")
set_property(CACHE AMBAR_SANITIZE PROPERTY STRINGS off address thread)

# The libFuzzer targets in fuzz/.  Off by default because they need clang --
# libFuzzer ships with it and with nothing else -- and because they choose
# their own sanitizers: every parser is fuzzed under ASan and UBSan, since a
# fuzzer without them only finds the crashes that were going to be found anyway.
option(AMBAR_FUZZ "Build the fuzz targets (clang only; implies address+undefined)" OFF)

if(MSVC)
  # /EHsc: standard C++ exception unwinding, which the standard library headers
  # assume. Without it, MSVC treats every function as nothrow-by-default and
  # warns wherever a header relies on unwinding through one -- e.g. <vector> --
  # which /WX then turns into a hard build failure with nothing wrong in this
  # engine's own code.
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  # fopen and strerror are deprecated only by MSVC, and their replacements are
  # not portable.  The engine uses the standard ones deliberately.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(AMBAR_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AMBAR_FUZZ needs clang: libFuzzer is part of it")
  endif()
  if(NOT AMBAR_SANITIZE STREQUAL "off")
    message(FATAL_ERROR "AMBAR_FUZZ selects its own sanitizers; leave AMBAR_SANITIZE off")
  endif()
  # fuzzer-no-link instruments everything for coverage without pulling in
  # libFuzzer's main; only the fuzz executables link that, below.  -O1 keeps
  # the assertions and most of the speed, which is the trade libFuzzer's own
  # documentation recommends.
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined
                      -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=address,undefined)
elseif(NOT MSVC)
  if(AMBAR_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
  elseif(AMBAR_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
  endif()
endif()

add_library(ambar
  src/status.cpp
  src/arena.cpp
  src/encoding.cpp
  src/memtable.cpp
  src/file.cpp
  src/wal.cpp
  src/bloom.cpp
  src/cache.cpp
  src/comparator.cpp
  src/internal_filter_policy.cpp
  src/iterator.cpp
  src/format.cpp
  src/block_builder.cpp
  src/block.cpp
  src/compress.cpp
  src/filter_block.cpp
  src/two_level_iterator.cpp
  src/table_builder.cpp
  src/table.cpp
  src/merger.cpp
  src/version_edit.cpp
  src/filename.cpp
  src/table_cache.cpp
  src/version_set.cpp
  src/db_iter.cpp
  src/builder.cpp
  src/db_impl.cpp
  src/write_batch.cpp
  src/repair.cpp
)
target_include_directories(ambar PUBLIC include PRIVATE src)

enable_testing()

find_package(Threads REQUIRED)

add_executable(ambar_tests
  tests/main.cpp
  tests/test_encoding.cpp
  tests/test_wal.cpp
  tests/test_dbformat.cpp
  tests/test_version.cpp
  tests/test_skiplist.cpp
  tests/test_memtable.cpp
  tests/test_write_batch.cpp
  tests/test_bloom.cpp
  tests/test_cache.cpp
  tests/test_compress.cpp
  tests/test_compressed_tables.cpp
  tests/test_filter_block.cpp
  tests/test_hash.cpp
  tests/test_table.cpp
  tests/test_merger.cpp
  tests/test_db.cpp
  tests/test_corrupt.cpp
  tests/test_repair.cpp
  tests/test_powercut.cpp
  tests/test_faults.cpp
  tests/test_stats.cpp
  tests/sim_file_system.cpp
)
target_link_libraries(ambar_tests PRIVATE ambar Threads::Threads)
target_include_directories(ambar_tests PRIVATE src tests)
# What project() above says the version is, for tests/test_version.cpp to
# hold ambar::kVersion against.
target_compile_definitions(ambar_tests PRIVATE
  AMBAR_CMAKE_VERSION="${PROJECT_VERSION}")
add_test(NAME unit COMMAND ambar_tests)

# Tools.  Built by default so that a change which breaks them fails the normal
# build rather than being discovered when someone comes to run them.
add_executable(ambar_repair tools/repair.cpp)
target_link_libraries(ambar_repair PRIVATE ambar Threads::Threads)

# The power-cut simulation: the disk is a test double from tests/, and the
# tool runs everywhere the tests do.
add_executable(ambar_powercut tools/powercut.cpp tests/sim_file_system.cpp)
target_link_libraries(ambar_powercut PRIVATE ambar Threads::Threads)
target_include_directories(ambar_powercut PRIVATE src tests)

if(NOT MSVC)
  add_executable(ambar_crash_test tools/crash_test.cpp)
  target_link_libraries(ambar_crash_test PRIVATE ambar Threads::Threads)
  target_include_directories(ambar_crash_test PRIVATE src)
endif()

add_executable(ambar_bench tools/bench.cpp)
target_link_libraries(ambar_bench PRIVATE ambar Threads::Threads)
target_include_directories(ambar_bench PRIVATE src)

# The block coder over the data blocks of an existing database: ratio and
# rate, and a dump for tools/compare_codecs.py to run zlib and LZ4 over.
add_executable(ambar_codec_bench tools/codec_bench.cpp)
target_link_libraries(ambar_codec_bench PRIVATE ambar Threads::Threads)
target_include_directories(ambar_codec_bench PRIVATE src)

# SQLite is optional.  Without it the benchmark still runs and says plainly
# that it has nothing to compare against, rather than printing numbers with
# no context.
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
  target_compile_definitions(ambar_bench PRIVATE AMBAR_BENCH_SQLITE)
  target_link_libraries(ambar_bench PRIVATE SQLite::SQLite3)
endif()

# Fuzzers.  One executable per parser of untrusted bytes, plus the tool that
# writes their seed corpora.  `cmake --build build --target fuzzers` builds
# exactly these and nothing else, which is what CI does.
if(AMBAR_FUZZ)
  add_custom_target(fuzzers)

  foreach(name IN ITEMS version_edit block filter_block write_batch table wal
                        compress)
    add_executable(fuzz_${name} fuzz/fuzz_${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE ambar Threads::Threads)
    target_include_directories(fuzz_${name} PRIVATE src)
    # This is the one place libFuzzer's main comes in.
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    add_dependencies(fuzzers fuzz_${name})
  endforeach()

  add_executable(fuzz_seeds fuzz/make_seeds.cpp)
  target_link_libraries(fuzz_seeds PRIVATE ambar Threads::Threads)
  target_include_directories(fuzz_seeds PRIVATE src)
  add_dependencies(fuzzers fuzz_seeds)
endif()
