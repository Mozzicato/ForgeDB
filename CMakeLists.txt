cmake_minimum_required(VERSION 3.20)
project(ForgeDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FORGEDB_TSAN "Build with ThreadSanitizer (Linux, gcc/clang)" OFF)
if(FORGEDB_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- the engine ------------------------------------------------------------
add_library(forgedb
  src/crc32.cpp
  src/record.cpp
  src/replay.cpp
  src/log_file.cpp
  src/db.cpp
  src/fault.cpp
)
target_include_directories(forgedb PUBLIC src)

# ---- tests -------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  # GitHub publishes no checksum for this release; the hash pins the exact
  # tarball we first downloaded. A local copy in third_party_cache/ is used
  # when present (slow GitHub downloads on the dev machine).
  set(GTEST_TARBALL "${CMAKE_SOURCE_DIR}/third_party_cache/googletest-1.17.0.tar.gz")
  if(NOT EXISTS "${GTEST_TARBALL}")
    set(GTEST_TARBALL "https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz")
  endif()
  FetchContent_Declare(googletest
    URL "${GTEST_TARBALL}"
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(forgedb_tests
    tests/crc32_test.cpp
    tests/record_test.cpp
    tests/replay_test.cpp
    tests/log_file_test.cpp
    tests/db_test.cpp
    tests/txn_test.cpp
    tests/concurrency_test.cpp
    tests/stress_test.cpp
    tests/fault_injection_test.cpp
  )
  target_link_libraries(forgedb_tests PRIVATE forgedb GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(forgedb_tests DISCOVERY_TIMEOUT 60)
endif()

# ---- benchmarks ----------------------------------------------------------------
add_executable(forgedb_bench benchmarks/forgedb_bench.cpp)
target_link_libraries(forgedb_bench PRIVATE forgedb)
