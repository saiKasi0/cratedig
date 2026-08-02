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
            rt/envelope.hpp
            rt/garbage_ring.hpp
            rt/handoff_ring.hpp
            rt/interpolator.hpp
            rt/pad_config.hpp
            rt/pad_event.hpp
            rt/result.hpp
            rt/rt_scope.hpp
            rt/sample.hpp
            rt/spsc_ring.hpp
            rt/voice_pool.hpp)

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

# The engine facade. STATIC (not header-only): it holds real state and is the
# boundary the TUI, io, and offline bounce all talk to.
#
# -fno-exceptions/-fno-rtti per CLAUDE.md — errors travel as rt::Result. The
# engine also must not see a device header; RtAudio/RtMidi belong to src/io/,
# which is what lets render() run in a plain loop for offline export and CI.
add_library(cratedig_engine STATIC engine/engine.cpp)
target_link_libraries(cratedig_engine PUBLIC cratedig_rt)
target_link_libraries(cratedig_engine PRIVATE cratedig_warnings cratedig_sanitizer)
target_compile_options(cratedig_engine PRIVATE -fno-exceptions -fno-rtti)

# Ingest: decoding, resampling, and later analysis and yt-dlp. Everything here
# runs on worker threads, so it is allowed to allocate, block, and fail — the
# only thing that reaches the audio lane is a finished rt::Sample.
#
# Exceptions are NOT disabled here: -fno-exceptions applies to src/rt/ and
# src/engine/ (CLAUDE.md), and this module uses std::string and std::filesystem
# freely. It depends on rt:: types only, never on the engine.
add_library(cratedig_ingest STATIC ingest/decoder.cpp ingest/fft.cpp ingest/onset.cpp ingest/slices.cpp
                                   ingest/peak_pyramid.cpp ingest/pool.cpp
                                   ingest/resampler.cpp)
target_link_libraries(cratedig_ingest PUBLIC cratedig_rt)
target_link_libraries(cratedig_ingest PRIVATE cratedig_ffmpeg SampleRate::samplerate pffft
                                              cratedig_warnings cratedig_sanitizer)

# The device layer. rtaudio is linked PRIVATE and RtAudio.h appears in exactly
# one file, io/audio_device.cpp — src/io/ is the only module permitted to see it
# at all (CLAUDE.md). scripts/check_layering.sh enforces that mechanically,
# because it is the kind of rule that decays the first time someone needs a
# device enum "just here".
add_library(cratedig_io STATIC io/audio_device.cpp io/midi_device.cpp io/midi_message.cpp)
target_link_libraries(cratedig_io PUBLIC cratedig_engine)
target_link_libraries(cratedig_io PRIVATE rtaudio rtmidi cratedig_warnings cratedig_sanitizer)

# The TUI lane. FTXUI is linked PUBLIC because the render layer's return type is
# an ftxui::Element — the snapshot tests build the same elements and render them
# to an offscreen Screen, so they need the headers too.
#
# Exceptions stay enabled here for the same reason as src/ingest/: -fno-exceptions
# applies to src/rt/ and src/engine/ (CLAUDE.md), and both FTXUI and std::string
# are used freely on the control thread.
add_library(cratedig_tui STATIC tui/app.cpp tui/cli.cpp tui/command.cpp tui/keys.cpp
                                tui/render.cpp tui/render_detail.cpp tui/render_edit.cpp
                                tui/render_mix.cpp
                                tui/ui_state.cpp tui/waveform.cpp)
target_link_libraries(cratedig_tui PUBLIC cratedig_engine cratedig_ingest cratedig_io
                                          ftxui::ftxui)
target_link_libraries(cratedig_tui PRIVATE cratedig_warnings cratedig_sanitizer)

# Single source of truth for the version string: it appears in --version and in
# the interface header, and the two drifting apart is the kind of thing nobody
# notices until a bug report quotes the wrong one.
target_compile_definitions(cratedig_tui PUBLIC CRATEDIG_VERSION="${PROJECT_VERSION}")

add_executable(cratedig tui/main.cpp)
target_link_libraries(cratedig PRIVATE cratedig_tui CLI11::CLI11 cratedig_warnings
                                       cratedig_sanitizer)
# The RT guard replaces global operator new/delete, so its objects must be linked
# into the executable directly — from a static library the linker would drop them
# and the guard would silently enforce nothing (see the OBJECT library note above).
target_sources(cratedig PRIVATE $<TARGET_OBJECTS:cratedig_rt_guard>)
