cmake_minimum_required(VERSION 3.20)
project(lsm_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_library(lsm
  src/bloom.cc
  src/crc32.cc
  src/db_impl.cc
  src/memtable.cc
  src/merger.cc
  src/sstable.cc
  src/version.cc
  src/wal.cc
)
target_include_directories(lsm PUBLIC include PRIVATE src)
target_link_libraries(lsm PUBLIC Threads::Threads)

foreach(tool lsm_bench lsm_crash_test lsm_cli)
  add_executable(${tool} tools/${tool}.cc)
  target_link_libraries(${tool} PRIVATE lsm)
  target_include_directories(${tool} PRIVATE src)
endforeach()

option(LSM_BUILD_TESTS "Build unit tests" ON)
if(LSM_BUILD_TESTS)
  find_package(GTest REQUIRED)
  enable_testing()
  add_executable(lsm_tests
    tests/arena_skiplist_test.cc
    tests/bloom_test.cc
    tests/crc32_test.cc
    tests/db_test.cc
    tests/sstable_test.cc
    tests/wal_test.cc
  )
  target_include_directories(lsm_tests PRIVATE src)
  target_link_libraries(lsm_tests PRIVATE lsm GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(lsm_tests)
endif()
