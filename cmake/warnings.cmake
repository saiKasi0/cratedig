# Interface target carrying the project-wide warning set. Link it PRIVATE from
# every real target; third-party code fetched via deps.cmake never sees it.

add_library(cratedig_warnings INTERFACE)

target_compile_options(
  cratedig_warnings
  INTERFACE -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wdouble-promotion
            -Wnull-dereference
            -Wnon-virtual-dtor
            -Wimplicit-fallthrough
            -Wcast-align)

if(CRATEDIG_WERROR)
  target_compile_options(cratedig_warnings INTERFACE -Werror)
endif()
