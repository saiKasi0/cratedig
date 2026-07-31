include(Catch)

# Enforces that every src/rt/ header compiles without exceptions or RTTI, the
# way real-time targets consume them. Nothing links this — building it is the test.
add_library(cratedig_rt_nofx_check OBJECT compile/all_rt_headers.cpp)
target_link_libraries(cratedig_rt_nofx_check PRIVATE cratedig_rt cratedig_warnings
                                                     cratedig_sanitizer)
target_compile_options(cratedig_rt_nofx_check PRIVATE -fno-exceptions -fno-rtti)

add_executable(
  cratedig_unit_tests
  unit/slices_test.cpp
  unit/smoke_test.cpp
  unit/audio_device_test.cpp
  unit/click_test.cpp
  unit/command_test.cpp
  unit/decode_test.cpp
  unit/engine_render_test.cpp
  unit/envelope_test.cpp
  unit/engine_telemetry_test.cpp
  unit/fft_test.cpp
  unit/fixture_decode_test.cpp
  unit/window_test.cpp
  unit/garbage_ring_test.cpp
  unit/handoff_ring_test.cpp
  unit/interpolator_test.cpp
  unit/keys_test.cpp
  unit/midi_device_test.cpp
  unit/midi_message_test.cpp
  unit/onset_accuracy_test.cpp
  unit/onset_test.cpp
  unit/peak_pyramid_test.cpp
  unit/result_test.cpp
  unit/rt_scope_test.cpp
  unit/sample_test.cpp
  unit/sequencer_test.cpp
  unit/spsc_ring_test.cpp
  unit/voice_pool_test.cpp
  unit/wave_view_test.cpp
  unit/waveform_test.cpp
  tui/layout_snapshot_test.cpp
  tui/waveform_perf_test.cpp
  integration/engine_threading_test.cpp
  integration/rt_safety_test.cpp
  e2e/chop_e2e_test.cpp
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

# --- the PTY session --------------------------------------------------------
# Python rather than C++: a forkpty harness plus the terminal replay it needs
# would be several times the code for the same assertion, and python3 is already
# a build dependency via scripts/verify_fixtures.py.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME tui_pty_session
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tui/pty_session.py
                   $<TARGET_FILE:cratedig> ${CMAKE_CURRENT_SOURCE_DIR}/tui/snapshots)
  set_tests_properties(tui_pty_session PROPERTIES LABELS tui)

  # The other half of the pair: a pty that ANSWERS the keyboard query, so the
  # negotiated path is driven end to end in CSI-u. The session above covers the
  # legacy path by never answering, which is what a terminal without the
  # protocol does -- between them both branches run on the real binary.
  add_test(NAME tui_pty_kitty_session
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tui/pty_kitty_session.py
                   $<TARGET_FILE:cratedig>)
  set_tests_properties(tui_pty_kitty_session PROPERTIES LABELS tui)

  # docs/ROADMAP.md's M3 acceptance, typed into the real binary. Labelled e2e
  # rather than tui because it is the acceptance rather than a layout check:
  # `ctest -L e2e` must run the whole sentence, analysis and all, and this is the
  # half of it that has a terminal in the way.
  add_test(NAME chop_pty_e2e
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/pty_chop_session.py
                   $<TARGET_FILE:cratedig>)
  set_tests_properties(chop_pty_e2e PROPERTIES LABELS e2e)
else()
  message(STATUS "cratedig: PTY session tests disabled (no python3 interpreter)")
endif()
