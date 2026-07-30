# FFmpeg — system package, dynamically linked, LGPL-covered APIs only.
#
# FFmpeg is deliberately NOT a FetchContent dependency: docs/LICENSING.md requires
# dynamic linking against a shared build we do not ship. pkg-config is how every
# platform describes the one it has.
#
# Version floor is FFmpeg 5.1 in practice: we use the AVChannelLayout API
# (swr_alloc_set_opts2, AVCodecContext::ch_layout) and never the removed
# channels/channel_layout fields. Verified against 6.1 (Ubuntu 24.04) and 8.1
# (Homebrew).

find_package(PkgConfig REQUIRED)

pkg_check_modules(
  FFMPEG
  REQUIRED
  IMPORTED_TARGET
  libavformat
  libavcodec
  libavutil
  libswresample)

message(STATUS "cratedig: FFmpeg avformat ${FFMPEG_libavformat_VERSION}, "
               "avcodec ${FFMPEG_libavcodec_VERSION}, avutil ${FFMPEG_libavutil_VERSION}, "
               "swresample ${FFMPEG_libswresample_VERSION}")

# Everything that includes an FFmpeg header links this, not PkgConfig::FFMPEG
# directly.
#
# libavutil/common.h contains an explicit #error for C++ translation units that
# have not defined __STDC_CONSTANT_MACROS: the headers use UINT64_C and friends,
# which C++ only exposes from <cstdint> when those macros are set. Carrying the
# definitions on one INTERFACE target means every consumer gets them and no
# consumer has to rediscover the diagnostic.
add_library(cratedig_ffmpeg INTERFACE)
target_link_libraries(cratedig_ffmpeg INTERFACE PkgConfig::FFMPEG)
target_compile_definitions(cratedig_ffmpeg INTERFACE __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS
                                                     __STDC_FORMAT_MACROS)

# The mechanical half of the licensing rule (docs/LICENSING.md, FFmpeg §3).
#
# A comment saying "release builds must link LGPL FFmpeg" is worth nothing on its
# own — the point of failure is a release build made on a developer machine whose
# FFmpeg happens to be a --enable-gpl distro package, which is the common case on
# both Homebrew and Debian/Ubuntu. So the check is executable, and the release
# preset turns it on. It is OFF by default because local development and CI
# distribute nothing and therefore trigger no GPL obligation.
option(CRATEDIG_REQUIRE_LGPL_FFMPEG "Fail configure unless the linked FFmpeg is an LGPL build" OFF)

if(CRATEDIG_REQUIRE_LGPL_FFMPEG)
  # .cpp, not .c: this project enables only CXX, and try_compile() refuses a
  # language the project never enabled. The FFmpeg headers are C, hence extern "C".
  set(_cratedig_lgpl_probe "${CMAKE_CURRENT_BINARY_DIR}/cratedig_ffmpeg_license_probe.cpp")
  file(
    WRITE "${_cratedig_lgpl_probe}"
    "#include <cstdio>\n"
    "extern \"C\" {\n"
    "#include <libavutil/avutil.h>\n"
    "}\n"
    "int main() { std::printf(\"%s\", avutil_license()); return 0; }\n")

  try_run(
    _cratedig_lgpl_run _cratedig_lgpl_compile "${CMAKE_CURRENT_BINARY_DIR}/cratedig_lgpl_probe"
    "${_cratedig_lgpl_probe}"
    # PkgConfig::FFMPEG and not cratedig_ffmpeg: try_run() generates a separate
    # throwaway project that can only resolve IMPORTED targets, so the INTERFACE
    # wrapper above is invisible to it and its compile definitions must be
    # repeated here.
    COMPILE_DEFINITIONS -D__STDC_CONSTANT_MACROS
    LINK_LIBRARIES PkgConfig::FFMPEG
    RUN_OUTPUT_VARIABLE _cratedig_ffmpeg_license
    COMPILE_OUTPUT_VARIABLE _cratedig_lgpl_compile_output)

  if(NOT _cratedig_lgpl_compile)
    message(FATAL_ERROR "cratedig: could not build the FFmpeg license probe:\n"
                        "${_cratedig_lgpl_compile_output}")
  endif()
  if(NOT _cratedig_lgpl_run EQUAL 0)
    message(FATAL_ERROR "cratedig: the FFmpeg license probe failed to run")
  endif()

  # avutil_license() returns e.g. "LGPL version 2.1 or later" or "GPL version 3 or later".
  if(NOT _cratedig_ffmpeg_license MATCHES "^LGPL")
    message(
      FATAL_ERROR
        "cratedig: CRATEDIG_REQUIRE_LGPL_FFMPEG is ON but the linked FFmpeg reports "
        "'${_cratedig_ffmpeg_license}'.\n"
        "  Distributing a CRATEDIG binary linked against a GPL FFmpeg would make the "
        "combination GPL rather than Apache-2.0 (docs/LICENSING.md).\n"
        "  Build FFmpeg without --enable-gpl / --enable-nonfree and point PKG_CONFIG_PATH "
        "at it, or turn this option off for a build that will not be distributed.")
  endif()
  message(STATUS "cratedig: FFmpeg license '${_cratedig_ffmpeg_license}' — OK for distribution")
endif()
