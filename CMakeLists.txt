cmake_minimum_required(VERSION 3.16)
project(LockFreeQueueBenchmark VERSION 1.0 LANGUAGES CXX)

# Force C++20 standard compliance for GCC 15.3
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimized compiler flags for performance benchmarking
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-O3 -Wall -Wextra -march=native)
endif()

# Locate the required POSIX threads library
find_package(Threads REQUIRED)

# Define the executable target using only main.cpp
add_executable(lf_queue_test
    cpp/main.cpp
)

# Map the 'include' directory so main.cpp can find the header files
target_include_directories(lf_queue_test PRIVATE include)

# Link the system thread library explicitly to the binary
target_link_libraries(lf_queue_test PRIVATE Threads::Threads)
