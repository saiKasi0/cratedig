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
# RtAudio (MIT-style) — device I/O, linked ONLY from src/io/
# FetchContent_Declare(rtaudio
#   URL https://github.com/thestk/rtaudio/archive/refs/tags/6.0.1.tar.gz
#   URL_HASH SHA256=<pin at activation> SYSTEM EXCLUDE_FROM_ALL)
# RtMidi (MIT-style) — MIDI input, linked ONLY from src/io/
# FetchContent_Declare(rtmidi
#   URL https://github.com/thestk/rtmidi/archive/refs/tags/6.0.0.tar.gz
#   URL_HASH SHA256=<pin at activation> SYSTEM EXCLUDE_FROM_ALL)
# FFmpeg (LGPL-2.1+) — SYSTEM PACKAGE, dynamic linking only (docs/LICENSING.md).
#   find_package via pkg-config: libavformat libavcodec libswresample
# libsamplerate (BSD-2)
# FetchContent_Declare(libsamplerate
#   URL https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz
#   URL_HASH SHA256=<pin at activation> SYSTEM EXCLUDE_FROM_ALL)
# CLI11 (BSD-3) — arg parsing for the cratedig binary
# FetchContent_Declare(cli11
#   URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.4.2.tar.gz
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
