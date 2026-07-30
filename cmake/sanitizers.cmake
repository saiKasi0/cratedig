# Sanitizer selection. Presets set CRATEDIG_SANITIZER (address|thread|undefined);
# empty means none. Kept here (not in the presets) so the flags live in one place
# and apply to compile and link of every target that links cratedig_sanitizer.

set(CRATEDIG_SANITIZER
    ""
    CACHE STRING "Sanitizer to enable: address, thread, undefined, or empty")

add_library(cratedig_sanitizer INTERFACE)

if(CRATEDIG_SANITIZER)
  if(NOT CRATEDIG_SANITIZER MATCHES "^(address|thread|undefined)$")
    message(FATAL_ERROR "CRATEDIG_SANITIZER must be address, thread, undefined, or empty "
                        "(got '${CRATEDIG_SANITIZER}')")
  endif()
  set(_cratedig_san_flags -fsanitize=${CRATEDIG_SANITIZER} -fno-omit-frame-pointer -g)
  target_compile_options(cratedig_sanitizer INTERFACE ${_cratedig_san_flags})
  target_link_options(cratedig_sanitizer INTERFACE ${_cratedig_san_flags})
endif()
