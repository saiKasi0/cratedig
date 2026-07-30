include(Catch)

add_executable(cratedig_unit_tests unit/smoke_test.cpp)

target_link_libraries(
  cratedig_unit_tests
  PRIVATE Catch2::Catch2WithMain cratedig_warnings cratedig_sanitizer)

catch_discover_tests(cratedig_unit_tests PROPERTIES LABELS unit)
