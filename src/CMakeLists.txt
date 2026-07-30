# Module targets land here milestone by milestone:
#   src/rt/     — M0 (header-only INTERFACE + RT_SCOPE guard OBJECT lib)
#   src/engine/ — M0 (STATIC, -fno-exceptions)
#   src/io/ src/ingest/ src/tui/ src/lua/ src/host/ — M1+

# The real-time lane is header-only: templates, constexpr tables, and small
# inline primitives. It deliberately depends on nothing outside src/rt/.
add_library(cratedig_rt INTERFACE)
target_include_directories(cratedig_rt INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(
  cratedig_rt
  INTERFACE FILE_SET
            HEADERS
            BASE_DIRS
            ${CMAKE_CURRENT_SOURCE_DIR}
            FILES
            rt/arch.hpp
            rt/result.hpp
            rt/rt_scope.hpp
            rt/spsc_ring.hpp)

# Every consumer must agree with rt_scope.cpp about whether the guard exists —
# the RT_SCOPE() macro and the operator replacements are compiled from the same
# switch, so it lives on the INTERFACE target rather than per-target.
target_compile_definitions(cratedig_rt INTERFACE CRATEDIG_RT_GUARD=$<BOOL:${CRATEDIG_RT_GUARD}>)

# The allocation guard replaces global operator new/delete. It MUST be an OBJECT
# library: from a static library the linker would only pull in objects that
# resolve an undefined symbol, and replacement operators resolve none, so the
# object would be dropped and the guard would silently enforce nothing. Consumers
# add $<TARGET_OBJECTS:cratedig_rt_guard> to their sources.
add_library(cratedig_rt_guard OBJECT rt/rt_scope.cpp)
target_link_libraries(cratedig_rt_guard PUBLIC cratedig_rt)
target_link_libraries(cratedig_rt_guard PRIVATE cratedig_warnings cratedig_sanitizer)
