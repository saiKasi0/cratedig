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
  unit/rt_scope_test.cpp
  unit/spsc_ring_test.cpp
  # The guard's operator new/delete replacements must be linked into the
  # executable directly, not via a library — see src/CMakeLists.txt.
  $<TARGET_OBJECTS:cratedig_rt_guard>)

target_link_libraries(
  cratedig_unit_tests PRIVATE Catch2::Catch2WithMain cratedig_rt cratedig_warnings
                              cratedig_sanitizer)

# Building the no-exceptions check is part of building the tests.
add_dependencies(cratedig_unit_tests cratedig_rt_nofx_check)

catch_discover_tests(cratedig_unit_tests PROPERTIES LABELS unit)

# --- standalone integration executables ---------------------------------------
# The default violation handler aborts, so it cannot be exercised from inside the
# Catch2 binary. ctest matches stderr instead of the exit status.
add_executable(cratedig_rt_scope_abort_test integration/rt_scope_abort_test.cpp
                                            $<TARGET_OBJECTS:cratedig_rt_guard>)
target_link_libraries(cratedig_rt_scope_abort_test PRIVATE cratedig_rt cratedig_warnings
                                                           cratedig_sanitizer)

add_test(
  NAME rt_scope_default_handler_aborts
  COMMAND
    ${CMAKE_COMMAND} -DTEST_EXECUTABLE=$<TARGET_FILE:cratedig_rt_scope_abort_test>
    -DEXPECT_REGEX=RT_SCOPE\ violation -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_abort.cmake)
set_tests_properties(
  rt_scope_default_handler_aborts
  PROPERTIES LABELS integration
             # The abort is the expected outcome, so the sanitizers must not turn
             # it into their own louder failure.
             ENVIRONMENT "ASAN_OPTIONS=abort_on_error=0;TSAN_OPTIONS=abort_on_error=0")
