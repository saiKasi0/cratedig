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
  unit/audio_device_test.cpp
  unit/decode_test.cpp
  unit/engine_render_test.cpp
  unit/engine_telemetry_test.cpp
  unit/fixture_decode_test.cpp
  unit/garbage_ring_test.cpp
  unit/interpolator_test.cpp
  unit/peak_pyramid_test.cpp
  unit/result_test.cpp
  unit/rt_scope_test.cpp
  unit/sample_test.cpp
  unit/spsc_ring_test.cpp
  unit/voice_pool_test.cpp
  unit/wave_view_test.cpp
  unit/waveform_test.cpp
  tui/layout_snapshot_test.cpp
  integration/engine_threading_test.cpp
  integration/rt_safety_test.cpp
  # The guard's operator new/delete replacements must be linked into the
  # executable directly, not via a library — see src/CMakeLists.txt.
  $<TARGET_OBJECTS:cratedig_rt_guard>)

target_link_libraries(
  cratedig_unit_tests PRIVATE Catch2::Catch2WithMain cratedig_rt cratedig_engine cratedig_ingest
                              cratedig_io cratedig_tui cratedig_warnings cratedig_sanitizer)

# Fixture tests locate the starter pack from the source tree, not the working
# directory: ctest runs tests from the build directory, and the pack deliberately
# lives beside the sources because it is fetched once and reused by every preset.
target_compile_definitions(
  cratedig_unit_tests PRIVATE CRATEDIG_STARTER_PACK_DIR="${CMAKE_SOURCE_DIR}/assets/starter-pack")

# Snapshots live beside their test in the source tree, not the build tree: they
# are committed goldens (docs/TESTING.md), and scripts/update_tui_snapshots.sh
# rewrites them in place.
target_compile_definitions(
  cratedig_unit_tests PRIVATE CRATEDIG_TUI_SNAPSHOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tui/snapshots")

# Building the no-exceptions check is part of building the tests.
add_dependencies(cratedig_unit_tests cratedig_rt_nofx_check)

# Labels come from each TEST_CASE's Catch2 tag, so the integration cases in this
# binary are labelled integration rather than unit.
catch_discover_tests(cratedig_unit_tests ADD_TAGS_AS_LABELS)

# --- standalone integration executables ---------------------------------------
# The default violation handler aborts, so it cannot be exercised from inside the
# Catch2 binary. ctest matches stderr instead of the exit status.
add_executable(cratedig_rt_scope_abort_test integration/rt_scope_abort_test.cpp
                                            $<TARGET_OBJECTS:cratedig_rt_guard>)
target_link_libraries(cratedig_rt_scope_abort_test PRIVATE cratedig_rt cratedig_warnings
                                                           cratedig_sanitizer)

# Under TSan the operator replacements are compiled out (its runtime defines them
# as strong symbols — see src/rt/rt_scope.hpp), so there is nothing to abort.
# Registering the test anyway would report a failure for a deliberate,
# documented configuration.
if(CRATEDIG_RT_GUARD AND NOT CRATEDIG_SANITIZER STREQUAL "thread")
  add_test(
    NAME rt_scope_default_handler_aborts
    COMMAND
      ${CMAKE_COMMAND} -DTEST_EXECUTABLE=$<TARGET_FILE:cratedig_rt_scope_abort_test>
      -DEXPECT_REGEX=RT_SCOPE\ violation -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_abort.cmake)
  set_tests_properties(
    rt_scope_default_handler_aborts
    PROPERTIES LABELS integration
               # The abort is the expected outcome, so the sanitizers must not
               # turn it into their own louder failure.
               ENVIRONMENT "ASAN_OPTIONS=abort_on_error=0")
else()
  message(STATUS "cratedig: RT_SCOPE abort test disabled (guard off or TSan build)")
endif()
