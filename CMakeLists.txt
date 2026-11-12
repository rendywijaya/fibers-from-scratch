cmake_minimum_required(VERSION 3.20)
project(fib LANGUAGES CXX ASM VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(FIB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(FIB_TESTS    "Build tests" ON)
option(FIB_BENCH    "Build benchmarks" ON)

# Sanitizer flags must precede every target: they are copied into a target
# when it is created, and an uninstrumented library under instrumented tests
# trips libc++'s container annotations.
if(FIB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

# Pick the context switch for this CPU.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" FIB_ARCH)
if(FIB_ARCH MATCHES "^(arm64|aarch64)$")
  set(FIB_SWITCH src/switch_aarch64.S)
elseif(FIB_ARCH MATCHES "^(x86_64|amd64)$")
  set(FIB_SWITCH src/switch_x86_64.S)
else()
  message(FATAL_ERROR "no context switch for ${CMAKE_SYSTEM_PROCESSOR}; this project implements AArch64 and x86-64")
endif()
message(STATUS "fib: context switch for ${CMAKE_SYSTEM_PROCESSOR} -> ${FIB_SWITCH}")

add_library(fib STATIC src/context.cpp src/fiber.cpp ${FIB_SWITCH})
target_include_directories(fib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(fib PUBLIC cxx_std_20)
target_compile_options(fib PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror>)
find_package(Threads REQUIRED)
target_link_libraries(fib PUBLIC Threads::Threads)

add_library(fib_warnings INTERFACE)
target_compile_options(fib_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror)

if(FIB_TESTS)
  enable_testing()
  # A sanitized test binary must not link a prebuilt, uninstrumented GoogleTest:
  # libc++'s container annotations then disagree between the two and report
  # false container-overflows. Under FIB_SANITIZE, GoogleTest is built here
  # with the same flags.
  if(NOT FIB_SANITIZE)
    find_package(GTest CONFIG QUIET)
  endif()
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  include(GoogleTest)
  foreach(t context fiber coro)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE fib fib_warnings GTest::gtest_main)
    gtest_discover_tests(test_${t} DISCOVERY_TIMEOUT 60)
  endforeach()
endif()

if(FIB_BENCH)
  add_executable(switch_bench bench/switch_bench.cpp)
  target_link_libraries(switch_bench PRIVATE fib fib_warnings)
endif()
