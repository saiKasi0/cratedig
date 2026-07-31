# Fixed dependency set (CLAUDE.md). Dependencies activate at the milestone that
# first needs them — staging table in docs/ARCHITECTURE.md. Every FetchContent
# uses URL + SHA256 (deterministic, proxy-cacheable), SYSTEM to keep third-party
# headers out of our warning set, and EXCLUDE_FROM_ALL so only linked targets build.
# Adding anything outside this set is a human decision — propose, don't vendor.

include(FetchContent)

# -- Catch2 (tests only, BSL-1.0) — active since M0 --------------------------------
if(CRATEDIG_BUILD_TESTS)
  FetchContent_Declare(
    Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.8.1.tar.gz
    URL_HASH SHA256=18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79
    SYSTEM EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

# -- M1: playback + decode ---------------------------------------------------------
#
# FFmpeg is NOT here: it is a system package, dynamically linked, handled by
# cmake/ffmpeg.cmake for the licensing reasons in docs/LICENSING.md.
#
# Third-party targets are built without our warning set and without sanitizer
# instrumentation, the same treatment Catch2 gets. -Werror is ours to satisfy, not
# theirs, and none of these libraries is exercised by the concurrency tests.

# These two lines are load-bearing; both were found by the dependency activation
# failing, not by reading ahead.
#
# CMP0077: RtAudio declares its API switches with option(). Its
# cmake_minimum_required is old enough that option() still *clears* a normal
# variable of the same name, so every set() below would be silently discarded and
# the API list would come from host detection instead. Defaulting the policy to
# NEW for subprojects is what makes the overrides take effect.
#
# CMAKE_POLICY_VERSION_MINIMUM: libsamplerate 0.2.2 declares
# cmake_minimum_required(VERSION 3.1..3.18). CMake 4 removed compatibility with
# < 3.5 and hard-errors; CMake 3.28 (Ubuntu 24.04, our CI image) accepts it. Left
# alone this configures in Docker and fails on a current macOS toolchain — the
# exact platform drift the Docker path exists to prevent, running backwards. Both
# are unset after the block so neither leaks into our own targets and hides a
# future problem there.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Every FetchContent dependency is linked statically. docs/LICENSING.md's table
# assumes it, and a binary that needs a .dylib out of build/_deps/ is not a
# binary anyone can run.
#
# This is a NORMAL variable, deliberately, and it is never unset. RtAudio's
# CMakeLists contains `option(BUILD_SHARED_LIBS "Build as shared library" ON)`,
# which creates a *global cache entry* set to ON. RtAudio itself escapes the
# consequences because RTAUDIO_BUILD_STATIC_LIBS wins in its own logic — but
# every dependency configured after it inherits the poisoned cache. That is
# exactly what happened through M1: libsamplerate silently built as
# libsamplerate.0.dylib and the cratedig binary linked @rpath against it, while
# NOTICE claimed static. A normal variable shadows the cache entry for the whole
# directory scope, and under CMP0077 NEW (set above) option() then leaves it
# alone instead of overwriting it.
set(BUILD_SHARED_LIBS OFF)

# BUILD_TESTING is not ours — we gate our suite on CRATEDIG_BUILD_TESTS and call
# enable_testing() directly — but RtAudio's CMakeLists calls include(CTest), which
# defines BUILD_TESTING=ON as a global cache entry. libsamplerate, configured
# afterwards, honours it and registers its own varispeed/float_short/snr_bw tests
# into our ctest run, where they fail as "Not Run" because their data files were
# never generated. Forcing it off keeps third-party suites out of our results.
set(BUILD_TESTING OFF)

# RtAudio (MIT-style) — device I/O, linked ONLY from src/io/
#
# RTAUDIO_BUILD_STATIC_LIBS rather than BUILD_SHARED_LIBS: RtAudio checks the
# former with a plain if(DEFINED), which does not depend on policy settings, and
# it does not touch the global BUILD_SHARED_LIBS that the rest of the build reads.
# The license table in docs/LICENSING.md assumes static linkage.
#
# RTAUDIO_BUILD_TESTING must be set explicitly: RtAudio's CMakeLists calls
# include(CTest), which defines BUILD_TESTING=ON, and it then defaults its own
# test suite to that. Left alone it would build and register RtAudio's
# interactive device tests into our ctest run.
#
# The API list is pinned rather than auto-detected so a machine that happens to
# have JACK or PulseAudio headers does not silently produce a different binary
# from CI's. ALSA on Linux, CoreAudio on macOS, nothing else.
set(RTAUDIO_BUILD_STATIC_LIBS ON)
set(RTAUDIO_BUILD_TESTING OFF)
set(RTAUDIO_BUILD_PYTHON OFF)
set(RTAUDIO_API_JACK OFF)
set(RTAUDIO_API_PULSE OFF)
set(RTAUDIO_API_OSS OFF)
set(RTAUDIO_API_DS OFF)
set(RTAUDIO_API_ASIO OFF)
set(RTAUDIO_API_WASAPI OFF)
set(RTAUDIO_API_ALSA ${LINUX})
set(RTAUDIO_API_CORE ${APPLE})
FetchContent_Declare(
  rtaudio
  URL https://github.com/thestk/rtaudio/archive/refs/tags/6.0.1.tar.gz
  URL_HASH SHA256=7206c8b6cee43b474f43d64988fefaadfdcfc4264ed38d8de5f5d0e6ddb0a123
  SYSTEM EXCLUDE_FROM_ALL)

# libsamplerate (BSD-2) — resampling at load time, in src/ingest/ only.
# Exports SampleRate::samplerate.
set(LIBSAMPLERATE_EXAMPLES OFF)
set(LIBSAMPLERATE_INSTALL OFF)
FetchContent_Declare(
  libsamplerate
  URL https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz
  URL_HASH SHA256=3258da280511d24b49d6b08615bbe824d0cacc9842b0e4caf11c52cf2b043893
  SYSTEM EXCLUDE_FROM_ALL)

# CLI11 (BSD-3) — arg parsing for the cratedig binary. Header-only by default.
FetchContent_Declare(
  cli11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.6.2.tar.gz
  URL_HASH SHA256=c6ea6b2e5608b3ea8617999bd5f47420c71b2ebdb8dc4767c1034d1da5785711
  SYSTEM EXCLUDE_FROM_ALL)

FetchContent_MakeAvailable(rtaudio libsamplerate cli11)

unset(CMAKE_POLICY_DEFAULT_CMP0077)
unset(CMAKE_POLICY_VERSION_MINIMUM)
unset(BUILD_TESTING)

# The static-linkage post-condition is asserted at the bottom of this file, over
# every dependency rather than only rtaudio — checking one of them is what let
# libsamplerate build shared unnoticed for a whole milestone.

# RtMidi (MIT-style) — MIDI input, linked ONLY from src/io/. Activates at M4.
# FetchContent_Declare(rtmidi
#   URL https://github.com/thestk/rtmidi/archive/refs/tags/6.0.0.tar.gz
#   URL_HASH SHA256=<pin at activation> SYSTEM EXCLUDE_FROM_ALL)

# -- M2: TUI -----------------------------------------------------------------------
#
# FTXUI (MIT) — the terminal interface, linked ONLY from src/tui/.
#
# CMP0077 again, for the same reason as RtAudio above but a different cause:
# FTXUI declares cmake_minimum_required(VERSION 3.12), and CMP0077 was introduced
# in 3.13. Below that, option() still clears a normal variable of the same name,
# so every FTXUI_* override here would be silently discarded and we would build
# its examples and tests. The guard is set and unset around this block alone so
# it cannot leak into our own targets.
#
# 3.12 is >= 3.5, so unlike libsamplerate this needs no
# CMAKE_POLICY_VERSION_MINIMUM shim on CMake 4.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

set(FTXUI_BUILD_DOCS OFF)
set(FTXUI_BUILD_EXAMPLES OFF)
set(FTXUI_BUILD_TESTS OFF)
set(FTXUI_BUILD_TESTS_FUZZER OFF)
# C++20 modules require the Ninja/MSVC generator and CMake >= 3.28.2, and we
# deliberately disable module scanning project-wide (see the root CMakeLists).
set(FTXUI_BUILD_MODULES OFF)
set(FTXUI_ENABLE_INSTALL OFF)
set(FTXUI_CLANG_TIDY OFF)
set(FTXUI_DEV_WARNINGS OFF)
set(FTXUI_QUIET ON)
FetchContent_Declare(
  ftxui
  URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v7.0.1.tar.gz
  URL_HASH SHA256=80f544bb47fab24d3e57bc561324da228c050b3f2e8683fe806883ca5cd561a2
  SYSTEM EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(ftxui)

unset(CMAKE_POLICY_DEFAULT_CMP0077)

# -- Static-linkage post-condition -------------------------------------------------
#
# Asserted rather than assumed. None of these libraries declares STATIC
# explicitly; they all follow BUILD_SHARED_LIBS, and the failure is silent — the
# build succeeds, the tests pass, and the binary only breaks once build/ is gone.
# Checking every target is the point: the M1 version of this check covered
# rtaudio alone, and libsamplerate went shared behind it.
foreach(_cratedig_dep IN ITEMS rtaudio samplerate screen dom component)
  get_target_property(_cratedig_dep_type ${_cratedig_dep} TYPE)
  if(NOT _cratedig_dep_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "cratedig: expected a static ${_cratedig_dep}, got ${_cratedig_dep_type}")
  endif()
endforeach()

# -- M3: onset detection -----------------------------------------------------------
#
# PFFFT (FFTPACK license — BSD-style, see docs/LICENSING.md and NOTICE).
#
# Upstream has no tagged releases, so the URL names a COMMIT hash rather than a
# branch: bitbucket serves whatever a branch currently points at, and a
# dependency that changed underneath us would change onset results without
# changing this repository. Verified byte-stable across two downloads, which is
# what makes URL_HASH usable here at all.
FetchContent_Declare(
  pffft
  URL https://bitbucket.org/jpommier/pffft/get/09796885cd5b.tar.gz
  URL_HASH SHA256=fdc80563de8c31d6380886bc1ba0ffb897abde58611707ac94eb8ed8ab850cbb
  SYSTEM EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(pffft)

# Upstream ships pffft.c/h and fftpack.c/h with NO build system of any kind, so
# the target is ours to declare. Only pffft.c is compiled: fftpack.c is the
# original Fortran-derived double-precision code, used by upstream's own
# benchmark for comparison and not by the library.
add_library(pffft STATIC ${pffft_SOURCE_DIR}/pffft.c)
target_include_directories(pffft SYSTEM PUBLIC ${pffft_SOURCE_DIR})

# Third-party C, held to its own standards rather than to ours -- the same
# treatment RtAudio and libsamplerate get. -Werror is ours to satisfy, not
# theirs, and pffft.c is 3000 lines of deliberate pointer arithmetic that would
# light up every warning in our set.
target_compile_options(pffft PRIVATE -w)
set_target_properties(pffft PROPERTIES POSITION_INDEPENDENT_CODE ON)

# -- M7: scripting -----------------------------------------------------------------
# Lua 5.4 (MIT) + sol2 (MIT, header-only)

# -- M8: plugin hosting ------------------------------------------------------------
# CLAP (MIT, header-only); lilv (ISC) as a system package (Linux/macOS)
