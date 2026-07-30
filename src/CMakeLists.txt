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
            rt/spsc_ring.hpp)
