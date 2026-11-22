cmake_minimum_required(VERSION 3.16)
project(LockFreeQueueBenchmark VERSION 1.0 LANGUAGES CXX)

# Force C++20 standard compliance for GCC 15.3
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE 
)

# For GoogleTest, it's best practice to suppress overriding the compiler's runtime flag
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Optimized compiler flags for performance benchmarking
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-O3 -Wall -Wextra -march=native)
endif()

# Locate the required POSIX threads library
find_package(Threads REQUIRED)

# Enable CTest integration
enable_testing()
include(GoogleTest)

# --- Benchmark Executables ---
add_executable(queue_benchmark
    benchmarks/QueueBenchmark.cpp
)
target_include_directories(queue_benchmark PRIVATE include)
target_link_libraries(queue_benchmark PRIVATE Threads::Threads)

add_executable(barrier_test
    unit_tests/BarrierTest.cpp
)
target_include_directories(barrier_test PRIVATE include)
target_link_libraries(barrier_test PRIVATE Threads::Threads)

# --- GoogleTest Unit Tests ---
# Update 'PoolTest.cpp' if your file inside unit_tests/ has a different name
add_executable(pool_unit_tests
    unit_tests/PoolTest.cpp
)
target_include_directories(pool_unit_tests PRIVATE include)
target_link_libraries(pool_unit_tests PRIVATE
    GTest::gtest_main
    Threads::Threads
)

# Optional: Run with ThreadSanitizer (-DENABLE_TSAN=ON)
option(ENABLE_TSAN "Build unit tests with ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    target_compile_options(pool_unit_tests PRIVATE -fsanitize=thread -g -O1)
    target_link_options(pool_unit_tests PRIVATE -fsanitize=thread)
endif()

gtest_discover_tests(pool_unit_tests)


