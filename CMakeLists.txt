cmake_minimum_required(VERSION 3.24)
project(lob_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LOB_WERROR "Treat warnings as errors" ON)
option(LOB_BUILD_TESTS "Build unit tests" ON)
option(LOB_BUILD_FUZZ "Build the libFuzzer target (needs LLVM clang)" OFF)
set(LOB_SANITIZE "" CACHE STRING "Sanitizers: empty, 'address,undefined' or 'thread'")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# Header-only engine.
add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob INTERFACE Threads::Threads)
target_compile_options(lob INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
if(LOB_WERROR)
  target_compile_options(lob INTERFACE -Werror)
endif()
if(LOB_SANITIZE)
  target_compile_options(lob INTERFACE -fsanitize=${LOB_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(lob INTERFACE -fsanitize=${LOB_SANITIZE})
endif()

add_subdirectory(tools)
add_subdirectory(bench)

if(LOB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(LOB_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
