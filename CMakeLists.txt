cmake_minimum_required(VERSION 3.20)
project(LimitOrderBook VERSION 0.1.0 LANGUAGES CXX)

# Enforce C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler Warnings
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# -----------------------------------------------------------------------------
# Core Library
# -----------------------------------------------------------------------------
add_library(lob_core
    src/order_book.cpp
)

target_include_directories(lob_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# -----------------------------------------------------------------------------
# Demo Executable (src/main.cpp)
# -----------------------------------------------------------------------------
add_executable(lob_demo
    src/main.cpp
)
target_link_libraries(lob_demo PRIVATE lob_core)

# -----------------------------------------------------------------------------
# Unit Tests (tests/order_book_tests.cpp)
# -----------------------------------------------------------------------------
enable_testing()
add_executable(order_book_tests
    tests/order_book_tests.cpp
)
target_link_libraries(order_book_tests PRIVATE lob_core)
add_test(NAME order_book_tests COMMAND order_book_tests)

# -----------------------------------------------------------------------------
# Benchmarks (benchmarks/bench_replay.cpp)
# -----------------------------------------------------------------------------
add_executable(bench_replay
    benchmarks/bench_replay.cpp
)
target_link_libraries(bench_replay PRIVATE lob_core)

