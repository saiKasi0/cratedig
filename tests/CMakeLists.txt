include(Catch)

# Enforces that every src/rt/ header compiles without exceptions or RTTI, the
# way real-time targets consume them. Nothing links this — building it is the test.
add_library(cratedig_rt_nofx_check OBJECT compile/all_rt_headers.cpp)
target_link_libraries(cratedig_rt_nofx_check PRIVATE cratedig_rt cratedig_warnings
                                                     cratedig_sanitizer)
target_compile_options(cratedig_rt_nofx_check PRIVATE -fno-exceptions -fno-rtti)

add_executable(
  cratedig_unit_tests
  unit/smoke_test.cpp
  unit/result_test.cpp
  unit/spsc_ring_test.cpp)

target_link_libraries(
  cratedig_unit_tests PRIVATE Catch2::Catch2WithMain cratedig_rt cratedig_warnings
                              cratedig_sanitizer)

# Building the no-exceptions check is part of building the tests.
add_dependencies(cratedig_unit_tests cratedig_rt_nofx_check)

catch_discover_tests(cratedig_unit_tests PROPERTIES LABELS unit)
