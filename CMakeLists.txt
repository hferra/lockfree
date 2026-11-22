cmake_minimum_required(VERSION 3.16)
project(lockfree VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_TSAN "Build everything with ThreadSanitizer" OFF)
option(LOCKFREE_NATIVE_ARCH "Compile with -march=native (disable for portable binaries)" ON)

find_package(Threads REQUIRED)

# --- Header-only library target -------------------------------------------------------------
add_library(lockfree INTERFACE)
target_include_directories(lockfree INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lockfree INTERFACE cxx_std_20)
target_link_libraries(lockfree INTERFACE Threads::Threads)

# --- Compiler options shared by the benchmark and the tests ---------------------------------
add_library(lockfree_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lockfree_options INTERFACE -Wall -Wextra)
    if(ENABLE_TSAN)
        target_compile_options(lockfree_options INTERFACE -fsanitize=thread -g -O1)
        target_link_options(lockfree_options INTERFACE -fsanitize=thread)
    else()
        target_compile_options(lockfree_options INTERFACE -O3)
    endif()
    if(LOCKFREE_NATIVE_ARCH)
        target_compile_options(lockfree_options INTERFACE -march=native)
    endif()
endif()

# --- GoogleTest -------------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# --- Benchmark (not run by ctest: 1000 runs, roughly 2.5 minutes) -----------------------------
add_executable(queue_benchmark benchmarks/QueueBenchmark.cpp)
target_link_libraries(queue_benchmark PRIVATE lockfree lockfree_options)

# --- Tests ------------------------------------------------------------------------------------
add_executable(pool_unit_tests unit_tests/PoolTest.cpp)
target_link_libraries(pool_unit_tests PRIVATE lockfree lockfree_options GTest::gtest_main)
gtest_discover_tests(pool_unit_tests PROPERTIES TIMEOUT 60)

add_executable(barrier_test unit_tests/BarrierTest.cpp)
target_link_libraries(barrier_test PRIVATE lockfree lockfree_options)
add_test(NAME BarrierTest COMMAND barrier_test)
set_tests_properties(BarrierTest PROPERTIES TIMEOUT 120)
