cmake_minimum_required(VERSION 3.28)

project(
  cratedig
  VERSION 0.0.1
  DESCRIPTION "the terminal crate-digging DAW"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CRATEDIG_BUILD_TESTS "Build unit/integration tests" ON)
option(CRATEDIG_WERROR "Treat warnings as errors" ON)
option(CRATEDIG_RT_GUARD "Enable the RT_SCOPE allocation guard" ON)

include(cmake/warnings.cmake)
include(cmake/sanitizers.cmake)
include(cmake/deps.cmake)

add_subdirectory(src)

if(CRATEDIG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
