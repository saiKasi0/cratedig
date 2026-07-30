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

# Fail loudly rather than at link time if an override above was ignored: a shared
# rtaudio, or one built against an unintended backend, is exactly the kind of
# silent host-dependent divergence the pinned API list exists to prevent.
get_target_property(_cratedig_rtaudio_type rtaudio TYPE)
if(NOT _cratedig_rtaudio_type STREQUAL "STATIC_LIBRARY")
  message(FATAL_ERROR "cratedig: expected a static rtaudio, got ${_cratedig_rtaudio_type}")
endif()

# RtMidi (MIT-style) — MIDI input, linked ONLY from src/io/. Activates at M4.
# FetchContent_Declare(rtmidi
#   URL https://github.com/thestk/rtmidi/archive/refs/tags/6.0.0.tar.gz
#   URL_HASH SHA256=<pin at activation> SYSTEM EXCLUDE_FROM_ALL)

# -- M2: TUI -----------------------------------------------------------------------
# FTXUI (MIT)
# FetchContent_Declare(ftxui
#   URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v5.0.0.tar.gz
#   URL_HASH SHA256=<pin at activation> SYSTEM EXCLUDE_FROM_ALL)

# -- M3: onset detection -----------------------------------------------------------
# PFFFT (FFTPACK license) — no tagged releases; pin a commit hash tarball
# FetchContent_Declare(pffft
#   URL https://bitbucket.org/jpommier/pffft/get/<commit>.tar.gz
#   URL_HASH SHA256=<pin at activation> SYSTEM EXCLUDE_FROM_ALL)

# -- M7: scripting -----------------------------------------------------------------
# Lua 5.4 (MIT) + sol2 (MIT, header-only)

# -- M8: plugin hosting ------------------------------------------------------------
# CLAP (MIT, header-only); lilv (ISC) as a system package (Linux/macOS)
