cmake_minimum_required(VERSION 3.20)
project(LimitOrderBook VERSION 0.1.0 LANGUAGES CXX)

# Enforce C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release when no preset / -DCMAKE_BUILD_TYPE is given, so a plain
# `cmake -B build` never produces an unoptimized benchmark by accident.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler Warnings
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Optional sanitizers: cmake --preset sanitize
# Not every toolchain has them: MinGW-w64 GCC (MSYS2 ucrt64/mingw64) ships NO libasan/libubsan,
# so linking fails there. We probe first and fall back to a plain build with a clear message.
option(LOB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
set(LOB_SANITIZER_FLAGS "-fsanitize=address,undefined" CACHE STRING "Sanitizer flags to probe/use (GCC/Clang)")
if(LOB_SANITIZE)
    if(MSVC)
        add_compile_options(/fsanitize=address)   # MSVC: ASan only (VS 2019 16.9+)
    else()
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_FLAGS "${LOB_SANITIZER_FLAGS}")
        check_cxx_source_compiles("int main() { return 0; }" LOB_SANITIZERS_WORK)
        unset(CMAKE_REQUIRED_FLAGS)
        if(LOB_SANITIZERS_WORK)
            add_compile_options(${LOB_SANITIZER_FLAGS} -fno-omit-frame-pointer)
            add_link_options(${LOB_SANITIZER_FLAGS})
            message(STATUS "Sanitizers enabled: ${LOB_SANITIZER_FLAGS}")
        else()
            message(WARNING
                "Sanitizers requested but this toolchain cannot link them (MinGW GCC has no libasan/libubsan). "
                "Building WITHOUT sanitizers. For ASan/UBSan runs use WSL/Linux, MSYS2 clang64, or MSVC.")
        endif()
    endif()
endif()

# Git commit recorded in benchmark output (optional)
execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE LOB_GIT_COMMIT
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT LOB_GIT_COMMIT)
    set(LOB_GIT_COMMIT "unknown")
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
# INTERIM FIX: these tests use assert(), which Release (-DNDEBUG) compiles out, so they
# verified nothing. Re-enable asserts for this target until the tests move to LOB_CHECK.
if(MSVC)
    target_compile_options(order_book_tests PRIVATE /UNDEBUG)
else()
    target_compile_options(order_book_tests PRIVATE -UNDEBUG)
endif()
add_test(NAME order_book_tests COMMAND order_book_tests)

# -----------------------------------------------------------------------------
# Differential fuzzer (tests/fuzz_oracle.cpp) - engine vs brute-force reference
# -----------------------------------------------------------------------------
add_executable(fuzz_oracle
    tests/fuzz_oracle.cpp
)
target_include_directories(fuzz_oracle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(fuzz_oracle PRIVATE lob_core)
add_test(NAME fuzz_oracle_quick COMMAND fuzz_oracle --seeds 300 --steps 500)
# Heavier run, not part of the default ctest:  ctest -C Heavy -R fuzz_oracle_heavy
add_test(NAME fuzz_oracle_heavy COMMAND fuzz_oracle --seeds 5000 --steps 2000
         CONFIGURATIONS Heavy)

# -----------------------------------------------------------------------------
# Benchmarks
# -----------------------------------------------------------------------------

add_executable(bench_v2
    benchmarks/bench_v2.cpp
)
target_include_directories(bench_v2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(bench_v2 PRIVATE
    LOB_GIT_COMMIT="${LOB_GIT_COMMIT}"
    LOB_BUILD_TYPE="$<CONFIG>")
target_link_libraries(bench_v2 PRIVATE lob_core)
