cmake_minimum_required(VERSION 3.16)
project(ambar LANGUAGES CXX)

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

if(NOT MSVC)
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
)
target_include_directories(ambar PUBLIC include PRIVATE src)

enable_testing()

find_package(Threads REQUIRED)

add_executable(ambar_tests
  tests/main.cpp
  tests/test_encoding.cpp
  tests/test_wal.cpp
  tests/test_dbformat.cpp
  tests/test_skiplist.cpp
  tests/test_memtable.cpp
  tests/test_write_batch.cpp
  tests/test_bloom.cpp
  tests/test_filter_block.cpp
  tests/test_hash.cpp
  tests/test_table.cpp
  tests/test_merger.cpp
  tests/test_db.cpp
  tests/test_corrupt.cpp
)
target_link_libraries(ambar_tests PRIVATE ambar Threads::Threads)
target_include_directories(ambar_tests PRIVATE src tests)
add_test(NAME unit COMMAND ambar_tests)

# Tools.  Built by default so that a change which breaks them fails the normal
# build rather than being discovered when someone comes to run them.
if(NOT MSVC)
  add_executable(ambar_crash_test tools/crash_test.cpp)
  target_link_libraries(ambar_crash_test PRIVATE ambar Threads::Threads)
  target_include_directories(ambar_crash_test PRIVATE src)
endif()

if(NOT MSVC)
  add_executable(ambar_bench tools/bench.cpp)
  target_link_libraries(ambar_bench PRIVATE ambar Threads::Threads)
  target_include_directories(ambar_bench PRIVATE src)

  # SQLite is optional.  Without it the benchmark still runs and says plainly
  # that it has nothing to compare against, rather than printing numbers with
  # no context.
  find_package(SQLite3 QUIET)
  if(SQLite3_FOUND)
    target_compile_definitions(ambar_bench PRIVATE AMBAR_BENCH_SQLITE)
    target_link_libraries(ambar_bench PRIVATE SQLite::SQLite3)
  endif()
endif()
