cmake_minimum_required(VERSION 3.28)

project(
  cratedig
  VERSION 0.0.1
  DESCRIPTION "the terminal crate-digging DAW"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# We do not use C++20 modules. Leaving the scanner on makes CMake emit .modmap
# response files into the compile database, which clang-tidy cannot resolve
# (they only exist mid-build), and it slows every build for no benefit.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(CRATEDIG_BUILD_TESTS "Build unit/integration tests" ON)
option(CRATEDIG_WERROR "Treat warnings as errors" ON)
option(CRATEDIG_RT_GUARD "Enable the RT_SCOPE allocation guard" ON)

include(cmake/warnings.cmake)
include(cmake/sanitizers.cmake)
include(cmake/ffmpeg.cmake)
include(cmake/deps.cmake)

add_subdirectory(src)

if(CRATEDIG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
